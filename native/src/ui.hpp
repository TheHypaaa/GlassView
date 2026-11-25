// GlassView Native – glass-style interface widgets on top of Dear ImGui.
#pragma once
#include <cstdint>
#include <imgui.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "model.hpp"

enum class Icon { Open, Tree, Fit, Ortho, Persp, Shaded, Edges, Wire, Mesh, Xray, Theme, Settings, Plus, Close, Eye, EyeOff, Chevron, ChevronDown, Cube, Ruler, Cut, Move, Mass, AM, Palette, Insert, Explode, Compare };

class Ui {
public:
  void init(float scale);
  void setTheme(bool night);
  void beginFrame();
  void endFrame() {}

  // containers
  void capsule(const char* id, ImVec2 pos, const std::function<void()>& body);
  void capsuleCentered(const char* id, float top, const std::function<void()>& body);
  void capsuleRight(const char* id, float top, const std::function<void()>& body);
  void panel(const char* title, ImVec2 pos, ImVec2 size, const std::function<void()>& body, const std::string& right = "");
  void settings(ImVec2 W, float m, const std::function<void()>& body);

  // controls
  bool iconButton(Icon ic, const char* tip, bool active = false);
  bool iconTextButton(Icon ic, const char* label, const char* tip);
  bool textButton(const char* label, const char* tip, bool active = false);
  void separator();
  bool segmented(const char* id, const char* const* items, int n, int& value);
  bool toggle(const char* label, bool& v);
  bool chip(const char* label, bool on, ImU32 dot);
  bool pillButton(const char* label, bool primary = false, float width = 0);
  void panelHeader(const char* title, bool* close);
  bool numberField(const char* id, double& v, const char* unit, float width);
  void label(const char* t);
  void muted(const char* t);
  void divider();
  void keyGrid(const std::vector<std::pair<const char*, const char*>>& rows);

  // overlays
  bool emptyState(ImVec2 W, const std::vector<std::string>& recent, std::string& picked);
  void toast(ImVec2 W, const char* text, bool err, bool spinner);
  void infoChip(ImVec2 bottomLeft, const char* l1, const char* l2, const char* l3, float width = 0);
  void tabs(ImVec2 W, const std::vector<std::string>& names, const std::vector<bool>& loading, int& active, int& close, bool& plus);
  void hoverTag(ImVec2 p, const char* text);

  // tree
  void tree(gv::Node* root, gv::Node* selected, gv::Node*& clicked, bool& visChanged, const std::function<ImU32(const gv::Node*)>& dotColor);
  gv::Node* rightClicked = nullptr;
  float infoH = 0;
  float capW[3] = {0, 0, 0}; // widths of the three top capsules (last frame)
  float topBar = 56;          // top of the side panels (unscaled px); 108 when the toolbar wraps
  bool heightChanged = false; // ask the app for one more frame        // height of the info bar last frame (tree panel ends above it)
  uint64_t blurTex = 0;   // blurred 3D view behind the glass (0 = flat frosted)
  bool solidNext = false; // next glass window nearly opaque (menus, sheets)
  void glassWindow(const char* id, ImVec2 pos, ImVec2 pivot, float width, const std::function<void()>& body);
  ImU32 fg() const { return cFg; }
  ImU32 fg2() const { return cFg2; }
  ImU32 sep() const { return cSep; }
  ImU32 hover() const { return cHover; }
  ImFont* bold() const { return fBold; }
  ImFont* small() const { return fSmall; }
  float sc() const { return s; }
  bool isNight() const { return night; }
  bool scrollToSelected = false;

  static void drawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float size, ImU32 col);

private:
  void glassBegin(const char* id, ImVec2 pos, ImVec2 pivot, float rounding, ImVec2 pad, ImVec2 size = ImVec2(0, 0));
  void glassEnd(float rounding);
  void treeRow(gv::Node* n, int depth, gv::Node* selected, gv::Node*& clicked, bool& visChanged, const std::function<ImU32(const gv::Node*)>& dotColor);
  void tip(const char* t);
  float s = 1;
  bool night = false;
  ImFont* fReg = nullptr;
  ImFont* fBold = nullptr;
  ImFont* fSmall = nullptr;
  ImFont* fTitle = nullptr;
  ImU32 cFg = 0, cFg2 = 0, cHover = 0, cSep = 0, cGlass = 0, cGlassStrong = 0, cRim = 0, cShadow = 0;
};
