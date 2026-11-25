// GlassView Native – STEP AP242 PMI: dimensions, geometric tolerances and datums.
// Reading (semantic + graphical presentation) and the PMI tool (overlay + list).
#include "features.hpp"

#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_GeomTolerance.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <XCAFDimTolObjects_GeomToleranceObject.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <TDF_LabelSequence.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <BRepBndLib.hxx>
#include <Prs3d_LineAspect.hxx>
#include <GLFW/glfw3.h>

// ======================================================================== tool
static std::vector<Handle(AIS_Shape)> presObjs;   // annotation lines of the active doc
static std::vector<Handle(AIS_Shape)> hiObjs;     // highlighted referenced faces
static Doc* shownFor = nullptr;
static int hoverItem = -1, pinnedItem = -1, viewHover = -1;
static bool filterKind[3] = {true, true, true};

static Quantity_Color pmiColor(int kind) {
  return kind == 0 ? Quantity_Color(0.04, 0.38, 0.85, Quantity_TOC_sRGB) : kind == 1 ? Quantity_Color(0.80, 0.30, 0.05, Quantity_TOC_sRGB) : Quantity_Color(0.10, 0.55, 0.30, Quantity_TOC_sRGB);
}
static ImU32 pmiCol32(int kind) { return kind == 0 ? IM_COL32(10, 132, 255, 255) : kind == 1 ? IM_COL32(230, 110, 20, 255) : IM_COL32(40, 160, 90, 255); }

static void clearPres(App& a) {
  for (auto& o : presObjs) a.ctx->Remove(o, false);
  presObjs.clear();
  for (auto& o : hiObjs) a.ctx->Remove(o, false);
  hiObjs.clear();
  shownFor = nullptr;
}

static void showPres(App& a, Doc& d) {
  clearPres(a);
  shownFor = &d;
  for (const Pmi& p : d.model->pmi) {
    if (p.pres.IsNull() || !filterKind[p.kind]) { presObjs.push_back(Handle(AIS_Shape)()); continue; }
    Handle(AIS_Shape) s = new AIS_Shape(p.pres);
    s->SetColor(pmiColor(p.kind));
    s->SetWidth(1.6);
    s->Attributes()->SetFaceBoundaryDraw(false);
    s->SetZLayer(Graphic3d_ZLayerId_Topmost);
    a.ctx->Display(s, AIS_WireFrame, -1, false);
    presObjs.push_back(s);
  }
}

/** Finds the part containing a referenced sub-shape → world-placed copy. */
static bool placeRef(Doc& d, const TopoDS_Shape& ref, TopoDS_Shape& out) {
  for (size_t k = 0; k < d.model->parts.size(); k++) {
    const Part& p = d.model->parts[k];
    if (p.isMesh) continue;
    TopTools_IndexedMapOfShape mp;
    TopExp::MapShapes(p.shape, ref.ShapeType(), mp);
    for (int i = 1; i <= mp.Extent(); i++)
      if (mp(i).IsSame(ref) || mp(i).TShape() == ref.TShape()) { out = mp(i).Moved(TopLoc_Location(partTrsf(d, (int)k))); return true; }
  }
  return false;
}

static void highlightRefs(App& a, Doc& d, int item) {
  for (auto& o : hiObjs) a.ctx->Remove(o, false);
  hiObjs.clear();
  if (item < 0 || item >= (int)d.model->pmi.size()) return;
  const Pmi& p = d.model->pmi[item];
  for (const TopoDS_Shape& r : p.refs) {
    TopoDS_Shape w;
    if (!placeRef(d, r, w)) continue;
    Handle(AIS_Shape) s = new AIS_Shape(w);
    s->SetColor(pmiColor(p.kind));
    s->SetTransparency(0.35);
    s->SetWidth(3.0);
    s->Attributes()->SetFaceBoundaryDraw(true);
    s->Attributes()->FaceBoundaryAspect()->SetColor(pmiColor(p.kind));
    s->Attributes()->FaceBoundaryAspect()->SetWidth(2.5);
    s->SetPolygonOffsets(Aspect_POM_Fill, -2.0f, -2.0f);
    a.ctx->Display(s, AIS_Shaded, -1, false);
    hiObjs.push_back(s);
  }
}

/** Anchor for an item's label: text point, else centre of its annotation, else of its faces. */
static bool anchorOf(Doc& d, const Pmi& p, gp_Pnt& at) {
  if (p.hasPt) { at = p.pt; return true; }
  Bnd_Box b;
  if (!p.pres.IsNull()) BRepBndLib::Add(p.pres, b);
  if (b.IsVoid())
    for (const TopoDS_Shape& r : p.refs) { TopoDS_Shape w; if (placeRef(d, r, w)) BRepBndLib::Add(w, b); }
  if (b.IsVoid()) return false;
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  at = gp_Pnt((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
  return true;
}

bool pmiAvailable(App& a) { Doc* d = activeDoc(a); return d && d->displayed && d->model && !d->model->pmi.empty(); }

void pmiSetActive(App& a, bool on) {
  F().pmiOn = on;
  hoverItem = pinnedItem = -1;
  if (!on) { clearPres(a); requestFrames(a); return; }
  Doc* d = activeDoc(a);
  if (d && d->displayed) showPres(a, *d);
  if (d && d->displayed && d->model->pmi.empty()) a.toastMsg("This file has no PMI (needs STEP AP242 with dimensions / tolerances)", false, 3.5);
  requestFrames(a);
}

void pmiDocShown(App& a) {
  if (!F().pmiOn) return;
  Doc* d = activeDoc(a);
  if (d != shownFor) { hoverItem = pinnedItem = -1; if (d && d->displayed) showPres(a, *d); else clearPres(a); }
}

void pmiOverlay(App& a) {
  if (!F().pmiOn) return;
  Doc* d = activeDoc(a);
  if (!d || !d->displayed || d != shownFor) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const float s = a.scale;
  ImFont* f = a.ui.bold();
  viewHover = -1;
  for (size_t i = 0; i < d->model->pmi.size(); i++) {
    const Pmi& p = d->model->pmi[i];
    if (!filterKind[p.kind]) continue;
    gp_Pnt at;
    ImVec2 q;
    if (!anchorOf(*d, p, at) || !toScreen(a, at, q)) continue;
    const bool hot = (int)i == hoverItem || (int)i == pinnedItem;
    const std::string t = p.kind == 2 ? p.title : p.title;
    ImVec2 ts = f->CalcTextSizeA(f->FontSize * 0.92f, 1e9f, 0, t.c_str());
    const float px = 7 * s, py = 3 * s;
    ImVec2 p0(q.x - ts.x / 2 - px, q.y - ts.y / 2 - py), p1(q.x + ts.x / 2 + px, q.y + ts.y / 2 + py);
    const ImU32 c = pmiCol32(p.kind);
    const float r = p.kind == 2 ? 2 * s : p.kind == 1 ? 3 * s : (p1.y - p0.y) / 2;
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 2 * s), ImVec2(p1.x, p1.y + 2 * s), IM_COL32(0, 0, 0, 28), r);
    dl->AddRectFilled(p0, p1, hot ? c : (a.night ? IM_COL32(40, 44, 52, 235) : IM_COL32(255, 255, 255, 238)), r);
    dl->AddRect(p0, p1, c, r, 0, (hot ? 2.0f : 1.3f) * s);
    dl->AddText(f, f->FontSize * 0.92f, ImVec2(q.x - ts.x / 2, q.y - ts.y / 2), hot ? IM_COL32(255, 255, 255, 255) : c, t.c_str());
    // hover the label in the view too
    const ImVec2 mp = ImGui::GetIO().MousePos;
    if (!ImGui::GetIO().WantCaptureMouse && mp.x >= p0.x && mp.x <= p1.x && mp.y >= p0.y && mp.y <= p1.y) {
      viewHover = (int)i;
      ImGui::SetTooltip("%s\n%s", p.title.c_str(), p.detail.c_str());
    }
  }
}

void pmiPanel(App& a, float& y) {
  Doc* d = activeDoc(a);
  Ui& ui = a.ui;
  const float s = a.scale, m = 12 * s;
  const ImVec2 W = ImGui::GetIO().DisplaySize;
  float h = 0;
  int hov = -1;
  ImGui::SetNextWindowSizeConstraints(ImVec2(312 * s, 0), ImVec2(312 * s, std::max(200.f, W.y - y - 80 * s)));
  ui.glassWindow("##pmi", ImVec2(W.x - m, y), ImVec2(1, 0), 312 * s, [&] {
    bool open = true;
    ui.panelHeader("PMI · dimensions & tolerances", &open);
    if (!open) { pmiSetActive(a, false); return; }
    if (!d || !d->displayed) { ui.muted("Open a model."); return; }
    const auto& L = d->model->pmi;
    if (L.empty()) { ui.muted("No PMI in this file. GlassView shows dimensions, GD&T and datums from STEP AP242 files (CATIA, NX, Creo … export with PMI / 3D annotations)."); return; }
    int cnt[3] = {0, 0, 0};
    for (auto& p : L) cnt[p.kind]++;
    static const char* names[3] = {"Dimensions", "Tolerances", "Datums"};
    static const ImU32 cols[3] = {IM_COL32(10, 132, 255, 255), IM_COL32(230, 110, 20, 255), IM_COL32(40, 160, 90, 255)};
    for (int k = 0; k < 3; k++) {
      if (!cnt[k]) continue;
      std::string l = std::string(names[k]) + " " + std::to_string(cnt[k]);
      if (ui.chip(l.c_str(), filterKind[k], cols[k])) { filterKind[k] = !filterKind[k]; showPres(a, *d); }
    }
    ImGui::NewLine();
    ui.muted("Hover an entry to highlight its faces, click to keep it highlighted.");
    for (int k = 0; k < 3; k++) {
      if (!cnt[k] || !filterKind[k]) continue;
      ui.divider();
      ImGui::TextDisabled("%s", names[k]);
      for (size_t i = 0; i < L.size(); i++) {
        if (L[i].kind != k) continue;
        ImGui::PushID((int)i);
        const bool sel = pinnedItem == (int)i;
        if (ImGui::Selectable(("##it" + std::to_string(i)).c_str(), sel, 0, ImVec2(0, 34 * s))) {
          pinnedItem = sel ? -1 : (int)i;
          highlightRefs(a, *d, pinnedItem);
        }
        if (ImGui::IsItemHovered()) hov = (int)i;
        ImVec2 r0 = ImGui::GetItemRectMin();
        ImDrawList* wl = ImGui::GetWindowDrawList();
        wl->AddText(ui.bold(), ui.bold()->FontSize, ImVec2(r0.x + 4 * s, r0.y + 2 * s), cols[k], L[i].title.c_str());
        wl->AddText(ui.small(), ui.small()->FontSize, ImVec2(r0.x + 4 * s, r0.y + 4 * s + ui.bold()->FontSize), ui.fg2(),
                    (L[i].detail + (L[i].refs.empty() ? "" : " · " + std::to_string(L[i].refs.size()) + (L[i].refs.size() == 1 ? " face" : " faces"))).c_str());
        ImGui::PopID();
      }
    }
    h = ImGui::GetWindowHeight();
  });
  // hover from the list wins over hover in the view; otherwise fall back to the pinned item
  if (d && d->displayed) {
    static int lastShown = -2;
    const int want = hov >= 0 ? hov : viewHover >= 0 ? viewHover : pinnedItem;
    hoverItem = hov >= 0 ? hov : viewHover;
    if (want != lastShown) { highlightRefs(a, *d, want); lastShown = want; requestFrames(a); }
  }
  y += h + 10 * s;
}
