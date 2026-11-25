// GlassView native geometry kernel (OpenCASCADE).
//
// Runs as a child process of GlassView. Protocol: one JSON object per line on stdin, one JSON
// object per line on stdout. Heavy data is written to files in GlassView's cache format (.gvc):
//   [u32 'GVC1'][u32 jsonLen][json][pad to 4][typed arrays...]
//
// Commands
//   {"cmd":"load","id":1,"path":"C:/a.step","out":"C:/cache/x.gvc","lin":0.5,"ang":0.25}
//      -> {"id":1,"ok":true,"meshes":N,"tris":T,"ms":..}
//   {"cmd":"refine","id":2,"path":"C:/a.step","mesh":3,"faces":[0,5,7],"lin":0.01,"ang":0.2,"out":"C:/tmp/r.gvc"}
//      -> {"id":2,"ok":true}       (a .gvc with one mesh holding only those faces, in the given order)
//   {"cmd":"ping","id":3} -> {"id":3,"ok":true,"version":"..."}
//
// Every mesh = one part instance (location applied, world coordinates, millimetres).
// Per mesh we emit: positions, normals (exact surface normals), indices, per-face triangle ranges,
// per-face edge-segment ranges, CAD edges (true B-rep edges, no seams) and the exact analytic
// surface of every face (plane / cylinder / cone / sphere / torus / other).

#include <STEPCAFControl_Reader.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <TDocStd_Document.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_ChildIterator.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <Bnd_Box.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Polygon3D.hxx>
#include <Quantity_Color.hxx>
#include <Interface_Static.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Standard_Failure.hxx>
#include <OSD_Parallel.hxx>
#include <gp_Pln.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Cone.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

#include <IMeshTools_Parameters.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GProp_PrincipalProps.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_StepModelType.hxx>
#include <TCollection_ExtendedString.hxx>
#include <OSD_OpenFile.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_GeomTolerance.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <XCAFDimTolObjects_GeomToleranceObject.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <TCollection_HAsciiString.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <XSAlgo_ShapeProcessor.hxx>
#include <XSControl_Reader.hxx>
#include <unordered_map>
#include <TDF_Tool.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <mutex>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static const char* KERNEL_VERSION = "glassview-kernel 1.3";
static const double PI_ = 3.14159265358979323846;

// ------------------------------------------------------------------ tiny JSON helpers
static std::string jsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o;
}

static std::string num(double v) {
  if (!std::isfinite(v)) return "0";
  char b[40];
  snprintf(b, sizeof b, "%.9g", v);
  return b;
}

// minimal flat-object parser: strings, numbers, arrays of numbers
struct Req {
  std::map<std::string, std::string> str;
  std::map<std::string, double> numv;
  std::map<std::string, std::vector<int>> arr;
  std::map<std::string, std::vector<double>> darr;
  std::map<std::string, std::vector<std::string>> sarr;
};

static Req parseReq(const std::string& line) {
  Req r;
  size_t i = 0, n = line.size();
  auto ws = [&] { while (i < n && isspace((unsigned char)line[i])) i++; };
  auto readStr = [&]() {
    std::string s;
    i++; // opening quote
    while (i < n && line[i] != '"') {
      if (line[i] == '\\' && i + 1 < n) {
        char e = line[i + 1];
        if (e == 'n') s += '\n'; else if (e == 't') s += '\t'; else if (e == 'u' && i + 5 < n) {
          unsigned cp = (unsigned)strtoul(line.substr(i + 2, 4).c_str(), nullptr, 16);
          if (cp < 0x80) s += (char)cp;
          else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
          else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
          i += 4;
        } else s += e;
        i += 2;
      } else s += line[i++];
    }
    i++;
    return s;
  };
  ws();
  if (i < n && line[i] == '{') i++;
  while (i < n) {
    ws();
    if (i >= n || line[i] == '}') break;
    if (line[i] == ',') { i++; continue; }
    if (line[i] != '"') break;
    std::string key = readStr();
    ws();
    if (i < n && line[i] == ':') i++;
    ws();
    if (i < n && line[i] == '"') r.str[key] = readStr();
    else if (i < n && line[i] == '[') {
      i++;
      std::vector<int> v;
      std::vector<double> dv;
      std::vector<std::string> sv;
      while (i < n && line[i] != ']') {
        ws();
        if (i >= n || line[i] == ']') break;
        if (line[i] == ',') { i++; continue; }
        if (line[i] == '"') { sv.push_back(readStr()); continue; }
        char* end;
        double d = strtod(line.c_str() + i, &end);
        if (end == line.c_str() + i) { i++; continue; }
        v.push_back((int)d);
        dv.push_back(d);
        i = end - line.c_str();
      }
      i++;
      r.arr[key] = v;
      r.darr[key] = dv;
      r.sarr[key] = sv;
    } else {
      char* end;
      double d = strtod(line.c_str() + i, &end);
      if (end != line.c_str() + i) { r.numv[key] = d; i = end - line.c_str(); }
      else { while (i < n && line[i] != ',' && line[i] != '}') i++; } // true/false/null
    }
  }
  return r;
}

// ------------------------------------------------------------------ model
struct PartInst {
  std::string name;
  TopoDS_Shape shape;        // unmoved part shape (shared between instances)
  TopLoc_Location loc;       // instance placement
  bool hasColor = false;
  Quantity_Color color;
  std::vector<TopoDS_Face> faces; // fixed face order (TopExp_Explorer)
  std::vector<char> faceHasColor;
  std::vector<Quantity_Color> faceColor;
};

struct TreeNode {
  std::string name;
  std::vector<int> meshes;
  std::vector<std::unique_ptr<TreeNode>> children;
};

struct Model {
  Handle(TDocStd_Document) doc;
  Handle(XCAFDoc_ShapeTool) st;
  Handle(XCAFDoc_ColorTool) ct;
  std::vector<PartInst> parts;
  std::unique_ptr<TreeNode> root;
  double avgSize = 1;
  std::map<std::string, int> partOfLabel; // product label entry -> first part using it
};

static std::map<std::string, std::unique_ptr<Model>> g_models;

static std::string labelName(const TDF_Label& l) {
  Handle(TDataStd_Name) n;
  if (l.FindAttribute(TDataStd_Name::GetID(), n)) {
    TCollection_AsciiString a(n->Get());
    return a.ToCString();
  }
  return "";
}

static bool labelColor(const Handle(XCAFDoc_ColorTool)& ct, const TDF_Label& l, Quantity_Color& c) {
  return ct->GetColor(l, XCAFDoc_ColorSurf, c) || ct->GetColor(l, XCAFDoc_ColorGen, c);
}

static void visit(Model& m, const TDF_Label& label, const TopLoc_Location& parentLoc, TreeNode& parent,
                  bool inhColor, const Quantity_Color& inh) {
  TDF_Label ref = label;
  TopLoc_Location loc = parentLoc;
  if (m.st->IsReference(label)) {
    m.st->GetReferredShape(label, ref);
    loc = parentLoc * m.st->GetLocation(label);
  }
  std::string name = labelName(label);
  if (name.empty()) name = labelName(ref);
  Quantity_Color c;
  bool hc = inhColor;
  if (labelColor(m.ct, ref, c)) hc = true; else c = inh;
  Quantity_Color c2;
  if (labelColor(m.ct, label, c2)) { c = c2; hc = true; }

  if (m.st->IsAssembly(ref)) {
    auto node = std::make_unique<TreeNode>();
    node->name = name;
    TDF_LabelSequence comps;
    m.st->GetComponents(ref, comps);
    for (int i = 1; i <= comps.Length(); i++) visit(m, comps.Value(i), loc, *node, hc, c);
    parent.children.push_back(std::move(node));
  } else {
    TopoDS_Shape shape = m.st->GetShape(ref);
    if (shape.IsNull()) return;
    PartInst p;
    p.name = name;
    p.shape = shape;
    p.loc = loc;
    p.hasColor = hc;
    p.color = c;
    // the same product placed again: reuse its faces and face colours
    TCollection_AsciiString entry;
    TDF_Tool::Entry(ref, entry);
    auto seen = m.partOfLabel.find(entry.ToCString());
    if (seen != m.partOfLabel.end()) {
      const PartInst& q = m.parts[seen->second];
      p.faces = q.faces; p.faceHasColor = q.faceHasColor; p.faceColor = q.faceColor;
    } else {
      for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) p.faces.push_back(TopoDS::Face(ex.Current()));
      if (p.faces.empty()) return;
      // Face colours come from the part's sub-shape labels. (Asking the colour tool per face searches
      // the whole document every time: ~10 s for a 70 000-face assembly.)
      p.faceHasColor.assign(p.faces.size(), 0);
      p.faceColor.resize(p.faces.size());
      TDF_LabelSequence subs;
      m.st->GetSubShapes(ref, subs);
      if (subs.Length()) {
        TopTools_IndexedMapOfShape faceIdx;
        for (auto& f : p.faces) faceIdx.Add(f);
        for (int pass = 0; pass < 2; pass++) // bodies / shells first, then single faces (more specific wins)
          for (int i = 1; i <= subs.Length(); i++) {
            Quantity_Color sc;
            if (!labelColor(m.ct, subs.Value(i), sc)) continue;
            TopoDS_Shape ss = m.st->GetShape(subs.Value(i));
            if (ss.IsNull() || (ss.ShapeType() == TopAbs_FACE) != (pass == 1)) continue;
            for (TopExp_Explorer ex(ss, TopAbs_FACE); ex.More(); ex.Next()) {
              int k = faceIdx.FindIndex(ex.Current());
              if (k > 0) { p.faceHasColor[k - 1] = 1; p.faceColor[k - 1] = sc; }
            }
          }
      }
      m.partOfLabel[entry.ToCString()] = (int)m.parts.size();
    }
    if (p.faces.empty()) return;
    auto node = std::make_unique<TreeNode>();
    node->name = name;
    node->meshes.push_back((int)m.parts.size());
    m.parts.push_back(std::move(p));
    parent.children.push_back(std::move(node));
  }
}

static std::string lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }

// ------------------------------------------------------------------ progress lines for the app
// {"id":N,"progress":"stage","pct":P} while a request runs (the final reply has "ok").
static int g_reqId = 0;
static std::mutex g_outMutex;
static double nowMs0() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void progressLine(const char* stage, int pct) {
  if (g_reqId <= 0) return;
  std::lock_guard<std::mutex> g(g_outMutex);
  std::cout << "{\"id\":" << g_reqId << ",\"progress\":\"" << stage << "\",\"pct\":" << pct << "}" << std::endl;
}
class JsonProgress : public Message_ProgressIndicator {
 public:
  explicit JsonProgress(const char* stage) : myStage(stage) {}
  void Show(const Message_ProgressScope&, const Standard_Boolean force) override {
    const double t = nowMs0();
    const int pct = (int)(GetPosition() * 100 + 0.5);
    if (!force && (t - myLast < 300 || pct == myPct)) return;
    myLast = t; myPct = pct;
    progressLine(myStage, pct);
  }
 private:
  const char* myStage;
  double myLast = 0;
  int myPct = -1;
};

// STEP shape healing. OCCT's default runs every repair on every face of every solid, single-threaded; on
// big assemblies that is most of the load time (a 82 MB AP242 file: 26 s of 30 s). The checks switched
// off here re-classify wire / face / shell orientation and hunt for self-intersections, which files
// from real CAD systems practically never need; pcurves, same-parameter and tolerance fixes (needed
// for meshing) stay on. Measured on that file: transfer 26 s -> 13 s, the same faces mesh.
static bool g_fullHeal = false;
static void fastHealing(XSControl_Reader& rd, const char* res, const char* seq) {
  if (g_fullHeal) return;
  try {
    auto data = XSAlgo_ShapeProcessor::ReadProcessingData(res, seq);
    static const char* off[] = {"FixOrientationMode", "FixFaceOrientationMode", "FixShellOrientationMode", "FixAddNaturalBoundMode",
                                "FixIntersectingWiresMode", "FixLoopWiresMode", "FixSelfIntersectionMode", "FixIntersectingEdgesMode",
                                "FixNonAdjacentIntersectingEdgesMode", "FixSplitFaceMode", "FixMissingSeamMode", "FixSmallAreaWireMode",
                                "RemoveSmallAreaFaceMode", "FixSmallMode"};
    for (const char* k : off) data.first.Bind(TCollection_AsciiString("FixShape.") + k, TCollection_AsciiString("0"));
    rd.SetShapeFixParameters(std::move(data.first));
    rd.SetShapeProcessFlags(data.second);
  } catch (...) {}
}

static std::unique_ptr<Model> readModel(const std::string& path, std::string& err) {
  auto m = std::make_unique<Model>();
  Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
  app->NewDocument("MDTV-XCAF", m->doc);
  std::string ext = lower(path.substr(path.find_last_of('.') + 1));
  bool ok = false;
  if (ext == "stp" || ext == "step") {
    STEPCAFControl_Reader r;
    r.SetColorMode(true); r.SetNameMode(true); r.SetLayerMode(false); r.SetGDTMode(true);
    progressLine("Reading the file", 0);
    if (r.ReadFile(path.c_str()) == IFSelect_RetDone) {
      fastHealing(r.ChangeReader(), "read.step.resource.name", "read.step.sequence"); // after ReadFile: the actor exists now
      Handle(JsonProgress) pi = new JsonProgress("Building the geometry");
      ok = r.Transfer(m->doc, pi->Start());
    }
  } else if (ext == "igs" || ext == "iges") {
    IGESCAFControl_Reader r;
    r.SetColorMode(true); r.SetNameMode(true);
    if (r.ReadFile(path.c_str()) == IFSelect_RetDone) ok = r.Transfer(m->doc);
  } else if (ext == "brep" || ext == "brp") {
    TopoDS_Shape s;
    BRep_Builder b;
    if (BRepTools::Read(s, path.c_str(), b)) {
      Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(m->doc->Main());
      st->AddShape(s);
      ok = true;
    }
  }
  if (!ok) { err = "could not read file"; return nullptr; }
  m->st = XCAFDoc_DocumentTool::ShapeTool(m->doc->Main());
  m->ct = XCAFDoc_DocumentTool::ColorTool(m->doc->Main());
  m->root = std::make_unique<TreeNode>();
  std::string base = path.substr(path.find_last_of("/\\") + 1);
  m->root->name = base.substr(0, base.find_last_of('.'));
  TDF_LabelSequence free;
  m->st->GetFreeShapes(free);
  Quantity_Color none;
  for (int i = 1; i <= free.Length(); i++) visit(*m, free.Value(i), TopLoc_Location(), *m->root, false, none);
  if (m->parts.empty()) { err = "no geometry found"; return nullptr; }
  Bnd_Box bb;
  for (auto& p : m->parts) BRepBndLib::Add(p.shape.Moved(p.loc), bb);
  if (!bb.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    m->avgSize = std::max(1e-6, ((x1 - x0) + (y1 - y0) + (z1 - z0)) / 3.0);
  }
  return m;
}

// ------------------------------------------------------------------ meshing + extraction
struct MeshOut {
  std::string name;
  bool hasColor = false;
  Quantity_Color color;
  std::vector<float> pos, nrm;
  std::vector<uint32_t> idx;
  std::vector<uint8_t> col; // per-vertex colours (only if some face has its own colour)
  std::vector<int32_t> ranges;  // per face: first tri, last tri
  std::vector<int32_t> eranges; // per face: first segment, segment count
  std::vector<float> edges;     // segments (x,y,z, x,y,z)
  int geom = -1;                // >= 0: same geometry as meshes[geom] (a repeated part), no arrays of its own
  bool hasMat = false;
  double mat[16];               // part placement, column-major (three.js order); geometry is in part coordinates
  std::string prims;            // JSON array
};

static void meshShapes(const std::vector<TopoDS_Shape>& shapes, double lin, double ang) {
  TopoDS_Compound comp;
  BRep_Builder b;
  b.MakeCompound(comp);
  for (auto& s : shapes) b.Add(comp, s);
  IMeshTools_Parameters prm;
  prm.Deflection = lin;
  prm.Angle = ang;
  prm.InParallel = true;
  prm.Relative = false;
  prm.MinSize = lin * 0.1;
  Handle(JsonProgress) pi = new JsonProgress("Meshing");
  BRepMesh_IncrementalMesh mesher(comp, prm, pi->Start());
}

static std::string primOf(const TopoDS_Face& face, const gp_Trsf& T, const std::vector<float>& pos, size_t v0, size_t v1) {
  try {
    BRepAdaptor_Surface s(face, false);
    bool rev = face.Orientation() == TopAbs_REVERSED;
    auto P = [&](const gp_Pnt& p) { gp_Pnt q = p.Transformed(T); return "[" + num(q.X()) + "," + num(q.Y()) + "," + num(q.Z()) + "]"; };
    auto D = [&](const gp_Dir& d) { gp_Dir q = d.Transformed(T); return "[" + num(q.X()) + "," + num(q.Y()) + "," + num(q.Z()) + "]"; };
    // axial extent of the face's samples (world space, already transformed)
    auto extent = [&](const gp_Ax1& ax, double& t0, double& t1) {
      gp_Pnt o = ax.Location().Transformed(T);
      gp_Dir a = ax.Direction().Transformed(T);
      t0 = 1e300; t1 = -1e300;
      for (size_t v = v0; v < v1; v++) {
        double t = (pos[v * 3] - o.X()) * a.X() + (pos[v * 3 + 1] - o.Y()) * a.Y() + (pos[v * 3 + 2] - o.Z()) * a.Z();
        t0 = std::min(t0, t); t1 = std::max(t1, t);
      }
      if (t0 > t1) { t0 = 0; t1 = 0; }
    };
    switch (s.GetType()) {
      case GeomAbs_Plane: {
        gp_Pln pl = s.Plane();
        gp_Dir n = pl.Axis().Direction();
        if (rev) n.Reverse();
        return "{\"t\":\"plane\",\"n\":" + D(n) + ",\"p\":" + P(pl.Location()) + "}";
      }
      case GeomAbs_Cylinder: {
        gp_Cylinder c = s.Cylinder();
        double t0, t1; extent(c.Axis(), t0, t1);
        return "{\"t\":\"cylinder\",\"a\":" + D(c.Axis().Direction()) + ",\"o\":" + P(c.Location()) + ",\"r\":" + num(c.Radius()) +
               ",\"t0\":" + num(t0) + ",\"t1\":" + num(t1) + "}";
      }
      case GeomAbs_Cone: {
        gp_Cone c = s.Cone();
        double t0, t1; extent(c.Axis(), t0, t1);
        // radius at axial position t (measured from the cone's reference plane)
        double r0 = c.RefRadius() + t0 * std::tan(c.SemiAngle()), r1 = c.RefRadius() + t1 * std::tan(c.SemiAngle());
        return "{\"t\":\"cone\",\"a\":" + D(c.Axis().Direction()) + ",\"o\":" + P(c.Location()) + ",\"t0\":" + num(t0) + ",\"t1\":" + num(t1) +
               ",\"r0\":" + num(std::fabs(r0)) + ",\"r1\":" + num(std::fabs(r1)) + ",\"ang\":" + num(std::fabs(c.SemiAngle()) * 180.0 / PI_) + "}";
      }
      case GeomAbs_Sphere: {
        gp_Sphere sp = s.Sphere();
        return "{\"t\":\"sphere\",\"c\":" + P(sp.Location()) + ",\"r\":" + num(sp.Radius()) + "}";
      }
      case GeomAbs_Torus: {
        gp_Torus to = s.Torus();
        double t0, t1; extent(to.Axis(), t0, t1);
        return "{\"t\":\"torus\",\"a\":" + D(to.Axis().Direction()) + ",\"o\":" + P(to.Location()) + ",\"R\":" + num(to.MajorRadius()) +
               ",\"r\":" + num(to.MinorRadius()) + ",\"t0\":" + num(t0) + ",\"t1\":" + num(t1) + "}";
      }
      case GeomAbs_BSplineSurface: return "{\"t\":\"other\",\"k\":\"bspline\"}";
      case GeomAbs_BezierSurface: return "{\"t\":\"other\",\"k\":\"bezier\"}";
      case GeomAbs_SurfaceOfRevolution: return "{\"t\":\"other\",\"k\":\"revolution\"}";
      case GeomAbs_SurfaceOfExtrusion: return "{\"t\":\"other\",\"k\":\"extrusion\"}";
      case GeomAbs_OffsetSurface: return "{\"t\":\"other\",\"k\":\"offset\"}";
      default: return "{\"t\":\"other\"}";
    }
  } catch (...) {
    return "{\"t\":\"other\"}";
  }
}

/** Append one face (triangles, normals, colour, edges, exact surface) to a MeshOut. */
static void emitFace(MeshOut& o, const TopoDS_Face& face, const TopLoc_Location& instLoc, const Quantity_Color* faceColor,
                     bool partColor, const Quantity_Color& pc, bool& anyFaceColor, std::vector<std::array<uint8_t, 3>>* faceCols) {
  TopLoc_Location fl;
  Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, fl);
  int32_t firstTri = (int32_t)(o.idx.size() / 3);
  int32_t firstSeg = (int32_t)(o.edges.size() / 6);
  size_t v0 = o.pos.size() / 3;
  gp_Trsf T = (instLoc * fl).Transformation();
  gp_Trsf TL = instLoc.Transformation();
  if (!tri.IsNull() && tri->NbTriangles() > 0) {
    if (!tri->HasNormals()) BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);
    bool rev = face.Orientation() == TopAbs_REVERSED;
    const int nn = tri->NbNodes();
    for (int i = 1; i <= nn; i++) {
      gp_Pnt p = tri->Node(i).Transformed(T);
      o.pos.push_back((float)p.X()); o.pos.push_back((float)p.Y()); o.pos.push_back((float)p.Z());
      gp_Dir d = tri->HasNormals() ? gp_Dir(tri->Normal(i)) : gp_Dir(0, 0, 1);
      d.Transform(T);
      if (rev) d.Reverse();
      o.nrm.push_back((float)d.X()); o.nrm.push_back((float)d.Y()); o.nrm.push_back((float)d.Z());
    }
    const int nt = tri->NbTriangles();
    for (int t = 1; t <= nt; t++) {
      int a, b, c;
      tri->Triangle(t).Get(a, b, c);
      if (rev) std::swap(b, c);
      o.idx.push_back((uint32_t)(v0 + a - 1)); o.idx.push_back((uint32_t)(v0 + b - 1)); o.idx.push_back((uint32_t)(v0 + c - 1));
    }
    // CAD edges of this face (polygon on triangulation; seams and degenerate edges skipped)
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
      const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
      if (BRep_Tool::Degenerated(e) || BRep_Tool::IsClosed(e, face)) continue;
      Handle(Poly_PolygonOnTriangulation) pt = BRep_Tool::PolygonOnTriangulation(e, tri, fl);
      if (pt.IsNull()) continue;
      const auto& nodes = pt->Nodes();
      for (int k = nodes.Lower(); k < nodes.Upper(); k++) {
        size_t ia = (v0 + nodes(k) - 1) * 3, ib = (v0 + nodes(k + 1) - 1) * 3;
        for (int q = 0; q < 3; q++) o.edges.push_back(o.pos[ia + q]);
        for (int q = 0; q < 3; q++) o.edges.push_back(o.pos[ib + q]);
      }
    }
  }
  size_t v1 = o.pos.size() / 3;
  int32_t lastTri = (int32_t)(o.idx.size() / 3) - 1;
  o.ranges.push_back(firstTri);
  o.ranges.push_back(lastTri);
  o.eranges.push_back(firstSeg);
  o.eranges.push_back((int32_t)(o.edges.size() / 6) - firstSeg);
  // face colour (XCAF sub-shape colour) for vertex colouring
  if (faceCols) {
    bool has = faceColor != nullptr;
    if (has) anyFaceColor = true;
    Quantity_Color use = has ? *faceColor : (partColor ? pc : Quantity_Color(0.62, 0.68, 0.76, Quantity_TOC_sRGB));
    double r, g, bl;
    use.Values(r, g, bl, Quantity_TOC_sRGB);
    for (size_t v = v0; v < v1; v++) faceCols->push_back({(uint8_t)std::lround(r * 255), (uint8_t)std::lround(g * 255), (uint8_t)std::lround(bl * 255)});
  }
  o.prims += (o.prims.empty() ? "" : ",") + primOf(face, TL, o.pos, v0, v1);
}

// ------------------------------------------------------------------ .gvc writer
struct Arr { const char* tag; const void* data; size_t count; size_t bytes; };

static bool writeGvc(const std::string& path, const std::string& treeJson, std::vector<MeshOut>& meshes) {
  std::vector<Arr> arrays;
  auto add = [&](const char* tag, const void* d, size_t count, size_t elem) -> int {
    if (count == 0 && strcmp(tag, "i32") != 0 && strcmp(tag, "f32") != 0) return -1;
    arrays.push_back({tag, d, count, count * elem});
    return (int)arrays.size() - 1;
  };
  std::ostringstream js;
  js << "{\"tree\":" << treeJson << ",\"units\":\"mm\",\"native\":true,\"meshes\":[";
  for (size_t i = 0; i < meshes.size(); i++) {
    MeshOut& m = meshes[i];
    std::string matJ;
    if (m.hasMat) {
      matJ = ",\"matrix\":[";
      for (int k = 0; k < 16; k++) matJ += (k ? "," : "") + num(m.mat[k]);
      matJ += "]";
    }
    if (m.geom >= 0) { // repeated part: points at the first copy's arrays
      if (i) js << ",";
      js << "{\"name\":\"" << jsonEscape(m.name) << "\",\"color\":";
      if (m.hasColor) { double r, g, b; m.color.Values(r, g, b, Quantity_TOC_sRGB); js << "[" << num(r) << "," << num(g) << "," << num(b) << "]"; }
      else js << "null";
      js << ",\"geom\":" << m.geom << matJ << "}";
      continue;
    }
    int ip = add("f32", m.pos.data(), m.pos.size(), 4);
    int in = add("f32", m.nrm.data(), m.nrm.size(), 4);
    int ii = add("u32", m.idx.data(), m.idx.size(), 4);
    int ic = m.col.empty() ? -1 : add("u8", m.col.data(), m.col.size(), 1);
    int ie = add("f32", m.edges.data(), m.edges.size(), 4);
    int ir = add("i32", m.ranges.data(), m.ranges.size(), 4);
    int ier = add("i32", m.eranges.data(), m.eranges.size(), 4);
    if (i) js << ",";
    js << "{\"name\":\"" << jsonEscape(m.name) << "\",\"color\":";
    if (m.hasColor) {
      double r, g, b; m.color.Values(r, g, b, Quantity_TOC_sRGB);
      js << "[" << num(r) << "," << num(g) << "," << num(b) << "]";
    } else js << "null";
    js << ",\"position\":" << ip << ",\"normal\":" << in << ",\"index\":" << ii << ",\"colors\":" << ic
       << ",\"edges\":" << ie << ",\"ranges\":" << ir << ",\"eranges\":" << ier << ",\"prims\":[" << m.prims << "]" << matJ << "}";
  }
  js << "],\"arrays\":[";
  for (size_t i = 0; i < arrays.size(); i++) js << (i ? "," : "") << "{\"t\":\"" << arrays[i].tag << "\",\"n\":" << arrays[i].count << "}";
  js << "]}";
  std::string json = js.str();
  std::string tmp = path + ".part";
  FILE* f = OSD_OpenFile(tmp.c_str(), "wb");
  if (!f) return false;
  uint32_t magic = 0x31435647, jl = (uint32_t)json.size();
  fwrite(&magic, 4, 1, f); fwrite(&jl, 4, 1, f); fwrite(json.data(), 1, json.size(), f);
  size_t off = 8 + json.size();
  static const char zeros[4] = {0, 0, 0, 0};
  auto pad = [&] { size_t p = (4 - off % 4) % 4; fwrite(zeros, 1, p, f); off += p; };
  pad();
  for (auto& a : arrays) { if (a.bytes) fwrite(a.data, 1, a.bytes, f); off += a.bytes; pad(); }
  fclose(f);
#ifdef _WIN32
  std::wstring wt = TCollection_ExtendedString(tmp.c_str(), true).ToWideString();
  std::wstring wp = TCollection_ExtendedString(path.c_str(), true).ToWideString();
  _wremove(wp.c_str());
  return _wrename(wt.c_str(), wp.c_str()) == 0;
#else
  remove(path.c_str());
  return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

static void treeJson(const TreeNode& n, std::ostringstream& o) {
  o << "{\"name\":\"" << jsonEscape(n.name) << "\",\"meshes\":[";
  for (size_t i = 0; i < n.meshes.size(); i++) o << (i ? "," : "") << n.meshes[i];
  o << "],\"children\":[";
  for (size_t i = 0; i < n.children.size(); i++) { if (i) o << ","; treeJson(*n.children[i], o); }
  o << "]}";
}

// ------------------------------------------------------------------ commands
static double nowMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static Model* getModel(const std::string& path, std::string& err) {
  auto it = g_models.find(path);
  if (it != g_models.end()) return it->second.get();
  auto m = readModel(path, err);
  if (!m) return nullptr;
  Model* raw = m.get();
  if (g_models.size() >= 3) g_models.erase(g_models.begin()); // keep memory bounded
  g_models[path] = std::move(m);
  return raw;
}

static std::string cmdLoad(const Req& r, int id) {
  double t0 = nowMs();
  std::string path = r.str.count("path") ? r.str.at("path") : "";
  std::string out = r.str.count("out") ? r.str.at("out") : "";
  std::string err;
  g_models.erase(path); // fresh read so tessellation parameters apply cleanly
  g_fullHeal = r.str.count("heal") && r.str.at("heal") == "full";
  Model* m = getModel(path, err);
  if (!m) return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
  double t1 = nowMs();
  double lin = r.numv.count("lin") ? r.numv.at("lin") : 0.001;
  if (r.str.count("linType") && r.str.at("linType") == "ratio") lin *= m->avgSize;
  double ang = r.numv.count("ang") ? r.numv.at("ang") : 0.5;
  std::vector<TopoDS_Shape> uniq;
  {
    TopTools_IndexedMapOfShape seen;
    for (auto& p : m->parts) if (!seen.Contains(p.shape)) { seen.Add(p.shape); uniq.push_back(p.shape); }
  }
  meshShapes(uniq, lin, ang);
  double t2 = nowMs();
  std::vector<MeshOut> meshes(m->parts.size());
  size_t tris = 0;
  // Repeated parts (the same shape, same colour) are written once, in part coordinates; every copy
  // only carries its placement. A wheel corner with 200 identical bolts stores one bolt.
  std::vector<int> owner(m->parts.size(), -1);
  {
    std::unordered_map<const void*, std::vector<int>> byShape;
    for (size_t i = 0; i < m->parts.size(); i++) {
      PartInst& p = m->parts[i];
      auto& cand = byShape[p.shape.TShape().get()];
      for (int j : cand) {
        PartInst& q = m->parts[j];
        if (!q.shape.IsEqual(p.shape) || q.hasColor != p.hasColor) continue;
        if (p.hasColor && !q.color.IsEqual(p.color)) continue;
        owner[i] = j;
        break;
      }
      if (owner[i] < 0) cand.push_back((int)i);
    }
  }
  OSD_Parallel::For(0, (int)m->parts.size(), [&](int i) {
    PartInst& p = m->parts[i];
    MeshOut& o = meshes[i];
    o.name = p.name;
    o.hasColor = p.hasColor;
    o.color = p.color;
    const gp_Trsf T = p.loc.Transformation();
    o.hasMat = true;
    for (int c = 0; c < 4; c++)
      for (int rr = 0; rr < 4; rr++) o.mat[c * 4 + rr] = c < 3 ? (rr < 3 ? T.Value(rr + 1, c + 1) : 0.0) : (rr < 3 ? T.Value(rr + 1, 4) : 1.0);
    if (owner[i] >= 0) { o.geom = owner[i]; return; }
    bool anyFace = false;
    std::vector<std::array<uint8_t, 3>> fc;
    for (size_t k = 0; k < p.faces.size(); k++)
      emitFace(o, p.faces[k], TopLoc_Location(), p.faceHasColor.empty() || !p.faceHasColor[k] ? nullptr : &p.faceColor[k], p.hasColor, p.color, anyFace, &fc);
    if (anyFace) { o.col.reserve(fc.size() * 3); for (auto& c : fc) { o.col.push_back(c[0]); o.col.push_back(c[1]); o.col.push_back(c[2]); } }
  });
  size_t uniqTris = 0;
  for (size_t i = 0; i < meshes.size(); i++) uniqTris += meshes[i].idx.size() / 3;
  for (size_t i = 0; i < meshes.size(); i++) tris += meshes[owner[i] >= 0 ? owner[i] : i].idx.size() / 3;
  progressLine("Saving", 100);
  std::ostringstream tj;
  treeJson(*m->root, tj);
  if (!writeGvc(out, tj.str(), meshes)) return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"cannot write output\"}";
  double t3 = nowMs();
  return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"meshes\":" + std::to_string(meshes.size()) + ",\"tris\":" + std::to_string(tris) +
         ",\"uniqueTris\":" + std::to_string(uniqTris) + ",\"heal\":\"" + (g_fullHeal ? "full" : "fast") + "\"" +
         ",\"lin\":" + num(lin) + ",\"readMs\":" + num(t1 - t0) + ",\"meshMs\":" + num(t2 - t1) + ",\"writeMs\":" + num(t3 - t2) + "}";
}


// ------------------------------------------------------------------ PMI (STEP AP242 dimensions, GD&T, datums)
static std::string hstr(const Handle(TCollection_HAsciiString)& h) { return h.IsNull() ? "" : std::string(h->ToCString()); }
static std::string gnum(double v) { char b[48]; snprintf(b, sizeof b, "%.4g", v); return b; }

static const char* dimTypeName(XCAFDimTolObjects_DimensionType t, std::string& prefix) {
  prefix.clear();
  switch (t) {
    case XCAFDimTolObjects_DimensionType_Size_Diameter: prefix = "\xC3\x98"; return "Diameter";
    case XCAFDimTolObjects_DimensionType_Size_SphericalDiameter: prefix = "S\xC3\x98"; return "Spherical diameter";
    case XCAFDimTolObjects_DimensionType_Size_Radius: prefix = "R"; return "Radius";
    case XCAFDimTolObjects_DimensionType_Size_SphericalRadius: prefix = "SR"; return "Spherical radius";
    case XCAFDimTolObjects_DimensionType_Size_Thickness: return "Thickness";
    case XCAFDimTolObjects_DimensionType_Size_Angular:
    case XCAFDimTolObjects_DimensionType_Location_Angular: return "Angle";
    case XCAFDimTolObjects_DimensionType_Size_CurveLength:
    case XCAFDimTolObjects_DimensionType_Location_CurvedDistance: return "Curve length";
    case XCAFDimTolObjects_DimensionType_CommonLabel:
    case XCAFDimTolObjects_DimensionType_DimensionPresentation: return "Note";
    default: return t >= XCAFDimTolObjects_DimensionType_Location_LinearDistance && t <= XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromInnerToInner ? "Distance" : "Dimension";
  }
}

static const char* tolTypeName(XCAFDimTolObjects_GeomToleranceType t) {
  switch (t) {
    case XCAFDimTolObjects_GeomToleranceType_Angularity: return "Angularity";
    case XCAFDimTolObjects_GeomToleranceType_CircularRunout: return "Circular runout";
    case XCAFDimTolObjects_GeomToleranceType_CircularityOrRoundness: return "Circularity";
    case XCAFDimTolObjects_GeomToleranceType_Coaxiality: return "Coaxiality";
    case XCAFDimTolObjects_GeomToleranceType_Concentricity: return "Concentricity";
    case XCAFDimTolObjects_GeomToleranceType_Cylindricity: return "Cylindricity";
    case XCAFDimTolObjects_GeomToleranceType_Flatness: return "Flatness";
    case XCAFDimTolObjects_GeomToleranceType_Parallelism: return "Parallelism";
    case XCAFDimTolObjects_GeomToleranceType_Perpendicularity: return "Perpendicularity";
    case XCAFDimTolObjects_GeomToleranceType_Position: return "Position";
    case XCAFDimTolObjects_GeomToleranceType_ProfileOfLine: return "Profile of a line";
    case XCAFDimTolObjects_GeomToleranceType_ProfileOfSurface: return "Profile of a surface";
    case XCAFDimTolObjects_GeomToleranceType_Straightness: return "Straightness";
    case XCAFDimTolObjects_GeomToleranceType_Symmetry: return "Symmetry";
    case XCAFDimTolObjects_GeomToleranceType_TotalRunout: return "Total runout";
    default: return "Tolerance";
  }
}

static std::string fitClass(const Handle(XCAFDimTolObjects_DimensionObject)& o) {
  if (!o->IsDimWithClassOfTolerance()) return "";
  Standard_Boolean hole = false;
  XCAFDimTolObjects_DimensionFormVariance fv;
  XCAFDimTolObjects_DimensionGrade g;
  if (!o->GetClassOfTolerance(hole, fv, g)) return "";
  static const char* fvs[] = {"", "A", "B", "C", "CD", "D", "E", "EF", "F", "FG", "G", "H", "JS", "J", "K", "M", "N", "P", "R", "S", "T", "U", "V", "X", "Y", "Z", "ZA", "ZB", "ZC"};
  std::string f = (int)fv >= 0 && (int)fv < 29 ? fvs[(int)fv] : "";
  if (!hole) for (auto& c : f) c = (char)tolower((unsigned char)c);
  const int gi = (int)g;
  return f + (gi == 0 ? std::string("01") : std::to_string(gi - 1));
}

/** Referenced faces → [[meshIndex, faceIndex], …] (faces of every instance of the part). */
static std::string pmiRefs(Model& m, const TDF_Label& l) {
  TDF_LabelSequence a, b;
  std::string out = "[";
  bool first = true;
  if (!XCAFDoc_DimTolTool::GetRefShapeLabel(l, a, b)) return "[]";
  a.Append(b);
  for (int i = 1; i <= a.Length(); i++) {
    TopoDS_Shape s = m.st->GetShape(a.Value(i));
    if (s.IsNull()) continue;
    std::vector<TopoDS_Shape> faces;
    if (s.ShapeType() == TopAbs_FACE) faces.push_back(s);
    else for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More() && faces.size() < 64; ex.Next()) faces.push_back(ex.Current());
    for (auto& f : faces)
      for (size_t pi = 0; pi < m.parts.size(); pi++)
        for (size_t fi = 0; fi < m.parts[pi].faces.size(); fi++)
          if (m.parts[pi].faces[fi].IsSame(f) || m.parts[pi].faces[fi].TShape() == f.TShape()) {
            out += std::string(first ? "" : ",") + "[" + std::to_string(pi) + "," + std::to_string(fi) + "]";
            first = false;
          }
  }
  return out + "]";
}

/** Annotation polylines → flat [x,y,z, x,y,z, …] segment list. */
static std::string pmiSegs(const TopoDS_Shape& pres) {
  if (pres.IsNull()) return "[]";
  std::string out = "[";
  bool first = true;
  size_t n = 0;
  for (TopExp_Explorer ex(pres, TopAbs_EDGE); ex.More() && n < 20000; ex.Next()) {
    const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
    if (BRep_Tool::Degenerated(e)) continue;
    try {
      BRepAdaptor_Curve c(e);
      GCPnts_TangentialDeflection d(c, 0.15, 0.01, 2);
      for (int i = 1; i < d.NbPoints(); i++, n++) {
        const gp_Pnt p = d.Value(i), q = d.Value(i + 1);
        out += std::string(first ? "" : ",") + num(p.X()) + "," + num(p.Y()) + "," + num(p.Z()) + "," + num(q.X()) + "," + num(q.Y()) + "," + num(q.Z());
        first = false;
      }
    } catch (...) {}
  }
  return out + "]";
}

static std::string cmdPmi(const Req& r, int id) {
  std::string path = r.str.count("path") ? r.str.at("path") : "";
  std::string err;
  Model* m = getModel(path, err);
  if (!m) return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
  std::ostringstream js;
  js << "{\"id\":" << id << ",\"ok\":true,\"items\":[";
  bool first = true;
  auto item = [&](int kind, const std::string& title, const std::string& detail, bool hasPt, const gp_Pnt& pt, const TopoDS_Shape& pres, const TDF_Label& l) {
    js << (first ? "" : ",") << "{\"kind\":" << kind << ",\"title\":\"" << jsonEscape(title) << "\",\"detail\":\"" << jsonEscape(detail) << "\"";
    if (hasPt) js << ",\"pt\":[" << num(pt.X()) << "," << num(pt.Y()) << "," << num(pt.Z()) << "]";
    js << ",\"segs\":" << pmiSegs(pres) << ",\"refs\":" << pmiRefs(*m, l) << "}";
    first = false;
  };
  if (XCAFDoc_DocumentTool::CheckDimTolTool(m->doc->Main())) {
    Handle(XCAFDoc_DimTolTool) dt = XCAFDoc_DocumentTool::DimTolTool(m->doc->Main());
    TDF_LabelSequence dl, dims, tols;
    dt->GetDatumLabels(dl);
    for (int i = 1; i <= dl.Length(); i++) {
      Handle(XCAFDoc_Datum) at;
      if (!dl.Value(i).FindAttribute(XCAFDoc_Datum::GetID(), at) || at->GetObject().IsNull()) continue;
      Handle(XCAFDimTolObjects_DatumObject) o = at->GetObject();
      std::string t = hstr(o->GetName());
      item(2, t.empty() ? "Datum" : t, "Datum feature", o->HasPointText(), o->HasPointText() ? o->GetPointTextAttach() : gp_Pnt(), o->GetPresentation(), dl.Value(i));
    }
    dt->GetDimensionLabels(dims);
    for (int i = 1; i <= dims.Length(); i++) {
      Handle(XCAFDoc_Dimension) at;
      if (!dims.Value(i).FindAttribute(XCAFDoc_Dimension::GetID(), at) || at->GetObject().IsNull()) continue;
      Handle(XCAFDimTolObjects_DimensionObject) o = at->GetObject();
      std::string prefix;
      const char* tn = dimTypeName(o->GetType(), prefix);
      const bool angle = std::string(tn) == "Angle";
      std::string val = o->IsDimWithRange() ? gnum(o->GetLowerBound()) + "\xE2\x80\x93" + gnum(o->GetUpperBound()) : gnum(o->GetValue());
      if (angle) val += "\xC2\xB0";
      std::string tol;
      if (o->IsDimWithPlusMinusTolerance()) {
        const double up = o->GetUpperTolValue(), lo = o->GetLowerTolValue();
        if (up > 0 && std::fabs(std::fabs(up) - std::fabs(lo)) < 1e-12) tol = " \xC2\xB1" + gnum(up);
        else tol = std::string(" ") + (up >= 0 ? "+" : "") + gnum(up) + " / " + gnum(lo);
      }
      const std::string fit = fitClass(o);
      const std::string title = prefix + val + (fit.empty() ? "" : " " + fit) + tol;
      const std::string pn = hstr(o->GetPresentationName());
      item(0, title, std::string(tn) + (pn.empty() || pn == title ? "" : " \xC2\xB7 " + pn), o->HasTextPoint(), o->HasTextPoint() ? o->GetPointTextAttach() : gp_Pnt(), o->GetPresentation(), dims.Value(i));
    }
    dt->GetGeomToleranceLabels(tols);
    for (int i = 1; i <= tols.Length(); i++) {
      Handle(XCAFDoc_GeomTolerance) at;
      if (!tols.Value(i).FindAttribute(XCAFDoc_GeomTolerance::GetID(), at) || at->GetObject().IsNull()) continue;
      Handle(XCAFDimTolObjects_GeomToleranceObject) o = at->GetObject();
      std::string datums;
      TDF_LabelSequence ds;
      if (XCAFDoc_DimTolTool::GetDatumOfTolerLabels(tols.Value(i), ds))
        for (int k = 1; k <= ds.Length(); k++) {
          Handle(XCAFDoc_Datum) da;
          if (ds.Value(k).FindAttribute(XCAFDoc_Datum::GetID(), da) && !da->GetObject().IsNull()) datums += " | " + hstr(da->GetObject()->GetName());
        }
      const bool dia = o->GetTypeOfValue() == XCAFDimTolObjects_GeomToleranceTypeValue_Diameter;
      item(1, std::string(tolTypeName(o->GetType())) + "  " + (dia ? "\xC3\x98" : "") + gnum(o->GetValue()) + datums, "Geometric tolerance",
           o->HasPointText(), o->HasPointText() ? o->GetPointTextAttach() : gp_Pnt(), o->GetPresentation(), tols.Value(i));
    }
  }
  js << "]}";
  return js.str();
}

static std::string cmdRefine(const Req& r, int id) {
  std::string path = r.str.count("path") ? r.str.at("path") : "";
  std::string out = r.str.count("out") ? r.str.at("out") : "";
  std::string err;
  Model* m = getModel(path, err);
  if (!m) return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
  int mi = r.numv.count("mesh") ? (int)r.numv.at("mesh") : -1;
  if (mi < 0 || mi >= (int)m->parts.size()) return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"bad mesh index\"}";
  PartInst& p = m->parts[mi];
  std::vector<int> faces = r.arr.count("faces") ? r.arr.at("faces") : std::vector<int>();
  double lin = r.numv.count("lin") ? r.numv.at("lin") : 0.01;
  double ang = r.numv.count("ang") ? r.numv.at("ang") : 0.3;
  std::vector<TopoDS_Shape> sel;
  for (int f : faces) if (f >= 0 && f < (int)p.faces.size()) { BRepTools::Clean(p.faces[f]); sel.push_back(p.faces[f]); }
  meshShapes(sel, lin, ang);
  std::vector<MeshOut> meshes(1);
  MeshOut& o = meshes[0];
  o.name = p.name;
  bool anyFace = false;
  std::vector<std::array<uint8_t, 3>> fc;
  for (int f : faces) if (f >= 0 && f < (int)p.faces.size())
    emitFace(o, p.faces[f], TopLoc_Location(), p.faceHasColor.empty() || !p.faceHasColor[f] ? nullptr : &p.faceColor[f], p.hasColor, p.color, anyFace, &fc);
  if (anyFace) { for (auto& c : fc) { o.col.push_back(c[0]); o.col.push_back(c[1]); o.col.push_back(c[2]); } }
  if (!writeGvc(out, "{\"name\":\"\",\"meshes\":[0],\"children\":[]}", meshes)) return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"cannot write output\"}";
  return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"tris\":" + std::to_string(o.idx.size() / 3) + "}";
}

// ------------------------------------------------------------------ items: parts placed by the app
// Requests reference parts as (paths[pathIdx[k]], mesh[k]) plus the part's 4x4 world matrix as the app
// shows it (column-major, as three.js stores it; includes the placement from the file).
struct Item { PartInst* part; TopoDS_Shape shape; int k; };

static gp_Trsf trsfFrom(const std::vector<double>& m, size_t k) {
  gp_Trsf t;
  if (m.size() < (k + 1) * 16) return t;
  const double* e = &m[k * 16];
  t.SetValues(e[0], e[4], e[8], e[12], e[1], e[5], e[9], e[13], e[2], e[6], e[10], e[14]);
  return t;
}

static bool collectItems(const Req& r, std::vector<Item>& items, std::string& err) {
  const auto& paths = r.sarr.count("paths") ? r.sarr.at("paths") : std::vector<std::string>();
  const auto& pidx = r.arr.count("pathIdx") ? r.arr.at("pathIdx") : std::vector<int>();
  const auto& mesh = r.arr.count("mesh") ? r.arr.at("mesh") : std::vector<int>();
  const auto& mat = r.darr.count("mat") ? r.darr.at("mat") : std::vector<double>();
  for (size_t k = 0; k < mesh.size(); k++) {
    int pi = k < pidx.size() ? pidx[k] : 0;
    if (pi < 0 || pi >= (int)paths.size()) { err = "bad path index"; return false; }
    Model* m = getModel(paths[pi], err);
    if (!m) return false;
    if (mesh[k] < 0 || mesh[k] >= (int)m->parts.size()) { err = "bad mesh index"; return false; }
    PartInst* p = &m->parts[mesh[k]];
    gp_Trsf t = trsfFrom(mat, k);
    // the app sends the part's complete world matrix (file placement included, geometry is in part coordinates)
    TopoDS_Shape s = p->shape.Moved(TopLoc_Location(t));
    items.push_back({p, s, (int)k});
  }
  return !items.empty() || (err = "nothing selected", false);
}

static std::string cmdMass(const Req& r, int id) {
  std::vector<Item> items;
  std::string err;
  if (!collectItems(r, items, err)) return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
  const auto& dens = r.darr.count("density") ? r.darr.at("density") : std::vector<double>();
  GProp_GProps massP, volP;
  double area = 0;
  int solids = 0, open = 0;
  for (auto& it : items) {
    for (TopExp_Explorer ex(it.shape, TopAbs_SOLID); ex.More(); ex.Next()) solids++;
    for (TopExp_Explorer ex(it.shape, TopAbs_SHELL, TopAbs_SOLID); ex.More(); ex.Next()) open++;
    GProp_GProps vp;
    BRepGProp::VolumeProperties(it.shape, vp, true);
    double rho = it.k < (int)dens.size() ? dens[it.k] : 1.0;
    massP.Add(vp, rho);
    volP.Add(vp, 1.0);
    GProp_GProps sp;
    BRepGProp::SurfaceProperties(it.shape, sp);
    area += sp.Mass();
  }
  double mass = massP.Mass();
  gp_Pnt c = massP.CentreOfMass();
  gp_Mat I = massP.MatrixOfInertia(); // about the centre of mass, density-weighted (g*mm^2 with g/mm^3)
  GProp_PrincipalProps pp = massP.PrincipalProperties();
  double i1, i2, i3;
  pp.Moments(i1, i2, i3);
  std::ostringstream o;
  o << "{\"id\":" << id << ",\"ok\":true,\"exact\":true,\"volume\":" << num(volP.Mass()) << ",\"area\":" << num(area) << ",\"mass\":" << num(mass)
    << ",\"cog\":[" << num(c.X()) << "," << num(c.Y()) << "," << num(c.Z()) << "],\"inertia\":[";
  for (int a = 1; a <= 3; a++) for (int b = 1; b <= 3; b++) o << (a + b > 2 ? "," : "") << num(I(a, b));
  o << "],\"principal\":[" << num(i1) << "," << num(i2) << "," << num(i3) << "],\"solids\":" << solids << ",\"openShells\":" << open << "}";
  return o.str();
}

static bool writeStl(const std::string& path, const std::vector<Item>& items, double lin, double ang) {
  std::vector<TopoDS_Shape> shapes;
  for (auto& it : items) shapes.push_back(it.shape);
  meshShapes(shapes, lin, ang);
  std::vector<float> tris; // 12 floats per triangle: normal + 3 vertices
  for (auto& it : items) {
    for (TopExp_Explorer ex(it.shape, TopAbs_FACE); ex.More(); ex.Next()) {
      const TopoDS_Face& f = TopoDS::Face(ex.Current());
      TopLoc_Location fl;
      Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(f, fl);
      if (tri.IsNull()) continue;
      gp_Trsf T = fl.Transformation();
      bool rev = f.Orientation() == TopAbs_REVERSED;
      for (int t = 1; t <= tri->NbTriangles(); t++) {
        int a, b, c;
        tri->Triangle(t).Get(a, b, c);
        if (rev) std::swap(b, c);
        gp_Pnt p[3] = {tri->Node(a).Transformed(T), tri->Node(b).Transformed(T), tri->Node(c).Transformed(T)};
        gp_Vec n = gp_Vec(p[0], p[1]).Crossed(gp_Vec(p[0], p[2]));
        double l = n.Magnitude();
        if (l > 0) n /= l;
        tris.push_back((float)n.X()); tris.push_back((float)n.Y()); tris.push_back((float)n.Z());
        for (auto& q : p) { tris.push_back((float)q.X()); tris.push_back((float)q.Y()); tris.push_back((float)q.Z()); }
      }
    }
  }
  FILE* fp = OSD_OpenFile(path.c_str(), "wb");
  if (!fp) return false;
  char header[80] = {0};
  snprintf(header, sizeof header, "GlassView STL export");
  fwrite(header, 1, 80, fp);
  uint32_t n = (uint32_t)(tris.size() / 12);
  fwrite(&n, 4, 1, fp);
  uint16_t attr = 0;
  for (uint32_t t = 0; t < n; t++) { fwrite(&tris[t * 12], 4, 12, fp); fwrite(&attr, 2, 1, fp); }
  fclose(fp);
  return true;
}

static bool writeStep(const std::string& path, const std::vector<Item>& items, const Req& r) {
  const auto& names = r.sarr.count("names") ? r.sarr.at("names") : std::vector<std::string>();
  const auto& cols = r.darr.count("colors") ? r.darr.at("colors") : std::vector<double>();
  Handle(TDocStd_Document) doc;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
  Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
  for (auto& it : items) {
    TDF_Label l = st->AddShape(it.shape, false);
    std::string nm = it.k < (int)names.size() && !names[it.k].empty() ? names[it.k] : it.part->name;
    TDF_Label ref;  // located shapes become an instance of a base product: name both
    const bool hasRef = st->GetReferredShape(l, ref);
    if (!nm.empty()) {
      TDataStd_Name::Set(l, TCollection_ExtendedString(nm.c_str(), true));
      if (hasRef) TDataStd_Name::Set(ref, TCollection_ExtendedString(nm.c_str(), true));
    }
    if ((size_t)(it.k * 3 + 2) < cols.size() && cols[it.k * 3] >= 0)
      ct->SetColor(hasRef ? ref : l, Quantity_Color(cols[it.k * 3], cols[it.k * 3 + 1], cols[it.k * 3 + 2], Quantity_TOC_sRGB), XCAFDoc_ColorSurf);
    else if (it.part->hasColor) ct->SetColor(hasRef ? ref : l, it.part->color, XCAFDoc_ColorSurf);
  }
  STEPCAFControl_Writer w;
  w.SetColorMode(true);
  w.SetNameMode(true);
  if (!w.Transfer(doc, STEPControl_AsIs)) return false;
  return w.Write(path.c_str()) == IFSelect_RetDone;
}

static std::string cmdExport(const Req& r, int id) {
  std::vector<Item> items;
  std::string err;
  if (!collectItems(r, items, err)) return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
  std::string fmt = r.str.count("format") ? r.str.at("format") : "step";
  std::string out = r.str.count("out") ? r.str.at("out") : "";
  bool ok;
  if (fmt == "stl") {
    double lin = r.numv.count("lin") ? r.numv.at("lin") : 0.01;
    double ang = r.numv.count("ang") ? r.numv.at("ang") : 0.1;
    ok = writeStl(out, items, lin, ang);
  } else ok = writeStep(out, items, r);
  if (!ok) return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"export failed\"}";
  return "{\"id\":" + std::to_string(id) + ",\"ok\":true}";
}

int main(int argc, char** argv) {
  // silence OCCT's own console output: stdout is our protocol channel
  Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_Printer));
  Interface_Static::SetCVal("xstep.cascade.unit", "MM");
  std::ios::sync_with_stdio(false);
  // one-shot CLI for testing: gvkernel load <in> <out> [ratio]
  if (argc >= 3 && std::string(argv[1]) == "pmi") {
    Req r;
    r.str["path"] = argv[2];
    std::cout << cmdPmi(r, 0) << std::endl;
    return 0;
  }
  if (argc >= 4 && std::string(argv[1]) == "load") {
    Req r;
    r.str["path"] = argv[2]; r.str["out"] = argv[3]; r.str["linType"] = "ratio";
    r.numv["lin"] = argc >= 5 ? atof(argv[4]) : 0.0008; r.numv["ang"] = argc >= 6 ? atof(argv[5]) : 0.25;
    std::cout << cmdLoad(r, 0) << std::endl;
    return 0;
  }
  std::cout << "{\"ready\":true,\"version\":\"" << KERNEL_VERSION << "\",\"threads\":" << OSD_Parallel::NbLogicalProcessors() << "}" << std::endl;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    Req r = parseReq(line);
    int id = r.numv.count("id") ? (int)r.numv.at("id") : 0;
    std::string cmd = r.str.count("cmd") ? r.str.at("cmd") : "";
    std::string reply;
    g_reqId = id;
    try {
      if (cmd == "load") reply = cmdLoad(r, id);
      else if (cmd == "refine") reply = cmdRefine(r, id);
      else if (cmd == "pmi") reply = cmdPmi(r, id);
      else if (cmd == "mass") reply = cmdMass(r, id);
      else if (cmd == "export") reply = cmdExport(r, id);
      else if (cmd == "ping") reply = "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"version\":\"" + KERNEL_VERSION + "\"}";
      else if (cmd == "quit") break;
      else reply = "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"unknown command\"}";
    } catch (Standard_Failure& e) {
      reply = "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" + jsonEscape(e.GetMessageString() ? e.GetMessageString() : "OCCT failure") + "\"}";
    } catch (std::exception& e) {
      reply = "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" + jsonEscape(e.what()) + "\"}";
    } catch (...) {
      reply = "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"unknown failure\"}";
    }
    { std::lock_guard<std::mutex> g(g_outMutex); std::cout << reply << std::endl; }
    g_reqId = 0;
  }
  return 0;
}
