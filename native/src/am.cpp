#include "am.hpp"

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
#include <map>

// ======================================================================== state
struct AmMesh {
  std::vector<gp_XYZ> p, n;      // per vertex (world)
  std::vector<std::array<int, 3>> t;
  std::vector<int> part;         // per triangle
};
struct AmStats { double height = 0, overhang = 0, support = 0, plate = 0, minWall = -1, thinShare = 0; bool thickness = false; };
struct Orient { gp_Dir d; double support, height, score; };

static gp_Dir buildDir(0, 0, 1);
static int mapMode = 0;           // 0 overhang, 1 wall thickness
static int limitDeg = 45;
static double minWall = 1.0;      // mm
static bool clickPlace = true;    // click a face → it goes onto the plate
static AmMesh mesh;
static AmStats stats;
static std::vector<Orient> orients;
static std::string amKey;
static std::vector<int> amParts;
static Handle(AIS_InteractiveObject) amObj, plateObj;

// ======================================================================== presentation objects
class AmPrs : public AIS_InteractiveObject {
public:
  const AmMesh* m = nullptr;
  std::vector<Quantity_Color> cols;
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    if (!m || m->t.empty()) return;
    Handle(Graphic3d_ArrayOfTriangles) arr = new Graphic3d_ArrayOfTriangles((int)m->p.size(), (int)m->t.size() * 3, Graphic3d_ArrayFlags_VertexNormal | Graphic3d_ArrayFlags_VertexColor);
    for (size_t i = 0; i < m->p.size(); i++) arr->AddVertex(gp_Pnt(m->p[i]), gp_Dir(m->n[i]), cols[i]);
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

class PlatePrs : public AIS_InteractiveObject {
public:
  gp_Pnt c[4];
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    Handle(Graphic3d_ArrayOfTriangles) arr = new Graphic3d_ArrayOfTriangles(4, 6);
    for (auto& p : c) arr->AddVertex(p);
    arr->AddEdges(1, 2, 3);
    arr->AddEdges(1, 3, 4);
    Handle(Graphic3d_AspectFillArea3d) asp = new Graphic3d_AspectFillArea3d();
    asp->SetInteriorStyle(Aspect_IS_SOLID);
    asp->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
    asp->SetFaceCulling(Graphic3d_TypeOfBackfacingModel_DoubleSided);
    const Quantity_Color col(0.25, 0.54, 0.95, Quantity_TOC_sRGB);
    Graphic3d_MaterialAspect mat(Graphic3d_NameOfMaterial_UserDefined);
    mat.SetColor(col);
    mat.SetTransparency(0.82f);
    asp->SetFrontMaterial(mat);
    asp->SetBackMaterial(mat);
    asp->SetInteriorColor(Quantity_ColorRGBA(col, 0.18f));
    asp->SetAlphaMode(Graphic3d_AlphaMode_Blend);
    asp->SetPolygonOffsets(Aspect_POM_Fill, 1.0f, 1.0f);
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(asp);
    g->AddPrimitiveArray(arr);
  }
};

// ======================================================================== mesh gathering
static void gather(App& a, Doc& d, const std::vector<int>& parts) {
  mesh = AmMesh();
  for (int k : parts) {
    const gp_Trsf T = partTrsf(d, k);
    for (TopExp_Explorer ex(d.model->parts[k].shape, TopAbs_FACE); ex.More(); ex.Next()) {
      const TopoDS_Face& fc = TopoDS::Face(ex.Current());
      TopLoc_Location l;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(fc, l);
      if (t.IsNull()) continue;
      if (!t->HasNormals()) { try { BRepLib_ToolTriangulatedShape::ComputeNormals(fc, t); } catch (...) {} }
      const gp_Trsf M = T * l.Transformation();
      const bool rev = fc.Orientation() == TopAbs_REVERSED;
      const int base = (int)mesh.p.size();
      for (int i = 1; i <= t->NbNodes(); i++) {
        mesh.p.push_back(t->Node(i).Transformed(M).XYZ());
        gp_Dir n = t->HasNormals() ? t->Normal(i) : gp_Dir(0, 0, 1);
        if (rev) n.Reverse();
        mesh.n.push_back(n.Transformed(M).XYZ());
      }
      for (int i = 1; i <= t->NbTriangles(); i++) {
        int i1, i2, i3;
        t->Triangle(i).Get(i1, i2, i3);
        if (rev) std::swap(i2, i3);
        mesh.t.push_back({base + i1 - 1, base + i2 - 1, base + i3 - 1});
        mesh.part.push_back(k);
      }
    }
  }
  (void)a;
}

// ======================================================================== BVH for wall thickness
struct Bvh {
  struct Node { float lo[3], hi[3]; int left = -1, right = -1, first = 0, count = 0; };
  std::vector<Node> nodes;
  std::vector<int> tri;
  const AmMesh* m = nullptr;
  void build(const AmMesh& mm, std::vector<int> subset) {
    m = &mm;
    tri = std::move(subset);
    nodes.clear();
    nodes.reserve(tri.size() * 2 / 4 + 8);
    split(0, (int)tri.size());
  }
  gp_XYZ ctr(int t) const { return (m->p[m->t[t][0]] + m->p[m->t[t][1]] + m->p[m->t[t][2]]) / 3.0; }
  int split(int first, int count) {
    Node n;
    for (int k = 0; k < 3; k++) { n.lo[k] = 1e30f; n.hi[k] = -1e30f; }
    for (int i = first; i < first + count; i++)
      for (int v = 0; v < 3; v++) {
        const gp_XYZ& p = m->p[m->t[tri[i]][v]];
        for (int k = 0; k < 3; k++) { n.lo[k] = std::min(n.lo[k], (float)p.Coord(k + 1)); n.hi[k] = std::max(n.hi[k], (float)p.Coord(k + 1)); }
      }
    const int id = (int)nodes.size();
    nodes.push_back(n);
    if (count <= 6) { nodes[id].first = first; nodes[id].count = count; return id; }
    int axis = 0;
    float ext = 0;
    for (int k = 0; k < 3; k++) if (n.hi[k] - n.lo[k] > ext) { ext = n.hi[k] - n.lo[k]; axis = k; }
    const int mid = first + count / 2;
    std::nth_element(tri.begin() + first, tri.begin() + mid, tri.begin() + first + count, [&](int x, int y) { return ctr(x).Coord(axis + 1) < ctr(y).Coord(axis + 1); });
    const int l = split(first, mid - first);
    const int r = split(mid, first + count - mid);
    nodes[id].left = l;
    nodes[id].right = r;
    return id;
  }
  static bool box(const Node& n, const gp_XYZ& o, const gp_XYZ& inv, double tmax) {
    double t0 = 0, t1 = tmax;
    for (int k = 0; k < 3; k++) {
      double a = (n.lo[k] - o.Coord(k + 1)) * inv.Coord(k + 1), b = (n.hi[k] - o.Coord(k + 1)) * inv.Coord(k + 1);
      if (a > b) std::swap(a, b);
      t0 = std::max(t0, a); t1 = std::min(t1, b);
      if (t0 > t1) return false;
    }
    return true;
  }
  /** Nearest hit distance along the ray (Möller–Trumbore, both sides). */
  double cast(const gp_XYZ& o, const gp_XYZ& d, double tmax) const {
    gp_XYZ inv(1 / (std::fabs(d.X()) > 1e-12 ? d.X() : 1e-12), 1 / (std::fabs(d.Y()) > 1e-12 ? d.Y() : 1e-12), 1 / (std::fabs(d.Z()) > 1e-12 ? d.Z() : 1e-12));
    double best = tmax;
    int stack[128], sp = 0;
    stack[sp++] = 0;
    while (sp) {
      const Node& n = nodes[stack[--sp]];
      if (!box(n, o, inv, best)) continue;
      if (n.left < 0) {
        for (int i = n.first; i < n.first + n.count; i++) {
          const auto& t = m->t[tri[i]];
          const gp_XYZ &a = m->p[t[0]], &b = m->p[t[1]], &c = m->p[t[2]];
          gp_XYZ e1 = b - a, e2 = c - a, pv = d.Crossed(e2);
          const double det = e1.Dot(pv);
          if (std::fabs(det) < 1e-14) continue;
          const double id = 1 / det;
          gp_XYZ tv = o - a;
          const double u = tv.Dot(pv) * id;
          if (u < 0 || u > 1) continue;
          gp_XYZ qv = tv.Crossed(e1);
          const double v = d.Dot(qv) * id;
          if (v < 0 || u + v > 1) continue;
          const double tt = e2.Dot(qv) * id;
          if (tt > 1e-9 && tt < best) best = tt;
        }
      } else if (sp < 126) { stack[sp++] = n.left; stack[sp++] = n.right; }
    }
    return best;
  }
};

// ======================================================================== analysis
static const Quantity_Color cPlate(0.25, 0.54, 0.95, Quantity_TOC_sRGB), cBad(0.93, 0.22, 0.20, Quantity_TOC_sRGB),
    cWarn(1.0, 0.75, 0.15, Quantity_TOC_sRGB), cOk(0.70, 0.73, 0.78, Quantity_TOC_sRGB), cGood(0.25, 0.78, 0.45, Quantity_TOC_sRGB);

static Quantity_Color mix(const Quantity_Color& a, const Quantity_Color& b, double t) {
  return Quantity_Color(a.Red() + (b.Red() - a.Red()) * t, a.Green() + (b.Green() - a.Green()) * t, a.Blue() + (b.Blue() - a.Blue()) * t, Quantity_TOC_RGB);
}

static void analyse(App& a, std::vector<Quantity_Color>& cols) {
  const gp_XYZ dir = buildDir.XYZ();
  const size_t nv = mesh.p.size();
  cols.assign(nv, cOk);
  stats = AmStats();
  if (!nv) return;
  double hmin = 1e300, hmax = -1e300;
  for (auto& p : mesh.p) { const double h = p.Dot(dir); hmin = std::min(hmin, h); hmax = std::max(hmax, h); }
  stats.height = hmax - hmin;
  const double tolH = std::max(1e-6, stats.height * 1e-3);
  const double cBadT = std::cos(limitDeg * M_PI / 180.0), cWarnT = std::cos(std::min(89.0, limitDeg + 10.0) * M_PI / 180.0);
  // per triangle stats
  for (auto& t : mesh.t) {
    const gp_XYZ &p0 = mesh.p[t[0]], &p1 = mesh.p[t[1]], &p2 = mesh.p[t[2]];
    gp_XYZ nn = (p1 - p0).Crossed(p2 - p0);
    const double area = 0.5 * nn.Modulus();
    if (area <= 0) continue;
    nn /= 2 * area;
    const double down = -nn.Dot(dir);
    const bool onPlate = std::fabs(p0.Dot(dir) - hmin) < tolH && std::fabs(p1.Dot(dir) - hmin) < tolH && std::fabs(p2.Dot(dir) - hmin) < tolH && down > 0.99;
    if (onPlate) stats.plate += area;
    else if (down > cBadT) { stats.overhang += area; stats.support += area * down; }
  }
  if (mapMode == 0) {
    for (size_t i = 0; i < nv; i++) {
      const double down = -mesh.n[i].Dot(dir);
      const bool plate = std::fabs(mesh.p[i].Dot(dir) - hmin) < tolH && down > 0.99;
      cols[i] = plate ? cPlate : down > cBadT ? cBad : down > cWarnT ? cWarn : cOk;
    }
  } else {
    a.toastMsg("Computing wall thickness…", false, 1);
    // one BVH per part: touching parts must not read as thin walls
    std::map<int, std::vector<int>> byPart;
    for (size_t i = 0; i < mesh.t.size(); i++) byPart[mesh.part[i]].push_back((int)i);
    std::map<int, Bvh> bvhs;
    for (auto& kv : byPart) bvhs[kv.first].build(mesh, kv.second);
    std::vector<int> vpart(nv, -1);
    for (size_t i = 0; i < mesh.t.size(); i++) for (int v : mesh.t[i]) vpart[v] = mesh.part[i];
    stats.thickness = true;
    double mn = 1e300;
    size_t thin = 0;
    for (size_t i = 0; i < nv; i++) {
      gp_XYZ n = mesh.n[i];
      const double nm = n.Modulus();
      if (nm < 1e-12) continue;
      n /= nm;
      const gp_XYZ o = mesh.p[i] - n * (stats.height * 1e-5 + 1e-6);
      if (vpart[i] < 0) continue;
      const double t = bvhs[vpart[i]].cast(o, n.Reversed(), minWall * 10);
      mn = std::min(mn, t);
      if (t < minWall) thin++;
      cols[i] = t < minWall ? cBad : t < 2 * minWall ? mix(cBad, cWarn, (t - minWall) / minWall) : t < 3 * minWall ? mix(cWarn, cGood, (t - 2 * minWall) / minWall) : cGood;
    }
    stats.minWall = mn < 1e299 ? mn : -1;
    stats.thinShare = (double)thin / nv;
  }
}

static void placePlate(App& a) {
  if (mesh.p.empty()) return;
  const gp_XYZ dir = buildDir.XYZ();
  gp_Dir u = std::fabs(buildDir.X()) < 0.9 ? gp_Dir(gp_Vec(buildDir).Crossed(gp_Vec(1, 0, 0))) : gp_Dir(gp_Vec(buildDir).Crossed(gp_Vec(0, 1, 0)));
  gp_Dir v = gp_Dir(gp_Vec(buildDir).Crossed(gp_Vec(u)));
  double hmin = 1e300, u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
  for (auto& p : mesh.p) {
    hmin = std::min(hmin, p.Dot(dir));
    const double pu = p.Dot(u.XYZ()), pv = p.Dot(v.XYZ());
    u0 = std::min(u0, pu); u1 = std::max(u1, pu); v0 = std::min(v0, pv); v1 = std::max(v1, pv);
  }
  const double pad = 0.12 * std::max(u1 - u0, v1 - v0);
  Handle(PlatePrs) pl = Handle(PlatePrs)::DownCast(plateObj);
  if (pl.IsNull()) { pl = new PlatePrs(); plateObj = pl; }
  const double us[4] = {u0 - pad, u1 + pad, u1 + pad, u0 - pad}, vs[4] = {v0 - pad, v0 - pad, v1 + pad, v1 + pad};
  for (int i = 0; i < 4; i++) pl->c[i] = gp_Pnt(dir * (hmin - stats.height * 2e-3) + u.XYZ() * us[i] + v.XYZ() * vs[i]);
  if (a.ctx->IsDisplayed(pl)) a.ctx->Redisplay(pl, false);
  else a.ctx->Display(pl, 0, -1, false);
}

static void apply(App& a) {
  Doc* d = activeDoc(a);
  if (!d || !d->displayed) return;
  // target: selection or the whole model
  amParts.clear();
  std::function<void(gv::Node*)> walk = [&](gv::Node* x) {
    for (int p : x->parts) if (a.nodeVisible(x)) amParts.push_back(p);
    for (auto& k : x->kids) walk(k.get());
  };
  walk(d->selected ? d->selected : d->model->root.get());
  gather(a, *d, amParts);
  Handle(AmPrs) prs = Handle(AmPrs)::DownCast(amObj);
  if (prs.IsNull()) { prs = new AmPrs(); amObj = prs; }
  analyse(a, prs->cols);
  prs->m = &mesh;
  for (int k : amParts) { a.ctx->Erase(d->prs[k], false); if (!d->soft[k].IsNull()) a.ctx->Erase(d->soft[k], false); }
  if (a.ctx->IsDisplayed(prs)) a.ctx->Redisplay(prs, false);
  else a.ctx->Display(prs, 0, -1, false);
  placePlate(a);
  amKey = std::to_string((size_t)d) + "|" + std::to_string((size_t)d->selected);
  requestFrames(a);
}

static void clearAm(App& a) {
  if (!amObj.IsNull()) a.ctx->Remove(amObj, false);
  if (!plateObj.IsNull()) a.ctx->Remove(plateObj, false);
  amKey.clear();
}

/** Try 126 build directions: least projected support area, then lowest height. */
static void autoOrient(App&) {
  orients.clear();
  if (mesh.t.empty()) return;
  std::vector<gp_Dir> dirs = {gp_Dir(0, 0, 1), gp_Dir(0, 0, -1), gp_Dir(1, 0, 0), gp_Dir(-1, 0, 0), gp_Dir(0, 1, 0), gp_Dir(0, -1, 0)};
  const int N = 120;
  for (int i = 0; i < N; i++) {
    const double y = 1 - 2 * (i + 0.5) / N, r = std::sqrt(1 - y * y), th = M_PI * (3 - std::sqrt(5.0)) * i;
    dirs.push_back(gp_Dir(r * std::cos(th), r * std::sin(th), y));
  }
  const size_t step = std::max<size_t>(1, mesh.t.size() / 150000);
  const double cBadT = std::cos(limitDeg * M_PI / 180.0);
  std::vector<Orient> res;
  double maxS = 1e-12, maxH = 1e-12;
  for (auto& d : dirs) {
    const gp_XYZ dir = d.XYZ();
    double hmin = 1e300, hmax = -1e300;
    for (size_t i = 0; i < mesh.p.size(); i += std::max<size_t>(1, mesh.p.size() / 50000)) { const double h = mesh.p[i].Dot(dir); hmin = std::min(hmin, h); hmax = std::max(hmax, h); }
    const double tolH = std::max(1e-6, (hmax - hmin) * 1e-3);
    double sup = 0;
    for (size_t ti = 0; ti < mesh.t.size(); ti += step) {
      const auto& t = mesh.t[ti];
      const gp_XYZ &p0 = mesh.p[t[0]], &p1 = mesh.p[t[1]], &p2 = mesh.p[t[2]];
      gp_XYZ nn = (p1 - p0).Crossed(p2 - p0);
      const double area = 0.5 * nn.Modulus();
      if (area <= 0) continue;
      nn /= 2 * area;
      const double down = -nn.Dot(dir);
      if (down <= cBadT) continue;
      if (down > 0.99 && std::fabs(p0.Dot(dir) - hmin) < tolH) continue; // on the plate
      sup += area * down * step;
    }
    res.push_back({d, sup, hmax - hmin, 0});
    maxS = std::max(maxS, sup);
    maxH = std::max(maxH, hmax - hmin);
  }
  for (auto& r : res) {
    r.score = r.support / maxS + 0.35 * r.height / maxH;
    const double ax = std::max({std::fabs(r.d.X()), std::fabs(r.d.Y()), std::fabs(r.d.Z())});
    if (ax > 0.9999) r.score -= 0.04;
  }
  std::sort(res.begin(), res.end(), [](const Orient& x, const Orient& y) { return x.score < y.score; });
  for (auto& r : res) {
    bool distinct = true;
    for (auto& o : orients) if (o.d.Angle(r.d) < 15 * M_PI / 180) distinct = false;
    if (distinct) orients.push_back(r);
    if (orients.size() >= 4) break;
  }
}

// ======================================================================== place a face on the plate
static void save(App& a);

/** Ray-picks the analysed mesh; the clicked facet's outward normal becomes "down". */
bool amPick(App& a, const Graphic3d_Vec2i& px) {
  if (!clickPlace || mesh.t.empty()) return true;
  double X, Y, Z, Vx, Vy, Vz;
  a.view->ConvertWithProj(px.x(), px.y(), X, Y, Z, Vx, Vy, Vz);
  const gp_XYZ o(X, Y, Z), dir = gp_XYZ(Vx, Vy, Vz).Normalized();
  const gp_XYZ camDir = a.view->Camera()->Direction().XYZ();
  double best = 1e300;
  int hit = -1;
  for (size_t i = 0; i < mesh.t.size(); i++) {
    const auto& t = mesh.t[i];
    const gp_XYZ &p0 = mesh.p[t[0]], &p1 = mesh.p[t[1]], &p2 = mesh.p[t[2]];
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
    const double tt = e2.Dot(qv) * inv;
    const gp_XYZ hp = o + dir * tt;
    const double depth = (hp - a.view->Camera()->Eye().XYZ()).Dot(camDir);
    if (depth < best) { best = depth; hit = (int)i; }
  }
  if (hit < 0) return true;
  const auto& t = mesh.t[hit];
  gp_XYZ n = (mesh.p[t[1]] - mesh.p[t[0]]).Crossed(mesh.p[t[2]] - mesh.p[t[0]]);
  if (n.Modulus() < 1e-18) return true;
  buildDir = gp_Dir(n.Reversed());   // outward normal points down onto the plate
  orients.clear();
  save(a);
  apply(a);
  a.toastMsg("Face placed on the build plate", false, 1.5);
  return true;
}

// ======================================================================== hooks
void amInit(App& a) {
  double x = 0, y = 0, z = 1;
  sscanf(a.set.get("amDir", "0,0,1").c_str(), "%lf,%lf,%lf", &x, &y, &z);
  if (x * x + y * y + z * z > 1e-12) buildDir = gp_Dir(x, y, z);
  mapMode = a.set.geti("amMap", 0);
  limitDeg = a.set.geti("amLimit", 45);
  minWall = a.set.getd("amWall", 1.0);
}

static void save(App& a) {
  char b[96];
  snprintf(b, sizeof b, "%.5f,%.5f,%.5f", buildDir.X(), buildDir.Y(), buildDir.Z());
  a.set.kv["amDir"] = b;
  a.set.kv["amMap"] = std::to_string(mapMode);
  a.set.kv["amLimit"] = std::to_string(limitDeg);
  a.set.kv["amWall"] = fixd(minWall, 4);
  a.set.save();
}

void amSetActive(App& a, bool on) {
  orients.clear();
  if (on) apply(a);
  else {
    clearAm(a);
    if (Doc* d = activeDoc(a)) a.showVisible(*d);
  }
}

void amDocShown(App& a) {
  if (!F().amOn) return;
  Doc* d = activeDoc(a);
  const std::string key = std::to_string((size_t)d) + "|" + std::to_string((size_t)(d ? d->selected : nullptr));
  if (key != amKey) { orients.clear(); apply(a); }
  else if (d) for (int k : amParts) { a.ctx->Erase(d->prs[k], false); if (!d->soft[k].IsNull()) a.ctx->Erase(d->soft[k], false); }
}

void amOverlay(App& a) {
  if (!F().amOn) return;
  Doc* d = activeDoc(a);
  const std::string key = std::to_string((size_t)d) + "|" + std::to_string((size_t)(d ? d->selected : nullptr));
  if (key != amKey && d && d->displayed) {
    if (d) a.showVisible(*d); // restore previously analysed parts, then analyse the new target
    orients.clear();
    apply(a);
  }
}

void amPanel(App& a, float& y) {
  Ui& ui = a.ui;
  const float s = a.scale, m = 12 * s;
  const ImVec2 W = ImGui::GetIO().DisplaySize;
  float h = 0;
  ui.glassWindow("##am", ImVec2(W.x - m, y), ImVec2(1, 0), 312 * s, [&] {
    bool open = true;
    ui.panelHeader("AM check", &open);
    if (!open) { F().amOn = false; amSetActive(a, false); return; }
    Doc* d = activeDoc(a);
    ImGui::TextDisabled("%s", d && d->selected ? d->selected->name.c_str() : "Whole model");
    static const char* dirs[] = {"+Z", "−Z", "+X", "−X", "+Y", "−Y"};
    static const gp_Dir dv[] = {gp_Dir(0, 0, 1), gp_Dir(0, 0, -1), gp_Dir(1, 0, 0), gp_Dir(-1, 0, 0), gp_Dir(0, 1, 0), gp_Dir(0, -1, 0)};
    int cur = -1;
    for (int i = 0; i < 6; i++) if (buildDir.IsEqual(dv[i], 1e-6)) cur = i;
    int c2 = cur;
    ImGui::TextDisabled("Build direction");
    if (ui.segmented("##bd", dirs, 6, c2) && c2 >= 0) { buildDir = dv[c2]; save(a); apply(a); }
    static const char* maps[] = {"Overhang", "Wall thickness"};
    int mm = mapMode;
    if (ui.segmented("##map", maps, 2, mm)) { mapMode = mm; save(a); apply(a); }
    if (mapMode == 0) {
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60 * s);
      if (ImGui::SliderInt("##lim", &limitDeg, 20, 70, "")) {}
      if (ImGui::IsItemDeactivatedAfterEdit()) { save(a); apply(a); }
      ImGui::SameLine();
      ImGui::Text("%d° limit", limitDeg);
    } else {
      double v = units().inch ? minWall / 25.4 : minWall;
      ImGui::TextDisabled("Minimum wall");
      ImGui::SameLine();
      if (ui.numberField("##mw", v, lenUnit(), 80 * s) && v > 0) { minWall = units().inch ? v * 25.4 : v; save(a); apply(a); }
    }
    // legend
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto key = [&](const Quantity_Color& c, const std::string& t) {
      ImVec2 p = ImGui::GetCursorScreenPos();
      double r, g, b;
      c.Values(r, g, b, Quantity_TOC_sRGB);
      dl->AddRectFilled(ImVec2(p.x, p.y + 3 * s), ImVec2(p.x + 10 * s, p.y + 13 * s), IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255), 3 * s);
      ImGui::Dummy(ImVec2(13 * s, 14 * s));
      ImGui::SameLine(0, 2 * s);
      ImGui::PushFont(ui.small());
      ImGui::TextDisabled("%s", t.c_str());
      ImGui::PopFont();
      ImGui::SameLine(0, 10 * s);
    };
    if (mapMode == 0) { key(cBad, "needs support"); key(cWarn, "borderline"); key(cPlate, "on plate"); }
    else { key(cBad, "< " + fLen(minWall, true, 2)); key(cWarn, "< " + fLen(2 * minWall, true, 2)); key(cGood, "≥ " + fLen(3 * minWall, true, 2)); }
    ImGui::NewLine();
    ui.divider();
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(v.c_str()).x); ImGui::TextUnformatted(v.c_str()); };
    kv("Build height", fLen(stats.height, true, 2));
    kv("Needs support", fArea(stats.overhang));
    kv("Support footprint", fArea(stats.support) + " (projected)");
    kv("On build plate", fArea(stats.plate));
    if (stats.thickness) {
      kv("Thinnest wall", stats.minWall >= 0 && stats.minWall < minWall * 10 ? fLen(stats.minWall, true, 2) : "≥ " + fLen(minWall * 10, true, 1));
      kv("Below minimum", fixd(stats.thinShare * 100, 1) + " % of points");
    }
    ui.toggle("Click a face to put it on the plate", clickPlace);
    if (ui.pillButton("Suggest build orientation", true, ImGui::GetContentRegionAvail().x)) {
      autoOrient(a);
      if (!orients.empty()) { buildDir = orients[0].d; save(a); apply(a); }
    }
    for (size_t i = 0; i < orients.size(); i++) {
      const Orient& o = orients[i];
      const double ax[3] = {o.d.X(), o.d.Y(), o.d.Z()};
      int bi = 0;
      for (int k = 1; k < 3; k++) if (std::fabs(ax[k]) > std::fabs(ax[bi])) bi = k;
      const double tilt = std::acos(std::min(1.0, std::fabs(ax[bi]))) * 180 / M_PI;
      std::string name = std::to_string(i + 1) + ".  " + (ax[bi] > 0 ? "+" : "−") + "XYZ"[bi] + (tilt >= 0.5 ? " tilted " + fixd(tilt, 0) + "°" : "");
      std::string lbl = name + "   " + fArea(o.support, 1) + " support · " + fLen(o.height, true, 0) + " high";
      ImGui::PushID((int)i);
      if (ImGui::Selectable(lbl.c_str(), buildDir.IsEqual(o.d, 1e-6))) { buildDir = o.d; save(a); apply(a); }
      ImGui::PopID();
    }
    ui.divider();
    if (ui.pillButton("Export 3MF in this orientation…", false, ImGui::GetContentRegionAvail().x)) exportBuild3mf(a, buildDir);
    h = ImGui::GetWindowHeight();
  });
  y += h + 10 * s;
}
void amSetMapMode(int m) { mapMode = m; }
