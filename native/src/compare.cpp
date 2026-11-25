// GlassView Native – compare two versions: the active model is coloured by its signed
// distance to another file (red = material added / outside, blue = removed / inside).
#include "features.hpp"
#include "am.hpp"
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

#include <BRep_Tool.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <Poly_Triangulation.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_Group.hxx>
#include <GLFW/glfw3.h>
#include <array>

// ======================================================================== meshes
struct CMesh {
  std::vector<gp_XYZ> p, n;
  std::vector<std::array<int, 3>> t;
};

static void gatherMesh(Doc* d, const Model& m, CMesh& out, bool useView) {
  out = CMesh();
  for (size_t k = 0; k < m.parts.size(); k++) {
    const Part& part = m.parts[k];
    const gp_Trsf T = useView && d ? partTrsf(*d, (int)k) : part.loc.Transformation();
    for (TopExp_Explorer ex(part.shape, TopAbs_FACE); ex.More(); ex.Next()) {
      const TopoDS_Face& fc = TopoDS::Face(ex.Current());
      TopLoc_Location l;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(fc, l);
      if (t.IsNull()) continue;
      if (!t->HasNormals()) { try { BRepLib_ToolTriangulatedShape::ComputeNormals(fc, t); } catch (...) {} }
      const gp_Trsf M = T * l.Transformation();
      const bool rev = fc.Orientation() == TopAbs_REVERSED;
      const int base = (int)out.p.size();
      for (int i = 1; i <= t->NbNodes(); i++) {
        out.p.push_back(t->Node(i).Transformed(M).XYZ());
        gp_Dir nn = t->HasNormals() ? t->Normal(i) : gp_Dir(0, 0, 1);
        if (rev) nn.Reverse();
        out.n.push_back(nn.Transformed(M).XYZ());
      }
      for (int i = 1; i <= t->NbTriangles(); i++) {
        int a, b, c;
        t->Triangle(i).Get(a, b, c);
        if (rev) std::swap(b, c);
        out.t.push_back({base + a - 1, base + b - 1, base + c - 1});
      }
    }
  }
}

// ======================================================================== closest-point BVH
static gp_XYZ closestOnTri(const gp_XYZ& p, const gp_XYZ& a, const gp_XYZ& b, const gp_XYZ& c) {
  // Ericson, Real-Time Collision Detection 5.1.5
  const gp_XYZ ab = b - a, ac = c - a, ap = p - a;
  const double d1 = ab.Dot(ap), d2 = ac.Dot(ap);
  if (d1 <= 0 && d2 <= 0) return a;
  const gp_XYZ bp = p - b;
  const double d3 = ab.Dot(bp), d4 = ac.Dot(bp);
  if (d3 >= 0 && d4 <= d3) return b;
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
  const gp_XYZ cp = p - c;
  const double d5 = ab.Dot(cp), d6 = ac.Dot(cp);
  if (d6 >= 0 && d5 <= d6) return c;
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double den = 1 / (va + vb + vc);
  return a + ab * (vb * den) + ac * (vc * den);
}

struct NearBvh {
  struct Node { double lo[3], hi[3]; int left = -1, right = -1, first = 0, count = 0; };
  std::vector<Node> nodes;
  std::vector<int> tri;
  const CMesh* m = nullptr;
  gp_XYZ ctr(int t) const { return (m->p[m->t[t][0]] + m->p[m->t[t][1]] + m->p[m->t[t][2]]) / 3.0; }
  void build(const CMesh& mm) {
    m = &mm;
    tri.resize(mm.t.size());
    for (size_t i = 0; i < tri.size(); i++) tri[i] = (int)i;
    nodes.clear();
    if (!tri.empty()) split(0, (int)tri.size());
  }
  int split(int first, int count) {
    Node n;
    for (int k = 0; k < 3; k++) { n.lo[k] = 1e300; n.hi[k] = -1e300; }
    for (int i = first; i < first + count; i++)
      for (int v = 0; v < 3; v++) {
        const gp_XYZ& p = m->p[m->t[tri[i]][v]];
        for (int k = 0; k < 3; k++) { n.lo[k] = std::min(n.lo[k], p.Coord(k + 1)); n.hi[k] = std::max(n.hi[k], p.Coord(k + 1)); }
      }
    const int id = (int)nodes.size();
    nodes.push_back(n);
    if (count <= 4) { nodes[id].first = first; nodes[id].count = count; return id; }
    int axis = 0;
    double ext = 0;
    for (int k = 0; k < 3; k++) if (n.hi[k] - n.lo[k] > ext) { ext = n.hi[k] - n.lo[k]; axis = k; }
    const int mid = first + count / 2;
    std::nth_element(tri.begin() + first, tri.begin() + mid, tri.begin() + first + count, [&](int x, int y) { return ctr(x).Coord(axis + 1) < ctr(y).Coord(axis + 1); });
    const int l = split(first, mid - first), r = split(mid, first + count - mid);
    nodes[id].left = l;
    nodes[id].right = r;
    return id;
  }
  static double boxDist2(const Node& n, const gp_XYZ& p) {
    double d = 0;
    for (int k = 0; k < 3; k++) {
      const double v = p.Coord(k + 1);
      if (v < n.lo[k]) d += (n.lo[k] - v) * (n.lo[k] - v);
      else if (v > n.hi[k]) d += (v - n.hi[k]) * (v - n.hi[k]);
    }
    return d;
  }
  /** Signed distance (positive = outside the other surface) or false if far beyond maxD. */
  bool nearest(const gp_XYZ& p, double maxD, double& signedD) const {
    if (nodes.empty()) return false;
    double best = maxD * maxD;
    int bestT = -1;
    gp_XYZ bestQ;
    int stack[128], sp = 0;
    stack[sp++] = 0;
    while (sp) {
      const Node& n = nodes[stack[--sp]];
      if (boxDist2(n, p) > best) continue;
      if (n.left < 0) {
        for (int i = n.first; i < n.first + n.count; i++) {
          const auto& t = m->t[tri[i]];
          const gp_XYZ q = closestOnTri(p, m->p[t[0]], m->p[t[1]], m->p[t[2]]);
          const double d2 = (p - q).SquareModulus();
          if (d2 < best) { best = d2; bestT = tri[i]; bestQ = q; }
        }
      } else if (sp < 126) {
        // nearer child last → popped first
        const double dl = boxDist2(nodes[n.left], p), dr = boxDist2(nodes[n.right], p);
        if (dl < dr) { stack[sp++] = n.right; stack[sp++] = n.left; }
        else { stack[sp++] = n.left; stack[sp++] = n.right; }
      }
    }
    if (bestT < 0) return false;
    const auto& t = m->t[bestT];
    gp_XYZ nn = (m->p[t[1]] - m->p[t[0]]).Crossed(m->p[t[2]] - m->p[t[0]]);
    // use the interpolated vertex normals near edges (more robust sign on curved surfaces)
    const gp_XYZ ns = m->n[t[0]] + m->n[t[1]] + m->n[t[2]];
    if (ns.Modulus() > 1e-12) nn = ns;
    const double d = std::sqrt(best);
    signedD = (p - bestQ).Dot(nn) >= 0 ? d : -d;
    return true;
  }
};

// ======================================================================== presentation
class DevPrs : public AIS_InteractiveObject {
public:
  const CMesh* m = nullptr;
  std::vector<Quantity_Color> cols;
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    if (!m || m->t.empty()) return;
    Handle(Graphic3d_ArrayOfTriangles) arr = new Graphic3d_ArrayOfTriangles((int)m->p.size(), (int)m->t.size() * 3, Graphic3d_ArrayFlags_VertexNormal | Graphic3d_ArrayFlags_VertexColor);
    for (size_t i = 0; i < m->p.size(); i++) arr->AddVertex(gp_Pnt(m->p[i]), gp_Dir(m->n[i].Modulus() > 1e-12 ? m->n[i] : gp_XYZ(0, 0, 1)), cols[i]);
    for (auto& t : m->t) arr->AddEdges(t[0] + 1, t[1] + 1, t[2] + 1);
    Handle(Graphic3d_AspectFillArea3d) asp = new Graphic3d_AspectFillArea3d();
    asp->SetInteriorStyle(Aspect_IS_SOLID);
    Graphic3d_MaterialAspect mat = cadMaterial();
    asp->SetFrontMaterial(mat);
    asp->SetBackMaterial(mat);
    asp->SetInteriorColor(Quantity_NOC_WHITE);
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(asp);
    g->AddPrimitiveArray(arr);
  }
};

// ======================================================================== state
struct CompareState {
  std::string otherPath, otherName;
  std::shared_ptr<Model> other;
  std::shared_ptr<LoadProgress> prog;
  std::shared_ptr<std::shared_ptr<Model>> res;
  std::shared_ptr<std::mutex> mtx;
  std::thread th;
  bool loading = false;
  Doc* doc = nullptr;
  CMesh a, b;
  std::vector<double> dev;          // per vertex of a (NaN = farther than the search radius)
  double tol = 0.05, range = 0;     // mm
  double maxPos = 0, maxNeg = 0, meanAbs = 0, within = 0;
  bool ghost = true;
  Handle(DevPrs) prs;
  Handle(AIS_Shape) ghostPrs;
  std::vector<int> hidden;
  float hoverDev = NAN;
  ImVec2 hoverAt;
};
static CompareState cs;

bool compareBusy() { return cs.loading; }

static Quantity_Color devColor(double d) {
  // green inside ±tol, then yellow → red (outside / added) and cyan → blue (inside / removed)
  if (std::isnan(d)) return Quantity_Color(0.55, 0.57, 0.62, Quantity_TOC_sRGB);
  const double R = std::max(cs.range, cs.tol * 1.0001);
  if (std::fabs(d) <= cs.tol) return Quantity_Color(0.24, 0.75, 0.42, Quantity_TOC_sRGB);
  const double t = std::min(1.0, (std::fabs(d) - cs.tol) / std::max(1e-12, R - cs.tol));
  auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
  if (d > 0) return Quantity_Color(lerp(1.0, 0.92, t), lerp(0.80, 0.16, t), lerp(0.20, 0.15, t), Quantity_TOC_sRGB);
  return Quantity_Color(lerp(0.30, 0.14, t), lerp(0.80, 0.32, t), lerp(0.95, 0.92, t), Quantity_TOC_sRGB);
}

static void recolour(App& a) {
  if (cs.prs.IsNull()) return;
  cs.prs->cols.resize(cs.a.p.size());
  for (size_t i = 0; i < cs.a.p.size(); i++) cs.prs->cols[i] = devColor(cs.dev[i]);
  a.ctx->Redisplay(cs.prs, false);
  requestFrames(a);
}

static void restoreDoc(App& a) {
  if (!cs.prs.IsNull()) a.ctx->Remove(cs.prs, false);
  if (!cs.ghostPrs.IsNull()) a.ctx->Remove(cs.ghostPrs, false);
  cs.prs.Nullify();
  cs.ghostPrs.Nullify();
  bool alive = false;
  for (auto& dp : a.docs) if (dp.get() == cs.doc) alive = true;
  if (alive && cs.doc->displayed) a.showVisible(*cs.doc);
  cs.hidden.clear();
}

void compareClose(App& a) {
  if (cs.th.joinable()) { if (cs.prog) cs.prog->cancel = true; cs.th.join(); }
  cs.loading = false;
  restoreDoc(a);
  cs.other.reset();
  cs.a = CMesh();
  cs.b = CMesh();
  cs.dev.clear();
  cs.doc = nullptr;
  F().compareOn = false;
  requestFrames(a);
}

void compareOpen(App& a, const std::string& path) {
  Doc* d = activeDoc(a);
  if (!d || !d->displayed) { a.toastMsg("Open the model to compare first", true, 3); return; }
  if (!isSupported(path)) { a.toastMsg("Unsupported file: " + fileNameOf(path), true, 3); return; }
  compareClose(a);
  if (F().amOn) { F().amOn = false; amSetActive(a, false); }
  F().compareOn = true;
  cs.doc = d;
  cs.otherPath = path;
  cs.otherName = fileNameOf(path);
  cs.prog = std::make_shared<LoadProgress>();
  cs.res = std::make_shared<std::shared_ptr<Model>>();
  cs.mtx = std::make_shared<std::mutex>();
  cs.loading = true;
  auto prog = cs.prog; auto res = cs.res; auto mtx = cs.mtx;
  Quality q = (Quality)std::max(a.set.quality, 1); // at least "Normal" – deviations need a decent mesh
  cs.th = std::thread([path, q, prog, res, mtx]() {
    auto m = loadModel(path, q, *prog);
    { std::lock_guard<std::mutex> lk(*mtx); *res = m; }
    glfwPostEmptyEvent();
  });
  a.toastMsg("Loading " + cs.otherName + " for comparison…", false, 2);
}

void compareDialog(App& a) {
#ifdef _WIN32
  auto files = openFileDialog(glfwGetWin32Window(a.win));
  if (!files.empty()) compareOpen(a, files[0]);
#else
  if (const char* p = getenv("GV_COMPARE")) compareOpen(a, p);
#endif
}

/** Splits long triangles so the colour map can show detail inside large flat faces. */
static void refine(CMesh& m, double maxEdge) {
  CMesh out;
  out.p = m.p;
  out.n = m.n;
  for (const auto& t : m.t) {
    const gp_XYZ &A = m.p[t[0]], &B = m.p[t[1]], &C = m.p[t[2]];
    const double L = std::max({(B - A).Modulus(), (C - B).Modulus(), (A - C).Modulus()});
    const int n = std::min(24, std::max(1, (int)std::ceil(L / maxEdge)));
    if (n == 1) { out.t.push_back(t); continue; }
    // barycentric grid with n segments per edge
    std::vector<int> idx((n + 1) * (n + 2) / 2);
    auto id = [&](int i, int j) -> int& { return idx[i * (2 * n + 3 - i) / 2 + j]; };
    for (int i = 0; i <= n; i++)
      for (int j = 0; j <= n - i; j++) {
        const int k = n - i - j;
        if (i == n) { id(i, j) = t[1]; continue; }
        if (j == n) { id(i, j) = t[2]; continue; }
        if (k == n) { id(i, j) = t[0]; continue; }
        const double u = (double)i / n, v = (double)j / n, w = 1 - u - v;
        out.p.push_back(A * w + B * u + C * v);
        out.n.push_back(m.n[t[0]] * w + m.n[t[1]] * u + m.n[t[2]] * v);
        id(i, j) = (int)out.p.size() - 1;
      }
    for (int i = 0; i < n; i++)
      for (int j = 0; j < n - i; j++) {
        out.t.push_back({id(i, j), id(i + 1, j), id(i, j + 1)});
        if (j < n - i - 1) out.t.push_back({id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
      }
  }
  m = std::move(out);
}

static void compute(App& a) {
  Doc& d = *cs.doc;
  gatherMesh(&d, *d.model, cs.a, true);
  {
    Bnd_Box bb = d.model->box;
    if (!bb.IsVoid()) {
      double x0, y0, z0, x1, y1, z1;
      bb.Get(x0, y0, z0, x1, y1, z1);
      const double diag = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
      if (cs.a.p.size() < 400000) refine(cs.a, diag / 120);
    }
  }
  gatherMesh(nullptr, *cs.other, cs.b, false);
  NearBvh bvh;
  bvh.build(cs.b);
  Bnd_Box bb = d.model->box;
  double diag = 100;
  if (!bb.IsVoid()) { double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1); diag = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0)); }
  const double maxD = diag * 0.1;
  cs.dev.assign(cs.a.p.size(), NAN);
  double sum = 0;
  size_t n = 0, in = 0;
  cs.maxPos = cs.maxNeg = 0;
  for (size_t i = 0; i < cs.a.p.size(); i++) {
    double sd;
    if (!bvh.nearest(cs.a.p[i], maxD, sd)) continue;
    cs.dev[i] = sd;
    sum += std::fabs(sd);
    n++;
    if (std::fabs(sd) <= cs.tol) in++;
    cs.maxPos = std::max(cs.maxPos, sd);
    cs.maxNeg = std::min(cs.maxNeg, sd);
  }
  cs.meanAbs = n ? sum / n : 0;
  cs.within = cs.a.p.empty() ? 0 : (double)in / cs.a.p.size();
  if (cs.range <= 0) cs.range = std::max({cs.tol * 4, std::fabs(cs.maxPos), std::fabs(cs.maxNeg)});
  // show: hide the real parts, display the coloured mesh (+ ghost of the other version)
  for (size_t k = 0; k < d.prs.size(); k++) {
    if (a.ctx->IsDisplayed(d.prs[k])) { a.ctx->Erase(d.prs[k], false); cs.hidden.push_back((int)k); }
    if (!d.soft[k].IsNull()) a.ctx->Erase(d.soft[k], false);
  }
  cs.prs = new DevPrs();
  cs.prs->m = &cs.a;
  cs.prs->cols.resize(cs.a.p.size());
  for (size_t i = 0; i < cs.a.p.size(); i++) cs.prs->cols[i] = devColor(cs.dev[i]);
  a.ctx->Display(cs.prs, 0, -1, false);
  TopoDS_Compound c;
  BRep_Builder bld;
  bld.MakeCompound(c);
  for (auto& p : cs.other->parts) bld.Add(c, p.shape.Moved(p.loc));
  cs.ghostPrs = new AIS_Shape(c);
  cs.ghostPrs->SetColor(Quantity_Color(0.35, 0.38, 0.45, Quantity_TOC_sRGB));
  cs.ghostPrs->SetWidth(1.0);
  if (cs.ghost) a.ctx->Display(cs.ghostPrs, AIS_WireFrame, -1, false);
  requestFrames(a);
}

void comparePoll(App& a) {
  if (!cs.loading) return;
  std::shared_ptr<Model> m;
  { std::lock_guard<std::mutex> lk(*cs.mtx); m = *cs.res; }
  if (!m) return;
  cs.th.join();
  cs.loading = false;
  bool alive = false;
  for (auto& dp : a.docs) if (dp.get() == cs.doc) alive = true;
  if (!alive) { compareClose(a); return; }
  if (!m->error.empty()) { a.toastMsg(cs.otherName + ": " + m->error, true, 4); compareClose(a); return; }
  cs.other = m;
  cs.range = 0;
  compute(a);
}

void compareDocShown(App& a) {
  if (!F().compareOn || cs.loading || !cs.doc) return;
  if (activeDoc(a) != cs.doc) { compareClose(a); return; }
  // showVisible() re-displayed the parts: hide them again
  for (int k : cs.hidden) { a.ctx->Erase(cs.doc->prs[k], false); if (!cs.doc->soft[k].IsNull()) a.ctx->Erase(cs.doc->soft[k], false); }
}

/** Deviation value under the cursor (ray against the coloured mesh). */
void compareOverlay(App& a) {
  cs.hoverDev = NAN;
  if (!F().compareOn || cs.prs.IsNull() || ImGui::GetIO().WantCaptureMouse) return;
  const ImVec2 mp = ImGui::GetIO().MousePos;
  const float fx = ImGui::GetIO().DisplayFramebufferScale.x, fy = ImGui::GetIO().DisplayFramebufferScale.y;
  double X, Y, Z, Vx, Vy, Vz;
  a.view->ConvertWithProj((int)(mp.x * fx), (int)(mp.y * fy), X, Y, Z, Vx, Vy, Vz);
  const gp_XYZ o(X, Y, Z), dir = gp_XYZ(Vx, Vy, Vz).Normalized();
  const gp_XYZ camDir = a.view->Camera()->Direction().XYZ(), eye = a.view->Camera()->Eye().XYZ();
  double best = 1e300;
  float val = NAN;
  for (const auto& t : cs.a.t) {
    const gp_XYZ &p0 = cs.a.p[t[0]], &p1 = cs.a.p[t[1]], &p2 = cs.a.p[t[2]];
    gp_XYZ e1 = p1 - p0, e2 = p2 - p0, pv = dir.Crossed(e2);
    const double det = e1.Dot(pv);
    if (std::fabs(det) < 1e-18) continue;
    const double inv = 1 / det;
    gp_XYZ tv = o - p0;
    const double u = tv.Dot(pv) * inv;
    if (u < 0 || u > 1) continue;
    gp_XYZ qv = tv.Crossed(e1);
    const double v = dir.Dot(qv) * inv;
    if (v < 0 || u + v > 1) continue;
    const gp_XYZ hp = o + dir * (e2.Dot(qv) * inv);
    const double depth = (hp - eye).Dot(camDir);
    if (depth >= best) continue;
    best = depth;
    const double d0 = cs.dev[t[0]], d1 = cs.dev[t[1]], d2 = cs.dev[t[2]];
    val = (std::isnan(d0) || std::isnan(d1) || std::isnan(d2)) ? NAN : (float)((1 - u - v) * d0 + u * d1 + v * d2);
  }
  if (best < 1e299) {
    cs.hoverDev = val;
    const std::string t = std::isnan(val) ? "no counterpart" : (val >= 0 ? "+" : "−") + fLen(std::fabs(val), true, units().inch ? 4 : 3);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float s = a.scale;
    ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    ImVec2 p0(mp.x + 16 * s, mp.y - ts.y - 14 * s), p1(p0.x + ts.x + 16 * s, p0.y + ts.y + 8 * s);
    double r, g, b;
    devColor(val).Values(r, g, b, Quantity_TOC_sRGB);
    dl->AddRectFilled(p0, p1, a.night ? IM_COL32(40, 44, 52, 240) : IM_COL32(255, 255, 255, 245), 8 * s);
    dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + 4 * s, p1.y), IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255), 2 * s);
    dl->AddText(ImVec2(p0.x + 9 * s, p0.y + 4 * s), a.ui.fg(), t.c_str());
  }
}

void comparePanel(App& a, float& y) {
  Ui& ui = a.ui;
  const float s = a.scale, m = 12 * s;
  const ImVec2 W = ImGui::GetIO().DisplaySize;
  float h = 0;
  ui.glassWindow("##compare", ImVec2(W.x - m, y), ImVec2(1, 0), 312 * s, [&] {
    bool open = true;
    ui.panelHeader("Compare versions", &open);
    if (!open) { compareClose(a); return; }
    if (cs.loading) { ui.muted(("Loading " + cs.otherName + "…").c_str()); h = ImGui::GetWindowHeight(); return; }
    if (!cs.other) { ui.muted("Pick the other version of this model to see where they differ."); if (ui.pillButton("Choose file…", true, ImGui::GetContentRegionAvail().x)) compareDialog(a); h = ImGui::GetWindowHeight(); return; }
    ImGui::TextDisabled("This model vs.");
    ImGui::TextUnformatted(cs.otherName.c_str());
    ui.divider();
    double tv = units().inch ? cs.tol / 25.4 : cs.tol;
    ImGui::TextDisabled("Tolerance ±");
    ImGui::SameLine();
    if (ui.numberField("##tol", tv, lenUnit(), 90 * s) && tv > 0) { cs.tol = units().inch ? tv * 25.4 : tv; size_t in = 0; for (double d : cs.dev) if (!std::isnan(d) && std::fabs(d) <= cs.tol) in++; cs.within = cs.dev.empty() ? 0 : (double)in / cs.dev.size(); recolour(a); }
    double rv = units().inch ? cs.range / 25.4 : cs.range;
    ImGui::TextDisabled("Colour range ±");
    ImGui::SameLine();
    if (ui.numberField("##rng", rv, lenUnit(), 90 * s) && rv > 0) { cs.range = units().inch ? rv * 25.4 : rv; recolour(a); }
    // legend bar
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x, bh = 10 * s;
    const int N = 48;
    for (int i = 0; i < N; i++) {
      const double d = -cs.range + 2 * cs.range * (i + 0.5) / N;
      double r, g, b;
      devColor(d).Values(r, g, b, Quantity_TOC_sRGB);
      dl->AddRectFilled(ImVec2(p.x + bw * i / N, p.y), ImVec2(p.x + bw * (i + 1) / N + 0.5f, p.y + bh), IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255));
    }
    ImGui::Dummy(ImVec2(bw, bh + 2 * s));
    ImGui::PushFont(ui.small());
    const std::string lo = "−" + fLen(cs.range, true, 2), hi = "+" + fLen(cs.range, true, 2);
    ImGui::TextDisabled("%s  removed", lo.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize((std::string("added  ") + hi).c_str()).x);
    ImGui::TextDisabled("added  %s", hi.c_str());
    ImGui::PopFont();
    ui.divider();
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(v.c_str()).x); ImGui::TextUnformatted(v.c_str()); };
    kv("Largest outward", "+" + fLen(cs.maxPos, true, 3));
    kv("Largest inward", "−" + fLen(std::fabs(cs.maxNeg), true, 3));
    kv("Mean |deviation|", fLen(cs.meanAbs, true, 3));
    kv("Within tolerance", fixd(cs.within * 100, 1) + " %");
    if (ui.toggle("Show other version as wireframe", cs.ghost)) {
      if (cs.ghost) a.ctx->Display(cs.ghostPrs, AIS_WireFrame, -1, false); else a.ctx->Erase(cs.ghostPrs, false);
      requestFrames(a);
    }
    ui.muted("Hover the model to read the deviation. Both files are compared in their own coordinates – move parts first if they are placed differently.");
    if (ui.pillButton("Compare with another file…", false, ImGui::GetContentRegionAvail().x)) compareDialog(a);
    h = ImGui::GetWindowHeight();
  });
  y += h + 10 * s;
}
