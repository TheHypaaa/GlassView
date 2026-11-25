#include "model.hpp"

#include <STEPCAFControl_Reader.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_LabelSequence.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <IMeshTools_Parameters.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <BRepLib.hxx>
#include <BRepBndLib.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <Poly_Triangulation.hxx>
#include <RWStl.hxx>
#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_AsciiString.hxx>

#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace gv {

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::string fileNameOf(const std::string& p) {
  size_t s = p.find_last_of("/\\");
  return s == std::string::npos ? p : p.substr(s + 1);
}
std::string lowerExt(const std::string& p) {
  size_t d = p.find_last_of('.');
  if (d == std::string::npos) return "";
  std::string e = p.substr(d + 1);
  for (auto& c : e) c = (char)tolower((unsigned char)c);
  return e;
}
bool isSupported(const std::string& p) {
  std::string e = lowerExt(p);
  return e == "step" || e == "stp" || e == "iges" || e == "igs" || e == "brep" || e == "brp" || e == "stl";
}

// ------------------------------------------------------------------ XCAF walk
static std::string labelName(const TDF_Label& l) {
  Handle(TDataStd_Name) n;
  if (l.FindAttribute(TDataStd_Name::GetID(), n)) {
    TCollection_AsciiString a(n->Get(), '?');
    // TCollection_AsciiString(ExtendedString) keeps only ASCII – rebuild as UTF-8
    const TCollection_ExtendedString& w = n->Get();
    std::string out;
    for (int i = 1; i <= w.Length(); i++) {
      unsigned c = w.Value(i);
      if (c < 0x80) out += (char)c;
      else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
      else { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
  }
  return "";
}

static bool labelColor(const Handle(XCAFDoc_ColorTool)& ct, const TDF_Label& l, Quantity_Color& c) {
  return ct->GetColor(l, XCAFDoc_ColorSurf, c) || ct->GetColor(l, XCAFDoc_ColorGen, c);
}

struct Walk {
  Model& m;
  Handle(XCAFDoc_ShapeTool) st;
  Handle(XCAFDoc_ColorTool) ct;
};

static void visit(Walk& w, const TDF_Label& label, const TopLoc_Location& parentLoc, Node& parent, bool inhColor, const Quantity_Color& inh) {
  TDF_Label ref = label;
  TopLoc_Location loc = parentLoc;
  if (w.st->IsReference(label)) {
    w.st->GetReferredShape(label, ref);
    loc = parentLoc * w.st->GetLocation(label);
  }
  std::string name = labelName(label);
  if (name.empty()) name = labelName(ref);
  Quantity_Color c = inh;
  bool hc = inhColor;
  Quantity_Color c1;
  if (labelColor(w.ct, ref, c1)) { c = c1; hc = true; }
  if (labelColor(w.ct, label, c1)) { c = c1; hc = true; }

  if (w.st->IsAssembly(ref)) {
    auto node = std::make_unique<Node>();
    node->name = name.empty() ? "Assembly" : name;
    node->parent = &parent;
    TDF_LabelSequence comps;
    w.st->GetComponents(ref, comps);
    for (int i = 1; i <= comps.Length(); i++) visit(w, comps.Value(i), loc, *node, hc, c);
    if (!node->kids.empty() || !node->parts.empty()) parent.kids.push_back(std::move(node));
  } else {
    TopoDS_Shape shape = w.st->GetShape(ref);
    if (shape.IsNull()) return;
    bool hasFace = TopExp_Explorer(shape, TopAbs_FACE).More();
    bool hasEdge = TopExp_Explorer(shape, TopAbs_EDGE).More();
    if (!hasFace && !hasEdge) return;
    Part p;
    p.name = name.empty() ? "Body" : name;
    p.shape = shape;
    // colours stored on sub-shapes (faces, shells, solids) of the part
    TDF_LabelSequence subs;
    w.st->GetSubShapes(ref, subs);
    for (int i = 1; i <= subs.Length(); i++) {
      Quantity_Color sc;
      if (!labelColor(w.ct, subs.Value(i), sc)) continue;
      TopoDS_Shape ss = w.st->GetShape(subs.Value(i));
      if (!ss.IsNull()) p.subColors.emplace_back(ss, sc);
    }

    p.loc = loc;
    p.hasColor = hc;
    p.color = c;
    auto node = std::make_unique<Node>();
    node->name = p.name;
    node->parent = &parent;
    node->parts.push_back((int)w.m.parts.size());
    w.m.parts.push_back(std::move(p));
    parent.kids.push_back(std::move(node));
  }
}

// ------------------------------------------------------------------ STL
/** STL → one face carrying a triangulation with crease-angle normals (30°). */
static TopoDS_Shape readStl(const std::string& path, size_t& tris) {
  Handle(Poly_Triangulation) src = RWStl::ReadFile(path.c_str());
  if (src.IsNull() || src->NbTriangles() == 0) return TopoDS_Shape();
  const int nT = src->NbTriangles(), nN = src->NbNodes();
  tris = (size_t)nT;
  std::vector<gp_Vec> fn(nT);
  std::vector<std::vector<int>> nodeTris(nN);
  for (int t = 1; t <= nT; t++) {
    int a, b, c;
    src->Triangle(t).Get(a, b, c);
    gp_Vec n = gp_Vec(src->Node(a), src->Node(b)).Crossed(gp_Vec(src->Node(a), src->Node(c)));
    double l = n.Magnitude();
    fn[t - 1] = l > 1e-300 ? n / l : gp_Vec(0, 0, 1);
    nodeTris[a - 1].push_back(t - 1);
    nodeTris[b - 1].push_back(t - 1);
    nodeTris[c - 1].push_back(t - 1);
  }
  Handle(Poly_Triangulation) out = new Poly_Triangulation(nT * 3, nT, false, true);
  const double cosCrease = std::cos(30.0 * M_PI / 180.0);
  for (int t = 1; t <= nT; t++) {
    int v[3];
    src->Triangle(t).Get(v[0], v[1], v[2]);
    for (int k = 0; k < 3; k++) {
      const int idx = (t - 1) * 3 + k + 1;
      out->SetNode(idx, src->Node(v[k]));
      gp_Vec sum(0, 0, 0);
      for (int o : nodeTris[v[k] - 1]) if (fn[o].Dot(fn[t - 1]) >= cosCrease) sum += fn[o];
      if (sum.Magnitude() < 1e-12) sum = fn[t - 1];
      sum.Normalize();
      out->SetNormal(idx, gp_Dir(sum));
    }
    out->SetTriangle(t, Poly_Triangle((t - 1) * 3 + 1, (t - 1) * 3 + 2, (t - 1) * 3 + 3));
  }
  TopoDS_Face f;
  BRep_Builder b;
  b.MakeFace(f, out);
  return f;
}

// ------------------------------------------------------------------ main entry
std::shared_ptr<Model> loadModel(const std::string& path, Quality q, LoadProgress& prog) {
  static std::mutex appMutex;
  auto t0 = Clock::now();
  auto m = std::make_shared<Model>();
  m->path = path;
  m->name = fileNameOf(path);
  m->root = std::make_unique<Node>();
  m->root->name = m->name;
  const std::string ext = lowerExt(path);
  try {
    prog.stage = 0;
    if (ext == "stl") {
      size_t tris = 0;
      TopoDS_Shape s = readStl(path, tris);
      if (s.IsNull()) { m->error = "Could not read the STL file"; return m; }
      Part p;
      p.name = m->name.substr(0, m->name.find_last_of('.'));
      p.shape = s;
      p.isMesh = true;
      p.tris = tris;
      auto node = std::make_unique<Node>();
      node->name = p.name;
      node->parent = m->root.get();
      node->parts.push_back(0);
      m->parts.push_back(p);
      m->root->kids.push_back(std::move(node));
      m->tris = tris;
      m->readMs = msSince(t0);
    } else if (getenv("GV_NOCACHE") == nullptr && cacheLoad(path, q, *m)) {
      m->fromCache = true;
      m->readMs = msSince(t0);
    } else {
      {
        std::lock_guard<std::mutex> lk(appMutex);
        XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", m->doc);
      }
      bool ok = false;
      if (ext == "step" || ext == "stp") {
        STEPCAFControl_Reader r;
        r.SetColorMode(true); r.SetNameMode(true); r.SetLayerMode(false); r.SetGDTMode(true);
        if (r.ReadFile(path.c_str()) == IFSelect_RetDone) ok = r.Transfer(m->doc);
      } else if (ext == "iges" || ext == "igs") {
        IGESCAFControl_Reader r;
        r.SetColorMode(true); r.SetNameMode(true);
        if (r.ReadFile(path.c_str()) == IFSelect_RetDone) ok = r.Transfer(m->doc);
      } else if (ext == "brep" || ext == "brp") {
        TopoDS_Shape s;
        BRep_Builder b;
        if (BRepTools::Read(s, path.c_str(), b)) { XCAFDoc_DocumentTool::ShapeTool(m->doc->Main())->AddShape(s); ok = true; }
      }
      if (!ok) { m->error = "Could not read the file"; return m; }
      m->readMs = msSince(t0);
      Walk w{*m, XCAFDoc_DocumentTool::ShapeTool(m->doc->Main()), XCAFDoc_DocumentTool::ColorTool(m->doc->Main())};
      TDF_LabelSequence free;
      w.st->GetFreeShapes(free);
      Quantity_Color none;
      for (int i = 1; i <= free.Length(); i++) visit(w, free.Value(i), TopLoc_Location(), *m->root, false, none);
      // a single top-level assembly becomes the root itself
      if (m->root->kids.size() == 1 && m->root->parts.empty() && !m->root->kids[0]->kids.empty()) {
        auto only = std::move(m->root->kids[0]);
        only->parent = nullptr;
        for (auto& k : only->kids) k->parent = only.get();
        m->root = std::move(only);
      }
      if (m->parts.empty()) { m->error = "No geometry found in the file"; return m; }
      if (prog.cancel) return m;
      try { readPmi(*m); } catch (...) {}

      // ---- tessellate unique shapes in parallel
      prog.stage = 1;
      auto t1 = Clock::now();
      Bnd_Box bb;
      for (auto& p : m->parts) BRepBndLib::Add(p.shape.Moved(p.loc), bb);
      double avg = 1;
      if (!bb.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        avg = std::max(1e-6, ((x1 - x0) + (y1 - y0) + (z1 - z0)) / 3.0);
      }
      static const double kLin[] = {0.003, 0.001, 0.0004, 0.00015};
      static const double kAng[] = {0.5, 0.35, 0.25, 0.15};
      TopTools_IndexedMapOfShape uniq;
      for (auto& p : m->parts) uniq.Add(p.shape);
      TopoDS_Compound comp;
      BRep_Builder b;
      b.MakeCompound(comp);
      for (int i = 1; i <= uniq.Extent(); i++) b.Add(comp, uniq(i));
      IMeshTools_Parameters prm;
      prm.Deflection = avg * kLin[(int)q];
      prm.Angle = kAng[(int)q];
      prm.InParallel = true;
      prm.Relative = false;
      prm.MinSize = prm.Deflection * 0.1;
      BRepMesh_IncrementalMesh mesher(comp, prm);
      m->meshMs = msSince(t1);
      if (prog.cancel) return m;

      // ---- sharp vs tangent edges: encode continuity, collect smooth edges per unique shape
      prog.stage = 2;
      std::vector<TopoDS_Compound> soft(uniq.Extent());
      std::vector<char> hasSoft(uniq.Extent(), 0);
      std::vector<size_t> triCount(uniq.Extent(), 0);
      OSD_Parallel::For(0, uniq.Extent(), [&](int i) {
        const TopoDS_Shape& s = uniq(i + 1);
        try { BRepLib::EncodeRegularity(s, 1.0 * M_PI / 180.0); } catch (...) {}
        BRep_Builder bb2;
        bb2.MakeCompound(soft[i]);
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(s, TopAbs_EDGE, edges);
        for (int e = 1; e <= edges.Extent(); e++) {
          const TopoDS_Edge& ed = TopoDS::Edge(edges(e));
          if (BRep_Tool::Degenerated(ed)) continue;
          if (BRep_Tool::MaxContinuity(ed) >= GeomAbs_G1) { bb2.Add(soft[i], ed); hasSoft[i] = 1; }
        }
        size_t n = 0;
        for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
          TopLoc_Location l;
          Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), l);
          if (!t.IsNull()) n += (size_t)t->NbTriangles();
        }
        triCount[i] = n;
      });
      for (auto& p : m->parts) {
        int i = uniq.FindIndex(p.shape) - 1;
        p.softEdges = soft[i];
        p.hasSoft = hasSoft[i];
        p.tris = triCount[i];
        m->tris += p.tris;
      }
    }
    for (auto& p : m->parts) BRepBndLib::Add(p.shape.Moved(p.loc), m->box, true);
  } catch (Standard_Failure& e) {
    m->error = std::string("OpenCASCADE error: ") + (e.GetMessageString() ? e.GetMessageString() : "?");
  } catch (std::exception& e) {
    m->error = e.what();
  }
  prog.stage = 3;
  m->totalMs = msSince(t0);
  if (!m->fromCache && ext != "stl" && !prog.cancel && getenv("GV_NOCACHE") == nullptr) cacheSave(path, q, *m);
  return m;
}


size_t remesh(Model& m, Quality q) {
  static const double kLin[] = {0.003, 0.001, 0.0004, 0.00015};
  static const double kAng[] = {0.5, 0.35, 0.25, 0.15};
  double avg = 1;
  if (!m.box.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    m.box.Get(x0, y0, z0, x1, y1, z1);
    avg = std::max(1e-6, ((x1 - x0) + (y1 - y0) + (z1 - z0)) / 3.0);
  }
  TopTools_IndexedMapOfShape uniq;
  for (auto& p : m.parts) if (!p.isMesh) uniq.Add(p.shape);
  TopoDS_Compound comp;
  BRep_Builder b;
  b.MakeCompound(comp);
  for (int i = 1; i <= uniq.Extent(); i++) { BRepTools::Clean(uniq(i)); b.Add(comp, uniq(i)); }
  IMeshTools_Parameters prm;
  prm.Deflection = avg * kLin[(int)q];
  prm.Angle = kAng[(int)q];
  prm.InParallel = true;
  prm.MinSize = prm.Deflection * 0.1;
  BRepMesh_IncrementalMesh mesher(comp, prm);
  size_t n = 0;
  for (auto& p : m.parts) {
    size_t c = 0;
    for (TopExp_Explorer ex(p.shape, TopAbs_FACE); ex.More(); ex.Next()) {
      TopLoc_Location l;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), l);
      if (!t.IsNull()) c += (size_t)t->NbTriangles();
    }
    p.tris = c;
    n += c;
  }
  m.tris = n;
  return n;
}

} // namespace gv
