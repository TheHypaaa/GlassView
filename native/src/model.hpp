// GlassView Native – model loading (runs on a worker thread, no graphics here).
#pragma once
#include <TopoDS_Shape.hxx>
#include <TopoDS_Compound.hxx>
#include <TopLoc_Location.hxx>
#include <Quantity_Color.hxx>
#include <Bnd_Box.hxx>
#include <TDocStd_Document.hxx>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace gv {

enum class Quality { Coarse = 0, Normal = 1, Fine = 2, Ultra = 3 };

struct Part {
  std::string name;
  TopoDS_Shape shape;      // unmoved shape (instances share it)
  TopLoc_Location loc;     // instance placement
  bool hasColor = false;
  Quantity_Color color;
  std::vector<std::pair<TopoDS_Shape, Quantity_Color>> subColors; // face / solid colours from the file
  TopoDS_Compound softEdges; // tangent-continuous edges (drawn light in sharp/tangent mode)
  bool hasSoft = false;
  bool isMesh = false;     // STL etc. – no B-rep
  size_t tris = 0;
};

struct Node {
  std::string name;
  std::vector<int> parts;  // part indices directly under this node
  std::vector<std::unique_ptr<Node>> kids;
  Node* parent = nullptr;
  bool visible = true;
  bool open = true;
};

/** Product manufacturing information read from STEP AP242 (semantic + graphical). */
struct Pmi {
  int kind = 0;                    // 0 dimension, 1 geometric tolerance, 2 datum
  std::string title, detail;       // "Ø12 H7", "Diameter · +0.018 / 0"
  TopoDS_Shape pres;               // annotation polylines (may be null)
  bool hasPt = false;
  gp_Pnt pt;                       // text position
  std::vector<TopoDS_Shape> refs;  // referenced faces / edges (product frame)
};

struct Model {
  std::vector<Pmi> pmi;
  bool fromCache = false;   // reloaded from the tessellation cache
  std::string path, name;
  std::vector<Part> parts;
  std::unique_ptr<Node> root;
  Handle(TDocStd_Document) doc;
  Bnd_Box box;
  size_t tris = 0;
  double readMs = 0, meshMs = 0, totalMs = 0;
  std::string error;
};

/** Progress shared with the UI thread. */
struct LoadProgress {
  std::atomic<int> stage{0}; // 0 reading, 1 meshing, 2 edges, 3 done
  std::atomic<bool> cancel{false};
};

std::shared_ptr<Model> loadModel(const std::string& utf8Path, Quality q, LoadProgress& prog);

/** Tessellation cache (modelcache.cpp). */
bool cacheLoad(const std::string& path, Quality q, Model& m);
void cacheSave(const std::string& path, Quality q, const Model& m);
void cacheClear();
uintmax_t cacheSize();

/** Reads AP242 PMI from the XCAF document into m.pmi (pmi.cpp). */
void readPmi(Model& m);

/** Re-tessellate all B-rep parts at a new quality; returns the triangle count. */
size_t remesh(Model& m, Quality q);

std::string fileNameOf(const std::string& path);
std::string lowerExt(const std::string& path);
bool isSupported(const std::string& path);

} // namespace gv
