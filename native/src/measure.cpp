// GlassView Native – measure tool on the exact B-rep (OpenCASCADE sub-shape picking).
#include "features.hpp"

#include <StdSelect_BRepOwner.hxx>
#include <SelectMgr_Filter.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Cone.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Lin.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Graphic3d_Group.hxx>
#include <GLFW/glfw3.h>

static const double PI = 3.14159265358979323846;
static const double DEG = 180.0 / PI;

// ======================================================================== text
std::string describeType(const MEnt& e, std::string* detail, int* cls) {
  std::string d, t;
  int c = 6;
  if (e.k == MEnt::Plane) { t = "Plane face"; c = 0; }
  else if (e.k == MEnt::Axis) {
    c = 1;
    if (e.src == 0) { t = "Line edge"; d = "L " + fLen(e.len); }
    else {
      t = e.fillet ? "Fillet axis" : "Axis";
      if (e.src == 1) d = "Cylinder Ø" + fLen(e.r * 2);
      else if (e.src == 2) d = "Cone " + fAng(e.ang * 2 * DEG) + " · Ø" + fLen(std::max(e.r0, e.r1) * 2);
      else d = "Torus R" + fLen(e.R) + " · r" + fLen(e.rr);
    }
  } else {
    if (e.kind == "vertex") { t = "Vertex"; c = 2; }
    else if (e.kind == "centre") { t = e.fillet ? "Arc centre" : "Centre"; d = "Ø" + fLen(e.r * 2); c = 3; }
    else if (e.kind == "sphere") { t = "Sphere centre"; d = "Ø" + fLen(e.r * 2); c = 3; }
    else if (e.kind == "mid") { t = "Midpoint"; d = "of edge"; c = 4; }
    else if (e.kind == "fcentre") { t = "Face centre"; d = e.on; c = 5; }
    else if (e.kind == "edge") { t = "Point on edge"; }
    else { t = "Point"; if (!e.on.empty()) d = "on " + e.on; }
  }
  if (detail) *detail = d;
  if (cls) *cls = c;
  return t;
}

// ======================================================================== measuring
static gp_Pnt footOnLine(const gp_Pnt& p, const gp_Pnt& o, const gp_Dir& d) { return o.Translated(gp_Vec(d) * gp_Vec(o, p).Dot(gp_Vec(d))); }
static gp_Pnt footOnPlane(const gp_Pnt& p, const gp_Pnt& q, const gp_Dir& n) { return p.Translated(gp_Vec(n) * -gp_Vec(q, p).Dot(gp_Vec(n))); }
static gp_Dir perp(const gp_Dir& d) { gp_Vec u = std::fabs(d.X()) < 0.9 ? gp_Vec(1, 0, 0) : gp_Vec(0, 1, 0); u -= gp_Vec(d) * u.Dot(gp_Vec(d)); return gp_Dir(u); }
static int sgn(const MEnt& e) { return e.hasCond() && e.cond ? (e.cond == 1 ? -1 : 1) : 0; }
static const char* condName(const MEnt& e) { return e.k == MEnt::Axis ? (e.hasCond() ? (e.cond == 1 ? "near" : e.cond == 2 ? "far" : "centre") : "axis") : e.k == MEnt::Plane ? "plane" : "point"; }

MRes measureEntities(const MEnt& A0, const MEnt& B0) {
  const MEnt *A = &A0, *B = &B0;
  if ((int)A->k > (int)B->k) std::swap(A, B);
  MRes r;
  auto line = [&](const gp_Pnt& a, const gp_Pnt& b) { r.hasLine = true; r.a = a; r.b = b; r.anchor = gp_Pnt((a.XYZ() + b.XYZ()) / 2); };
  auto condTxt = [&]() { return (sgn(*A) || sgn(*B)) ? std::string(" · ") + condName(*A) + " ↔ " + condName(*B) : std::string(); };
  const double sinPar = std::sin(0.01 / DEG);
  if (A->k == MEnt::Point && B->k == MEnt::Point) {
    r.kind = "Distance"; r.main = fLen(A->p.Distance(B->p)); r.sub = "point to point"; line(A->p, B->p);
  } else if (A->k == MEnt::Point && B->k == MEnt::Plane) {
    gp_Pnt f = footOnPlane(A->p, B->p, B->n);
    r.kind = "Point ↔ plane"; r.main = fLen(f.Distance(A->p)); r.sub = "perpendicular distance"; line(A->p, f);
  } else if (A->k == MEnt::Point && B->k == MEnt::Axis) {
    gp_Pnt f = footOnLine(A->p, B->o, B->d);
    const double d = f.Distance(A->p);
    gp_Dir u = d > 1e-9 ? gp_Dir(gp_Vec(A->p, f)) : perp(B->d);
    gp_Pnt e = f.Translated(gp_Vec(u) * (sgn(*B) * B->r));
    std::string det;
    describeType(*B, &det);
    r.kind = B->src == 0 ? "Point ↔ line" : sgn(*B) ? "Point ↔ cylinder" : "Point ↔ axis";
    r.main = fLen(e.Distance(A->p)); r.sub = "to " + det + condTxt(); r.cond = B->hasCond(); line(A->p, e);
  } else if (A->k == MEnt::Plane && B->k == MEnt::Plane) {
    const double c = std::fabs(gp_Vec(A->n).Dot(gp_Vec(B->n)));
    if (c > std::cos(0.01 / DEG)) {
      gp_Pnt f = footOnPlane(A->click, B->p, B->n);
      r.kind = "Plane ↔ plane"; r.main = fLen(f.Distance(A->click)); r.sub = "parallel planes"; line(A->click, f);
    } else {
      const double ang = std::acos(std::min(1.0, c)) * DEG;
      r.kind = "Angle"; r.main = fAng(ang); r.sub = "between planes (supplement " + fAng(180 - ang) + ")"; r.angle = true;
      line(A->click, B->click);
    }
  } else if (A->k == MEnt::Axis && B->k == MEnt::Axis) {
    r.cond = A->hasCond() || B->hasCond();
    const double s = gp_Vec(A->d).Crossed(gp_Vec(B->d)).Magnitude();
    gp_Pnt pA, pB;
    std::string sub;
    if (s < sinPar) { pA = A->mid(); pB = footOnLine(pA, B->o, B->d); sub = "parallel"; }
    else {
      gp_Vec w(B->o, A->o);
      const double b = gp_Vec(A->d).Dot(gp_Vec(B->d)), d = gp_Vec(A->d).Dot(w), e = gp_Vec(B->d).Dot(w), den = 1 - b * b;
      pA = A->o.Translated(gp_Vec(A->d) * ((b * e - d) / den));
      pB = B->o.Translated(gp_Vec(B->d) * ((e - b * d) / den));
      const double ang = std::acos(std::min(1.0, std::fabs(b))) * DEG;
      if (pA.Distance(pB) < 1e-4 && !sgn(*A) && !sgn(*B)) {
        r.kind = "Angle"; r.main = fAng(ang); r.sub = "intersecting"; r.angle = true; r.anchor = pA; return r;
      }
      sub = "skew · angle " + fAng(ang);
    }
    const double dd = pA.Distance(pB);
    gp_Dir u = dd > 1e-9 ? gp_Dir(gp_Vec(pA, pB)) : perp(A->d);
    gp_Pnt a2 = pA.Translated(gp_Vec(u) * (-sgn(*A) * A->r)), b2 = pB.Translated(gp_Vec(u) * (sgn(*B) * B->r));
    r.kind = (sgn(*A) || sgn(*B)) ? "Cylinder ↔ cylinder" : (A->src == 0 || B->src == 0) ? "Line ↔ line" : "Axis ↔ axis";
    r.main = fLen(a2.Distance(b2)); r.sub = sub + condTxt(); line(a2, b2);
  } else if (A->k == MEnt::Axis && B->k == MEnt::Plane) {
    const double sn = std::fabs(gp_Vec(A->d).Dot(gp_Vec(B->n)));
    if (sn < sinPar) {
      gp_Pnt p1 = A->mid(), f = footOnPlane(p1, B->p, B->n);
      const double d = f.Distance(p1);
      gp_Dir u = d > 1e-9 ? gp_Dir(gp_Vec(p1, f)) : B->n;
      gp_Pnt a2 = p1.Translated(gp_Vec(u) * (-sgn(*A) * A->r));
      r.kind = sgn(*A) ? "Cylinder ↔ plane" : A->src == 0 ? "Line ↔ plane" : "Axis ↔ plane";
      r.main = fLen(a2.Distance(f)); r.sub = "parallel to plane" + condTxt(); r.cond = A->hasCond(); line(a2, f);
    } else {
      const double ang = std::asin(std::min(1.0, sn)) * DEG;
      r.kind = "Angle"; r.main = fAng(ang); r.sub = ang > 89.99 ? "perpendicular to plane" : "to plane"; r.angle = true; r.anchor = A->mid();
    }
  }
  return r;
}

// ======================================================================== snap data
PartSnaps& snapsOf(App& a, Doc& d, int part) {
  (void)a;
  DocExt& x = ext(d);
  if (x.snaps.size() < d.model->parts.size()) x.snaps.resize(d.model->parts.size());
  PartSnaps& s = x.snaps[part];
  if (s.built) return s;
  s.built = true;
  const Part& p = d.model->parts[part];
  if (p.isMesh) return s;
  Bnd_Box bb;
  BRepBndLib::Add(p.shape, bb);
  double size = 1;
  if (!bb.IsVoid()) { double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1); size = std::max({x1 - x0, y1 - y0, z1 - z0, 1e-6}); }
  const double tol = size * 1e-5;
  TopTools_IndexedMapOfShape verts, edges, faces;
  TopExp::MapShapes(p.shape, TopAbs_VERTEX, verts);
  TopExp::MapShapes(p.shape, TopAbs_EDGE, edges);
  TopExp::MapShapes(p.shape, TopAbs_FACE, faces);
  for (int i = 1; i <= verts.Extent(); i++) s.corners.push_back(BRep_Tool::Pnt(TopoDS::Vertex(verts(i))));
  auto addCentre = [&](const gp_Pnt& c, double r, bool fillet, int kind) {
    for (auto& q : s.centres) if (q.p.Distance(c) < tol && std::fabs(q.r - r) < tol) { q.fillet = q.fillet && fillet; return; }
    s.centres.push_back({c, r, fillet, kind, ""});
  };
  for (int i = 1; i <= edges.Extent(); i++) {
    const TopoDS_Edge& e = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(e)) continue;
    try {
      BRepAdaptor_Curve c(e);
      const double f = c.FirstParameter(), l = c.LastParameter();
      if (c.GetType() == GeomAbs_Line) s.mids.push_back(c.Value((f + l) / 2));
      else if (c.GetType() == GeomAbs_Circle) addCentre(c.Circle().Location(), c.Circle().Radius(), (l - f) < PI - 1e-6, 0);
    } catch (...) {}
  }
  struct Grp { SnapAx ax; };
  std::vector<SnapAx> raw;
  for (int i = 1; i <= faces.Extent(); i++) {
    const TopoDS_Face& fc = TopoDS::Face(faces(i));
    try {
      BRepAdaptor_Surface sf(fc, true);
      double u0, u1, v0, v1;
      BRepTools::UVBounds(fc, u0, u1, v0, v1);
      switch (sf.GetType()) {
        case GeomAbs_Plane: {
          GProp_GProps g;
          BRepGProp::SurfaceProperties(fc, g);
          s.fcentres.push_back({g.CentreOfMass(), 0, false, 2, "plane"});
          break;
        }
        case GeomAbs_Cylinder: {
          gp_Cylinder cy = sf.Cylinder();
          const gp_Ax1 ax = cy.Axis();
          SnapAx A;
          A.src = 1; A.r = cy.Radius(); A.d = ax.Direction();
          A.a = ax.Location().Translated(gp_Vec(ax.Direction()) * v0);
          A.b = ax.Location().Translated(gp_Vec(ax.Direction()) * v1);
          A.span = u1 - u0;
          raw.push_back(A);
          s.fcentres.push_back({gp_Pnt((A.a.XYZ() + A.b.XYZ()) / 2), A.r, A.span < PI - 1e-6, 2, "cylinder (on axis)"});
          break;
        }
        case GeomAbs_Cone: {
          gp_Cone co = sf.Cone();
          const gp_Ax1 ax = co.Axis();
          gp_Pnt q0 = sf.Value(u0, v0), q1 = sf.Value(u0, v1);
          SnapAx A;
          A.src = 2; A.d = ax.Direction(); A.ang = co.SemiAngle();
          A.a = footOnLine(q0, ax.Location(), ax.Direction());
          A.b = footOnLine(q1, ax.Location(), ax.Direction());
          A.r0 = q0.Distance(A.a); A.r1 = q1.Distance(A.b); A.r = std::max(A.r0, A.r1);
          A.span = u1 - u0;
          raw.push_back(A);
          break;
        }
        case GeomAbs_Sphere: addCentre(sf.Sphere().Location(), sf.Sphere().Radius(), false, 1); break;
        case GeomAbs_Torus: {
          gp_Torus t = sf.Torus();
          const gp_Ax1 ax = t.Axis();
          SnapAx A;
          A.src = 3; A.d = ax.Direction(); A.R = t.MajorRadius(); A.rr = t.MinorRadius();
          A.a = ax.Location().Translated(gp_Vec(ax.Direction()) * -A.rr);
          A.b = ax.Location().Translated(gp_Vec(ax.Direction()) * A.rr);
          A.fillet = true;
          raw.push_back(A);
          break;
        }
        default: {
          GProp_GProps g;
          BRepGProp::SurfaceProperties(fc, g);
          if (g.Mass() > 0) s.fcentres.push_back({g.CentreOfMass(), 0, false, 2, "surface"});
        }
      }
    } catch (...) {}
  }
  // merge coaxial faces of the same kind and size; fillets = less than half a turn in total
  for (auto& A : raw) {
    bool merged = false;
    for (auto& G : s.axes) {
      if (G.src != A.src || std::fabs(G.r - A.r) > tol || std::fabs(G.R - A.R) > tol) continue;
      if (std::fabs(std::fabs(gp_Vec(G.d).Dot(gp_Vec(A.d))) - 1) > 1e-6) continue;
      if (footOnLine(A.a, G.a, G.d).Distance(A.a) > tol * 10) continue;
      double L = G.a.Distance(G.b);
      gp_Dir dir = L > 1e-12 ? gp_Dir(gp_Vec(G.a, G.b)) : G.d;
      double t0 = 0, t1 = L;
      for (const gp_Pnt& q : {A.a, A.b}) { double t = gp_Vec(G.a, q).Dot(gp_Vec(dir)); t0 = std::min(t0, t); t1 = std::max(t1, t); }
      gp_Pnt a0 = G.a;
      G.a = a0.Translated(gp_Vec(dir) * t0);
      G.b = a0.Translated(gp_Vec(dir) * t1);
      G.span += A.span;
      merged = true;
      break;
    }
    if (!merged) s.axes.push_back(A);
  }
  for (auto& G : s.axes) G.fillet = G.src == 3 || G.span < PI * 0.999;
  return s;
}

// ======================================================================== helpers
static Handle(AIS_InteractiveObject) objOfPart(Doc& d, int k) { return d.prs[k]; }
static int partOfObj(Doc& d, const Handle(SelectMgr_SelectableObject)& o) {
  auto it = d.partOf.find(dynamic_cast<const AIS_InteractiveObject*>(o.get()));
  return it == d.partOf.end() ? -1 : it->second;
}

/** Face-lock filter: only the locked face and its own edges / vertices can be picked. */
class FaceLockFilter : public SelectMgr_Filter {
public:
  Handle(SelectMgr_SelectableObject) obj;
  TopTools_IndexedMapOfShape subs;
  Standard_Boolean IsOk(const Handle(SelectMgr_EntityOwner)& o) const override {
    if (o.IsNull() || o->Selectable() != obj) return false;
    Handle(StdSelect_BRepOwner) b = Handle(StdSelect_BRepOwner)::DownCast(o);
    return !b.IsNull() && subs.Contains(b->Shape());
  }
};

/** Exact point on a sub-shape nearest to p (all in part-local coordinates). */
static gp_Pnt nearestOn(const TopoDS_Shape& s, const gp_Pnt& p) {
  try {
    BRepExtrema_DistShapeShape ex(BRepBuilderAPI_MakeVertex(p).Vertex(), s);
    if (ex.IsDone() && ex.NbSolution() > 0) return ex.PointOnShape2(1);
  } catch (...) {}
  return p;
}

/** Entity for a picked sub-shape. hitW = 3D hit point (world). */
static bool entityFromShape(App& app, Doc& d, int part, const TopoDS_Shape& sub, const gp_Pnt& hitW, bool pointMode, bool lockedEdgePoint, MEnt& e) {
  const gp_Trsf T = partTrsf(d, part), Ti = T.Inverted();
  const gp_Pnt hit = hitW.Transformed(Ti);
  e = MEnt();
  e.part = part;
  try {
    if (sub.ShapeType() == TopAbs_VERTEX) {
      e.k = MEnt::Point; e.kind = "vertex"; e.p = BRep_Tool::Pnt(TopoDS::Vertex(sub)).Transformed(T);
      return true;
    }
    if (sub.ShapeType() == TopAbs_EDGE) {
      const TopoDS_Edge& ed = TopoDS::Edge(sub);
      BRepAdaptor_Curve c(ed);
      if (!pointMode && !lockedEdgePoint && c.GetType() == GeomAbs_Line) {
        e.k = MEnt::Axis; e.src = 0;
        gp_Pnt a = c.Value(c.FirstParameter()).Transformed(T), b = c.Value(c.LastParameter()).Transformed(T);
        e.o = a; e.len = a.Distance(b); e.d = e.len > 1e-12 ? gp_Dir(gp_Vec(a, b)) : gp_Dir(0, 0, 1);
        e.click = nearestOn(ed, hit).Transformed(T);
        return true;
      }
      if (!pointMode && !lockedEdgePoint && c.GetType() == GeomAbs_Circle) {
        e.k = MEnt::Point; e.kind = "centre"; e.p = c.Circle().Location().Transformed(T); e.r = c.Circle().Radius();
        e.fillet = (c.LastParameter() - c.FirstParameter()) < PI - 1e-6;
        return true;
      }
      e.k = MEnt::Point; e.kind = "edge"; e.p = nearestOn(ed, hit).Transformed(T);
      return true;
    }
    if (sub.ShapeType() == TopAbs_FACE) {
      const TopoDS_Face& fc = TopoDS::Face(sub);
      BRepAdaptor_Surface sf(fc, true);
      gp_Pnt on = nearestOn(fc, hit);
      static const char* names[] = {"plane", "cylinder", "cone", "sphere", "torus", "Bézier surface", "B-spline surface", "surface of revolution", "extrusion", "offset surface", "surface"};
      const int t = (int)sf.GetType();
      if (pointMode) { e.k = MEnt::Point; e.kind = "surface"; e.on = names[std::min(t, 10)]; e.p = on.Transformed(T); return true; }
      e.click = on.Transformed(T);
      switch (sf.GetType()) {
        case GeomAbs_Plane: {
          gp_Pln pl = sf.Plane();
          e.k = MEnt::Plane; e.p = pl.Location().Transformed(T); e.n = pl.Axis().Direction().Transformed(T);
          return true;
        }
        case GeomAbs_Cylinder: case GeomAbs_Cone: case GeomAbs_Torus: {
          // use the merged axis record of this part (whole hole / boss, fillet flag)
          PartSnaps& S = snapsOf(app, d, part);
          gp_Ax1 ax = sf.GetType() == GeomAbs_Cylinder ? sf.Cylinder().Axis() : sf.GetType() == GeomAbs_Cone ? sf.Cone().Axis() : sf.Torus().Axis();
          const int src = sf.GetType() == GeomAbs_Cylinder ? 1 : sf.GetType() == GeomAbs_Cone ? 2 : 3;
          const double rad = src == 1 ? sf.Cylinder().Radius() : src == 3 ? sf.Torus().MajorRadius() : 0;
          for (auto& A : S.axes) {
            if (A.src != src || (src != 2 && std::fabs((src == 3 ? A.R : A.r) - rad) > 1e-6 * std::max(1.0, rad))) continue;
            if (std::fabs(std::fabs(gp_Vec(A.d).Dot(gp_Vec(ax.Direction()))) - 1) > 1e-6) continue;
            if (footOnLine(ax.Location(), A.a, A.d).Distance(ax.Location()) > 1e-4 * std::max(1.0, rad)) continue;
            e.k = MEnt::Axis; e.src = src; e.r = A.r; e.r0 = A.r0; e.r1 = A.r1; e.ang = A.ang; e.R = A.R; e.rr = A.rr; e.fillet = A.fillet;
            e.o = A.a.Transformed(T); gp_Pnt b = A.b.Transformed(T);
            e.len = e.o.Distance(b); e.d = e.len > 1e-12 ? gp_Dir(gp_Vec(e.o, b)) : ax.Direction().Transformed(T);
            return true;
          }
          e.k = MEnt::Point; e.kind = "surface"; e.on = names[t]; e.p = e.click;
          return true;
        }
        case GeomAbs_Sphere:
          e.k = MEnt::Point; e.kind = "sphere"; e.p = sf.Sphere().Location().Transformed(T); e.r = sf.Sphere().Radius();
          return true;
        default:
          e.k = MEnt::Point; e.kind = "surface"; e.on = names[std::min(t, 10)]; e.p = e.click;
          return true;
      }
    }
  } catch (...) {}
  return false;
}

// ======================================================================== snap-point dots
class SnapDots : public AIS_InteractiveObject {
public:
  std::vector<gp_Pnt> pts[5]; // corner, centre, mid, face centre, fillet centre
  gp_Vec bias;
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    static const Quantity_Color cols[5] = {
      Quantity_Color(0.10, 0.11, 0.13, Quantity_TOC_sRGB), Quantity_Color(1.0, 0.62, 0.04, Quantity_TOC_sRGB),
      Quantity_Color(0.12, 0.72, 0.80, Quantity_TOC_sRGB), Quantity_Color(1.0, 0.18, 0.57, Quantity_TOC_sRGB),
      Quantity_Color(1.0, 0.80, 0.50, Quantity_TOC_sRGB)};
    static const double sizes[5] = {1.5, 2.2, 1.4, 1.7, 1.5};
    for (int k = 0; k < 5; k++) {
      if (pts[k].empty()) continue;
      Handle(Graphic3d_ArrayOfPoints) arr = new Graphic3d_ArrayOfPoints((int)pts[k].size());
      for (auto& p : pts[k]) arr->AddVertex(p.Translated(bias));
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_BALL, cols[k], sizes[k]));
      g->AddPrimitiveArray(arr);
    }
  }
};

static void rebuildDots(App& a) {
  Features& f = F();
  Doc* d = activeDoc(a);
  for (auto& dp : a.docs) {
    if (!dp->ext || dp->ext->dots.IsNull()) continue;
    if (dp.get() != d || !f.measureOn || !f.showDots || !f.snap) a.ctx->Erase(dp->ext->dots, false);
  }
  if (!d || !d->displayed || !f.measureOn || !f.showDots || !f.snap) return;
  DocExt& x = ext(*d);
  Handle(SnapDots) dots = Handle(SnapDots)::DownCast(x.dots);
  if (dots.IsNull()) { dots = new SnapDots(); x.dots = dots; }
  for (auto& v : dots->pts) v.clear();
  for (size_t k = 0; k < d->prs.size(); k++) {
    if (!a.ctx->IsDisplayed(d->prs[k])) continue;
    PartSnaps& s = snapsOf(a, *d, (int)k);
    const gp_Trsf T = partTrsf(*d, (int)k);
    if (f.show[0]) for (auto& p : s.corners) dots->pts[0].push_back(p.Transformed(T));
    if (f.show[1]) for (auto& p : s.mids) dots->pts[2].push_back(p.Transformed(T));
    for (auto& c : s.centres) if (f.show[2] && (!c.fillet || f.show[5])) dots->pts[c.fillet ? 4 : 1].push_back(c.p.Transformed(T));
    if (f.show[3]) for (auto& c : s.fcentres) if (!c.fillet || f.show[5]) dots->pts[3].push_back(c.p.Transformed(T));
  }
  // nudge towards the viewer so dots on visible edges win the depth test
  Bnd_Box b = d->model->box;
  double size = 100;
  if (!b.IsVoid()) { double x0, y0, z0, x1, y1, z1; b.Get(x0, y0, z0, x1, y1, z1); size = std::max({x1 - x0, y1 - y0, z1 - z0}); }
  dots->bias = gp_Vec(a.view->Camera()->Direction()) * (-size * 0.004);
  f.dotsDir = a.view->Camera()->Direction();
  if (a.ctx->IsDisplayed(dots)) a.ctx->Redisplay(dots, false);
  else a.ctx->Display(dots, 0, -1, false);
  f.dotsDirty = false;
}

void measureBeforeRedraw(App& a) {
  Features& f = F();
  if (!f.measureOn) return;
  if (f.dotsDirty || f.dotsDir.Angle(a.view->Camera()->Direction()) > 0.02) rebuildDots(a);
}

// ======================================================================== activation
static void setSelectionModes(App& a, bool measure) {
  Doc* d = activeDoc(a);
  if (!d || !d->displayed) return;
  for (size_t k = 0; k < d->prs.size(); k++) {
    const Handle(AIS_Shape)& o = d->prs[k];
    if (!a.ctx->IsDisplayed(o)) continue;
    if (measure) {
      a.ctx->Deactivate(o, 0);
      const bool mesh = d->model->parts[k].isMesh;
      if (!mesh) {
        a.ctx->Activate(o, AIS_Shape::SelectionMode(TopAbs_VERTEX));
        a.ctx->Activate(o, AIS_Shape::SelectionMode(TopAbs_EDGE));
        a.ctx->SetSelectionSensitivity(o, AIS_Shape::SelectionMode(TopAbs_VERTEX), (int)(9 * a.scale));
        a.ctx->SetSelectionSensitivity(o, AIS_Shape::SelectionMode(TopAbs_EDGE), (int)(5 * a.scale));
      }
      a.ctx->Activate(o, AIS_Shape::SelectionMode(TopAbs_FACE));
    } else {
      a.ctx->Deactivate(o);
      a.ctx->Activate(o, 0);
    }
  }
}

static void unlock(App& a) {
  Features& f = F();
  if (!f.lockFilter.IsNull()) { a.ctx->RemoveFilter(f.lockFilter); f.lockFilter.Nullify(); }
  if (!f.lockPrs.IsNull()) { a.ctx->Remove(f.lockPrs, false); f.lockPrs.Nullify(); }
  f.lockObj.Nullify();
  f.lockFace.Nullify();
  f.lockPart = -1;
  f.dotsDirty = true;
}

static Handle(AIS_Shape) facePrs(App& a, Doc& d, int part, const TopoDS_Face& fc, const Quantity_Color& col, double transp) {
  Handle(AIS_Shape) s = new AIS_Shape(fc);
  s->SetLocalTransformation(partTrsf(d, part));
  s->SetColor(col);
  s->SetTransparency(transp);
  s->Attributes()->SetFaceBoundaryDraw(true);
  s->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(col, Aspect_TOL_SOLID, 2.0));
  s->Attributes()->ShadingAspect()->Aspect()->SetPolygonOffsets(Aspect_POM_Fill, -2.0f, -4.0f);
  s->Attributes()->SetAutoTriangulation(false);
  a.ctx->Display(s, AIS_Shaded, -1, false);
  return s;
}

static void lockTo(App& a, Doc& d, int part, const TopoDS_Face& fc) {
  Features& f = F();
  unlock(a);
  f.lockObj = d.prs[part];
  f.lockFace = fc;
  f.lockPart = part;
  Handle(FaceLockFilter) flt = new FaceLockFilter();
  flt->obj = d.prs[part];
  flt->subs.Add(fc);
  TopExp::MapShapes(fc, TopAbs_EDGE, flt->subs);
  TopExp::MapShapes(fc, TopAbs_VERTEX, flt->subs);
  f.lockFilter = flt;
  a.ctx->AddFilter(flt);
  f.lockPrs = facePrs(a, d, part, fc, Quantity_Color(0.04, 0.52, 1.0, Quantity_TOC_sRGB), 0.55);
  a.ctx->Erase(ext(d).dots.IsNull() ? Handle(AIS_InteractiveObject)() : ext(d).dots, false);
}

void measureCancel(App& a) {
  Features& f = F();
  f.hasPending = false;
  if (!f.pendingPrs.IsNull()) { a.ctx->Remove(f.pendingPrs, false); f.pendingPrs.Nullify(); }
  requestFrames(a);
}

void measureSetActive(App& a, bool on) {
  Features& f = F();
  f.measureOn = on;
  unlock(a);
  measureCancel(a);
  f.hasHover = false;
  a.ctx->ClearSelected(false);
  a.ctx->ClearDetected(false);
  setSelectionModes(a, on);
  f.dotsDirty = true;
  rebuildDots(a);
  a.view->Invalidate();
  requestFrames(a);
}

void measureDocShown(App& a) {
  Features& f = F();
  if (!f.measureOn) return;
  unlock(a);
  measureCancel(a);
  setSelectionModes(a, true);
  f.dotsDirty = true;
}

void measureRelabel(App& a) {
  for (auto& dp : a.docs) {
    if (!dp->ext) continue;
    for (auto& m : dp->ext->meas) m.res = measureEntities(m.A, m.B);
  }
}

void measureOnModifier(App& a, bool held) {
  Features& f = F();
  if (!f.measureOn) return;
  if (!held) { if (!f.lockFilter.IsNull()) unlock(a); requestFrames(a); return; }
  if (!f.lockFilter.IsNull()) return;
  Doc* d = activeDoc(a);
  if (!d || !a.ctx->HasDetected()) return;
  Handle(StdSelect_BRepOwner) bo = Handle(StdSelect_BRepOwner)::DownCast(a.ctx->DetectedOwner());
  if (bo.IsNull()) return;
  const int part = partOfObj(*d, bo->Selectable());
  if (part < 0) return;
  // lock onto the face under the cursor (the detected owner may be one of its edges)
  TopoDS_Shape s = bo->Shape();
  if (s.ShapeType() != TopAbs_FACE) {
    // find a face of this part containing the edge/vertex, the one nearest to the hit
    const Handle(StdSelect_ViewerSelector3d)& sel = a.ctx->MainSelector();
    gp_Pnt hit = sel->NbPicked() ? sel->PickedPoint(1).Transformed(partTrsf(*d, part).Inverted()) : gp_Pnt();
    double best = 1e300;
    TopoDS_Face bf;
    for (TopExp_Explorer ex(d->model->parts[part].shape, TopAbs_FACE); ex.More(); ex.Next()) {
      TopTools_IndexedMapOfShape m;
      TopExp::MapShapes(ex.Current(), s.ShapeType(), m);
      if (!m.Contains(s)) continue;
      const double dd = nearestOn(ex.Current(), hit).Distance(hit);
      if (dd < best) { best = dd; bf = TopoDS::Face(ex.Current()); }
    }
    if (bf.IsNull()) return;
    lockTo(a, *d, part, bf);
  } else lockTo(a, *d, part, TopoDS::Face(s));
  a.ctx->ClearDetected(false);
  requestFrames(a);
}

// ======================================================================== hover
static bool modDown(App& a, int k1, int k2) { return glfwGetKey(a.win, k1) == GLFW_PRESS || glfwGetKey(a.win, k2) == GLFW_PRESS; }

/** Best snap candidate near the cursor among a part's centres / face centres / midpoints. */
static bool snapNear(App& a, Doc& d, int part, const ImVec2& m, const std::optional<gp_Pnt>& hit, MEnt& out, const TopoDS_Face* onlyFace) {
  Features& f = F();
  PartSnaps& s = snapsOf(a, d, part);
  const gp_Trsf T = partTrsf(d, part);
  struct Cand { double score; MEnt e; };
  std::vector<Cand> cands;
  const gp_Dir vd = a.view->Camera()->Direction();
  const gp_Pnt eye = a.view->Camera()->Eye();
  Bnd_Box bb = d.model->box;
  double size = 100;
  if (!bb.IsVoid()) { double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1); size = std::max({x1 - x0, y1 - y0, z1 - z0}); }
  auto test = [&](const gp_Pnt& pl, float rad, MEnt e) {
    gp_Pnt p = pl.Transformed(T);
    ImVec2 sc;
    if (!toScreen(a, p, sc)) return;
    const double d2 = (sc.x - m.x) * (sc.x - m.x) + (sc.y - m.y) * (sc.y - m.y);
    if (d2 > rad * rad * a.scale * a.scale) return;
    if (hit && !onlyFace) {
      const double dp = gp_Vec(eye, p).Dot(gp_Vec(vd)), dh = gp_Vec(eye, *hit).Dot(gp_Vec(vd));
      if (dp > dh + size * 0.01) return; // hidden behind the surface under the cursor
    }
    e.p = p;
    e.part = part;
    cands.push_back({d2 / (rad * rad), e});
  };
  Bnd_Box fb;
  if (onlyFace) { BRepBndLib::Add(*onlyFace, fb); fb.Enlarge(size * 1e-4); }
  auto inFace = [&](const gp_Pnt& p) { return !onlyFace || (!fb.IsOut(p) && nearestOn(*onlyFace, p).Distance(p) < size * 1e-4 + 1e-7); };
  auto onBoundary = [&](const gp_Pnt& p) {
    if (!onlyFace) return true;
    if (fb.IsOut(p)) return false;
    for (TopExp_Explorer ex(*onlyFace, TopAbs_EDGE); ex.More(); ex.Next()) if (nearestOn(ex.Current(), p).Distance(p) < size * 1e-5 + 1e-7) return true;
    return false;
  };
  for (auto& c : s.centres) {
    if (!(f.show[2] || onlyFace) || (c.fillet && !f.show[5] && !onlyFace)) continue;
    if (onlyFace) {
      // centres of circles bounding this face (lie on its plane / axis)
      BRepAdaptor_Surface sf(*onlyFace, true);
      if (sf.GetType() == GeomAbs_Plane) { if (std::fabs(sf.Plane().Distance(c.p)) > size * 1e-5 + 1e-7) continue; if (fb.IsOut(c.p)) continue; }
      else continue;
    }
    MEnt e; e.k = MEnt::Point; e.kind = c.kind == 1 ? "sphere" : "centre"; e.r = c.r; e.fillet = c.fillet;
    test(c.p, 15, e);
  }
  for (auto& c : s.fcentres) {
    if (!(f.show[3] || onlyFace) || (c.fillet && !f.show[5] && !onlyFace)) continue;
    if (onlyFace && !inFace(c.p) && c.on.find("axis") == std::string::npos) continue;
    MEnt e; e.k = MEnt::Point; e.kind = "fcentre"; e.on = c.on;
    test(c.p, 12, e);
  }
  for (auto& p : s.mids) {
    if (!(f.show[1] || onlyFace)) continue;
    if (onlyFace && !onBoundary(p)) continue;
    MEnt e; e.k = MEnt::Point; e.kind = "mid";
    test(p, 9, e);
  }
  if (cands.empty()) return false;
  std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score < y.score; });
  out = cands[0].e;
  return true;
}

/** Axis of the part under the cursor whose screen projection passes near it. */
static bool axisNear(App& a, Doc& d, int part, const ImVec2& m, MEnt& out) {
  Features& f = F();
  if (!f.show[4]) return false;
  PartSnaps& s = snapsOf(a, d, part);
  const gp_Trsf T = partTrsf(d, part);
  double best = 6.0 * a.scale;
  const SnapAx* hit = nullptr;
  for (auto& A : s.axes) {
    if (A.fillet && !f.show[5]) continue;
    gp_Pnt p0 = A.a.Transformed(T), p1 = A.b.Transformed(T);
    const double L = p0.Distance(p1);
    if (L < 1e-9) continue;
    gp_Vec dir(p0, p1);
    dir.Normalize();
    const double ext = std::max(L * 0.15, 1e-3);
    ImVec2 s0, s1;
    if (!toScreen(a, p0.Translated(dir * -ext), s0) || !toScreen(a, p1.Translated(dir * ext), s1)) continue;
    const double dx = s1.x - s0.x, dy = s1.y - s0.y, L2 = dx * dx + dy * dy;
    if (L2 < 4) continue;
    const double t = std::max(0.0, std::min(1.0, ((m.x - s0.x) * dx + (m.y - s0.y) * dy) / L2));
    const double dd = std::hypot(s0.x + t * dx - m.x, s0.y + t * dy - m.y);
    if (dd < best) { best = dd; hit = &A; }
  }
  if (!hit) return false;
  MEnt e;
  e.k = MEnt::Axis; e.src = hit->src; e.r = hit->r; e.r0 = hit->r0; e.r1 = hit->r1; e.ang = hit->ang; e.R = hit->R; e.rr = hit->rr; e.fillet = hit->fillet;
  e.o = hit->a.Transformed(T);
  gp_Pnt b = hit->b.Transformed(T);
  e.len = e.o.Distance(b);
  e.d = gp_Dir(gp_Vec(e.o, b));
  e.click = e.mid();
  e.part = part;
  out = e;
  return true;
}

void measureAfterFlush(App& a) {
  Features& f = F();
  if (!f.measureOn) return;
  f.hasHover = false;
  f.hoverSnapped = f.hoverOnAxis = false;
  f.hoverLocked = !f.lockFilter.IsNull();
  Doc* d = activeDoc(a);
  ImGuiIO& io = ImGui::GetIO();
  if (!d || !d->displayed || io.WantCaptureMouse) return;
  const ImVec2 m = io.MousePos;
  const bool pointMode = f.mmode == 1 || modDown(a, GLFW_KEY_LEFT_CONTROL, GLFW_KEY_RIGHT_CONTROL);
  // what OCCT found under the cursor
  Handle(StdSelect_BRepOwner) bo;
  int part = -1;
  std::optional<gp_Pnt> hit;
  if (a.ctx->HasDetected()) {
    bo = Handle(StdSelect_BRepOwner)::DownCast(a.ctx->DetectedOwner());
    if (!bo.IsNull()) part = partOfObj(*d, bo->Selectable());
    const Handle(StdSelect_ViewerSelector3d)& sel = a.ctx->MainSelector();
    if (sel->NbPicked() > 0) hit = sel->PickedPoint(1);
  }
  if (part >= 0) f.axisPart = part;
  if (f.axisPart >= (int)d->prs.size() || (f.axisPart >= 0 && !a.ctx->IsDisplayed(d->prs[f.axisPart]))) f.axisPart = -1;

  if (f.hoverLocked) {
    MEnt e;
    if (snapNear(a, *d, f.lockPart, m, hit, e, &f.lockFace)) { f.hover = e; f.hasHover = f.hoverSnapped = true; a.ctx->ClearDetected(false); return; }
    if (part == f.lockPart && !bo.IsNull()) {
      const bool isFace = bo->Shape().ShapeType() == TopAbs_FACE;
      if (entityFromShape(a, *d, part, bo->Shape(), hit ? *hit : gp_Pnt(), pointMode, !isFace, e)) { f.hover = e; f.hasHover = true; f.hoverSnapped = !isFace; return; }
    }
    // cursor off the face: the face itself
    GProp_GProps g;
    BRepGProp::SurfaceProperties(f.lockFace, g);
    gp_Pnt c = g.CentreOfMass().Transformed(partTrsf(*d, f.lockPart));
    if (entityFromShape(a, *d, f.lockPart, f.lockFace, c, pointMode, false, e)) { f.hover = e; f.hasHover = true; }
    return;
  }
  if (f.snap && !pointMode && f.axisPart >= 0) {
    MEnt e;
    if (snapNear(a, *d, f.axisPart, m, hit, e, nullptr)) { f.hover = e; f.hasHover = f.hoverSnapped = true; a.ctx->ClearDetected(false); return; }
    if (f.mmode == 0 && axisNear(a, *d, f.axisPart, m, e)) { f.hover = e; f.hasHover = f.hoverOnAxis = true; a.ctx->ClearDetected(false); return; }
  }
  if (part < 0 || bo.IsNull()) return;
  const TopoDS_Shape& s = bo->Shape();
  if (!f.snap && s.ShapeType() != TopAbs_FACE && !pointMode) {
    // snapping off: faces only
  }
  MEnt e;
  if (entityFromShape(a, *d, part, s, hit ? *hit : gp_Pnt(), pointMode, false, e)) {
    f.hover = e;
    f.hasHover = true;
    f.hoverSnapped = s.ShapeType() != TopAbs_FACE;
  }
}

bool measurePick(App& a) {
  Features& f = F();
  if (!f.measureOn) return false;
  Doc* d = activeDoc(a);
  if (!d) return true;
  if (!f.hasHover) return true;
  if (!f.hasPending) {
    f.pending = f.hover;
    f.hasPending = true;
    // orange face for picked planes / cylinders
    if ((f.hover.k == MEnt::Plane || (f.hover.k == MEnt::Axis && f.hover.src != 0 && !f.hoverOnAxis)) && a.ctx->HasDetected()) {
      Handle(StdSelect_BRepOwner) bo = Handle(StdSelect_BRepOwner)::DownCast(a.ctx->DetectedOwner());
      if (!bo.IsNull() && bo->Shape().ShapeType() == TopAbs_FACE) f.pendingPrs = facePrs(a, *d, f.hover.part, TopoDS::Face(bo->Shape()), Quantity_Color(1.0, 0.62, 0.04, Quantity_TOC_sRGB), 0.45);
    } else if (f.hoverLocked && f.hover.k == MEnt::Plane) {
      f.pendingPrs = facePrs(a, *d, f.lockPart, f.lockFace, Quantity_Color(1.0, 0.62, 0.04, Quantity_TOC_sRGB), 0.45);
    }
    requestFrames(a);
    return true;
  }
  Measurement mm;
  mm.A = f.pending;
  mm.B = f.hover;
  mm.res = measureEntities(mm.A, mm.B);
  ext(*d).meas.push_back(mm);
  measureCancel(a);
  return true;
}

// ======================================================================== overlay drawing
static ImU32 kindCol(int cls) {
  switch (cls) {
    case 0: return IM_COL32(10, 132, 255, 255);
    case 1: return IM_COL32(94, 92, 230, 255);
    case 2: return IM_COL32(28, 31, 36, 255);
    case 3: return IM_COL32(255, 159, 10, 255);
    case 4: return IM_COL32(31, 184, 201, 255);
    case 5: return IM_COL32(255, 45, 146, 255);
    default: return IM_COL32(142, 142, 147, 255);
  }
}

static void dashed(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float w, float dash) {
  const float L = std::hypot(b.x - a.x, b.y - a.y);
  if (L < 1) return;
  const int n = (int)(L / (dash * 2)) + 1;
  for (int i = 0; i < n; i++) {
    float t0 = i * 2 * dash / L, t1 = std::min(1.f, (i * 2 + 1) * dash / L);
    if (t0 >= 1) break;
    dl->AddLine(ImVec2(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0), ImVec2(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1), c, w);
  }
}

static void drawAxis(App& a, ImDrawList* dl, const MEnt& e, ImU32 col, float w, bool dash) {
  const double ext = std::max(e.len * 0.15, 1e-3);
  ImVec2 s0, s1;
  if (!toScreen(a, e.o.Translated(gp_Vec(e.d) * -ext), s0) || !toScreen(a, e.o.Translated(gp_Vec(e.d) * (e.len + ext)), s1)) return;
  if (dash) dashed(dl, s0, s1, col, w, 5 * a.scale);
  else { dl->AddLine(s0, s1, col, w); dl->AddCircleFilled(s0, w * 1.2f, col); dl->AddCircleFilled(s1, w * 1.2f, col); }
}

static void glassLabel(App& a, ImDrawList* dl, ImVec2 c, const std::string& main, const std::string& sub, ImU32 accent, ImVec2* outMin = nullptr, ImVec2* outMax = nullptr) {
  ImFont* fb = ImGui::GetFont();
  const float s = a.scale;
  ImVec2 t1 = ImGui::CalcTextSize(main.c_str());
  ImVec2 t2 = sub.empty() ? ImVec2(0, 0) : ImVec2(ImGui::CalcTextSize(sub.c_str()).x * 0.82f, ImGui::GetFontSize() * 0.82f);
  const float w = std::max(t1.x, t2.x) + 20 * s, h = t1.y + (sub.empty() ? 0 : t2.y + 2 * s) + 12 * s;
  ImVec2 p0(c.x - w / 2, c.y - h / 2), p1(c.x + w / 2, c.y + h / 2);
  const bool night = a.night;
  dl->AddRectFilled(ImVec2(p0.x, p0.y + 3 * s), ImVec2(p1.x, p1.y + 3 * s), IM_COL32(0, 0, 0, night ? 70 : 26), 11 * s);
  dl->AddRectFilled(p0, p1, night ? IM_COL32(36, 40, 50, 236) : IM_COL32(255, 255, 255, 236), 11 * s);
  dl->AddRect(p0, p1, night ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 255), 11 * s);
  dl->AddText(ImVec2(c.x - t1.x / 2, p0.y + 6 * s), accent, main.c_str());
  if (!sub.empty()) dl->AddText(fb, ImGui::GetFontSize() * 0.82f, ImVec2(c.x - t2.x / 2, p0.y + 6 * s + t1.y + 1 * s), night ? IM_COL32(170, 176, 188, 255) : IM_COL32(110, 112, 120, 255), sub.c_str());
  if (outMin) *outMin = p0;
  if (outMax) *outMax = p1;
}

void measureDrawOverlay(App& a) {
  Features& f = F();
  Doc* d = activeDoc(a);
  if (!d || !d->displayed) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const float s = a.scale;
  const ImU32 accent = IM_COL32(10, 132, 255, 255);
  DocExt& x = ext(*d);
  // --- measurements
  for (size_t i = 0; i < x.meas.size(); i++) {
    Measurement& mm = x.meas[i];
    const MRes& r = mm.res;
    for (const MEnt* e : {&mm.A, &mm.B}) if (e->k == MEnt::Axis) drawAxis(a, dl, *e, IM_COL32(10, 132, 255, 150), 1.4f * s, true);
    ImVec2 sa, sb;
    if (r.hasLine && !r.angle && toScreen(a, r.a, sa) && toScreen(a, r.b, sb)) {
      dl->AddLine(sa, sb, accent, 2.0f * s);
      for (ImVec2 q : {sa, sb}) { dl->AddCircleFilled(q, 4.5f * s, IM_COL32(255, 255, 255, 255)); dl->AddCircleFilled(q, 3.0f * s, accent); }
      if (f.xyz) {
        gp_Pnt p = r.a;
        const double dv[3] = {r.b.X() - r.a.X(), r.b.Y() - r.a.Y(), r.b.Z() - r.a.Z()};
        const ImU32 lc[3] = {IM_COL32(255, 69, 58, 255), IM_COL32(40, 184, 76, 255), IM_COL32(10, 132, 255, 255)};
        const char* ax[3] = {"X", "Y", "Z"};
        for (int k = 0; k < 3; k++) {
          if (std::fabs(dv[k]) < 1e-9) continue;
          gp_Pnt q = p;
          q.SetCoord(k + 1, q.Coord(k + 1) + dv[k]);
          ImVec2 s0, s1;
          if (toScreen(a, p, s0) && toScreen(a, q, s1)) {
            dashed(dl, s0, s1, lc[k], 1.6f * s, 4 * s);
            std::string t = std::string(ax[k]) + " " + fLen(std::fabs(dv[k]));
            ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            ImVec2 c((s0.x + s1.x) / 2, (s0.y + s1.y) / 2);
            dl->AddRectFilled(ImVec2(c.x - ts.x * 0.45f - 5 * s, c.y - ts.y * 0.45f - 2 * s), ImVec2(c.x + ts.x * 0.45f + 5 * s, c.y + ts.y * 0.45f + 2 * s), lc[k], 6 * s);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.82f, ImVec2(c.x - ts.x * 0.41f, c.y - ts.y * 0.41f), IM_COL32(255, 255, 255, 255), t.c_str());
          }
          p = q;
        }
      }
    }
    // label (draggable: an invisible ImGui window catches the mouse)
    gp_Pnt at = mm.hasLabelPos ? mm.labelPos : r.anchor;
    ImVec2 c;
    if (!toScreen(a, at, c)) continue;
    if (!mm.hasLabelPos) c.y -= 26 * s;
    else {
      ImVec2 an;
      if (toScreen(a, r.anchor, an)) dl->AddLine(an, c, IM_COL32(142, 142, 147, 200), 1.2f * s);
    }
    ImVec2 p0, p1;
    glassLabel(a, dl, c, r.main, r.kind, accent, &p0, &p1);
    char id[32];
    snprintf(id, sizeof id, "##ml%zu", i);
    ImGui::SetNextWindowPos(p0);
    ImGui::SetNextWindowSize(ImVec2(p1.x - p0.x, p1.y - p0.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::InvisibleButton("##drag", ImVec2(p1.x - p0.x, p1.y - p0.y));
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.f)) {
      ImVec2 dd = ImGui::GetIO().MouseDelta;
      const double wpp = worldPerPixel(a);
      Handle(Graphic3d_Camera) cam = a.view->Camera();
      gp_Vec side = gp_Vec(cam->Direction()).Crossed(gp_Vec(cam->Up()));
      if (!mm.hasLabelPos) {
        // start from where the label is drawn (above its anchor)
        mm.labelPos = r.anchor.Translated(gp_Vec(cam->Up()) * (26 * s * wpp));
        mm.hasLabelPos = true;
      }
      mm.labelPos.Translate(side.Normalized() * (dd.x * wpp) + gp_Vec(cam->Up()) * (-dd.y * wpp));
      requestFrames(a);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) mm.hasLabelPos = false;
    ImGui::End();
    ImGui::PopStyleVar();
  }
  if (!f.measureOn) return;
  // --- axes of the part under the cursor
  if (f.axisPart >= 0 && f.mmode == 0 && f.show[4] && f.lockFilter.IsNull()) {
    PartSnaps& S = snapsOf(a, *d, f.axisPart);
    const gp_Trsf T = partTrsf(*d, f.axisPart);
    for (auto& A : S.axes) {
      if (A.fillet && !f.show[5]) continue;
      MEnt e;
      e.o = A.a.Transformed(T);
      gp_Pnt b = A.b.Transformed(T);
      e.len = e.o.Distance(b);
      if (e.len < 1e-9) continue;
      e.d = gp_Dir(gp_Vec(e.o, b));
      drawAxis(a, dl, e, IM_COL32(94, 92, 230, 120), 1.2f * s, true);
    }
  }
  // --- pending pick
  if (f.hasPending) {
    const ImU32 orange = IM_COL32(255, 159, 10, 255);
    if (f.pending.k == MEnt::Axis) drawAxis(a, dl, f.pending, orange, 3.0f * s, false);
    else if (f.pending.k == MEnt::Point) { ImVec2 q; if (toScreen(a, f.pending.p, q)) { dl->AddCircleFilled(q, 6 * s, IM_COL32(255, 255, 255, 255)); dl->AddCircleFilled(q, 4.5f * s, orange); } }
  }
  // --- hover feedback
  ImGuiIO& io = ImGui::GetIO();
  if (f.hasHover && !io.WantCaptureMouse) {
    int cls = 6;
    std::string det;
    std::string type = describeType(f.hover, &det, &cls);
    const ImU32 kc = kindCol(cls);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (f.hover.k == MEnt::Axis && (f.hoverOnAxis || f.hover.src != 0)) drawAxis(a, fg, f.hover, IM_COL32(94, 92, 230, 255), 3.0f * s, false);
    if (f.hover.k == MEnt::Point) {
      ImVec2 q;
      if (toScreen(a, f.hover.p, q)) {
        const float r = (cls == 3 ? 10.f : cls == 2 ? 7.f : 6.f) * s;
        if (cls == 3) { fg->AddCircle(q, r, IM_COL32(255, 255, 255, 230), 32, 4.f * s); fg->AddCircle(q, r, kc, 32, 2.2f * s); fg->AddCircleFilled(q, 2.5f * s, kc); }
        else if (cls == 2) { fg->AddRectFilled(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r, q.y + r), IM_COL32(255, 255, 255, 255), 3 * s); fg->AddRect(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r, q.y + r), kc, 3 * s, 0, 2.2f * s); }
        else { fg->AddCircleFilled(q, r, IM_COL32(255, 255, 255, 255)); fg->AddCircleFilled(q, r - 2 * s, kc); }
      }
    }
    // chip next to the cursor
    std::string res;
    if (f.hasPending) res = "→ " + measureEntities(f.pending, f.hover).main;
    ImVec2 mp = io.MousePos;
    ImVec2 p(mp.x + 16 * s, mp.y + 18 * s);
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 0.9f;
    ImVec2 tt = font->CalcTextSizeA(fs, 1e9f, 0, type.c_str());
    ImVec2 td = det.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(fs, 1e9f, 0, det.c_str());
    ImVec2 tl = f.hoverLocked ? font->CalcTextSizeA(fs * 0.9f, 1e9f, 0, "this face") : ImVec2(0, 0);
    ImVec2 tr = res.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(fs, 1e9f, 0, res.c_str());
    const float pad = 7 * s, gap = 7 * s;
    float w = pad + tt.x + 12 * s + (det.empty() ? 0 : gap + td.x) + (f.hoverLocked ? gap + tl.x + 10 * s : 0) + (res.empty() ? 0 : gap + tr.x) + pad;
    float h = tt.y + 12 * s;
    fg->AddRectFilled(ImVec2(p.x, p.y + 3 * s), ImVec2(p.x + w, p.y + h + 3 * s), IM_COL32(0, 0, 0, a.night ? 70 : 24), 10 * s);
    fg->AddRectFilled(p, ImVec2(p.x + w, p.y + h), a.night ? IM_COL32(36, 40, 50, 240) : IM_COL32(255, 255, 255, 240), 10 * s);
    float x0 = p.x + pad;
    fg->AddRectFilled(ImVec2(x0 - 1 * s, p.y + 4 * s), ImVec2(x0 + tt.x + 11 * s, p.y + h - 4 * s), kc, 6 * s);
    fg->AddText(font, fs, ImVec2(x0 + 5 * s, p.y + (h - tt.y) / 2), cls == 2 && !a.night ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 255), type.c_str());
    x0 += tt.x + 12 * s;
    const ImU32 fg2 = a.night ? IM_COL32(220, 224, 232, 255) : IM_COL32(40, 42, 48, 255);
    if (!det.empty()) { x0 += gap; fg->AddText(font, fs, ImVec2(x0, p.y + (h - td.y) / 2), fg2, det.c_str()); x0 += td.x; }
    if (f.hoverLocked) { x0 += gap; fg->AddRect(ImVec2(x0 - 2 * s, p.y + 5 * s), ImVec2(x0 + tl.x + 8 * s, p.y + h - 5 * s), IM_COL32(142, 142, 147, 200), 5 * s); fg->AddText(font, fs * 0.9f, ImVec2(x0 + 3 * s, p.y + (h - tl.y) / 2), IM_COL32(142, 142, 147, 255), "this face"); x0 += tl.x + 10 * s; }
    if (!res.empty()) { x0 += gap; fg->AddText(font, fs, ImVec2(x0, p.y + (h - tr.y) / 2), accent, res.c_str()); }
    // rubber band
    if (f.hasPending) {
      ImVec2 q0, q1;
      gp_Pnt b = f.hover.k == MEnt::Point ? f.hover.p : f.hoverOnAxis ? f.hover.mid() : f.hover.click;
      if (toScreen(a, f.pending.anchor(), q0) && toScreen(a, b, q1)) dashed(fg, q0, q1, accent, 1.5f * s, 5 * s);
    }
  }
}
