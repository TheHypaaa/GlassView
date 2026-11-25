// GlassView Native – tools: shortcuts, section cut, move/rotate, colours, insert, export,
// mass properties, context menu, tool panels.
#include "features.hpp"
#include "am.hpp"

#include <AIS_Manipulator.hxx>
#include <Graphic3d_ClipPlane.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GProp_PrincipalProps.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <Poly_Triangulation.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <TDataStd_Name.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_StepModelType.hxx>
#include <math_Matrix.hxx>
#include <math_Jacobi.hxx>
#include <GLFW/glfw3.h>
#include <set>
#include <TopoDS_Iterator.hxx>
#include <deque>

// ======================================================================== state & helpers
Features& F() { static Features f; return f; }
DocExt& ext(Doc& d) { if (!d.ext) d.ext = std::make_shared<DocExt>(); return *d.ext; }
Doc* activeDoc(App& a) { return a.active >= 0 && a.active < (int)a.docs.size() ? a.docs[a.active].get() : nullptr; }
void requestFrames(App& a, int n) { a.framesLeft = std::max(a.framesLeft, n); a.view->Invalidate(); }
gp_Trsf partTrsf(Doc& d, int part) { return d.prs[part]->LocalTransformation(); }

bool toScreen(App& a, const gp_Pnt& p, ImVec2& out) {
  const gp_Pnt n = a.view->Camera()->Project(p);
  if (n.Z() > 1.0 || n.Z() < -1.0) return false;
  const ImVec2 W = ImGui::GetIO().DisplaySize;
  out = ImVec2((float)((n.X() + 1) * 0.5 * W.x), (float)((1 - n.Y()) * 0.5 * W.y));
  return true;
}

double worldPerPixel(App& a) {
  Handle(Graphic3d_Camera) c = a.view->Camera();
  const double H = std::max(1.f, ImGui::GetIO().DisplaySize.y);
  return c->Scale() / H;
}

static void persist(App& a, const std::string& k, const std::string& v) { a.set.kv[k] = v; a.set.save(); }
static void persistB(App& a, const std::string& k, bool v) { persist(a, k, v ? "1" : "0"); }

/** Parts under a tree node (or all parts), visible ones only. */
static std::vector<int> partsUnder(App& a, Doc& d, gv::Node* n) {
  std::vector<int> out;
  std::function<void(gv::Node*)> walk = [&](gv::Node* x) {
    for (int p : x->parts) if (a.ctx->IsDisplayed(d.prs[p])) out.push_back(p);
    for (auto& k : x->kids) walk(k.get());
  };
  walk(n ? n : d.model->root.get());
  return out;
}

static std::string nodeTitle(Doc& d, gv::Node* n) { return n ? n->name : d.name; }

// ======================================================================== shortcuts
struct Action { std::string id, group, label, def; std::function<void(App&)> run; bool fixed = false; };
static std::vector<Action>& actions() { static std::vector<Action> v; return v; }
static std::string comboFor(App& a, const Action& x) { return a.set.get("key." + x.id, x.def); }

static std::string keyName(int key) {
  if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) return std::string(1, (char)('0' + key - GLFW_KEY_0));
  if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return std::string(1, (char)('0' + key - GLFW_KEY_KP_0));
  switch (key) {
    case GLFW_KEY_ESCAPE: return "Esc"; case GLFW_KEY_DELETE: return "Del"; case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_SPACE: return "Space"; case GLFW_KEY_ENTER: return "Enter"; case GLFW_KEY_BACKSPACE: return "Backspace";
    case GLFW_KEY_UP: return "Up"; case GLFW_KEY_DOWN: return "Down"; case GLFW_KEY_LEFT: return "Left"; case GLFW_KEY_RIGHT: return "Right";
    case GLFW_KEY_HOME: return "Home"; case GLFW_KEY_END: return "End"; case GLFW_KEY_PAGE_UP: return "PgUp"; case GLFW_KEY_PAGE_DOWN: return "PgDn";
    default: break;
  }
  if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
  const char* n = glfwGetKeyName(key, 0); // follows the keyboard layout (QWERTZ …)
  if (n && *n) { std::string s(n); for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
  return "";
}

static std::string comboOf(int key, int mods) {
  std::string k = keyName(key);
  if (k.empty()) return "";
  std::string c;
  if (mods & GLFW_MOD_CONTROL) c += "Ctrl+";
  if (mods & GLFW_MOD_ALT) c += "Alt+";
  if (mods & GLFW_MOD_SHIFT) c += "Shift+";
  return c + k;
}

static std::string captureId; // action being re-bound in the editor
static bool keySheetOpen = false;

// ======================================================================== tool switching
static void setMeasure(App& a, bool on);
static void setCut(App& a, bool on);
static void setMove(App& a, bool on);
static void setMass(App& a, bool on) { F().massOn = on; requestFrames(a); }
static void setAMTool(App& a, bool on);
static void insertDialog(App& a);
static void exportNode(App& a, gv::Node* n, int fmt, const gp_Trsf& extra = gp_Trsf(), bool drop = false, const std::string& suffix = "");
static bool hideUi = false;
static void exportPng(App& a) {
  Doc* d = activeDoc(a);
  std::string base = d ? d->name : "view";
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
#ifdef _WIN32
  std::string path = saveFileDialog(glfwGetWin32Window(a.win), base + ".png", "png");
#else
  std::string path = saveFileDialog(nullptr, base + ".png", "png");
#endif
  if (path.empty()) return;
  const bool ok = exportViewPng(a, path, a.set.geti("pngScale", 2));
  a.toastMsg(ok ? "Saved " + fileNameOf(path) : "Could not save the image", !ok, 3);
}

static void registerActions() {
  auto& v = actions();
  if (!v.empty()) return;
  auto add = [&](const char* id, const char* g, const char* l, const char* def, std::function<void(App&)> fn, bool fixed = false) { v.push_back({id, g, l, def, fn, fixed}); };
  add("open", "Files & tabs", "Open file…", "Ctrl+O", [](App& a) { a.openDialog(); });
  add("insert", "Files & tabs", "Insert part into assembly…", "Ctrl+I", [](App& a) { insertDialog(a); });
  add("png", "Files & tabs", "Export view as PNG…", "Ctrl+Shift+E", [](App& a) { exportPng(a); });
  add("closeTab", "Files & tabs", "Close tab", "Ctrl+W", [](App& a) { if (a.active >= 0) a.closeDoc(a.active); });
  add("nextTab", "Files & tabs", "Next tab", "Ctrl+Tab", [](App& a) { if (a.docs.size() > 1) a.activate((a.active + 1) % (int)a.docs.size()); });
  add("prevTab", "Files & tabs", "Previous tab", "Ctrl+Shift+Tab", [](App& a) { if (a.docs.size() > 1) a.activate((a.active + (int)a.docs.size() - 1) % (int)a.docs.size()); });
  add("fit", "View", "Fit all", "F", [](App& a) { a.fitAll(true); });
  add("iso", "View", "Isometric view", "0", [](App& a) { a.animateTo(V3d_XposYnegZpos); });
  add("home", "View", "Home view", "H", [](App& a) { a.animateTo(V3d_XposYnegZpos); });
  add("viewXY", "View", "Look at XY plane", "7", [](App& a) { a.animateTo(V3d_Zpos); });
  add("viewYZ", "View", "Look at YZ plane", "8", [](App& a) { a.animateTo(V3d_Xpos); });
  add("viewZX", "View", "Look at ZX plane", "9", [](App& a) { a.animateTo(V3d_Yneg); });
  add("proj", "View", "Orthographic / perspective", "P", [](App& a) { a.set.ortho = !a.set.ortho; a.applyProjection(); a.set.save(); });
  add("shaded", "Display", "Shaded", "1", [](App& a) { a.setDisplayMode(0); });
  add("edges", "Display", "Shaded with edges", "2", [](App& a) { a.setDisplayMode(1); });
  add("wire", "Display", "Wireframe", "3", [](App& a) { a.setDisplayMode(2); });
  add("mesh", "Display", "Show tessellation", "4", [](App& a) { a.setDisplayMode(3); });
  add("xray", "Display", "X-ray", "X", [](App& a) { a.setXray(!a.xray); });
  add("edgeStyle", "Display", "Edges: uniform ↔ sharp / tangent", "E", [](App& a) { a.set.catiaEdges = !a.set.catiaEdges; a.set.save(); a.applyEdgeStyle(); });
  add("bg", "Display", "Light / dark mode", "B", [](App& a) { a.set.theme = a.night ? 0 : 1; a.applyTheme(); a.set.save(); });
  add("panels", "Display", "Hide / show panels", "U", [](App&) { hideUi = !hideUi; });
  add("tree", "Display", "Model tree", "T", [](App& a) { a.set.tree = !a.set.tree; a.set.save(); });
  add("explode", "Tools", "Exploded view", "V", [](App& a) { F().explodeOn = !F().explodeOn; requestFrames(a); });
  add("compare", "Tools", "Compare with another version…", "Ctrl+Shift+C", [](App& a) { if (F().compareOn) compareClose(a); else compareDialog(a); });
  add("pmi", "Tools", "PMI – dimensions & tolerances", "D", [](App& a) { pmiSetActive(a, !F().pmiOn); });
  add("measure", "Tools", "Measure", "M", [](App& a) { setMeasure(a, !F().measureOn); });
  add("cut", "Tools", "Section cut", "C", [](App& a) { setCut(a, !F().cutOn); });
  add("move", "Tools", "Move / rotate parts", "G", [](App& a) { setMove(a, !F().moveOn); });
  add("colors", "Tools", "Part colours", "K", [](App& a) { F().colorsOn = !F().colorsOn; requestFrames(a); });
  add("mass", "Tools", "Mass properties", "W", [](App& a) { setMass(a, !F().massOn); });
  add("am", "Tools", "AM check", "A", [](App& a) { setAMTool(a, !F().amOn); });
  add("units", "Tools", "Switch mm ↔ inch", "I", [](App& a) { units().inch = !units().inch; persistB(a, "inch", units().inch); measureRelabel(a); });
  add("settings", "Tools", "Settings", ",", [](App& a) { a.showSettings = !a.showSettings; });
  add("escape", "Tools", "Cancel / deselect", "Esc", [](App& a) {
    Features& f = F();
    if (f.measureOn && f.hasPending) measureCancel(a);
    else if (a.ctx->NbSelected() > 0) { a.ctx->ClearSelected(false); if (Doc* d = activeDoc(a)) d->selected = nullptr; }
    else if (f.measureOn) setMeasure(a, false);
    else if (f.moveOn) setMove(a, false);
    a.showSettings = false;
    keySheetOpen = false;
  }, true);
}

// ======================================================================== measure
static void setMeasure(App& a, bool on) {
  if (on && F().amOn) setAMTool(a, false);
  if (on && F().moveOn) setMove(a, false);
  measureSetActive(a, on);
}

// ======================================================================== section cut
static Handle(Graphic3d_ClipPlane) clip;
static int cutAxis = 2;      // 0 X, 1 Y, 2 Z
static bool cutFlip = false;
static double cutPos = 0;
static bool cutDragging = false;

static void cutBounds(App& a, double& lo, double& hi) {
  lo = -100; hi = 100;
  Doc* d = activeDoc(a);
  if (!d || !d->displayed || d->model->box.IsVoid()) return;
  double b[6];
  d->model->box.Get(b[0], b[1], b[2], b[3], b[4], b[5]);
  lo = b[cutAxis]; hi = b[cutAxis + 3];
}

static void applyCut(App& a) {
  if (clip.IsNull()) {
    clip = new Graphic3d_ClipPlane();
    clip->SetCapping(true);
    clip->SetUseObjectMaterial(true);
    clip->SetUseObjectTexture(false);
    clip->SetUseObjectShader(false);
  }
  gp_Dir n(cutAxis == 0, cutAxis == 1, cutAxis == 2);
  if (!cutFlip) n.Reverse(); // keep the side below the plane by default
  gp_Pnt p(cutAxis == 0 ? cutPos : 0, cutAxis == 1 ? cutPos : 0, cutAxis == 2 ? cutPos : 0);
  clip->SetEquation(gp_Pln(p, n));
  for (auto& dp : a.docs) {
    Doc& d = *dp;
    if (!d.displayed) continue;
    for (size_t k = 0; k < d.prs.size(); k++) {
      for (const Handle(AIS_InteractiveObject)& o : {Handle(AIS_InteractiveObject)(d.prs[k]), Handle(AIS_InteractiveObject)(d.soft[k])}) {
        if (o.IsNull()) continue;
        o->RemoveClipPlane(clip);
        if (F().cutOn && dp.get() == activeDoc(a)) o->AddClipPlane(clip);
      }
    }
  }
  a.view->Invalidate();
  requestFrames(a);
}

static void setCut(App& a, bool on) {
  F().cutOn = on;
  if (on) {
    double lo, hi;
    cutBounds(a, lo, hi);
    cutPos = (lo + hi) / 2;
  }
  applyCut(a);
}

/** Plane outline + drag handle (drawn in 2D over the view). */
static void drawCutHandle(App& a) {
  if (!F().cutOn) return;
  Doc* d = activeDoc(a);
  if (!d || !d->displayed || d->model->box.IsVoid()) return;
  double b[6];
  d->model->box.Get(b[0], b[1], b[2], b[3], b[4], b[5]);
  const double pad = 0.08 * std::max({b[3] - b[0], b[4] - b[1], b[5] - b[2]});
  const int u = (cutAxis + 1) % 3, v = (cutAxis + 2) % 3;
  gp_Pnt corners[4];
  const double us[4] = {b[u] - pad, b[u + 3] + pad, b[u + 3] + pad, b[u] - pad}, vs[4] = {b[v] - pad, b[v] - pad, b[v + 3] + pad, b[v + 3] + pad};
  ImVec2 sc[4];
  for (int i = 0; i < 4; i++) {
    double c[3];
    c[cutAxis] = cutPos; c[u] = us[i]; c[v] = vs[i];
    corners[i] = gp_Pnt(c[0], c[1], c[2]);
    if (!toScreen(a, corners[i], sc[i])) return;
  }
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const float s = a.scale;
  dl->AddConvexPolyFilled(sc, 4, IM_COL32(10, 132, 255, 26));
  dl->AddPolyline(sc, 4, IM_COL32(10, 132, 255, 170), ImDrawFlags_Closed, 1.5f * s);
  // handle: arrow along the normal at the plane centre
  double c[3];
  c[cutAxis] = cutPos; c[u] = (b[u] + b[u + 3]) / 2; c[v] = (b[v] + b[v + 3]) / 2;
  gp_Pnt ctr(c[0], c[1], c[2]);
  gp_Vec nv(cutAxis == 0, cutAxis == 1, cutAxis == 2);
  const double len = 60 * s * worldPerPixel(a);
  ImVec2 p0, p1;
  if (!toScreen(a, ctr, p0) || !toScreen(a, ctr.Translated(nv * len), p1)) return;
  // invisible window so the handle can be dragged without rotating the view
  const float r = 11 * s;
  ImGui::SetNextWindowPos(ImVec2(p1.x - r, p1.y - r));
  ImGui::SetNextWindowSize(ImVec2(2 * r, 2 * r));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##cuthandle", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
  ImGui::InvisibleButton("##h", ImVec2(2 * r, 2 * r));
  const bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
  if (ImGui::IsItemActive()) {
    ImVec2 md = ImGui::GetIO().MouseDelta;
    const double sx = p1.x - p0.x, sy = p1.y - p0.y, L = std::max(1.0, std::hypot(sx, sy));
    const double along = (md.x * sx + md.y * sy) / L; // pixels along the projected normal
    double lo, hi;
    cutBounds(a, lo, hi);
    cutPos = std::max(lo, std::min(hi, cutPos + along * len / L));
    applyCut(a);
    cutDragging = true;
  } else cutDragging = false;
  ImGui::End();
  ImGui::PopStyleVar();
  const ImU32 col = hov ? IM_COL32(64, 160, 255, 255) : IM_COL32(10, 132, 255, 255);
  dl = ImGui::GetForegroundDrawList();
  dl->AddLine(p0, p1, col, 2.5f * s);
  dl->AddCircleFilled(p0, 4 * s, col);
  dl->AddCircleFilled(p1, r * 0.75f, IM_COL32(255, 255, 255, 240));
  dl->AddCircle(p1, r * 0.75f, col, 24, 2.5f * s);
}

// ======================================================================== move / rotate
static Handle(AIS_Manipulator) manip;
static std::vector<int> moveParts;
static gv::Node* moveNode = nullptr;

static void detachManip(App& a) {
  if (!manip.IsNull()) {
    if (manip->IsAttached()) manip->Detach();
    a.ctx->Remove(manip, false);
    manip.Nullify();
  }
  moveParts.clear();
  moveNode = nullptr;
}

static void attachManip(App& a) {
  detachManip(a);
  Doc* d = activeDoc(a);
  if (!F().moveOn || !d || !d->displayed || !d->selected) return;
  moveNode = d->selected;
  moveParts = partsUnder(a, *d, d->selected);
  if (moveParts.empty()) return;
  manip = new AIS_Manipulator();
  manip->SetPart(AIS_MM_Scaling, false);
  manip->SetModeActivationOnDetection(true);
  AIS_Manipulator::OptionsForAttach opt;
  opt.SetAdjustPosition(true).SetAdjustSize(true).SetEnableModes(true);
#if OCC_VERSION_HEX >= 0x080000
  occ::handle<NCollection_HSequence<occ::handle<AIS_InteractiveObject>>> seq = new NCollection_HSequence<occ::handle<AIS_InteractiveObject>>();
#else
  Handle(AIS_ManipulatorObjectSequence) seq = new AIS_ManipulatorObjectSequence();
#endif
  for (int k : moveParts) seq->Append(d->prs[k]);
  manip->Attach(seq, opt);
  a.view->Invalidate();
}

/** Keep the tangent-edge overlays glued to moved parts. */
static void syncSoft(App& a) {
  Doc* d = activeDoc(a);
  if (!d || !d->displayed) return;
  for (size_t k = 0; k < d->prs.size(); k++)
    if (!d->soft[k].IsNull() && !(d->soft[k]->LocalTransformation().TranslationPart().IsEqual(d->prs[k]->LocalTransformation().TranslationPart(), 1e-12) &&
                                  d->soft[k]->LocalTransformation().HVectorialPart().Row(1).IsEqual(d->prs[k]->LocalTransformation().HVectorialPart().Row(1), 1e-12)))
      d->soft[k]->SetLocalTransformation(d->prs[k]->LocalTransformation());
}

static void setMove(App& a, bool on) {
  if (on && F().measureOn) setMeasure(a, false);
  if (on && F().amOn) setAMTool(a, false);
  F().moveOn = on;
  a.ctl->SetAllowDragging(on);
  if (on) attachManip(a);
  else detachManip(a);
  requestFrames(a);
}

static void movePartsBy(App& a, const gp_Trsf& t, bool aboutCentre) {
  Doc* d = activeDoc(a);
  if (!d || moveParts.empty()) return;
  gp_Trsf T = t;
  if (aboutCentre) {
    Bnd_Box b;
    for (int k : moveParts) { Bnd_Box bk; d->prs[k]->BoundingBox(bk); b.Add(bk); }
    if (!b.IsVoid()) {
      double x0, y0, z0, x1, y1, z1;
      b.Get(x0, y0, z0, x1, y1, z1);
      gp_Vec c((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
      gp_Trsf to, back;
      to.SetTranslation(-c);
      back.SetTranslation(c);
      T = back * t * to;
    }
  }
  for (int k : moveParts) {
    d->prs[k]->SetLocalTransformation(T * d->prs[k]->LocalTransformation());
    if (!d->soft[k].IsNull()) d->soft[k]->SetLocalTransformation(d->prs[k]->LocalTransformation());
  }
  ext(*d).snaps.clear();
  F().dotsDirty = true;
  attachManip(a);
  requestFrames(a);
}

static void resetParts(App& a) {
  Doc* d = activeDoc(a);
  if (!d) return;
  for (int k : moveParts) {
    d->prs[k]->SetLocalTransformation(d->model->parts[k].loc.Transformation());
    if (!d->soft[k].IsNull()) d->soft[k]->SetLocalTransformation(d->prs[k]->LocalTransformation());
  }
  F().dotsDirty = true;
  attachManip(a);
  requestFrames(a);
}


// ======================================================================== exploded view
static void rightPanel(App& a, const char* id, const std::function<void()>& body);
static void applyExplode(App& a, Doc& d, double f) {
  DocExt& x = ext(d);
  const size_t n = d.prs.size();
  x.exOff.resize(n, gp_Vec(0, 0, 0));
  // groups: the assembly's first level below the root (or single parts)
  gv::Node* top = d.model->root.get();
  while (top->kids.size() == 1 && top->parts.empty() && !top->kids[0]->kids.empty()) top = top->kids[0].get();
  std::vector<std::vector<int>> groups;
  for (int p : top->parts) groups.push_back({p});
  for (auto& k : top->kids) {
    std::vector<int> g;
    std::function<void(gv::Node*)> walk = [&](gv::Node* q) { for (int p : q->parts) g.push_back(p); for (auto& c : q->kids) walk(c.get()); };
    walk(k.get());
    if (!g.empty()) groups.push_back(g);
  }
  if (groups.size() == 1 && groups[0].size() > 1) { std::vector<int> g = groups[0]; groups.clear(); for (int p : g) groups.push_back({p}); }
  // base centres (without the current explode offset)
  auto centreOf = [&](const std::vector<int>& g) {
    Bnd_Box b;
    for (int p : g) {
      Bnd_Box bp;
      BRepBndLib::Add(d.model->parts[p].shape.Moved(TopLoc_Location(partTrsf(d, p))), bp, false);
      if (bp.IsVoid()) continue;
      double x0, y0, z0, x1, y1, z1;
      bp.Get(x0, y0, z0, x1, y1, z1);
      const gp_Vec o = x.exOff[p];
      b.Add(gp_Pnt(x0 - o.X(), y0 - o.Y(), z0 - o.Z()));
      b.Add(gp_Pnt(x1 - o.X(), y1 - o.Y(), z1 - o.Z()));
    }
    return b;
  };
  Bnd_Box all;
  std::vector<gp_Pnt> gc;
  for (auto& g : groups) {
    Bnd_Box b = centreOf(g);
    if (b.IsVoid()) { gc.push_back(gp_Pnt()); continue; }
    all.Add(b);
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    gc.push_back(gp_Pnt((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2));
  }
  if (all.IsVoid()) return;
  double x0, y0, z0, x1, y1, z1;
  all.Get(x0, y0, z0, x1, y1, z1);
  const gp_Pnt ac((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
  const double diag = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
  for (size_t gi = 0; gi < groups.size(); gi++) {
    gp_Vec v(ac, gc[gi]);
    if (v.Magnitude() < diag * 0.02) v = gp_Vec(0, 0, diag * 0.25);  // centred part: lift it
    const gp_Vec off = v * (1.4 * f);
    for (int p : groups[gi]) {
      const gp_Vec delta = off - x.exOff[p];
      gp_Trsf t;
      t.SetTranslation(delta);
      d.prs[p]->SetLocalTransformation(t * d.prs[p]->LocalTransformation());
      if (!d.soft[p].IsNull()) d.soft[p]->SetLocalTransformation(d.prs[p]->LocalTransformation());
      x.exOff[p] = off;
    }
  }
  x.explode = f;
  x.snaps.clear();
  F().dotsDirty = true;
  if (F().moveOn) attachManip(a);
  requestFrames(a);
}

static void explodePanel(App& a) {
  Ui& ui = a.ui;
  Doc* d = activeDoc(a);
  rightPanel(a, "##explode", [&] {
    bool open = true;
    ui.panelHeader("Exploded view", &open);
    if (!open) { F().explodeOn = false; return; }
    if (!d || !d->displayed) { ui.muted("Open an assembly."); return; }
    DocExt& x = ext(*d);
    float f = (float)(x.explode * 100);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 52 * a.scale);
    if (ImGui::SliderFloat("##ex", &f, 0, 100, "")) applyExplode(a, *d, f / 100.0);
    ImGui::SameLine();
    ImGui::Text("%.0f %%", f);
    if (ui.pillButton("Collapse", false, ImGui::GetContentRegionAvail().x)) applyExplode(a, *d, 0);
    ui.muted("Parts move apart from the assembly centre, by sub-assembly. Exports and measurements use the positions you see.");
  });
}

// ======================================================================== colours
static const ImU32 kSwatches[24] = {
  IM_COL32(255, 59, 48, 255), IM_COL32(255, 107, 26, 255), IM_COL32(255, 159, 10, 255), IM_COL32(255, 214, 10, 255),
  IM_COL32(162, 230, 53, 255), IM_COL32(48, 209, 88, 255), IM_COL32(0, 199, 190, 255), IM_COL32(100, 210, 255, 255),
  IM_COL32(10, 132, 255, 255), IM_COL32(37, 99, 235, 255), IM_COL32(94, 92, 230, 255), IM_COL32(191, 90, 242, 255),
  IM_COL32(224, 64, 251, 255), IM_COL32(255, 45, 146, 255), IM_COL32(255, 55, 95, 255), IM_COL32(162, 132, 94, 255),
  IM_COL32(255, 255, 255, 255), IM_COL32(229, 232, 236, 255), IM_COL32(201, 206, 214, 255), IM_COL32(167, 176, 188, 255),
  IM_COL32(125, 134, 147, 255), IM_COL32(86, 94, 106, 255), IM_COL32(47, 52, 60, 255), IM_COL32(17, 19, 23, 255)};
static const ImU32 kAuto[] = {
  IM_COL32(10, 132, 255, 255), IM_COL32(255, 159, 10, 255), IM_COL32(48, 209, 88, 255), IM_COL32(255, 55, 95, 255), IM_COL32(191, 90, 242, 255),
  IM_COL32(100, 210, 255, 255), IM_COL32(255, 214, 10, 255), IM_COL32(255, 69, 58, 255), IM_COL32(94, 92, 230, 255), IM_COL32(0, 199, 190, 255),
  IM_COL32(255, 107, 26, 255), IM_COL32(162, 230, 53, 255), IM_COL32(224, 64, 251, 255), IM_COL32(37, 99, 235, 255), IM_COL32(244, 63, 94, 255),
  IM_COL32(20, 184, 166, 255), IM_COL32(250, 204, 21, 255), IM_COL32(139, 92, 246, 255), IM_COL32(34, 197, 94, 255), IM_COL32(251, 113, 133, 255)};
static const int kAutoN = (int)(sizeof kAuto / sizeof kAuto[0]);
static Quantity_Color qc(ImU32 c) { return Quantity_Color(((c >> 0) & 255) / 255.0, ((c >> 8) & 255) / 255.0, ((c >> 16) & 255) / 255.0, Quantity_TOC_sRGB); }
static int colorMode = 0;       // 0 file, 1 auto, 2 uniform
static ImU32 uniformCol = IM_COL32(167, 176, 188, 255);

Quantity_Color partColor(Doc& d, int k) {
  DocExt& x = ext(d);
  if (k < (int)x.colorOverride.size() && x.colorOverride[k]) return *x.colorOverride[k];
  if (colorMode == 1) return qc(kAuto[k % kAutoN]);
  if (colorMode == 2) return qc(uniformCol);
  const Part& p = d.model->parts[k];
  return p.hasColor ? p.color : Quantity_Color(0.62, 0.68, 0.76, Quantity_TOC_sRGB);
}

/** Colour of one face for exports: its own file colour (if shown), else the part colour. */
bool faceColorOf(Doc& d, int k, const TopoDS_Face& f, Quantity_Color& c) {
  DocExt& x = ext(d);
  if (colorMode != 0 || (k < (int)x.colorOverride.size() && x.colorOverride[k])) return false;
  for (auto& sc : d.model->parts[k].subColors) {
    if (sc.first.ShapeType() == TopAbs_FACE) { if (sc.first.IsSame(f)) { c = sc.second; return true; } continue; }
    for (TopExp_Explorer e(sc.first, TopAbs_FACE); e.More(); e.Next()) if (e.Current().IsSame(f)) { c = sc.second; return true; }
  }
  return false;
}

static void applyColors(App& a, Doc& d) {
  if (!d.displayed) return;
  DocExt& x = ext(d);
  x.colorOverride.resize(d.prs.size());
  for (size_t k = 0; k < d.prs.size(); k++) {
    Handle(AIS_ColoredShape) s = Handle(AIS_ColoredShape)::DownCast(d.prs[k]);
    if (s.IsNull()) continue;
    s->ClearCustomAspects();
    s->SetColor(partColor(d, (int)k));
    if (colorMode == 0 && !x.colorOverride[k]) for (auto& sc : d.model->parts[k].subColors) s->SetCustomColor(sc.first, sc.second);
    a.styleObject(d, (int)k);
    if (a.ctx->IsDisplayed(s)) a.ctx->Redisplay(s, false);
  }
  requestFrames(a);
}

// ======================================================================== insert part
struct PendingInsert { std::shared_ptr<LoadProgress> prog; std::shared_ptr<std::shared_ptr<Model>> res; std::shared_ptr<std::mutex> mtx; std::thread th; Doc* doc; std::string name; };
static std::vector<std::unique_ptr<PendingInsert>> inserts;

static void insertFile(App& a, const std::string& path) {
  Doc* d = activeDoc(a);
  if (!d || !d->displayed) { a.open(path); return; }
  auto pi = std::make_unique<PendingInsert>();
  pi->prog = std::make_shared<LoadProgress>();
  pi->res = std::make_shared<std::shared_ptr<Model>>();
  pi->mtx = std::make_shared<std::mutex>();
  pi->doc = d;
  pi->name = fileNameOf(path);
  auto prog = pi->prog; auto res = pi->res; auto mtx = pi->mtx;
  Quality q = (Quality)a.set.quality;
  pi->th = std::thread([path, q, prog, res, mtx]() {
    auto m = loadModel(path, q, *prog);
    { std::lock_guard<std::mutex> lk(*mtx); *res = m; }
    glfwPostEmptyEvent();
  });
  a.toastMsg("Inserting " + pi->name + "…", false, 30);
  inserts.push_back(std::move(pi));
}

static void insertDialog(App& a) {
#ifdef _WIN32
  for (auto& p : openFileDialog(glfwGetWin32Window(a.win))) insertFile(a, p);
#else
  if (const char* p = getenv("GV_INSERT")) insertFile(a, p);
#endif
}

static void pollInserts(App& a) {
  for (size_t i = 0; i < inserts.size(); i++) {
    PendingInsert& pi = *inserts[i];
    std::shared_ptr<Model> m;
    { std::lock_guard<std::mutex> lk(*pi.mtx); m = *pi.res; }
    if (!m) continue;
    pi.th.join();
    bool alive = false;
    for (auto& dp : a.docs) if (dp.get() == pi.doc) alive = true;
    if (alive && m->error.empty()) {
      Doc& d = *pi.doc;
      Model& dm = *d.model;
      const size_t first = dm.parts.size();
      // place the new part beside the assembly (+X), on the same floor
      gp_Trsf shift;
      if (!dm.box.IsVoid() && !m->box.IsVoid()) {
        double a0, a1, a2, a3, a4, a5, b0, b1, b2, b3, b4, b5;
        dm.box.Get(a0, a1, a2, a3, a4, a5);
        m->box.Get(b0, b1, b2, b3, b4, b5);
        shift.SetTranslation(gp_Vec(a3 - b0 + 0.1 * (a3 - a0), (a1 + a4) / 2 - (b1 + b4) / 2, a2 - b2));
      }
      for (auto& p : m->parts) { p.loc = TopLoc_Location(shift) * p.loc; dm.parts.push_back(p); }
      auto node = std::move(m->root);
      std::function<void(gv::Node*)> renum = [&](gv::Node* n) { for (int& p : n->parts) p += (int)first; for (auto& k : n->kids) renum(k.get()); };
      renum(node.get());
      node->name = pi.name;
      node->parent = dm.root.get();
      dm.root->kids.push_back(std::move(node));
      dm.tris += m->tris;
      for (size_t k = first; k < dm.parts.size(); k++) BRepBndLib::Add(dm.parts[k].shape.Moved(dm.parts[k].loc), dm.box, true);
      a.build(d, first);
      ext(d).colorOverride.resize(d.prs.size());
      if (&d == activeDoc(a)) { a.showVisible(d); a.updateGrid(); applyColors(a, d); }
      a.toastMsg("Inserted " + pi.name, false, 2);
    } else if (!m->error.empty()) a.toastMsg(pi.name + ": " + m->error, true, 5);
    inserts.erase(inserts.begin() + i);
    i--;
  }
}

// ======================================================================== export
static bool writeStl(App& a, Doc& d, const std::vector<int>& parts, const std::string& path) {
  FILE* f = fopenUtf8(path, "wb");
  if (!f) return false;
  char head[80] = "GlassView STL export";
  fwrite(head, 1, 80, f);
  uint32_t n = 0;
  long countPos = ftell(f);
  fwrite(&n, 4, 1, f);
  for (int k : parts) {
    const gp_Trsf T = partTrsf(d, k);
    for (TopExp_Explorer ex(d.model->parts[k].shape, TopAbs_FACE); ex.More(); ex.Next()) {
      const TopoDS_Face& fc = TopoDS::Face(ex.Current());
      TopLoc_Location l;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(fc, l);
      if (t.IsNull()) continue;
      const gp_Trsf M = T * l.Transformation();
      const bool rev = fc.Orientation() == TopAbs_REVERSED;
      for (int i = 1; i <= t->NbTriangles(); i++) {
        int i1, i2, i3;
        t->Triangle(i).Get(i1, i2, i3);
        if (rev) std::swap(i2, i3);
        gp_Pnt p[3] = {t->Node(i1).Transformed(M), t->Node(i2).Transformed(M), t->Node(i3).Transformed(M)};
        gp_Vec nn = gp_Vec(p[0], p[1]).Crossed(gp_Vec(p[0], p[2]));
        if (nn.Magnitude() > 1e-300) nn.Normalize();
        float buf[12] = {(float)nn.X(), (float)nn.Y(), (float)nn.Z()};
        for (int j = 0; j < 3; j++) { buf[3 + j * 3] = (float)p[j].X(); buf[4 + j * 3] = (float)p[j].Y(); buf[5 + j * 3] = (float)p[j].Z(); }
        fwrite(buf, 4, 12, f);
        uint16_t attr = 0;
        fwrite(&attr, 2, 1, f);
        n++;
      }
    }
  }
  fseek(f, countPos, SEEK_SET);
  fwrite(&n, 4, 1, f);
  fclose(f);
  return true;
}

static bool writeStep(App& a, Doc& d, const std::vector<int>& parts, const std::string& path) {
  Handle(TDocStd_Document) doc;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
  Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
  for (int k : parts) {
    const Part& p = d.model->parts[k];
    if (p.isMesh) continue;
    TopLoc_Location L(partTrsf(d, k));
    TopoDS_Shape s = p.shape;
    if (s.ShapeType() == TopAbs_COMPOUND) { // unwrap single-solid compounds (avoids an extra "SOLID" product level)
      TopoDS_Iterator it(s);
      if (it.More()) { TopoDS_Shape c = it.Value(); it.Next(); if (!it.More()) s = c.Moved(s.Location()); }
    }
    s = s.Moved(L);
    TDF_Label lab = st->AddShape(s, false);
    TDataStd_Name::Set(lab, TCollection_ExtendedString(p.name.c_str(), true));
    ct->SetColor(lab, partColor(d, k), XCAFDoc_ColorSurf);
    DocExt& x = ext(d);
    if (colorMode == 0 && (k >= (int)x.colorOverride.size() || !x.colorOverride[k]))
      for (auto& sc : p.subColors) {
        TDF_Label sl = st->AddSubShape(lab, sc.first.Moved(L));
        if (!sl.IsNull()) ct->SetColor(sl, sc.second, XCAFDoc_ColorSurf);
      }
  }
  STEPCAFControl_Writer w;
  w.SetColorMode(true);
  w.SetNameMode(true);
  if (!w.Transfer(doc, STEPControl_AsIs)) return false;
  return w.Write(path.c_str()) == IFSelect_RetDone;
}

std::string write3mf(App& a, Doc& d, const std::vector<int>& parts, const gp_Trsf& extra, const std::string& path, bool dropToPlate);

/** fmt: 0 STEP, 1 STL, 2 3MF. Everything is written where it currently is (moves, explode). */
static void exportNode(App& a, gv::Node* n, int fmt, const gp_Trsf& extra, bool drop, const std::string& suffix) {
  Doc* d = activeDoc(a);
  if (!d || !d->displayed) return;
  std::vector<int> parts = partsUnder(a, *d, n);
  if (parts.empty()) return;
  bool anyBrep = false;
  for (int k : parts) anyBrep |= !d->model->parts[k].isMesh;
  if (fmt == 0 && !anyBrep) { a.toastMsg("STEP export needs CAD (B-rep) parts", true, 3); return; }
  std::string base = nodeTitle(*d, n);
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos && dot + 5 >= base.size()) base = base.substr(0, dot);
  for (auto& c : base) if (strchr("\\/:*?\"<>|", c)) c = '_';
  static const char* ext[] = {"step", "stl", "3mf"};
#ifdef _WIN32
  std::string path = saveFileDialog(glfwGetWin32Window(a.win), base + suffix + "." + ext[fmt], ext[fmt]);
#else
  std::string path = saveFileDialog(nullptr, base + suffix + "." + ext[fmt], ext[fmt]);
#endif
  if (path.empty()) return;
  std::string err;
  if (fmt == 0) err = writeStep(a, *d, parts, path) ? "" : "could not write";
  else if (fmt == 1) err = writeStl(a, *d, parts, path) ? "" : "could not write";
  else err = write3mf(a, *d, parts, extra, path, drop);
  a.toastMsg(err.empty() ? "Exported " + fileNameOf(path) : "Export failed: " + err, !err.empty(), 3);
}

void exportBuild3mf(App& a, const gp_Dir& build) {
  // rotate the build direction onto +Z, then sit the parts on the plate
  gp_Trsf r;
  if (!build.IsEqual(gp_Dir(0, 0, 1), 1e-9)) {
    if (build.IsOpposite(gp_Dir(0, 0, 1), 1e-9)) r.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(1, 0, 0)), M_PI);
    else r.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(gp_Vec(build).Crossed(gp_Vec(0, 0, 1)))), build.Angle(gp_Dir(0, 0, 1)));
  }
  Doc* d = activeDoc(a);
  exportNode(a, d ? d->selected : nullptr, 2, r, true, "_print");
}

// ======================================================================== mass properties
struct Material { const char* name; double rho; };
static const Material kMaterials[] = {
  {"Aluminium 6061", 2.70}, {"Aluminium 7075", 2.81}, {"AlSi10Mg (AM)", 2.67}, {"Steel", 7.85}, {"Stainless 316L", 8.00},
  {"Titanium Ti-6Al-4V", 4.43}, {"Inconel 718", 8.19}, {"Copper", 8.96}, {"CFRP (quasi-iso)", 1.55}, {"GFRP", 1.90},
  {"PA12", 1.01}, {"PA12-CF", 1.15}, {"PLA", 1.24}, {"PETG", 1.27}, {"ABS", 1.04}, {"PEEK", 1.30}, {"Custom", 0}};
static int matIdx = 0;
static double densityGcm3 = 2.70;

struct MassResult { bool ok = false, exact = true; double vol = 0, area = 0, mass = 0; gp_Pnt cog; double I[9] = {0}; double P[3] = {0}; std::string target; int solids = 0; };
static MassResult lastMass;
static std::string massKey;

static void principal(const double I[9], double P[3]) {
  math_Matrix m(1, 3, 1, 3);
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m(i + 1, j + 1) = I[i * 3 + j];
  math_Jacobi J(m);
  if (!J.IsDone()) { P[0] = I[0]; P[1] = I[4]; P[2] = I[8]; return; }
  std::vector<double> v = {J.Value(1), J.Value(2), J.Value(3)};
  std::sort(v.begin(), v.end());
  for (int i = 0; i < 3; i++) P[i] = v[i];
}

/** Mesh parts: volume / centroid / inertia from signed tetrahedra (density in g/mm³). */
static void meshMass(const TopoDS_Shape& s, const gp_Trsf& T, double rho, double& V, double& A, double m1[3], double C[9]) {
  for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
    const TopoDS_Face& fc = TopoDS::Face(ex.Current());
    TopLoc_Location l;
    Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(fc, l);
    if (t.IsNull()) continue;
    const gp_Trsf M = T * l.Transformation();
    for (int i = 1; i <= t->NbTriangles(); i++) {
      int i1, i2, i3;
      t->Triangle(i).Get(i1, i2, i3);
      gp_XYZ a = t->Node(i1).Transformed(M).XYZ(), b = t->Node(i2).Transformed(M).XYZ(), c = t->Node(i3).Transformed(M).XYZ();
      A += 0.5 * (b - a).Crossed(c - a).Modulus();
      const double det = a.Dot(b.Crossed(c));
      V += det / 6.0;
      for (int k = 0; k < 3; k++) m1[k] += det / 24.0 * (a.Coord(k + 1) + b.Coord(k + 1) + c.Coord(k + 1));
      // second moments of the tetrahedron (0,a,b,c)
      for (int r = 0; r < 3; r++)
        for (int q = 0; q < 3; q++) {
          const double ar = a.Coord(r + 1), br = b.Coord(r + 1), cr = c.Coord(r + 1), aq = a.Coord(q + 1), bq = b.Coord(q + 1), cq = c.Coord(q + 1);
          C[r * 3 + q] += det / 120.0 * (2 * (ar * aq + br * bq + cr * cq) + ar * bq + ar * cq + br * aq + br * cq + cr * aq + cr * bq);
        }
    }
  }
  (void)rho;
}

static void computeMass(App& a) {
  Doc* d = activeDoc(a);
  lastMass = MassResult();
  if (!d || !d->displayed) return;
  std::vector<int> parts = partsUnder(a, *d, d->selected);
  lastMass.target = d->selected ? d->selected->name : "Whole model · " + d->name;
  const double rho = densityGcm3 / 1000.0; // g/mm³
  GProp_GProps all;
  bool any = false;
  double mV = 0, mA = 0, m1[3] = {0, 0, 0}, C[9] = {0};
  bool anyMesh = false;
  for (int k : parts) {
    const Part& p = d->model->parts[k];
    const gp_Trsf T = partTrsf(*d, k);
    if (p.isMesh) { meshMass(p.shape, T, rho, mV, mA, m1, C); anyMesh = true; continue; }
    TopoDS_Shape s = p.shape.Moved(TopLoc_Location(T));
    GProp_GProps vp, sp;
    BRepGProp::VolumeProperties(s, vp, true);
    BRepGProp::SurfaceProperties(s, sp);
    if (vp.Mass() > 0) { all.Add(vp, rho); any = true; lastMass.solids++; }
    lastMass.area += sp.Mass();
    lastMass.vol += vp.Mass();
  }
  lastMass.exact = !anyMesh;
  double M = 0;
  gp_XYZ cog(0, 0, 0);
  double I[9] = {0};
  if (any) {
    M = all.Mass();
    cog = all.CentreOfMass().XYZ();
    gp_Mat mi = all.MatrixOfInertia();
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) I[i * 3 + j] = mi(i + 1, j + 1);
  }
  if (anyMesh && std::fabs(mV) > 1e-12) {
    // combine the mesh contribution (moments about the origin) with the exact part
    const double mm = mV * rho;
    gp_XYZ mc(m1[0] / mV, m1[1] / mV, m1[2] / mV);
    double Ic[9];
    // covariance about the mesh centroid → inertia
    double Cc[9];
    for (int r = 0; r < 3; r++) for (int q = 0; q < 3; q++) Cc[r * 3 + q] = C[r * 3 + q] - mV * mc.Coord(r + 1) * mc.Coord(q + 1);
    const double tr = Cc[0] + Cc[4] + Cc[8];
    for (int r = 0; r < 3; r++) for (int q = 0; q < 3; q++) Ic[r * 3 + q] = rho * ((r == q ? tr : 0) - Cc[r * 3 + q]);
    const double Mt = M + mm;
    gp_XYZ ct = (cog * M + mc * mm) / Mt;
    // parallel axis shift of both to the common centre
    auto shift = [&](double* Iin, double m, const gp_XYZ& c, double* out) {
      gp_XYZ dd = c - ct;
      const double d2 = dd.SquareModulus();
      for (int r = 0; r < 3; r++) for (int q = 0; q < 3; q++) out[r * 3 + q] += Iin[r * 3 + q] + m * ((r == q ? d2 : 0) - dd.Coord(r + 1) * dd.Coord(q + 1));
    };
    double It[9] = {0};
    if (M > 0) shift(I, M, cog, It);
    shift(Ic, mm, mc, It);
    for (int i = 0; i < 9; i++) I[i] = It[i];
    M = Mt;
    cog = ct;
    lastMass.vol += std::fabs(mV);
    lastMass.area += mA;
  }
  lastMass.mass = M;
  lastMass.cog = gp_Pnt(cog);
  for (int i = 0; i < 9; i++) lastMass.I[i] = I[i];
  principal(I, lastMass.P);
  lastMass.ok = true;
}

static void copyMass(App& a) {
  const MassResult& r = lastMass;
  const double kI = inertiaFactor();
  std::string t = r.target + " — " + kMaterials[matIdx].name + " (" + fDensity(densityGcm3) + ")" + (r.exact ? " — exact B-rep" : " — from mesh") + "\n";
  t += "Volume\t" + fVol(r.vol) + "\nSurface area\t" + fArea(r.area) + "\nMass\t" + fMass(r.mass) + "\n";
  t += "CoG\t" + fLen(r.cog.X(), false) + "\t" + fLen(r.cog.Y(), false) + "\t" + fLen(r.cog.Z(), false) + "\t" + lenUnit() + "\n";
  for (int i = 0; i < 3; i++) t += std::string(i == 0 ? "Ixx Ixy Ixz" : i == 1 ? "Iyx Iyy Iyz" : "Izx Izy Izz") + "\t" + fixd(r.I[i * 3] * kI, 4) + "\t" + fixd(r.I[i * 3 + 1] * kI, 4) + "\t" + fixd(r.I[i * 3 + 2] * kI, 4) + "\t" + inertiaUnit() + "\n";
  t += "Principal\t" + fixd(r.P[0] * kI, 4) + "\t" + fixd(r.P[1] * kI, 4) + "\t" + fixd(r.P[2] * kI, 4) + "\t" + inertiaUnit() + "\n";
  glfwSetClipboardString(a.win, t.c_str());
  a.toastMsg("Copied to clipboard", false, 1.5);
}

// ======================================================================== AM tool switch
static void setAMTool(App& a, bool on) {
  if (on && F().measureOn) setMeasure(a, false);
  if (on && F().moveOn) setMove(a, false);
  F().amOn = on;
  amSetActive(a, on);
  requestFrames(a);
}

// ======================================================================== context menu
static gv::Node* ctxNode = nullptr;
static bool ctxOpen = false, ctxWhole = false;
static ImVec2 ctxPos;

static void openContext(App& a, gv::Node* n, ImVec2 at) {
  ctxNode = n;
  ctxWhole = n == nullptr;
  ctxPos = at;
  ctxOpen = true;
  requestFrames(a);
}

static void isolate(App& a, Doc& d, gv::Node* n) {
  std::set<gv::Node*> keep;
  for (gv::Node* p = n; p; p = p->parent) keep.insert(p);
  std::function<void(gv::Node*, bool)> walk = [&](gv::Node* x, bool inside) {
    const bool in = inside || x == n;
    x->visible = in || keep.count(x);
    for (auto& k : x->kids) walk(k.get(), in);
  };
  walk(d.model->root.get(), false);
  a.showVisible(d);
}

static void showAll(App& a, Doc& d) {
  std::function<void(gv::Node*)> walk = [&](gv::Node* x) { x->visible = true; for (auto& k : x->kids) walk(k.get()); };
  walk(d.model->root.get());
  a.showVisible(d);
}

static void drawContext(App& a) {
  if (!ctxOpen) return;
  Doc* d = activeDoc(a);
  if (!d || !d->displayed) { ctxOpen = false; return; }
  Ui& ui = a.ui;
  const float s = a.scale;
  bool open = true;
  {
    const ImVec2 W = ImGui::GetIO().DisplaySize;
    const float mh = 420 * s, mw = 240 * s;
    ctxPos.x = std::max(8 * s, std::min(ctxPos.x, W.x - mw));
    ctxPos.y = std::max(8 * s, std::min(ctxPos.y, W.y - mh));
  }
  ui.solidNext = true;
  ui.glassWindow("##ctx", ctxPos, ImVec2(0, 0), 230 * s, [&] {
    ImGui::PushFont(ui.small());
    ImGui::TextDisabled("%s", nodeTitle(*d, ctxNode).c_str());
    ImGui::PopFont();
    auto item = [&](const char* label, const char* hint, bool enabled = true) {
      ImGui::PushID(label);
      ImVec2 p = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x, h = 28 * s;
      bool c = ImGui::InvisibleButton("##it", ImVec2(w, h)) && enabled;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      if (ImGui::IsItemHovered() && enabled) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(10, 132, 255, 255), 8 * s);
      const ImU32 tc = !enabled ? ui.fg2() : ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : ui.fg();
      dl->AddText(ImVec2(p.x + 10 * s, p.y + (h - ImGui::GetFontSize()) / 2), tc, label);
      if (hint) { ImVec2 hs = ImGui::CalcTextSize(hint); dl->AddText(ImVec2(p.x + w - hs.x - 10 * s, p.y + (h - ImGui::GetFontSize()) / 2), ImGui::IsItemHovered() && enabled ? IM_COL32(255, 255, 255, 180) : ui.fg2(), hint); }
      ImGui::PopID();
      if (c) open = false;
      return c;
    };
    if (item("Export as STEP…", nullptr)) exportNode(a, ctxNode, 0);
    if (item("Export as STL…", nullptr)) exportNode(a, ctxNode, 1);
    if (item("Export as 3MF…", nullptr)) exportNode(a, ctxNode, 2);
    if (item("Export view as PNG…", nullptr)) exportPng(a);
    ui.divider();
    if (item("Mass properties", "W")) { a.selectNode(*d, ctxNode); setMass(a, true); }
    if (item("AM check", "A")) { a.selectNode(*d, ctxNode); setAMTool(a, true); }
    if (item("Colour…", "K")) { a.selectNode(*d, ctxNode); F().colorsOn = true; }
    if (item("Move / rotate", "G")) { a.selectNode(*d, ctxNode); setMove(a, true); }
    ui.divider();
    if (item("Fit to view", nullptr)) {
      std::vector<int> ps = partsUnder(a, *d, ctxNode);
      Bnd_Box b;
      for (int k : ps) { Bnd_Box bk; d->prs[k]->BoundingBox(bk); b.Add(bk); }
      if (!b.IsVoid()) { a.view->FitAll(b, 0.1, false); a.view->ZFitAll(); }
    }
    if (ctxNode && item("Isolate", nullptr)) isolate(a, *d, ctxNode);
    if (item("Show all", nullptr)) showAll(a, *d);
  });
  if (!open || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureMouse)) ctxOpen = false;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ctxOpen = false;
}

// ======================================================================== panels
static float panelY;
static void rightPanel(App& a, const char* id, const std::function<void()>& body) {
  const float s = a.scale, m = 12 * s;
  const ImVec2 W = ImGui::GetIO().DisplaySize;
  float h = 0;
  a.ui.glassWindow(id, ImVec2(W.x - m, panelY), ImVec2(1, 0), 312 * s, [&] { body(); h = ImGui::GetWindowHeight(); });
  panelY += h + 10 * s;
}

static void kv(const char* k, const std::string& v) {
  ImGui::TextDisabled("%s", k);
  ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(v.c_str()).x);
  ImGui::TextUnformatted(v.c_str());
}

static void measurePanel(App& a) {
  Features& f = F();
  Ui& ui = a.ui;
  Doc* d = activeDoc(a);
  rightPanel(a, "##measure", [&] {
    bool open = true;
    ui.panelHeader("Measure", &open);
    if (!open) { setMeasure(a, false); return; }
    static const char* modes[] = {"Faces & axes", "Points"};
    int mm = f.mmode;
    if (ui.segmented("##mm", modes, 2, mm)) { f.mmode = mm; persist(a, "mMode", std::to_string(mm)); measureCancel(a); }
    static const char* un[] = {"mm", "inch"};
    int u = units().inch ? 1 : 0;
    if (ui.segmented("##un", un, 2, u)) { units().inch = u == 1; persistB(a, "inch", units().inch); measureRelabel(a); }
    if (ui.toggle("Snapping", f.snap)) { persistB(a, "mSnap", f.snap); f.dotsDirty = true; }
    if (ui.toggle("Show snap points", f.showDots)) { persistB(a, "mDots", f.showDots); f.dotsDirty = true; }
    if (ui.toggle("X / Y / Z components", f.xyz)) persistB(a, "mXyz", f.xyz);
    static const char* names[6] = {"Corners", "Midpoints", "Centres", "Face centres", "Axes", "Fillets"};
    static const ImU32 cols[6] = {IM_COL32(28, 31, 36, 255), IM_COL32(31, 184, 201, 255), IM_COL32(255, 159, 10, 255), IM_COL32(255, 45, 146, 255), IM_COL32(94, 92, 230, 255), IM_COL32(255, 204, 128, 255)};
    for (int i = 0; i < 6; i++) if (ui.chip(names[i], f.show[i], cols[i])) { f.show[i] = !f.show[i]; persistB(a, "mShow" + std::to_string(i), f.show[i]); f.dotsDirty = true; }
    ImGui::NewLine();
    ui.muted(f.hasPending ? ("1st: " + describeType(f.pending) + " — now pick the second. Esc cancels.").c_str()
                          : "Click two entities. Hold Shift or Alt to lock onto one face, Ctrl for a free point on the surface. Drag a result label to move it.");
    if (!d) return;
    DocExt& x = ext(*d);
    for (size_t i = 0; i < x.meas.size(); i++) {
      Measurement& m = x.meas[i];
      ImGui::PushID((int)i);
      ui.divider();
      ImGui::TextDisabled("%zu", i + 1);
      ImGui::SameLine();
      ImGui::PushFont(ui.bold());
      ImGui::TextUnformatted(m.res.main.c_str());
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::TextDisabled("%s", m.res.kind.c_str());
      ImGui::SameLine(ImGui::GetContentRegionMax().x - 18 * a.scale);
      if (ImGui::SmallButton("×")) { x.meas.erase(x.meas.begin() + i); ImGui::PopID(); break; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");
      if (m.res.hasLine && !m.res.angle) {
        ImGui::PushFont(ui.small());
        ImGui::TextColored(ImVec4(0.9f, 0.27f, 0.24f, 1), "ΔX %s", fLen(std::fabs(m.res.b.X() - m.res.a.X()), false).c_str()); ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.13f, 0.64f, 0.28f, 1), "ΔY %s", fLen(std::fabs(m.res.b.Y() - m.res.a.Y()), false).c_str()); ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.04f, 0.52f, 1.0f, 1), "ΔZ %s", fLen(std::fabs(m.res.b.Z() - m.res.a.Z()), false).c_str());
        ImGui::PopFont();
      }
      if (m.res.cond) {
        for (MEnt* e : {&m.A, &m.B}) {
          if (!e->hasCond()) continue;
          ImGui::PushID(e);
          ImGui::PushFont(ui.small());
          ImGui::TextDisabled("Ø%s", fLen(e->r * 2, false, 2).c_str());
          ImGui::SameLine();
          static const char* cn[] = {"Centre", "Near side", "Far side"};
          for (int c = 0; c < 3; c++) {
            if (c) ImGui::SameLine(0, 2);
            const bool on = e->cond == c;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.04f, 0.52f, 1, 1));
            if (on) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            if (ImGui::SmallButton(cn[c])) { e->cond = c; m.res = measureEntities(m.A, m.B); }
            if (on) ImGui::PopStyleColor(2);
          }
          ImGui::PopFont();
          ImGui::PopID();
        }
      }
      ImGui::PopID();
    }
    if (!x.meas.empty()) { ui.divider(); if (ui.pillButton("Clear all", false, ImGui::GetContentRegionAvail().x)) x.meas.clear(); }
  });
}

static void cutPanel(App& a) {
  Ui& ui = a.ui;
  rightPanel(a, "##cut", [&] {
    bool open = true;
    ui.panelHeader("Section cut", &open);
    if (!open) { setCut(a, false); return; }
    static const char* ax[] = {"X", "Y", "Z"};
    int c = cutAxis;
    if (ui.segmented("##ax", ax, 3, c)) { cutAxis = c; double lo, hi; cutBounds(a, lo, hi); cutPos = (lo + hi) / 2; applyCut(a); }
    ImGui::SameLine();
    if (ui.pillButton(cutFlip ? "Flipped" : "Flip")) { cutFlip = !cutFlip; applyCut(a); }
    double lo, hi;
    cutBounds(a, lo, hi);
    float v = (float)cutPos;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::SliderFloat("##pos", &v, (float)lo, (float)hi, "")) { cutPos = v; applyCut(a); }
    double pv = units().inch ? cutPos / 25.4 : cutPos;
    if (ui.numberField("##cp", pv, lenUnit(), 120 * a.scale)) { cutPos = std::max(lo, std::min(hi, units().inch ? pv * 25.4 : pv)); applyCut(a); }
    ui.muted("Drag the round handle in the view, or use the slider. Cut faces are capped.");
  });
}

static void movePanel(App& a) {
  Ui& ui = a.ui;
  Doc* d = activeDoc(a);
  rightPanel(a, "##move", [&] {
    bool open = true;
    ui.panelHeader("Move / rotate", &open);
    if (!open) { setMove(a, false); return; }
    if (!d || !d->selected) { ui.muted("Select a part in the view or the tree. Drag the arrows to move, the rings to rotate."); }
    else {
      ImGui::TextUnformatted(d->selected->name.c_str());
      static double t[3] = {0, 0, 0};
      static double r = 90;
      ImGui::PushFont(ui.small());
      ImGui::TextDisabled("Translate by");
      ImGui::PopFont();
      const char* xyz[3] = {"X", "Y", "Z"};
      for (int i = 0; i < 3; i++) { if (i) ImGui::SameLine(); ImGui::PushID(i); ImGui::SetNextItemWidth(54 * a.scale); ImGui::InputDouble(xyz[i], &t[i], 0, 0, "%.3g"); ImGui::PopID(); }
      ImGui::SameLine();
      if (ui.pillButton("Apply")) {
        const double k = units().inch ? 25.4 : 1;
        gp_Trsf tr;
        tr.SetTranslation(gp_Vec(t[0] * k, t[1] * k, t[2] * k));
        movePartsBy(a, tr, false);
      }
      ImGui::PushFont(ui.small());
      ImGui::TextDisabled("Rotate about part centre");
      ImGui::PopFont();
      ImGui::SetNextItemWidth(70 * a.scale);
      ImGui::InputDouble("°", &r, 0, 0, "%.4g");
      for (int i = 0; i < 3; i++) {
        ImGui::SameLine();
        std::string l = std::string(xyz[i]);
        if (ui.pillButton(l.c_str())) {
          gp_Trsf tr;
          tr.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(i == 0, i == 1, i == 2)), r * M_PI / 180.0);
          movePartsBy(a, tr, true);
        }
      }
      if (ui.pillButton("Reset part", false, ImGui::GetContentRegionAvail().x)) resetParts(a);
    }
    if (ui.pillButton("Insert part…", true, ImGui::GetContentRegionAvail().x)) insertDialog(a);
  });
}

static void massPanel(App& a) {
  Ui& ui = a.ui;
  Doc* d = activeDoc(a);
  // recompute when target / density / units change
  const std::string key = std::to_string((size_t)d) + "|" + std::to_string((size_t)(d ? d->selected : nullptr)) + "|" + std::to_string(densityGcm3);
  if (key != massKey) { massKey = key; computeMass(a); }
  rightPanel(a, "##mass", [&] {
    bool open = true;
    ui.panelHeader("Mass properties", &open);
    if (!open) { setMass(a, false); return; }
    const MassResult& r = lastMass;
    if (!r.ok) { ui.muted("Open a model."); return; }
    ImGui::TextColored(r.exact ? ImVec4(0.12f, 0.62f, 0.28f, 1) : ImVec4(0.55f, 0.56f, 0.6f, 1), r.exact ? "Exact · B-rep" : "From mesh");
    ImGui::TextUnformatted(r.target.c_str());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.58f);
    if (ImGui::BeginCombo("##mat", kMaterials[matIdx].name)) {
      for (int i = 0; i < (int)(sizeof kMaterials / sizeof kMaterials[0]); i++) {
        std::string l = kMaterials[i].rho > 0 ? std::string(kMaterials[i].name) + " · " + fDensity(kMaterials[i].rho) : kMaterials[i].name;
        if (ImGui::Selectable(l.c_str(), i == matIdx)) {
          matIdx = i;
          if (kMaterials[i].rho > 0) densityGcm3 = kMaterials[i].rho;
          persist(a, "material", std::to_string(matIdx));
          persist(a, "density", fixd(densityGcm3, 6));
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    double dv = densityGcm3 * densityFactor();
    if (ui.numberField("##rho", dv, densityLabel(), 70 * a.scale) && dv > 0) {
      densityGcm3 = dv / densityFactor();
      matIdx = (int)(sizeof kMaterials / sizeof kMaterials[0]) - 1;
      for (int i = 0; i < matIdx; i++) if (std::fabs(kMaterials[i].rho - densityGcm3) < 1e-6) matIdx = i;
      persist(a, "material", std::to_string(matIdx));
      persist(a, "density", fixd(densityGcm3, 6));
    }
    ui.divider();
    kv("Volume", fVol(r.vol));
    kv("Surface area", fArea(r.area));
    kv("Mass", fMass(r.mass));
    const int dec = units().inch ? 4 : 2;
    kv("Centre of gravity", fLen(r.cog.X(), false, dec) + ", " + fLen(r.cog.Y(), false, dec) + ", " + fLen(r.cog.Z(), false, dec) + " " + lenUnit());
    ImGui::TextDisabled("Inertia about CoG (%s)", inertiaUnit());
    const double kI = inertiaFactor();
    if (ImGui::BeginTable("##I", 3)) {
      for (int i = 0; i < 9; i++) { ImGui::TableNextColumn(); ImGui::Text("%s", fixd(r.I[i] * kI, 3).c_str()); }
      ImGui::EndTable();
    }
    kv("Principal moments", fixd(r.P[0] * kI, 2) + " · " + fixd(r.P[1] * kI, 2) + " · " + fixd(r.P[2] * kI, 2));
    if (r.exact && r.solids == 0) ImGui::TextColored(ImVec4(1, 0.62f, 0.04f, 1), "Surface model – no closed solids.");
    if (ui.pillButton("Copy values", false, ImGui::GetContentRegionAvail().x)) copyMass(a);
  });
  // CoG marker
  if (lastMass.ok && lastMass.mass > 0) {
    ImVec2 q;
    if (toScreen(a, lastMass.cog, q)) {
      const float s = a.scale;
      ImDrawList* fg = ImGui::GetBackgroundDrawList();
      fg->AddCircle(q, 9 * s, IM_COL32(255, 255, 255, 230), 32, 4 * s);
      fg->AddCircle(q, 9 * s, IM_COL32(255, 159, 10, 255), 32, 2 * s);
      fg->AddLine(ImVec2(q.x - 13 * s, q.y), ImVec2(q.x + 13 * s, q.y), IM_COL32(255, 159, 10, 255), 1.5f * s);
      fg->AddLine(ImVec2(q.x, q.y - 13 * s), ImVec2(q.x, q.y + 13 * s), IM_COL32(255, 159, 10, 255), 1.5f * s);
      fg->AddText(ImVec2(q.x + 12 * s, q.y - 22 * s), IM_COL32(255, 159, 10, 255), "CoG");
    }
  }
}

static void colorsPanel(App& a) {
  Ui& ui = a.ui;
  Doc* d = activeDoc(a);
  rightPanel(a, "##colors", [&] {
    bool open = true;
    ui.panelHeader("Colours", &open);
    if (!open) { F().colorsOn = false; return; }
    static const char* cm[] = {"From file", "Auto", "Uniform"};
    int c = colorMode;
    if (ui.segmented("##cm", cm, 3, c)) { colorMode = c; persist(a, "colorMode", std::to_string(c)); for (auto& dp : a.docs) applyColors(a, *dp); }
    auto swatches = [&](const char* id, std::function<void(ImU32)> pick, ImU32 current) {
      ImGui::PushID(id);
      const float sz = 26 * a.scale, gap = 6 * a.scale;
      for (int i = 0; i < 24; i++) {
        if (i % 8) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##s", ImVec2(sz, sz))) pick(kSwatches[i]);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(p.x + sz / 2, p.y + sz / 2), sz / 2 - 1, kSwatches[i], 24);
        dl->AddCircle(ImVec2(p.x + sz / 2, p.y + sz / 2), sz / 2 - 1, IM_COL32(0, 0, 0, 40), 24);
        if (kSwatches[i] == current || ImGui::IsItemHovered()) dl->AddCircle(ImVec2(p.x + sz / 2, p.y + sz / 2), sz / 2 + 2, IM_COL32(10, 132, 255, 255), 24, 2.f);
        ImGui::PopID();
      }
      ImGui::PopID();
    };
    if (colorMode == 2) {
      ImGui::TextDisabled("Uniform colour");
      swatches("u", [&](ImU32 col) { uniformCol = col; persist(a, "uniformCol", std::to_string(col)); for (auto& dp : a.docs) applyColors(a, *dp); }, uniformCol);
    }
    ui.divider();
    if (!d || !d->selected) { ui.muted("Select a part to give it its own colour."); return; }
    ImGui::TextUnformatted(d->selected->name.c_str());
    std::vector<int> parts = partsUnder(a, *d, d->selected);
    swatches("p", [&](ImU32 col) {
      DocExt& x = ext(*d);
      x.colorOverride.resize(d->prs.size());
      for (int k : parts) x.colorOverride[k] = qc(col);
      applyColors(a, *d);
    }, 0);
    if (ui.pillButton("Reset to file / mode colour", false, ImGui::GetContentRegionAvail().x)) {
      DocExt& x = ext(*d);
      x.colorOverride.resize(d->prs.size());
      for (int k : parts) x.colorOverride[k].reset();
      applyColors(a, *d);
    }
  });
}

// ======================================================================== key editor sheet
static void keySheet(App& a) {
  if (!keySheetOpen) return;
  Ui& ui = a.ui;
  const ImVec2 W = ImGui::GetIO().DisplaySize;
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), W, IM_COL32(0, 0, 0, 40));
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(560 * a.scale, W.y - 80 * a.scale));
  ui.solidNext = true;
  ui.glassWindow("##keys", ImVec2(W.x / 2, W.y / 2), ImVec2(0.5f, 0.5f), 520 * a.scale, [&] {
    ui.panelHeader("Keyboard shortcuts", &keySheetOpen);
    ui.muted("Click a shortcut, then press the new keys. Esc cancels, × removes. Mouse: left drag rotate · right/middle drag pan · wheel zoom · double-click sets the rotation centre · Shift/Alt in Measure locks onto a face.");
    ImGui::BeginChild("##klist", ImVec2(0, std::min(460 * a.scale, W.y - 260 * a.scale)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    std::string group;
    for (auto& x : actions()) {
      if (x.group != group) { group = x.group; ImGui::Dummy(ImVec2(0, 4 * a.scale)); ImGui::TextDisabled("%s", group.c_str()); }
      ImGui::PushID(x.id.c_str());
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(x.label.c_str());
      const std::string combo = comboFor(a, x);
      const bool custom = a.set.kv.count("key." + x.id) > 0;
      ImGui::SameLine(ImGui::GetContentRegionMax().x - 150 * a.scale);
      const bool rec = captureId == x.id;
      std::string lbl = rec ? "Press keys…" : combo.empty() ? "—" : combo;
      if (rec) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.04f, 0.52f, 1, 1));
      else if (custom) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.04f, 0.52f, 1, 0.18f));
      else ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(ui.hover()));
      ImGui::BeginDisabled(x.fixed);
      if (ImGui::Button(lbl.c_str(), ImVec2(118 * a.scale, 0))) captureId = rec ? "" : x.id;
      ImGui::EndDisabled();
      ImGui::PopStyleColor();
      if (!x.fixed) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (ImGui::SmallButton("×")) { a.set.kv["key." + x.id] = ""; a.set.save(); captureId.clear(); }
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
    if (ui.pillButton("Reset all")) {
      for (auto it = a.set.kv.begin(); it != a.set.kv.end();) it = it->first.rfind("key.", 0) == 0 ? a.set.kv.erase(it) : std::next(it);
      a.set.save();
    }
    ImGui::SameLine();
    if (ui.pillButton("Done", true)) { keySheetOpen = false; captureId.clear(); }
  });
}

// ======================================================================== hooks
static bool recentOpen = false;
static ImVec2 recentAt;
namespace feat {

void init(App& a) {
  registerActions();
  Features& f = F();
  units().inch = a.set.geti("inch", 0);
  units().density = a.set.geti("densityUnit", 0);
  f.mmode = a.set.geti("mMode", 0);
  f.snap = a.set.geti("mSnap", 1);
  f.showDots = a.set.geti("mDots", 1);
  f.xyz = a.set.geti("mXyz", 0);
  for (int i = 0; i < 6; i++) f.show[i] = a.set.geti("mShow" + std::to_string(i), i == 5 ? 0 : 1);
  colorMode = a.set.geti("colorMode", 0);
  uniformCol = (ImU32)std::stoul(a.set.get("uniformCol", std::to_string(uniformCol)));
  matIdx = a.set.geti("material", 0);
  densityGcm3 = a.set.getd("density", 2.70);
  amInit(a);
  // weekly update check (only when a release repository is configured)
  if (a.shotPath.empty() && a.set.geti("autoUpdate", 1) && !a.set.get("updateRepo", "").empty() &&
      time(nullptr) - std::atoll(a.set.get("updateLastCheck", "0").c_str()) > 7 * 24 * 3600)
    checkForUpdates(a, true);
}

bool onKey(App& a, int key, int mods) {
  const bool release = mods & 0x10000;
  mods &= 0xFFFF;
  if (key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT || key == GLFW_KEY_LEFT_ALT || key == GLFW_KEY_RIGHT_ALT) {
    auto down = [&](int k) { return k != key && glfwGetKey(a.win, k) == GLFW_PRESS; };
    const bool held = !release || down(GLFW_KEY_LEFT_SHIFT) || down(GLFW_KEY_RIGHT_SHIFT) || down(GLFW_KEY_LEFT_ALT) || down(GLFW_KEY_RIGHT_ALT);
    if (F().measureOn) measureOnModifier(a, held);
    return true;
  }
  if (release) return false;
  if (key == GLFW_KEY_LEFT_CONTROL || key == GLFW_KEY_RIGHT_CONTROL || key == GLFW_KEY_LEFT_SUPER || key == GLFW_KEY_RIGHT_SUPER) return false;
  const std::string c = comboOf(key, mods);
  if (c.empty()) return false;
  if (!captureId.empty()) {
    if (c == "Esc") { captureId.clear(); return true; }
    for (auto& x : actions()) if (x.fixed && comboFor(a, x) == c) { a.toastMsg(c + " is reserved for " + x.label, true, 2.5); captureId.clear(); return true; }
    for (auto& x : actions()) if (x.id != captureId && comboFor(a, x) == c) { a.set.kv["key." + x.id] = ""; a.toastMsg(c + " moved from “" + x.label + "”", false, 2.5); }
    for (auto& x : actions()) if (x.id == captureId) { if (c == x.def) a.set.kv.erase("key." + x.id); else a.set.kv["key." + x.id] = c; }
    a.set.save();
    captureId.clear();
    return true;
  }
  for (auto& x : actions()) {
    if (comboFor(a, x) == c) { x.run(a); a.view->Invalidate(); return true; }
  }
  return false;
}

bool onPick(App& a, const Graphic3d_Vec2i& px, int) {
  if (F().measureOn) return measurePick(a);
  if (F().amOn) return amPick(a, px);
  return false;
}

void onRightClick(App& a, const Graphic3d_Vec2i&) {
  Doc* d = activeDoc(a);
  if (!d || !d->displayed) return;
  gv::Node* n = nullptr;
  if (a.ctx->HasDetected()) {
    auto it = d->partOf.find(a.ctx->DetectedInteractive().get());
    if (it != d->partOf.end()) {
      std::function<gv::Node*(gv::Node*)> find = [&](gv::Node* x) -> gv::Node* {
        for (int p : x->parts) if (p == it->second) return x;
        for (auto& k : x->kids) if (gv::Node* r = find(k.get())) return r;
        return nullptr;
      };
      n = find(d->model->root.get());
    }
  }
  openContext(a, n, ImGui::GetIO().MousePos);
}

void afterFlush(App& a) {
  pollInserts(a);
  pollUpdates(a);
  comparePoll(a);
  measureAfterFlush(a);
  if (F().moveOn) {
    syncSoft(a);
    Doc* d = activeDoc(a);
    if (d && d->selected != moveNode) attachManip(a);
  }
}

// test automation: "m x y; c; k actionId; w frames; s (shift down); S (shift up)"
static std::deque<std::string> script;
static int scriptWait = 0;
void setScript(const std::string& sc) {
  std::string cur;
  for (char c : sc + ";") { if (c == ';') { if (!cur.empty()) script.push_back(cur); cur.clear(); } else if (!(cur.empty() && c == ' ')) cur += c; }
}
static void stepScript(App& a) {
  if (script.empty()) return;
  requestFrames(a, 4);
  if (scriptWait > 0) { scriptWait--; return; }
  std::string st = script.front();
  script.pop_front();
  scriptWait = 4;
  int x, y;
  char id[64];
  if (sscanf(st.c_str(), "m %d %d", &x, &y) == 2) {
    glfwSetCursorPos(a.win, x, y);
    ImGui::GetIO().AddMousePosEvent((float)x, (float)y);
    a.ctl->UpdateMousePosition(Graphic3d_Vec2i(x, y), Aspect_VKeyMouse_NONE, Aspect_VKeyFlags_NONE, false);
  } else if (st == "c" || st == "r") {
    double cx, cy;
    glfwGetCursorPos(a.win, &cx, &cy);
    Graphic3d_Vec2i p((int)cx, (int)cy);
    const Aspect_VKeyMouse b = st == "c" ? Aspect_VKeyMouse_LeftButton : Aspect_VKeyMouse_RightButton;
    if (st == "c") { ImGui::GetIO().AddMouseButtonEvent(0, true); ImGui::GetIO().AddMouseButtonEvent(0, false); }
    if (st == "c" && ImGui::GetIO().WantCaptureMouse) return;
    a.ctl->PressMouseButton(p, b, Aspect_VKeyFlags_NONE, false);
    a.ctl->ReleaseMouseButton(p, b, Aspect_VKeyFlags_NONE, false);
    if (st == "r") onRightClick(a, p);
  } else if (sscanf(st.c_str(), "k %63s", id) == 1) runAction(a, id);
  else if (sscanf(st.c_str(), "w %d", &x) == 1) scriptWait = x;
  else if (st == "s") measureOnModifier(a, true);
  else if (st == "S") measureOnModifier(a, false);
}

void beforeRedraw(App& a) { stepScript(a); measureBeforeRedraw(a); }

void onDocShown(App& a) {
  Doc* d = activeDoc(a);
  if (d && d->displayed && !d->ext) { ext(*d).colorOverride.resize(d->prs.size()); if (colorMode != 0) applyColors(a, *d); }
  measureDocShown(a);
  if (F().cutOn) applyCut(a);
  if (F().moveOn) attachManip(a);
  massKey.clear();
  amDocShown(a);
  pmiDocShown(a);
  compareDocShown(a);
}

void onPartsAdded(App&, Doc&, size_t) {}

bool wantsFrames(App&) { return !inserts.empty() || cutDragging || compareBusy(); }

void toolButtons(App& a) {
  Features& f = F();
  Ui& ui = a.ui;
  ui.separator();
  if (ui.iconButton(Icon::Ruler, "Measure (M)", f.measureOn)) setMeasure(a, !f.measureOn);
  ImGui::SameLine();
  if (ui.iconButton(Icon::Cut, "Section cut (C)", f.cutOn)) setCut(a, !f.cutOn);
  ImGui::SameLine();
  if (ui.iconButton(Icon::Move, "Move / rotate parts (G)", f.moveOn)) setMove(a, !f.moveOn);
  ImGui::SameLine();
  if (ui.iconButton(Icon::Mass, "Mass properties (W)", f.massOn)) setMass(a, !f.massOn);
  ImGui::SameLine();
  if (ui.iconButton(Icon::AM, "Additive manufacturing check (A)", f.amOn)) setAMTool(a, !f.amOn);
  ImGui::SameLine();
  if (ui.iconButton(Icon::Explode, "Exploded view (V)", f.explodeOn)) f.explodeOn = !f.explodeOn;
  ImGui::SameLine();
  if (ui.iconButton(Icon::Compare, "Compare with another version (Ctrl+Shift+C)", f.compareOn)) { if (f.compareOn) compareClose(a); else compareDialog(a); }
  ImGui::SameLine();
  if (ui.textButton("PMI", pmiAvailable(a) ? "Dimensions & tolerances from the STEP file (D)" : "PMI – this file has none (D)", f.pmiOn)) pmiSetActive(a, !f.pmiOn);
}

void colorButton(App& a) {
  if (a.ui.iconButton(Icon::Palette, "Part colours (K)", F().colorsOn)) F().colorsOn = !F().colorsOn;
}

void runAction(App& a, const std::string& id) {
  registerActions();
  if (id == "keys") { keySheetOpen = true; return; }
  if (id == "ctx") { Doc* d = activeDoc(a); openContext(a, d && !d->model->root->kids.empty() ? d->model->root->kids[0].get() : nullptr, ImVec2(500, 300)); return; }
  if (id == "auto") { colorMode = 1; for (auto& dp : a.docs) applyColors(a, *dp); return; }
  if (id == "ex60") { F().explodeOn = true; if (Doc* d = activeDoc(a)) applyExplode(a, *d, 0.6); return; }
  if (id == "expstep") { exportNode(a, nullptr, 0); return; }
  if (id == "expstl") { exportNode(a, nullptr, 1); return; }
  if (id == "exp3mf") { exportNode(a, nullptr, 2); return; }
  if (id == "rot") { Doc* d = activeDoc(a); if (d && d->selected) { moveParts = partsUnder(a, *d, d->selected); gp_Trsf t; t.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(0, 0, 1)), M_PI / 6); movePartsBy(a, t, true); } return; }
  if (id == "wall") { amSetMapMode(1); setAMTool(a, true); return; }
  if (id == "cutx") { setCut(a, true); cutAxis = 0; double lo, hi; cutBounds(a, lo, hi); cutPos = (lo + hi) / 2; applyCut(a); return; }
  for (auto& x : actions()) if (x.id == id) x.run(a);
}
bool uiHidden() { return hideUi; }
bool hoverSuppressed() { return F().measureOn; }

void treeRightClick(App& a, gv::Node* n) { openContext(a, n, ImGui::GetIO().MousePos); }

void settingsExtra(App& a) {
  Ui& ui = a.ui;
  ui.label("Units");
  static const char* lu[] = {"mm", "inch"};
  int l = units().inch ? 1 : 0;
  if (ui.segmented("##lu", lu, 2, l)) { units().inch = l == 1; persistB(a, "inch", units().inch); measureRelabel(a); }
  static const char* du[] = {"g/cm³", "kg/m³", "lb/in³", "lb/ft³"};
  int dd = units().density;
  if (ui.segmented("##du", du, 4, dd)) { units().density = dd; persist(a, "densityUnit", std::to_string(dd)); }
  ui.label("Show on screen");
  for (auto [key, label] : std::initializer_list<std::pair<const char*, const char*>>{{"showCube", "View cube"}, {"showInfo", "Model info bar"}, {"showHover", "Part names on hover"}, {"showTabs", "Tab bar"}, {"showMeas", "Measurement labels & lines"}}) {
    bool v = a.set.geti(key, 1);
    if (ui.toggle(label, v)) {
      persistB(a, key, v);
      if (std::string(key) == "showCube") { if (v) a.ctx->Display(a.cube, false); else a.ctx->Erase(a.cube, false); a.view->Invalidate(); }
    }
  }
  // ---- Windows integration
  ui.label("Windows");
  {
    const bool reg = fileTypesRegistered();
    if (ui.pillButton(reg ? "Remove from “Open with”" : "Add to “Open with” for STEP / IGES / STL", false, ImGui::GetContentRegionAvail().x)) {
      const bool ok = registerFileTypes(!reg);
      a.toastMsg(!ok ? "Could not change the file types" : reg ? "Removed from “Open with”" : "Added – choose GlassView in “Open with” or as default app", !ok, 4);
    }
    if (reg && ui.pillButton("Make default app… (Windows settings)", false, ImGui::GetContentRegionAvail().x)) openDefaultAppsSettings();
    {
      bool pv = previewRegistered();
      ImGui::BeginDisabled(!previewDllPresent());
      if (ui.toggle("3D preview in Explorer (preview pane)", pv)) {
        const bool ok = registerPreview(pv);
        a.toastMsg(!ok ? "Could not change the Explorer preview" : pv ? "Explorer preview on – select a STEP / STL file with the preview pane open (Alt+P)" : "Explorer preview off", !ok, 5);
      }
      ImGui::EndDisabled();
      if (!previewDllPresent()) ui.muted("GlassViewPreview.dll is missing next to the program.");
    }
    {
      static uintmax_t cached = (uintmax_t)-1;
      if (cached == (uintmax_t)-1) cached = gv::cacheSize();
      const std::string l = "Clear model cache (" + fixd(cached / 1048576.0, 0) + " MB)";
      if (ui.pillButton(l.c_str(), false, ImGui::GetContentRegionAvail().x)) { gv::cacheClear(); cached = gv::cacheSize(); a.toastMsg("Cache cleared", false, 2); }
      ui.muted("Files you opened before load from this cache in a fraction of the time.");
    }
    bool single = a.set.geti("singleInstance", 1);
    if (ui.toggle("Open files in this window", single)) persistB(a, "singleInstance", single);
    static const char* ps[] = {"1×", "2×", "4×"};
    int pi = a.set.geti("pngScale", 2) >= 4 ? 2 : a.set.geti("pngScale", 2) >= 2 ? 1 : 0;
    ImGui::TextDisabled("PNG export size");
    if (ui.segmented("##ps", ps, 3, pi)) persist(a, "pngScale", std::to_string(pi == 0 ? 1 : pi == 1 ? 2 : 4));
    if (portableMode()) ui.muted("Portable mode: settings are stored in GlassView.ini next to the program.");
    else ui.muted("Tip: put an empty GlassView.ini next to GlassViewNative.exe to run it portable (e.g. from a USB stick).");
  }
  // ---- updates
  ui.label("Updates");
  {
    static char repo[128] = "";
    static bool loaded = false;
    if (!loaded) { snprintf(repo, sizeof repo, "%s", a.set.get("updateRepo", "").c_str()); loaded = true; }
    ImGui::TextDisabled("GitHub releases (owner/repo)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputTextWithHint("##repo", "e.g. peter/glassview", repo, sizeof repo)) persist(a, "updateRepo", repo);
    bool au = a.set.geti("autoUpdate", 1);
    if (ui.toggle("Check weekly", au)) persistB(a, "autoUpdate", au);
    const std::string avail = a.set.get("updateAvailable", "");
    if (!avail.empty() && ui.pillButton(("Download " + avail + "…").c_str(), true, ImGui::GetContentRegionAvail().x)) openUrl(a.set.get("updateUrl", ""));
    ImGui::BeginDisabled(repo[0] == 0 || updateBusy());
    if (ui.pillButton(updateBusy() ? "Checking…" : "Check now", false, ImGui::GetContentRegionAvail().x)) checkForUpdates(a, false);
    ImGui::EndDisabled();
  }
}

void recentButton(App& a) {
  if (a.ui.iconButton(Icon::ChevronDown, "Recent files", recentOpen)) recentOpen = !recentOpen;
  recentAt = ImVec2(ImGui::GetItemRectMin().x - 100 * a.scale, ImGui::GetItemRectMax().y + 14 * a.scale);
}

static void drawRecent(App& a) {
  if (!recentOpen) return;
  std::vector<std::string> r = recentFiles(a);
  std::string pick;
  bool clear = false;
  a.ui.solidNext = true;
  a.ui.glassWindow("##recent", ImVec2(std::max(12 * a.scale, recentAt.x), recentAt.y), ImVec2(0, 0), 360 * a.scale, [&] {
    a.ui.label("Recent files");
    if (r.empty()) a.ui.muted("Nothing opened yet.");
    for (size_t i = 0; i < r.size(); i++) {
      ImGui::PushID((int)i);
      const size_t sl = r[i].find_last_of("\\/");
      std::string name = sl == std::string::npos ? r[i] : r[i].substr(sl + 1);
      if (ImGui::Selectable(name.c_str())) pick = r[i];
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r[i].c_str());
      ImGui::PopID();
    }
    if (!r.empty()) { a.ui.divider(); if (a.ui.pillButton("Clear list", false, ImGui::GetContentRegionAvail().x)) clear = true; }
  });
  if (!pick.empty()) { recentOpen = false; a.open(pick); }
  if (clear) { recentOpen = false; clearRecent(a); }
  if ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureMouse) || ImGui::IsKeyPressed(ImGuiKey_Escape)) recentOpen = false;
}

void shutdown(App&) { updateShutdown(); }

void settingsKeys(App& a) {
  std::vector<std::pair<std::string, std::string>> rows = {{"Left drag", "rotate"}, {"Right / middle", "pan"}, {"Double-click", "rotation centre"}, {"Right-click", "part menu"}};
  for (auto& x : actions()) { std::string c = comboFor(a, x); if (!c.empty()) rows.push_back({c, x.label}); }
  std::vector<std::pair<const char*, const char*>> r2;
  for (auto& r : rows) r2.push_back({r.first.c_str(), r.second.c_str()});
  a.ui.keyGrid(r2);
  if (a.ui.pillButton("Customise shortcuts…", false, ImGui::GetContentRegionAvail().x)) { keySheetOpen = true; a.showSettings = false; }
}

void drawUi(App& a) {
  Features& f = F();
  const float s = a.scale;
  panelY = 12 * s + a.ui.topBar * s + 150 * s;
  if (a.set.geti("showMeas", 1)) measureDrawOverlay(a);
  drawCutHandle(a);
  if (!hideUi) {
    if (f.measureOn) measurePanel(a);
    if (f.cutOn) cutPanel(a);
    if (f.moveOn) movePanel(a);
    if (f.colorsOn) colorsPanel(a);
    if (f.massOn) massPanel(a);
    if (f.amOn) amPanel(a, panelY);
    if (f.explodeOn) explodePanel(a);
    if (f.compareOn) comparePanel(a, panelY);
    if (f.pmiOn) pmiPanel(a, panelY);
  }
  amOverlay(a);
  pmiOverlay(a);
  compareOverlay(a);
  drawRecent(a);
  drawContext(a);
  keySheet(a);
}

} // namespace feat
