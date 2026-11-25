// GlassView Native – tessellation cache. A STEP / IGES file that was opened before is reloaded
// from a compact binary B-rep (with its triangulation) instead of being parsed and meshed again.
// Cache files live in %LOCALAPPDATA%\GlassView\cache (Linux: ~/.cache/glassview-native).
#include "model.hpp"

#include <BinTools.hxx>
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <Standard_Failure.hxx>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace fs = std::filesystem;

namespace gv {

static const char kMagic[8] = {'G', 'V', 'N', 'C', 'A', 'C', 'H', '2'};

static fs::path cacheDir() {
#ifdef _WIN32
  const wchar_t* la = _wgetenv(L"LOCALAPPDATA");
  fs::path d = la ? fs::path(la) : fs::temp_directory_path();
  d = d / L"GlassView" / L"cache";
#else
  const char* x = getenv("XDG_CACHE_HOME");
  const char* h = getenv("HOME");
  fs::path d = x ? fs::path(x) : fs::path(h ? h : "/tmp") / ".cache";
  d = d / "glassview-native";
#endif
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

/** Key: path, size, modification time, quality, cache format. */
static bool keyOf(const std::string& path, Quality q, std::string& key, std::string& file) {
  std::error_code ec;
  const fs::path p = fs::u8path(path);
  const auto size = fs::file_size(p, ec);
  if (ec) return false;
  const auto mt = fs::last_write_time(p, ec);
  if (ec) return false;
  std::ostringstream k;
  k << path << '|' << size << '|' << (long long)mt.time_since_epoch().count() << "|q" << (int)q << "|v2";
  key = k.str();
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
  char buf[32];
  snprintf(buf, sizeof buf, "%016llx.gvn", (unsigned long long)h);
  file = buf;
  return true;
}

// ---- tiny binary helpers
template <class T> static void put(std::ostream& o, const T& v) { o.write((const char*)&v, sizeof v); }
template <class T> static bool get(std::istream& i, T& v) { return (bool)i.read((char*)&v, sizeof v); }
static void putS(std::ostream& o, const std::string& s) { uint32_t n = (uint32_t)s.size(); put(o, n); o.write(s.data(), n); }
static bool getS(std::istream& i, std::string& s) { uint32_t n; if (!get(i, n) || n > (1u << 24)) return false; s.resize(n); return (bool)i.read(s.data(), n); }
static void putC(std::ostream& o, const Quantity_Color& c) { put(o, c.Red()); put(o, c.Green()); put(o, c.Blue()); }
static bool getC(std::istream& i, Quantity_Color& c) { double r, g, b; if (!get(i, r) || !get(i, g) || !get(i, b)) return false; c = Quantity_Color(r, g, b, Quantity_TOC_RGB); return true; }
static void putT(std::ostream& o, const gp_Trsf& t) {
  for (int r = 1; r <= 3; r++) for (int c = 1; c <= 4; c++) put(o, t.Value(r, c));
}
static bool getT(std::istream& i, gp_Trsf& t) {
  double v[12];
  for (double& x : v) if (!get(i, x)) return false;
  try { t.SetValues(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]); } catch (...) { return false; }
  return true;
}

static void putNode(std::ostream& o, const Node& n) {
  putS(o, n.name);
  put(o, (uint32_t)n.parts.size());
  for (int p : n.parts) put(o, (int32_t)p);
  put(o, (uint32_t)n.kids.size());
  for (auto& k : n.kids) putNode(o, *k);
}
static bool getNode(std::istream& i, Node& n, int depth) {
  if (depth > 200 || !getS(i, n.name)) return false;
  uint32_t np, nk;
  if (!get(i, np) || np > (1u << 24)) return false;
  n.parts.resize(np);
  for (auto& p : n.parts) { int32_t v; if (!get(i, v)) return false; p = v; }
  if (!get(i, nk) || nk > (1u << 24)) return false;
  for (uint32_t k = 0; k < nk; k++) {
    auto c = std::make_unique<Node>();
    c->parent = &n;
    if (!getNode(i, *c, depth + 1)) return false;
    n.kids.push_back(std::move(c));
  }
  return true;
}

bool cacheLoad(const std::string& path, Quality q, Model& m) {
  try {
    std::string key, file;
    if (!keyOf(path, q, key, file)) return false;
    const fs::path f = cacheDir() / file;
    std::ifstream in(f, std::ios::binary);
    if (!in) return false;
    char magic[8];
    std::string k2;
    if (!in.read(magic, 8) || memcmp(magic, kMagic, 8) != 0 || !getS(in, k2) || k2 != key) return false;
    // shapes: one compound of the unique part shapes (with triangulation)
    TopoDS_Shape comp;
    BinTools::Read(comp, in);
    if (comp.IsNull()) return false;
    std::vector<TopoDS_Shape> uniq;
    for (TopoDS_Iterator it(comp, false, false); it.More(); it.Next()) uniq.push_back(it.Value());
    std::vector<TopTools_IndexedMapOfShape> maps(uniq.size());
    for (size_t u = 0; u < uniq.size(); u++) TopExp::MapShapes(uniq[u], maps[u]);
    uint32_t np;
    if (!get(in, np) || np > (1u << 24)) return false;
    m.parts.resize(np);
    for (auto& p : m.parts) {
      int32_t u;
      uint8_t hc;
      gp_Trsf t;
      if (!getS(in, p.name) || !get(in, u) || u < 0 || u >= (int32_t)uniq.size() || !getT(in, t) || !get(in, hc) || !getC(in, p.color)) return false;
      p.shape = uniq[u];
      p.loc = TopLoc_Location(t);
      p.hasColor = hc != 0;
      uint32_t ns;
      if (!get(in, ns) || ns > (1u << 24)) return false;
      for (uint32_t s = 0; s < ns; s++) {
        int32_t idx;
        Quantity_Color c;
        if (!get(in, idx) || !getC(in, c)) return false;
        if (idx >= 1 && idx <= maps[u].Extent()) p.subColors.emplace_back(maps[u](idx), c);
      }
      uint32_t ne;
      if (!get(in, ne) || ne > (1u << 26)) return false;
      BRep_Builder b;
      b.MakeCompound(p.softEdges);
      for (uint32_t e = 0; e < ne; e++) {
        int32_t idx;
        if (!get(in, idx)) return false;
        if (idx >= 1 && idx <= maps[u].Extent()) { b.Add(p.softEdges, maps[u](idx)); p.hasSoft = true; }
      }
      uint64_t tr;
      if (!get(in, tr)) return false;
      p.tris = (size_t)tr;
      m.tris += p.tris;
    }
    m.root = std::make_unique<Node>();
    if (!getNode(in, *m.root, 0)) return false;
    for (auto& p : m.parts) BRepBndLib::Add(p.shape.Moved(p.loc), m.box, true);
    std::error_code ec;
    fs::last_write_time(f, fs::file_time_type::clock::now(), ec); // LRU
    return true;
  } catch (...) {
    return false;
  }
}

static void prune(const fs::path& dir, uintmax_t limit) {
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, fs::path>> files;
  uintmax_t total = 0;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".gvn") continue;
    total += e.file_size(ec);
    files.push_back({e.last_write_time(ec), e.path()});
  }
  if (total <= limit) return;
  std::sort(files.begin(), files.end());
  for (auto& f : files) {
    if (total <= limit * 8 / 10) break;
    total -= fs::file_size(f.second, ec);
    fs::remove(f.second, ec);
  }
}

void cacheSave(const std::string& path, Quality q, const Model& m) {
  try {
    if (!m.error.empty() || m.parts.empty() || !m.pmi.empty()) return;  // PMI files are always read fresh
    for (auto& p : m.parts) if (p.isMesh) return;
    std::string key, file;
    if (!keyOf(path, q, key, file)) return;
    const fs::path dir = cacheDir(), f = dir / file, tmp = dir / (file + ".tmp");
    // unique shapes in first-use order
    TopTools_IndexedMapOfShape uniqMap;
    for (auto& p : m.parts) uniqMap.Add(p.shape);
    TopoDS_Compound comp;
    BRep_Builder b;
    b.MakeCompound(comp);
    for (int i = 1; i <= uniqMap.Extent(); i++) b.Add(comp, uniqMap(i));
    std::vector<TopTools_IndexedMapOfShape> maps(uniqMap.Extent());
    for (int i = 1; i <= uniqMap.Extent(); i++) TopExp::MapShapes(uniqMap(i), maps[i - 1]);
    {
      std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
      if (!o) return;
      o.write(kMagic, 8);
      putS(o, key);
      BinTools::Write(comp, o, Standard_True, Standard_True, BinTools_FormatVersion_CURRENT);
      put(o, (uint32_t)m.parts.size());
      for (auto& p : m.parts) {
        const int u = uniqMap.FindIndex(p.shape) - 1;
        putS(o, p.name);
        put(o, (int32_t)u);
        putT(o, p.loc.Transformation());
        put(o, (uint8_t)(p.hasColor ? 1 : 0));
        putC(o, p.color);
        put(o, (uint32_t)p.subColors.size());
        for (auto& sc : p.subColors) { put(o, (int32_t)maps[u].FindIndex(sc.first)); putC(o, sc.second); }
        std::vector<int32_t> se;
        if (p.hasSoft) {
          TopTools_IndexedMapOfShape em;
          TopExp::MapShapes(p.softEdges, TopAbs_EDGE, em);
          for (int e = 1; e <= em.Extent(); e++) { const int idx = maps[u].FindIndex(em(e)); if (idx > 0) se.push_back(idx); }
        }
        put(o, (uint32_t)se.size());
        for (int32_t v : se) put(o, v);
        put(o, (uint64_t)p.tris);
      }
      putNode(o, *m.root);
      if (!o) { o.close(); std::error_code ec; fs::remove(tmp, ec); return; }
    }
    std::error_code ec;
    fs::rename(tmp, f, ec);
    if (ec) { fs::remove(f, ec); fs::rename(tmp, f, ec); }
    prune(dir, (uintmax_t)2 << 30);  // keep the cache below ~2 GB
  } catch (...) {
  }
}

void cacheClear() {
  std::error_code ec;
  for (auto& e : fs::directory_iterator(cacheDir(), ec)) if (e.path().extension() == ".gvn") fs::remove(e.path(), ec);
}

uintmax_t cacheSize() {
  std::error_code ec;
  uintmax_t t = 0;
  for (auto& e : fs::directory_iterator(cacheDir(), ec)) if (e.path().extension() == ".gvn") t += e.file_size(ec);
  return t;
}

} // namespace gv
