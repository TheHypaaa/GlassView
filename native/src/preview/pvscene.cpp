#include "pvscene.hpp"

#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <cmath>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <GL/gl.h>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>

namespace pv {

static void rgb(const Quantity_Color& c, unsigned char out[3]) {
  double r, g, b;
  c.Values(r, g, b, Quantity_TOC_sRGB);
  out[0] = (unsigned char)std::lround(std::fmin(1.0, std::fmax(0.0, r)) * 255);
  out[1] = (unsigned char)std::lround(std::fmin(1.0, std::fmax(0.0, g)) * 255);
  out[2] = (unsigned char)std::lround(std::fmin(1.0, std::fmax(0.0, b)) * 255);
}

void build(const gv::Model& m, Scene& s) {
  s = Scene();
  s.name = m.name;
  s.parts = (int)m.parts.size();
  Bnd_Box box;
  for (const gv::Part& p : m.parts) {
    const gp_Trsf T = p.loc.Transformation();
    unsigned char base[3];
    rgb(p.hasColor ? p.color : Quantity_Color(0.62, 0.68, 0.76, Quantity_TOC_sRGB), base);
    // face colours from the file
    // face colour: the face's own colour wins, else the smallest shell / solid that contains it
    auto faceCol = [&](const TopoDS_Face& f, unsigned char out[3]) {
      int best = -1;
      const Quantity_Color* bc = nullptr;
      for (auto& sc : p.subColors) {
        const TopAbs_ShapeEnum t = sc.first.ShapeType();
        if (t == TopAbs_FACE) { if (sc.first.IsSame(f)) { rgb(sc.second, out); return; } continue; }
        if (t > TopAbs_FACE || (int)t <= best) continue;
        for (TopExp_Explorer e(sc.first, TopAbs_FACE); e.More(); e.Next())
          if (e.Current().IsSame(f)) { best = (int)t; bc = &sc.second; break; }
      }
      if (bc) { rgb(*bc, out); return; }
      out[0] = base[0]; out[1] = base[1]; out[2] = base[2];
    };
    for (TopExp_Explorer ex(p.shape, TopAbs_FACE); ex.More(); ex.Next()) {
      const TopoDS_Face& fc = TopoDS::Face(ex.Current());
      TopLoc_Location l;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(fc, l);
      if (t.IsNull()) continue;
      if (!t->HasNormals()) { try { BRepLib_ToolTriangulatedShape::ComputeNormals(fc, t); } catch (...) {} }
      const gp_Trsf M = T * l.Transformation();
      const bool rev = fc.Orientation() == TopAbs_REVERSED;
      unsigned char fcol[3];
      if (p.subColors.empty()) { fcol[0] = base[0]; fcol[1] = base[1]; fcol[2] = base[2]; } else faceCol(fc, fcol);
      for (int i = 1; i <= t->NbTriangles(); i++) {
        int n[3];
        t->Triangle(i).Get(n[0], n[1], n[2]);
        if (rev) std::swap(n[1], n[2]);
        for (int k = 0; k < 3; k++) {
          const gp_Pnt q = t->Node(n[k]).Transformed(M);
          gp_Dir d = t->HasNormals() ? t->Normal(n[k]) : gp_Dir(0, 0, 1);
          if (rev) d.Reverse();
          d.Transform(M);
          s.pos.insert(s.pos.end(), {(float)q.X(), (float)q.Y(), (float)q.Z()});
          s.nrm.insert(s.nrm.end(), {(float)d.X(), (float)d.Y(), (float)d.Z()});
          s.col.insert(s.col.end(), {fcol[0], fcol[1], fcol[2]});
          box.Add(q);
        }
      }
      s.tris += (size_t)t->NbTriangles();
    }
    // edges: face boundaries on the triangulation; tangent ones are "soft"
    if (p.isMesh) continue;
    TopTools_IndexedMapOfShape softMap;
    if (p.hasSoft) TopExp::MapShapes(p.softEdges, TopAbs_EDGE, softMap);
    TopTools_IndexedDataMapOfShapeListOfShape e2f;
    TopExp::MapShapesAndAncestors(p.shape, TopAbs_EDGE, TopAbs_FACE, e2f);
    for (int i = 1; i <= e2f.Extent(); i++) {
      const TopoDS_Edge& e = TopoDS::Edge(e2f.FindKey(i));
      if (BRep_Tool::Degenerated(e) || e2f(i).IsEmpty()) continue;
      const TopoDS_Face& f = TopoDS::Face(e2f(i).First());
      TopLoc_Location l;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(f, l);
      if (t.IsNull()) continue;
      Handle(Poly_PolygonOnTriangulation) pol = BRep_Tool::PolygonOnTriangulation(e, t, l);
      if (pol.IsNull()) continue;
      const gp_Trsf M = T * l.Transformation();
      std::vector<float>& out = softMap.Contains(e) ? s.soft : s.hard;
      for (int k = 1; k < pol->NbNodes(); k++) {
        const gp_Pnt a = t->Node(pol->Node(k)).Transformed(M), b = t->Node(pol->Node(k + 1)).Transformed(M);
        out.insert(out.end(), {(float)a.X(), (float)a.Y(), (float)a.Z(), (float)b.X(), (float)b.Y(), (float)b.Z()});
      }
    }
  }
  if (box.IsVoid()) { s.error = "No geometry"; return; }
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  s.c[0] = (float)(x0 + x1) / 2; s.c[1] = (float)(y0 + y1) / 2; s.c[2] = (float)(z0 + z1) / 2;
  s.size[0] = (float)(x1 - x0); s.size[1] = (float)(y1 - y0); s.size[2] = (float)(z1 - z0);
  s.radius = 0.5f * std::sqrt(s.size[0] * s.size[0] + s.size[1] * s.size[1] + s.size[2] * s.size[2]);
  if (s.radius <= 0) s.radius = 1;
  s.ready = true;
}

bool View::mouseMove(int x, int y) {
  const int dx = x - lx, dy = y - ly;
  lx = x; ly = y;
  if (rot) {
    // like three.js OrbitControls: a drag across the full height turns 360° × rotateSpeed
    const float k = 6.2831853f * rotateSpeed / (float)(h > 0 ? h : 1);
    yaw -= dx * k;
    pitch += dy * k;
    if (pitch > 1.55f) pitch = 1.55f;
    if (pitch < -1.55f) pitch = -1.55f;
    return true;
  }
  if (pan) {
    const float k = 2.0f / (float)(h > 0 ? h : 1);
    panX += dx * k;
    panY -= dy * k;
    return true;
  }
  return false;
}

void View::wheel(float notches, int x, int y) {
  // OrbitControls: scale 0.95^(zoomSpeed × |deltaY| / 100) per event, a notch being deltaY 100
  if (reverseWheel) notches = -notches;
  const float z0 = zoom;
  float z1 = zoom * std::pow(0.95f, -zoomSpeed * notches);
  z1 = z1 < 0.05f ? 0.05f : z1 > 200 ? 200 : z1;
  // keep the point under the cursor where it is
  const float aspect = (float)w / (float)(h > 0 ? h : 1);
  const float ax = aspect >= 1 ? aspect : 1, ay = aspect >= 1 ? 1 : 1 / aspect;  // half extents / ux
  const float nx = 2.0f * x / (float)(w > 0 ? w : 1) - 1, ny = 1 - 2.0f * y / (float)(h > 0 ? h : 1);
  const float f = z0 / z1;  // new extent / old extent
  panX = (panX + nx * ax * (f - 1)) / f;
  panY = (panY + ny * ay * (f - 1)) / f;
  zoom = z1;
}

void render(const Scene& s, const View& v) {
  glViewport(0, 0, v.w, v.h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_LIGHTING);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  // background: Explorer's own preview-pane colour, flat, so the model sits in the pane like a picture
  glClearColor(v.bg[0] / 255.0f, v.bg[1] / 255.0f, v.bg[2] / 255.0f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!s.ready || s.pos.empty()) return;

  // camera: orthographic, orbit around the model centre (Z up)
  const float aspect = (float)v.w / (float)(v.h > 0 ? v.h : 1);
  const float r = s.radius * 1.12f / v.zoom;  // some air around the model, like a thumbnail
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  float hw = r, hh = r;
  if (aspect >= 1) hw = r * aspect; else hh = r / aspect;
  const float ux = (aspect >= 1 ? hh : hw);  // world units per half-height → pan in screen terms
  glOrtho(-hw - v.panX * ux, hw - v.panX * ux, -hh - v.panY * ux, hh - v.panY * ux, -s.radius * 4, s.radius * 4);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  // headlight in eye space
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glEnable(GL_LIGHT1);
  const float l0[4] = {0.25f, 0.35f, 1, 0}, d0[4] = {0.62f, 0.62f, 0.62f, 1}, a0[4] = {0.46f, 0.46f, 0.47f, 1}, s0[4] = {0.18f, 0.18f, 0.18f, 1};
  const float l1[4] = {-0.4f, 0.9f, 0.3f, 0}, d1[4] = {0.18f, 0.18f, 0.19f, 1}, z[4] = {0, 0, 0, 1};
  glLightfv(GL_LIGHT0, GL_POSITION, l0);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, d0);
  glLightfv(GL_LIGHT0, GL_AMBIENT, a0);
  glLightfv(GL_LIGHT0, GL_SPECULAR, s0);
  glLightfv(GL_LIGHT1, GL_POSITION, l1);
  glLightfv(GL_LIGHT1, GL_DIFFUSE, d1);
  glLightfv(GL_LIGHT1, GL_AMBIENT, z);
  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
  const float spec[4] = {0.35f, 0.35f, 0.35f, 1};
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 40);
  glEnable(GL_COLOR_MATERIAL);
  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
  glEnable(GL_NORMALIZE);
  // view rotation: Z up, looking from (yaw, pitch)
  glRotatef(-90, 1, 0, 0);
  glRotatef(v.pitch * 57.29578f, 1, 0, 0);
  glRotatef(-v.yaw * 57.29578f - 90, 0, 0, 1);
  glTranslatef(-s.c[0], -s.c[1], -s.c[2]);

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.0f, 1.0f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_NORMAL_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, s.pos.data());
  glNormalPointer(GL_FLOAT, 0, s.nrm.data());
  glColorPointer(3, GL_UNSIGNED_BYTE, 0, s.col.data());
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(s.pos.size() / 3));
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_NORMAL_ARRAY);
  glDisable(GL_POLYGON_OFFSET_FILL);

  // edges: sharp dark, tangent light
  glDisable(GL_LIGHTING);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_LINE_SMOOTH);
  glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
  if (!s.soft.empty()) {
    glLineWidth(1.0f);
    if (v.dark) glColor4ub(0, 0, 0, 70); else glColor4ub(20, 24, 32, 55);  // tangent edges: a faint darkening
    glVertexPointer(3, GL_FLOAT, 0, s.soft.data());
    glDrawArrays(GL_LINES, 0, (GLsizei)(s.soft.size() / 3));
  }
  if (!s.hard.empty()) {
    glLineWidth(1.2f);
    if (v.dark) glColor4ub(8, 9, 12, 220); else glColor4ub(28, 31, 38, 210);
    glVertexPointer(3, GL_FLOAT, 0, s.hard.data());
    glDrawArrays(GL_LINES, 0, (GLsizei)(s.hard.size() / 3));
  }
  glDisableClientState(GL_VERTEX_ARRAY);
  glDisable(GL_LINE_SMOOTH);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
}

} // namespace pv
