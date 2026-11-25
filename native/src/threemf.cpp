// GlassView Native – 3MF export (3D printing): welded watertight meshes per part,
// part and face colours, current positions (moves / explode) and optional build orientation.
#include "features.hpp"

#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <Poly_Triangulation.hxx>
#include <unordered_map>

// ======================================================================== minimal ZIP (stored)
static uint32_t crc32(const std::string& d) {
  static uint32_t t[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : d) c = t[(c ^ ch) & 255] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

static void put16(std::string& s, uint16_t v) { s.push_back((char)(v & 255)); s.push_back((char)(v >> 8)); }
static void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 255)); }

static bool writeZip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& files) {
  FILE* f = fopenUtf8(path, "wb");
  if (!f) return false;
  std::string central;
  uint32_t offset = 0;
  bool ok = true;
  for (auto& [name, data] : files) {
    const uint32_t crc = crc32(data), size = (uint32_t)data.size();
    std::string h;
    put32(h, 0x04034b50); put16(h, 20); put16(h, 0); put16(h, 0); put16(h, 0); put16(h, 0x21);
    put32(h, crc); put32(h, size); put32(h, size); put16(h, (uint16_t)name.size()); put16(h, 0);
    h += name;
    ok &= fwrite(h.data(), 1, h.size(), f) == h.size();
    ok &= fwrite(data.data(), 1, data.size(), f) == data.size();
    std::string c;
    put32(c, 0x02014b50); put16(c, 20); put16(c, 20); put16(c, 0); put16(c, 0); put16(c, 0); put16(c, 0x21);
    put32(c, crc); put32(c, size); put32(c, size); put16(c, (uint16_t)name.size()); put16(c, 0); put16(c, 0); put16(c, 0); put16(c, 0);
    put32(c, 0); put32(c, offset);
    c += name;
    central += c;
    offset += (uint32_t)(h.size() + data.size());
  }
  std::string end;
  put32(end, 0x06054b50); put16(end, 0); put16(end, 0); put16(end, (uint16_t)files.size()); put16(end, (uint16_t)files.size());
  put32(end, (uint32_t)central.size()); put32(end, offset); put16(end, 0);
  ok &= fwrite(central.data(), 1, central.size(), f) == central.size();
  ok &= fwrite(end.data(), 1, end.size(), f) == end.size();
  fclose(f);
  return ok;
}

// ======================================================================== 3MF model
static std::string xmlEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break; case '\'': o += "&apos;"; break;
      default: if ((unsigned char)c >= 32 || c == '\t') o += c;
    }
  }
  return o;
}

static std::string hexColor(const Quantity_Color& c) {
  double r, g, b;
  c.Values(r, g, b, Quantity_TOC_sRGB);
  char buf[16];
  snprintf(buf, sizeof buf, "#%02X%02X%02XFF", (int)std::lround(std::clamp(r, 0.0, 1.0) * 255), (int)std::lround(std::clamp(g, 0.0, 1.0) * 255), (int)std::lround(std::clamp(b, 0.0, 1.0) * 255));
  return buf;
}

Quantity_Color partColor(Doc& d, int k);
bool faceColorOf(Doc& d, int k, const TopoDS_Face& f, Quantity_Color& c);

/**
 * Writes the given parts as a 3MF file. `extra` is applied after each part's placement
 * (identity, or the build-orientation transform). Returns an error text or "".
 */
std::string write3mf(App& a, Doc& d, const std::vector<int>& parts, const gp_Trsf& extra, const std::string& path, bool dropToPlate) {
  struct Obj { std::string name; std::vector<gp_XYZ> v; std::vector<std::array<int, 4>> t; };  // t: v1 v2 v3 material
  std::vector<Obj> objs;
  std::vector<std::string> mats;
  std::unordered_map<std::string, int> matIdx;
  auto mat = [&](const Quantity_Color& c) {
    const std::string h = hexColor(c);
    auto it = matIdx.find(h);
    if (it != matIdx.end()) return it->second;
    matIdx[h] = (int)mats.size();
    mats.push_back(h);
    return (int)mats.size() - 1;
  };
  Bnd_Box all;
  for (int k : parts) {
    const Part& p = d.model->parts[k];
    const gp_Trsf T = extra * partTrsf(d, k);
    Obj o;
    o.name = p.name;
    // weld coincident nodes across faces → watertight mesh for the slicer
    double x0 = 1e300, y0 = 1e300, z0 = 1e300, x1 = -1e300, y1 = -1e300, z1 = -1e300;
    for (TopExp_Explorer ex(p.shape, TopAbs_FACE); ex.More(); ex.Next()) {
      TopLoc_Location l;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), l);
      if (t.IsNull()) continue;
      for (int i = 1; i <= t->NbNodes(); i++) {
        gp_Pnt q = t->Node(i).Transformed(T * l.Transformation());
        x0 = std::min(x0, q.X()); y0 = std::min(y0, q.Y()); z0 = std::min(z0, q.Z());
        x1 = std::max(x1, q.X()); y1 = std::max(y1, q.Y()); z1 = std::max(z1, q.Z());
      }
    }
    const double tol = std::max(1e-7, 1e-7 * std::max({x1 - x0, y1 - y0, z1 - z0}));
    std::unordered_map<std::string, int> weld;
    const int defMat = mat(partColor(d, k));
    for (TopExp_Explorer ex(p.shape, TopAbs_FACE); ex.More(); ex.Next()) {
      const TopoDS_Face& fc = TopoDS::Face(ex.Current());
      TopLoc_Location l;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(fc, l);
      if (t.IsNull()) continue;
      const gp_Trsf M = T * l.Transformation();
      Quantity_Color fcol;
      const int m = faceColorOf(d, k, fc, fcol) ? mat(fcol) : defMat;
      std::vector<int> map(t->NbNodes() + 1);
      for (int i = 1; i <= t->NbNodes(); i++) {
        gp_XYZ q = t->Node(i).Transformed(M).XYZ();
        char key[96];
        snprintf(key, sizeof key, "%lld,%lld,%lld", (long long)std::llround(q.X() / tol), (long long)std::llround(q.Y() / tol), (long long)std::llround(q.Z() / tol));
        auto it = weld.find(key);
        if (it == weld.end()) { weld[key] = (int)o.v.size(); map[i] = (int)o.v.size(); o.v.push_back(q); all.Add(gp_Pnt(q)); }
        else map[i] = it->second;
      }
      const bool rev = fc.Orientation() == TopAbs_REVERSED;
      for (int i = 1; i <= t->NbTriangles(); i++) {
        int a1, a2, a3;
        t->Triangle(i).Get(a1, a2, a3);
        if (rev) std::swap(a2, a3);
        const int v1 = map[a1], v2 = map[a2], v3 = map[a3];
        if (v1 == v2 || v2 == v3 || v1 == v3) continue; // degenerate after welding
        o.t.push_back({v1, v2, v3, m});
      }
    }
    if (!o.t.empty()) objs.push_back(std::move(o));
  }
  if (objs.empty()) return "nothing to export (no tessellated parts)";

  // optionally sit the result on z = 0 and centre it on the plate origin
  gp_XYZ shift(0, 0, 0);
  if (dropToPlate && !all.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    all.Get(x0, y0, z0, x1, y1, z1);
    shift = gp_XYZ(-(x0 + x1) / 2, -(y0 + y1) / 2, -z0);
  }

  std::string x;
  x.reserve(1 << 20);
  x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  x += "<model unit=\"millimeter\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n";
  x += " <metadata name=\"Application\">GlassView " GV_VERSION "</metadata>\n";
  x += " <metadata name=\"Title\">" + xmlEsc(d.name) + "</metadata>\n";
  x += " <resources>\n  <basematerials id=\"1\">\n";
  for (size_t i = 0; i < mats.size(); i++) x += "   <base name=\"Colour " + std::to_string(i + 1) + "\" displaycolor=\"" + mats[i] + "\"/>\n";
  x += "  </basematerials>\n";
  char buf[160];
  for (size_t i = 0; i < objs.size(); i++) {
    const Obj& o = objs[i];
    const int firstMat = o.t.empty() ? 0 : o.t[0][3];
    x += "  <object id=\"" + std::to_string(i + 2) + "\" type=\"model\" name=\"" + xmlEsc(o.name) + "\" pid=\"1\" pindex=\"" + std::to_string(firstMat) + "\">\n   <mesh>\n    <vertices>\n";
    for (const gp_XYZ& v : o.v) {
      snprintf(buf, sizeof buf, "     <vertex x=\"%.6g\" y=\"%.6g\" z=\"%.6g\"/>\n", v.X() + shift.X(), v.Y() + shift.Y(), v.Z() + shift.Z());
      x += buf;
    }
    x += "    </vertices>\n    <triangles>\n";
    for (auto& t : o.t) {
      if (t[3] == firstMat) snprintf(buf, sizeof buf, "     <triangle v1=\"%d\" v2=\"%d\" v3=\"%d\"/>\n", t[0], t[1], t[2]);
      else snprintf(buf, sizeof buf, "     <triangle v1=\"%d\" v2=\"%d\" v3=\"%d\" pid=\"1\" p1=\"%d\"/>\n", t[0], t[1], t[2], t[3]);
      x += buf;
    }
    x += "    </triangles>\n   </mesh>\n  </object>\n";
  }
  x += " </resources>\n <build>\n";
  for (size_t i = 0; i < objs.size(); i++) x += "  <item objectid=\"" + std::to_string(i + 2) + "\"/>\n";
  x += " </build>\n</model>\n";

  const std::string types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
      " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
      " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
      "</Types>\n";
  const std::string rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
      " <Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
      "</Relationships>\n";
  if (!writeZip(path, {{"[Content_Types].xml", types}, {"_rels/.rels", rels}, {"3D/3dmodel.model", x}})) return "could not write the file";
  (void)a;
  return "";
}
