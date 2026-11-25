// GlassView Native – interface layout and entry point.
#include "app.hpp"
#include "units.hpp"
#include "version.hpp"
std::vector<std::string> recentFiles(App& a);
Quantity_Color partColor(Doc& d, int k);

// ======================================================================== screenshot (tests)
#include <GL/gl.h>
void App::screenshot(const std::string& path) {
  int w, h;
  glfwGetFramebufferSize(win, &w, &h);
  std::vector<unsigned char> px((size_t)w * h * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadBuffer(GL_BACK);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; y--) fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
  fclose(f);
}

// ======================================================================== interface
static std::string fmtNum(size_t n) {
  char b[32];
  if (n >= 1000000) snprintf(b, sizeof b, "%.2f M", n / 1e6);
  else if (n >= 1000) snprintf(b, sizeof b, "%.1f k", n / 1e3);
  else snprintf(b, sizeof b, "%zu", n);
  return b;
}
static std::string fmtLen(double v) {
  char b[32];
  snprintf(b, sizeof b, std::fabs(v) >= 100 ? "%.0f" : std::fabs(v) >= 10 ? "%.1f" : "%.2f", v);
  return b;
}

void App::drawUi() {
  ImGuiIO& io = ImGui::GetIO();
  const float s = scale;
  const ImVec2 W = io.DisplaySize;
  const float m = 12 * s;
  ui.beginFrame();
  Doc* d = active >= 0 ? docs[active].get() : nullptr;
  const bool hidden = feat::uiHidden();
  if (hidden) { feat::drawUi(*this); if (glfwGetTime() < toastUntil) ui.toast(W, toast.c_str(), toastErr, false); ui.endFrame(); return; }

  // ---- top-left: open / tree
  ui.capsule("##left", ImVec2(m, m), [&] {
    if (ui.iconTextButton(Icon::Open, "Open", "Open a model (Ctrl+O)")) openDialog();
    ImGui::SameLine();
    if (ui.iconButton(Icon::Tree, "Model tree (T)", set.tree)) { set.tree = !set.tree; set.save(); }
    ImGui::SameLine();
    feat::recentButton(*this);
  });

  // ---- top-centre: views
  ui.capsuleCentered("##views", m, [&] {
    if (ui.textButton("XY", "Look at the XY plane (7)")) animateTo(V3d_Zpos);
    ImGui::SameLine();
    if (ui.textButton("YZ", "Look at the YZ plane (8)")) animateTo(V3d_Xpos);
    ImGui::SameLine();
    if (ui.textButton("ZX", "Look at the ZX plane (9)")) animateTo(V3d_Yneg);
    ImGui::SameLine();
    if (ui.textButton("ISO", "Isometric (0)")) animateTo(V3d_XposYnegZpos);
    ui.separator();
    if (ui.iconButton(Icon::Fit, "Fit all (F)")) fitAll(true);
    ImGui::SameLine();
    if (ui.iconButton(set.ortho ? Icon::Ortho : Icon::Persp, set.ortho ? "Orthographic – switch to perspective (P)" : "Perspective – switch to orthographic (P)")) {
      set.ortho = !set.ortho; applyProjection(); set.save();
    }
    feat::toolButtons(*this);
  });

  // ---- top-right: display
  ui.capsuleRight("##disp", m, [&] {
    const Icon icons[4] = {Icon::Shaded, Icon::Edges, Icon::Wire, Icon::Mesh};
    const char* tips[4] = {"Shaded (1)", "Shaded with edges (2)", "Wireframe (3)", "Show tessellation (4)"};
    for (int i = 0; i < 4; i++) {
      if (i) ImGui::SameLine();
      if (ui.iconButton(icons[i], tips[i], set.displayMode == i)) setDisplayMode(i);
    }
    ImGui::SameLine();
    if (ui.iconButton(Icon::Xray, "X-ray (X)", xray)) setXray(!xray);
    ImGui::SameLine();
    feat::colorButton(*this);
    ui.separator();
    if (ui.iconButton(Icon::Theme, night ? "Dark mode – switch to light (B)" : "Light mode – switch to dark (B)")) { set.theme = night ? 0 : 1; applyTheme(); set.save(); }
    ImGui::SameLine();
    if (ui.iconButton(Icon::Settings, "Settings (,)", showSettings)) showSettings = !showSettings;
  });

  // ---- model tree
  if (set.tree && d && d->displayed) {
    ui.panel("Model", ImVec2(m, m + ui.topBar * s), ImVec2(270 * s, std::max(200.f, W.y - (m + ui.topBar * s) - (d && d->displayed && set.geti("showInfo", 1) && ui.infoH > 0 ? ui.infoH + m + 10 * s : m))), [&] {
      Node* clicked = nullptr;
      bool visChanged = false;
      ui.tree(d->model->root.get(), d->selected, clicked, visChanged, [&](const Node* n) {
        if (!n->parts.empty() && n->kids.empty()) {
          Quantity_Color c = partColor(*d, n->parts[0]);
          double r, g, b;
          c.Values(r, g, b, Quantity_TOC_sRGB);
          return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
        }
        return IM_COL32(140, 146, 156, 255);
      });
      if (visChanged) showVisible(*d);
      if (ui.rightClicked) feat::treeRightClick(*this, ui.rightClicked);
      if (clicked) selectNode(*d, clicked == d->selected ? nullptr : clicked);
    }, std::to_string(d->model->parts.size()) + (d->model->parts.size() == 1 ? " body" : " bodies"));
  }

  // ---- empty state
  if (docs.empty()) {
    std::string pick;
    if (ui.emptyState(W, recentFiles(*this), pick)) openDialog();
    if (!pick.empty()) open(pick);
  }

  // ---- loading / toast
  if (d && d->loading) {
    static const char* stages[] = {"Reading", "Tessellating", "Finding sharp and tangent edges", "Preparing view"};
    ui.toast(W, (std::string(stages[std::min(3, d->prog->stage.load())]) + " " + d->name + "…").c_str(), false, true);
  } else if (glfwGetTime() < toastUntil) {
    ui.toast(W, toast.c_str(), toastErr, false);
  }

  // ---- info chip
  if (d && d->displayed && set.geti("showInfo", 1)) {
    const Model& md = *d->model;
    Bnd_Box b = md.box;
    std::string dims = "–";
    if (!b.IsVoid()) {
      double x0, y0, z0, x1, y1, z1;
      b.Get(x0, y0, z0, x1, y1, z1);
      dims = fLen(x1 - x0, false) + " × " + fLen(y1 - y0, false) + " × " + fLen(z1 - z0, false) + " " + lenUnit();
    }
    char t[64];
    snprintf(t, sizeof t, "%.2f s", md.totalMs / 1000.0);
    std::string l2 = dims + "\n" + std::to_string(md.parts.size()) + (md.parts.size() == 1 ? " body · " : " bodies · ") + fmtNum(md.tris) + " triangles · " + t;
    std::string l3;
    if (d->selected) l3 = d->selected->name;
    ui.infoChip(ImVec2(m, W.y - m), d->name.c_str(), l2.c_str(), l3.empty() ? nullptr : l3.c_str(), 270 * s);
  }

  // ---- tabs
  if (!docs.empty() && set.geti("showTabs", 1)) {
    int closeIdx = -1, act = active;
    bool plus = false;
    std::vector<std::string> names;
    std::vector<bool> loading;
    for (auto& x : docs) { names.push_back(x->name); loading.push_back(x->loading); }
    ui.tabs(W, names, loading, act, closeIdx, plus);
    if (act != active) activate(act);
    if (closeIdx >= 0) closeDoc(closeIdx);
    if (plus) openDialog();
  }

  // ---- hover name chip
  if (d && d->displayed && !io.WantCaptureMouse && ctx->HasDetected() && set.geti("showHover", 1) && !feat::hoverSuppressed()) {
    Handle(AIS_InteractiveObject) o = ctx->DetectedInteractive();
    auto it = d->partOf.find(o.get());
    if (it != d->partOf.end()) ui.hoverTag(io.MousePos, d->model->parts[it->second].name.c_str());
  }

  // ---- rotation centre marker (after a double-click and while orbiting)
  if (ctl->hasPivot && (glfwGetTime() < pivotShownUntil || ctl->PressedMouseButtons() == Aspect_VKeyMouse_LeftButton)) {
    int xp, yp;
    view->Convert(ctl->pivot.X(), ctl->pivot.Y(), ctl->pivot.Z(), xp, yp);
    int ww, wh, fw, fh;
    glfwGetWindowSize(win, &ww, &wh);
    glfwGetFramebufferSize(win, &fw, &fh);
    ImVec2 c(xp * (float)ww / std::max(1, fw), yp * (float)wh / std::max(1, fh));
    float fade = (float)std::min(1.0, std::max(0.0, (pivotShownUntil - glfwGetTime()) / 0.4));
    if (ctl->PressedMouseButtons() == Aspect_VKeyMouse_LeftButton) fade = 1.f;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImU32 ring = IM_COL32(255, 159, 10, (int)(255 * fade)), white = IM_COL32(255, 255, 255, (int)(230 * fade));
    fg->AddCircle(c, 9 * s, white, 32, 4.f * s);
    fg->AddCircle(c, 9 * s, ring, 32, 2.f * s);
    fg->AddCircleFilled(c, 2.6f * s, ring);
    framesLeft = std::max(framesLeft, 1);
  }

  // ---- settings
  if (showSettings) {
    ui.settings(W, m, [&] {
      ui.label("STEP / IGES tessellation");
      static const char* q[] = {"Coarse", "Normal", "Fine", "Ultra"};
      int qq = set.quality;
      if (ui.segmented("##q", q, 4, qq)) { set.quality = qq; set.save(); retessellate(); }
      ui.label("Edges");
      static const char* e[] = {"Uniform", "Sharp / tangent"};
      int ee = set.catiaEdges ? 1 : 0;
      if (ui.segmented("##e", e, 2, ee)) { set.catiaEdges = ee == 1; set.save(); applyEdgeStyle(); }
      ui.label("Appearance");
      static const char* th[] = {"Light", "Dark", "Auto"};
      int tt = set.theme;
      if (ui.segmented("##t", th, 3, tt)) { set.theme = tt; set.save(); applyTheme(); }
      {
        bool b = set.geti("blur", 1);
        if (ui.toggle("Glass blur behind panels", b)) { set.kv["blur"] = b ? "1" : "0"; set.save(); framesLeft = 3; }
      }
      ui.label("Projection");
      static const char* pr[] = {"Orthographic", "Perspective"};
      int pp = set.ortho ? 0 : 1;
      if (ui.segmented("##p", pr, 2, pp)) { set.ortho = pp == 0; set.save(); applyProjection(); }
      if (ui.toggle("Ground grid", set.grid)) { set.save(); updateGrid(); }
      if (ui.toggle("Reverse scroll-wheel zoom", set.wheelReverse)) set.save();
      ui.label("Zoom speed");
      static const char* zs[] = {"Slow", "Normal", "Fast"};
      int zz = set.zoomSpeed < 75 ? 0 : set.zoomSpeed > 150 ? 2 : 1;
      if (ui.segmented("##z", zs, 3, zz)) { set.zoomSpeed = zz == 0 ? 50 : zz == 2 ? 200 : 100; set.save(); }
      feat::settingsExtra(*this);
      ui.divider();
      feat::settingsKeys(*this);
      if (false) ui.keyGrid({{"Left drag", "rotate"}, {"Right / middle", "pan"}, {"Double-click", "rotation centre"}, {"Click", "select"}, {"Wheel", "zoom to cursor"}, {"Ctrl+O", "open"},
                  {"F", "fit"}, {"0", "iso"}, {"7 / 8 / 9", "XY / YZ / ZX"}, {"P", "ortho / persp"},
                  {"1 – 4", "display"}, {"X", "x-ray"}, {"E", "edge style"}, {"T", "tree"},
                  {"B", "light / dark"}, {"Ctrl+W", "close tab"}, {"Ctrl+Tab", "next tab"}, {"Esc", "deselect"}});
      ui.divider();
      ui.muted((std::string("GlassView Native ") + GV_VERSION + " · OpenCASCADE " + std::string(OCC_VERSION_COMPLETE)).c_str());
    });
  }
  feat::drawUi(*this);
  if (ui.heightChanged) { ui.heightChanged = false; framesLeft = std::max(framesLeft, 2); }
  ui.endFrame();
}

// ======================================================================== main
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
static std::vector<std::string> wideArgs() {
  int n = 0;
  LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
  std::vector<std::string> out;
  for (int i = 0; i < n; i++) out.push_back(toUtf8(a[i]));
  LocalFree(a);
  return out;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  std::vector<std::string> args = wideArgs();
#else
  std::vector<std::string> args(argv, argv + argc);
#endif
  Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
  App app;
  app.set.load();
  int w = 1440, h = 900;
  std::vector<std::string> files;
  std::string view;
  std::vector<std::string> actionsArg;
  for (size_t i = 1; i < args.size(); i++) {
    const std::string& a = args[i];
    if (a == "--shot" && i + 1 < args.size()) app.shotPath = args[++i];
    else if (a == "--frames" && i + 1 < args.size()) app.shotFrames = atoi(args[++i].c_str());
    else if (a == "--size" && i + 1 < args.size()) sscanf(args[++i].c_str(), "%dx%d", &w, &h);
    else if (a == "--view" && i + 1 < args.size()) view = args[++i];
    else if (a == "--night") app.set.theme = 1;
    else if (a == "--day") app.set.theme = 0;
    else if (a == "--settings") app.showSettings = true;
    else if (a == "--mode" && i + 1 < args.size()) app.set.displayMode = atoi(args[++i].c_str());
    else if (a == "--noblur") app.set.kv["blur"] = "0";
    else if (a == "--uniform") app.set.catiaEdges = false;
    else if (a == "--script" && i + 1 < args.size()) feat::setScript(args[++i]);
    else if (a == "--action" && i + 1 < args.size()) actionsArg.push_back(args[++i]);
    else if (a.rfind("--", 0) != 0) files.push_back(a);
  }
  if (app.shotFrames < 0) app.shotFrames = 60;
  // a second launch ("Open with", double-click in Explorer) hands its files to the open window
  if (app.shotPath.empty() && app.set.geti("singleInstance", 1) && forwardToRunningInstance(files)) return 0;
  if (!app.init(w, h)) { fprintf(stderr, "could not create the window\n"); return 1; }
  feat::init(app);
#ifdef _WIN32
  receiveForwardedFiles(glfwGetWin32Window(app.win), [&app](const std::vector<std::string>& fs) {
    { std::lock_guard<std::mutex> lk(app.pendingMtx); for (auto& f : fs) app.pendingOpen.push_back(f); }
    app.framesLeft = 3;
    glfwPostEmptyEvent();
  });
#endif
  for (auto& f : files) app.open(f);
  if (!view.empty()) {
    // wait for loads (automation only)
    while (app.anyLoading()) { glfwWaitEventsTimeout(0.05); app.pollLoads(); }
    if (view == "select") { Doc& d = *app.docs[app.active]; if (!d.model->root->kids.empty()) app.selectNode(d, d.model->root->kids[0].get()); }
  }
  if (!actionsArg.empty()) { while (app.anyLoading()) { glfwWaitEventsTimeout(0.05); app.pollLoads(); } for (auto& x : actionsArg) feat::runAction(app, x); }
  app.run();
  app.shutdown();
  return 0;
}
