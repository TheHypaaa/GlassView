// GlassView Native – shared state of the tools (measure, cut, move, colours, mass, AM …).
#pragma once
#include "app.hpp"
#include "units.hpp"
#include "version.hpp"

// compare.cpp
void compareOpen(App& a, const std::string& path);
void compareDialog(App& a);
void compareClose(App& a);
void comparePoll(App& a);
void compareDocShown(App& a);
void compareOverlay(App& a);
void comparePanel(App& a, float& y);
bool compareBusy();

// pmi.cpp
bool pmiAvailable(App& a);
void pmiSetActive(App& a, bool on);
void pmiDocShown(App& a);
void pmiOverlay(App& a);
void pmiPanel(App& a, float& y);

// shell.cpp
std::vector<std::string> recentFiles(App& a);
void addRecent(App& a, const std::string& path);
void clearRecent(App& a);
bool exportViewPng(App& a, const std::string& path, int factor);
bool writePng(const std::string& path, int w, int h, int comps, const unsigned char* data, int stride);
void checkForUpdates(App& a, bool silent);
void pollUpdates(App& a);
void updateShutdown();
bool updateBusy();
#include <TopoDS_Face.hxx>
#include <optional>
#include <BRepBndLib.hxx>

// ------------------------------------------------------------------ measuring entities
struct MEnt {
  enum K { Point, Axis, Plane } k = Point;
  // point
  gp_Pnt p;
  std::string kind = "surface"; // vertex, centre, sphere, mid, fcentre, edge, surface
  std::string on;               // surface type for points on faces
  // axis / line
  gp_Pnt o; gp_Dir d; double len = 0;
  int src = 0;                  // 0 line edge, 1 cylinder, 2 cone, 3 torus
  double r = 0, r0 = 0, r1 = 0, ang = 0, R = 0, rr = 0;
  int cond = 0;                 // cylinders: 0 centre, 1 near side, 2 far side
  // plane
  gp_Dir n;
  gp_Pnt click;
  bool fillet = false;
  int part = -1;
  bool hasCond() const { return k == Axis && src == 1 && r > 0; }
  gp_Pnt mid() const { return k == Axis ? o.Translated(gp_Vec(d) * (len / 2)) : k == Plane ? click : p; }
  gp_Pnt anchor() const { return k == Point ? p : click; }
};

struct MRes {
  std::string kind, main, sub;
  bool hasLine = false, angle = false, cond = false;
  gp_Pnt a, b, anchor;
};

struct Measurement {
  MEnt A, B;
  MRes res;
  bool hasLabelPos = false;
  gp_Pnt labelPos;
};

std::string describeType(const MEnt& e, std::string* detail = nullptr, int* cls = nullptr);
MRes measureEntities(const MEnt& A, const MEnt& B);

// ------------------------------------------------------------------ snap data per part (part-local)
struct SnapC { gp_Pnt p; double r = 0; bool fillet = false; int kind = 0; std::string on; }; // kind 0 centre, 1 sphere, 2 face centre
struct SnapAx { gp_Pnt a, b; gp_Dir d; int src = 1; double r = 0, r0 = 0, r1 = 0, ang = 0, R = 0, rr = 0; double span = 0; bool fillet = false; };
struct PartSnaps {
  bool built = false;
  std::vector<gp_Pnt> corners, mids;
  std::vector<SnapC> centres, fcentres;
  std::vector<SnapAx> axes;
};

// ------------------------------------------------------------------ per document
struct DocExt {
  std::vector<Measurement> meas;
  std::vector<PartSnaps> snaps;
  std::vector<std::optional<Quantity_Color>> colorOverride;
  Handle(AIS_InteractiveObject) dots;
  std::vector<Handle(AIS_InteractiveObject)> amPrs;
  double explode = 0;                 // 0 … 1
  std::vector<gp_Vec> exOff;          // offset currently applied per part
};
DocExt& ext(Doc& d);

// ------------------------------------------------------------------ global tool state
struct Features {
  // measure
  bool measureOn = false;
  int mmode = 0;               // 0 faces & axes, 1 points
  bool snap = true, showDots = true, xyz = false;
  bool show[6] = {true, true, true, true, true, false}; // corners, mids, centres, face centres, axes, fillets
  bool hasHover = false;
  MEnt hover;
  bool hoverSnapped = false, hoverOnAxis = false, hoverLocked = false;
  bool hasPending = false;
  MEnt pending;
  int axisPart = -1;           // part whose axes are shown / pickable
  Handle(AIS_InteractiveObject) lockObj;
  TopoDS_Face lockFace;
  int lockPart = -1;
  Handle(AIS_Shape) lockPrs, pendingPrs;
  Handle(SelectMgr_Filter) lockFilter;
  gp_Dir dotsDir;
  bool dotsDirty = true;
  // other tools
  bool pmiOn = false, explodeOn = false, compareOn = false;
  bool cutOn = false, moveOn = false, massOn = false, amOn = false, colorsOn = false;
};
Features& F();

// measure.cpp
void measureSetActive(App& a, bool on);
void measureAfterFlush(App& a);
bool measurePick(App& a);
void measureDrawOverlay(App& a);
void measureDrawPanel(App& a, ImVec2 pos);
void measureOnModifier(App& a, bool held);
void measureDocShown(App& a);
void measureBeforeRedraw(App& a);
void measureCancel(App& a);
void measureRelabel(App& a);
PartSnaps& snapsOf(App& a, Doc& d, int part);

// helpers (features.cpp)
bool toScreen(App& a, const gp_Pnt& p, ImVec2& out);
double worldPerPixel(App& a);
gp_Trsf partTrsf(Doc& d, int part);
Doc* activeDoc(App& a);
void requestFrames(App& a, int n = 3);
