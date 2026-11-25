#include <map>
#include "ui.hpp"
#include "platform.hpp"
#include <imgui_internal.h>
#include <cmath>
#include <cstdio>

static const ImU32 kAccentCol = IM_COL32(10, 132, 255, 255);

// ======================================================================== setup
void Ui::init(float scale) {
  s = scale;
  ImGuiIO& io = ImGui::GetIO();
  const std::string reg = gv::uiFontPath(false), bold = gv::uiFontPath(true);
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 2;
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0x2190, 0x21FF, 0x0370, 0x03FF, 0x2200, 0x22FF, 0x00D7, 0x00D7, 0x25CF, 0x25CF, 0};
  if (!reg.empty()) {
    fReg = io.Fonts->AddFontFromFileTTF(reg.c_str(), 15.f * s, &cfg, ranges);
    fSmall = io.Fonts->AddFontFromFileTTF(reg.c_str(), 12.5f * s, &cfg, ranges);
  }
  if (!bold.empty()) {
    fBold = io.Fonts->AddFontFromFileTTF(bold.c_str(), 15.f * s, &cfg, ranges);
    fTitle = io.Fonts->AddFontFromFileTTF(bold.c_str(), 26.f * s, &cfg, ranges);
  }
  if (!fReg) { ImFontConfig c2; c2.SizePixels = 13.f * s; fReg = io.Fonts->AddFontDefault(&c2); }
  if (!fSmall) fSmall = fReg;
  if (!fBold) fBold = fReg;
  if (!fTitle) fTitle = fBold;
  io.FontDefault = fReg;

  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowBorderSize = 0;
  st.FrameBorderSize = 0;
  st.WindowRounding = 18 * s;
  st.FrameRounding = 10 * s;
  st.PopupRounding = 12 * s;
  st.ScrollbarSize = 8 * s;
  st.ScrollbarRounding = 4 * s;
  st.ItemSpacing = ImVec2(6 * s, 6 * s);
  st.FramePadding = ImVec2(10 * s, 6 * s);
  st.WindowPadding = ImVec2(8 * s, 8 * s);
  st.AntiAliasedFill = st.AntiAliasedLines = true;
  setTheme(false);
}

void Ui::setTheme(bool n) {
  night = n;
  ImGuiStyle& st = ImGui::GetStyle();
  ImVec4* c = st.Colors;
  if (!night) {
    cFg = IM_COL32(29, 29, 31, 255); cFg2 = IM_COL32(110, 112, 120, 255);
    cHover = IM_COL32(0, 0, 0, 16); cSep = IM_COL32(0, 0, 0, 26);
    cGlass = IM_COL32(255, 255, 255, 178); cGlassStrong = IM_COL32(255, 255, 255, 228);
    cRim = IM_COL32(255, 255, 255, 230); cShadow = IM_COL32(20, 30, 50, 255);
  } else {
    cFg = IM_COL32(242, 244, 248, 255); cFg2 = IM_COL32(160, 166, 178, 255);
    cHover = IM_COL32(255, 255, 255, 22); cSep = IM_COL32(255, 255, 255, 28);
    cGlass = IM_COL32(38, 42, 52, 190); cGlassStrong = IM_COL32(34, 38, 47, 236);
    cRim = IM_COL32(255, 255, 255, 46); cShadow = IM_COL32(0, 0, 0, 255);
  }
  c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(cFg);
  c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(cFg2);
  c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(cGlassStrong);
  c[ImGuiCol_Border] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(cHover);
  c[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ButtonHovered] = ImGui::ColorConvertU32ToFloat4(cHover);
  c[ImGuiCol_ButtonActive] = ImGui::ColorConvertU32ToFloat4(cSep);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = ImGui::ColorConvertU32ToFloat4(cSep);
  c[ImGuiCol_ScrollbarGrabHovered] = ImGui::ColorConvertU32ToFloat4(cFg2);
  c[ImGuiCol_Header] = ImVec4(0.04f, 0.52f, 1.f, 0.18f);
  c[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(cHover);
}

void Ui::beginFrame() {}

void Ui::tip(const char* t) {
  if (!t || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) return;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * s, 6 * s));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10 * s);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(cGlassStrong));
  ImGui::SetTooltip("%s", t);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

// ======================================================================== glass containers
void Ui::glassBegin(const char* id, ImVec2 pos, ImVec2 pivot, float rounding, ImVec2 pad, ImVec2 size) {
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
  if (size.x > 0) ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);
  ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoFocusOnAppearing;
  if (size.x <= 0) f |= ImGuiWindowFlags_AlwaysAutoResize;
  ImGui::Begin(id, nullptr, f);
  ImGui::PopStyleVar(2);
  // shadow behind, glass body, rim light
  ImVec2 p0 = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize(), p1(p0.x + sz.x, p0.y + sz.y);
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  const int a0 = night ? 34 : 16;
  for (int i = 1; i <= 5; i++) {
    float g = i * 3.2f * s;
    ImU32 col = (cShadow & 0x00FFFFFF) | ((ImU32)(a0 / i) << 24);
    bg->AddRectFilled(ImVec2(p0.x - g * 0.6f, p0.y - g * 0.2f + 4 * s), ImVec2(p1.x + g * 0.6f, p1.y + g + 4 * s), col, rounding + g);
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (blurTex && !solidNext) {
    // real frosted glass: the blurred 3D view under the panel, then a lighter tint
    const ImVec2 D = ImGui::GetIO().DisplaySize;
    dl->AddImageRounded((ImTextureID)blurTex, p0, p1, ImVec2(p0.x / D.x, 1 - p0.y / D.y), ImVec2(p1.x / D.x, 1 - p1.y / D.y), IM_COL32_WHITE, rounding);
    const ImU32 a = ((cGlass >> 24) & 255) * (night ? 82 : 72) / 100;
    dl->AddRectFilled(p0, p1, (cGlass & 0x00FFFFFF) | (a << 24), rounding);
  } else
    dl->AddRectFilled(p0, p1, solidNext ? ((cGlass & 0x00FFFFFF) | 0xF4000000) : cGlass, rounding);
  solidNext = false;
  dl->AddRect(ImVec2(p0.x + 0.5f, p0.y + 0.5f), ImVec2(p1.x - 0.5f, p1.y - 0.5f), cRim, rounding, 0, 1.0f * s);
  // soft specular sheen on the top half
  dl->AddRectFilledMultiColor(ImVec2(p0.x + rounding * 0.5f, p0.y + 1), ImVec2(p1.x - rounding * 0.5f, p0.y + sz.y * 0.45f),
                              IM_COL32(255, 255, 255, night ? 10 : 40), IM_COL32(255, 255, 255, night ? 10 : 40), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
}

void Ui::glassEnd(float) { ImGui::End(); }

void Ui::capsule(const char* id, ImVec2 pos, const std::function<void()>& body) {
  glassBegin(id, pos, ImVec2(0, 0), 22 * s, ImVec2(6 * s, 6 * s));
  body();
  capW[0] = ImGui::GetWindowWidth();
  glassEnd(22 * s);
}

void Ui::capsuleCentered(const char* id, float top, const std::function<void()>& body) {
  // centred if it fits between the left and right capsules, else squeezed into the gap,
  // else (very narrow window) on a second row – the panels then start lower
  const float W = ImGui::GetIO().DisplaySize.x, gap = 10 * s, cw = capW[1];
  const float leftEnd = top + capW[0] + gap, rightStart = W - top - capW[2] - gap;
  float x = W * 0.5f, y = top;
  topBar = 56;
  if (cw > 0 && (x - cw / 2 < leftEnd || x + cw / 2 > rightStart)) {
    if (leftEnd + cw <= rightStart) x = (leftEnd + rightStart) * 0.5f;
    else { y = top + 52 * s; topBar = 108; }
  }
  glassBegin(id, ImVec2(x, y), ImVec2(0.5f, 0), 22 * s, ImVec2(6 * s, 6 * s));
  body();
  capW[1] = ImGui::GetWindowWidth();
  glassEnd(22 * s);
}

void Ui::capsuleRight(const char* id, float top, const std::function<void()>& body) {
  glassBegin(id, ImVec2(ImGui::GetIO().DisplaySize.x - top, top), ImVec2(1, 0), 22 * s, ImVec2(6 * s, 6 * s));
  body();
  capW[2] = ImGui::GetWindowWidth();
  glassEnd(22 * s);
}

void Ui::panel(const char* title, ImVec2 pos, ImVec2 size, const std::function<void()>& body, const std::string& right) {
  glassBegin(title, pos, ImVec2(0, 0), 22 * s, ImVec2(10 * s, 12 * s), size);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6 * s);
  ImGui::PushFont(fBold);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (!right.empty()) {
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right.c_str()).x - 16 * s);
    ImGui::TextDisabled("%s", right.c_str());
  }
  ImGui::Dummy(ImVec2(0, 2 * s));
  ImGui::BeginChild("##body", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  body();
  ImGui::EndChild();
  glassEnd(22 * s);
}

void Ui::settings(ImVec2 W, float m, const std::function<void()>& body) {
  const float w = 340 * s;
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, std::max(200.f, W.y - (m + topBar * s) - m)));
  solidNext = blurTex == 0;
  glassBegin("##settings", ImVec2(W.x - m, m + topBar * s), ImVec2(1, 0), 22 * s, ImVec2(14 * s, 14 * s), ImVec2(0, 0));
  ImGui::PushFont(fBold);
  ImGui::TextUnformatted("Settings");
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, 2 * s));
  body();
  glassEnd(22 * s);
}

// ======================================================================== controls
bool Ui::iconButton(Icon ic, const char* t, bool active) {
  const float sz = 34 * s;
  ImGui::PushID((int)ic);
  ImGui::PushID(t);
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton("##ib", ImVec2(sz, sz));
  bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (active) dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), night ? IM_COL32(255, 255, 255, 34) : IM_COL32(255, 255, 255, 235), sz * 0.5f);
  else if (hov) dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), cHover, sz * 0.5f);
  if (active && !night) dl->AddRect(p, ImVec2(p.x + sz, p.y + sz), IM_COL32(0, 0, 0, 14), sz * 0.5f);
  drawIcon(dl, ic, ImVec2(p.x + sz * 0.5f, p.y + sz * 0.5f), 18 * s, active ? kAccentCol : cFg);
  tip(t);
  ImGui::PopID();
  ImGui::PopID();
  return clicked;
}

bool Ui::iconTextButton(Icon ic, const char* label, const char* t) {
  const float h = 34 * s;
  ImVec2 ts = ImGui::CalcTextSize(label);
  const float w = 12 * s + 18 * s + 8 * s + ts.x + 14 * s;
  ImGui::PushID(label);
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton("##itb", ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), cHover, h * 0.5f);
  drawIcon(dl, ic, ImVec2(p.x + 12 * s + 9 * s, p.y + h * 0.5f), 18 * s, cFg);
  dl->AddText(fBold, fBold->FontSize, ImVec2(p.x + 12 * s + 18 * s + 8 * s, p.y + (h - fBold->FontSize) * 0.5f), cFg, label);
  tip(t);
  ImGui::PopID();
  return clicked;
}

bool Ui::textButton(const char* label, const char* t, bool active) {
  const float h = 34 * s;
  ImGui::PushFont(fBold);
  ImVec2 ts = ImGui::CalcTextSize(label);
  const float w = std::max(h, ts.x + 22 * s);
  ImGui::PushID(label);
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton("##tb", ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (active) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), night ? IM_COL32(255, 255, 255, 34) : IM_COL32(255, 255, 255, 235), h * 0.5f);
  else if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), cHover, h * 0.5f);
  dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), active ? kAccentCol : cFg, label);
  ImGui::PopID();
  ImGui::PopFont();
  tip(t);
  return clicked;
}

void Ui::separator() {
  ImGui::SameLine();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 3 * s, p.y + 9 * s), ImVec2(p.x + 3 * s, p.y + 25 * s), cSep, 1.0f * s);
  ImGui::Dummy(ImVec2(6 * s, 34 * s));
  ImGui::SameLine();
}

bool Ui::segmented(const char* id, const char* const* items, int n, int& value) {
  ImGui::PushID(id);
  const float h = 30 * s;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float total = 0;
  std::vector<float> ws(n);
  for (int i = 0; i < n; i++) { ws[i] = ImGui::CalcTextSize(items[i]).x + 22 * s; total += ws[i]; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + total + 6 * s, p.y + h + 6 * s), cHover, (h + 6 * s) * 0.5f);
  bool changed = false;
  float x = p.x + 3 * s;
  for (int i = 0; i < n; i++) {
    ImGui::SetCursorScreenPos(ImVec2(x, p.y + 3 * s));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##seg", ImVec2(ws[i], h))) { if (value != i) { value = i; changed = true; } }
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (value == i) {
      dl->AddRectFilled(ImVec2(x, p.y + 3 * s), ImVec2(x + ws[i], p.y + 3 * s + h), night ? kAccentCol : IM_COL32(255, 255, 255, 250), h * 0.5f);
    } else if (hov) dl->AddRectFilled(ImVec2(x, p.y + 3 * s), ImVec2(x + ws[i], p.y + 3 * s + h), cHover, h * 0.5f);
    ImVec2 ts = ImGui::CalcTextSize(items[i]);
    dl->AddText(fBold, fBold->FontSize, ImVec2(x + (ws[i] - ts.x) * 0.5f, p.y + 3 * s + (h - fBold->FontSize) * 0.5f), value == i ? (night ? IM_COL32(255, 255, 255, 255) : kAccentCol) : cFg, items[i]);
    x += ws[i];
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 6 * s));
  ImGui::Dummy(ImVec2(total, 4 * s));
  ImGui::PopID();
  return changed;
}

bool Ui::toggle(const char* label, bool& v) {
  ImGui::PushID(label);
  const float w = 42 * s, h = 25 * s;
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float full = ImGui::GetContentRegionAvail().x;
  bool clicked = ImGui::InvisibleButton("##tg", ImVec2(full, h + 4 * s));
  if (clicked) v = !v;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(p.x, p.y + (h + 4 * s - ImGui::GetFontSize()) * 0.5f), cFg, label);
  ImVec2 a(p.x + full - w, p.y + 2 * s);
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), v ? IM_COL32(48, 209, 88, 255) : cSep, h * 0.5f);
  float kx = v ? a.x + w - h * 0.5f : a.x + h * 0.5f;
  dl->AddCircleFilled(ImVec2(kx, a.y + h * 0.5f + 1 * s), h * 0.5f - 2 * s, IM_COL32(0, 0, 0, 40));
  dl->AddCircleFilled(ImVec2(kx, a.y + h * 0.5f), h * 0.5f - 2 * s, IM_COL32(255, 255, 255, 255));
  ImGui::PopID();
  return clicked;
}

void Ui::label(const char* t) {
  ImGui::Dummy(ImVec2(0, 2 * s));
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(cFg2));
  ImGui::TextUnformatted(t);
  ImGui::PopStyleColor();
}

void Ui::muted(const char* t) {
  ImGui::PushFont(fSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(cFg2));
  ImGui::TextWrapped("%s", t);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void Ui::divider() {
  ImGui::Dummy(ImVec2(0, 4 * s));
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), cSep, 1.f);
  ImGui::Dummy(ImVec2(0, 6 * s));
}

void Ui::keyGrid(const std::vector<std::pair<const char*, const char*>>& rows) {
  // one row per shortcut: key chip in a fixed column, description after it
  ImGui::PushFont(fSmall);
  float kmax = 0;
  for (auto& r : rows) kmax = std::max(kmax, ImGui::CalcTextSize(r.first).x);
  const float kcol = kmax + 18 * s;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (auto& r : rows) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ks = ImGui::CalcTextSize(r.first);
    const float kw = ks.x + 10 * s, kh = ks.y + 4 * s;
    dl->AddRectFilled(p, ImVec2(p.x + kw, p.y + kh), cHover, 6 * s);
    dl->AddText(ImVec2(p.x + 5 * s, p.y + 2 * s), cFg, r.first);
    dl->AddText(ImVec2(p.x + kcol, p.y + 2 * s), cFg2, r.second);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, kh + 2 * s));
  }
  ImGui::PopFont();
}

bool Ui::chip(const char* label, bool on, ImU32 dot) {
  ImGui::PushID(label);
  ImVec2 ts = fSmall->CalcTextSizeA(fSmall->FontSize, 1e9f, 0, label);
  const float h = 26 * s, w = ts.x + 30 * s;
  ImVec2 p = ImGui::GetCursorScreenPos();
  if (p.x + w > ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 10 * s && p.x > ImGui::GetWindowPos().x + 20 * s) {
    ImGui::NewLine();
    p = ImGui::GetCursorScreenPos();
  }
  bool clicked = ImGui::InvisibleButton("##chip", ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (on) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), cHover, h * 0.5f);
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), on ? kAccentCol : cSep, h * 0.5f, 0, 1.0f * s);
  ImU32 dc = on ? dot : ((dot & 0x00FFFFFF) | 0x50000000);
  dl->AddCircleFilled(ImVec2(p.x + 12 * s, p.y + h * 0.5f), 4.5f * s, dc);
  dl->AddText(fSmall, fSmall->FontSize, ImVec2(p.x + 21 * s, p.y + (h - ts.y) * 0.5f), on ? cFg : cFg2, label);
  ImGui::PopID();
  ImGui::SameLine(0, 5 * s);
  return clicked;
}

bool Ui::pillButton(const char* label, bool primary, float width) {
  ImGui::PushID(label);
  ImVec2 ts = fBold->CalcTextSizeA(fBold->FontSize, 1e9f, 0, label);
  const float h = 32 * s, w = width > 0 ? width : ts.x + 28 * s;
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton("##pill", ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (primary) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? IM_COL32(40, 150, 255, 255) : kAccentCol, h * 0.5f);
  else dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? cSep : cHover, h * 0.5f);
  dl->AddText(fBold, fBold->FontSize, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), primary ? IM_COL32(255, 255, 255, 255) : cFg, label);
  ImGui::PopID();
  return clicked;
}

void Ui::panelHeader(const char* title, bool* close) {
  ImGui::PushFont(fBold);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (close) {
    ImGui::SameLine(ImGui::GetWindowWidth() - 38 * s);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(title);
    if (ImGui::InvisibleButton("##close", ImVec2(24 * s, 22 * s))) *close = false;
    if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 12 * s, p.y + 11 * s), 11 * s, cHover);
    drawIcon(ImGui::GetWindowDrawList(), Icon::Close, ImVec2(p.x + 12 * s, p.y + 11 * s), 11 * s, cFg2);
    ImGui::PopID();
  }
}

bool Ui::numberField(const char* id, double& v, const char* unit, float width) {
  ImGui::PushID(id);
  ImGui::SetNextItemWidth(width);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8 * s);
  // edit a shadow copy; commit on Enter / focus loss
  static std::map<ImGuiID, double> shadow;
  const ImGuiID key = ImGui::GetID("##v");
  auto it = shadow.find(key);
  double tmp = it != shadow.end() ? it->second : v;
  ImGui::InputDouble("##v", &tmp, 0, 0, "%.4g");
  bool ch = false;
  if (ImGui::IsItemActive()) shadow[key] = tmp;
  else if (it != shadow.end()) shadow.erase(key);
  if (ImGui::IsItemDeactivatedAfterEdit()) { v = tmp; ch = true; shadow.erase(key); }
  ImGui::PopStyleVar();
  if (unit && *unit) { ImGui::SameLine(0, 4 * s); ImGui::TextDisabled("%s", unit); }
  ImGui::PopID();
  return ch;
}

void Ui::glassWindow(const char* id, ImVec2 pos, ImVec2 pivot, float width, const std::function<void()>& body) {
  glassBegin(id, pos, pivot, 20 * s, ImVec2(14 * s, 12 * s), ImVec2(width, 0));
  body();
  glassEnd(20 * s);
}

// ======================================================================== overlays
bool Ui::emptyState(ImVec2 W, const std::vector<std::string>& recent, std::string& picked) {
  const float w = 380 * s;
  glassBegin("##empty", ImVec2(W.x * 0.5f, W.y * 0.47f), ImVec2(0.5f, 0.5f), 28 * s, ImVec2(28 * s, 26 * s), ImVec2(w, 0));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float cw = ImGui::GetContentRegionAvail().x;
  drawIcon(dl, Icon::Cube, ImVec2(p.x + cw * 0.5f, p.y + 28 * s), 46 * s, kAccentCol);
  ImGui::Dummy(ImVec2(0, 64 * s));
  ImGui::PushFont(fTitle);
  const char* h = "Open a model";
  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(h).x) * 0.5f);
  ImGui::TextUnformatted(h);
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(cFg2));
  const char* t = "Drop STEP, IGES, STL or BREP files anywhere, or";
  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(t).x) * 0.5f);
  ImGui::TextUnformatted(t);
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 8 * s));
  const float bw = 160 * s, bh = 40 * s;
  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
  ImVec2 bp = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton("##choose", ImVec2(bw, bh));
  dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), ImGui::IsItemHovered() ? IM_COL32(40, 150, 255, 255) : kAccentCol, bh * 0.5f);
  const char* bt = "Choose file…";
  ImVec2 bts = fBold->CalcTextSizeA(fBold->FontSize, 1e9f, 0, bt);
  dl->AddText(fBold, fBold->FontSize, ImVec2(bp.x + (bw - bts.x) * 0.5f, bp.y + (bh - bts.y) * 0.5f), IM_COL32(255, 255, 255, 255), bt);
  if (!recent.empty()) {
    ImGui::Dummy(ImVec2(0, 10 * s));
    divider();
    label("Recent");
    for (size_t i = 0; i < recent.size() && i < 6; i++) {
      const std::string& p = recent[i];
      const size_t sl = p.find_last_of("\\/");
      const std::string name = sl == std::string::npos ? p : p.substr(sl + 1), dir = sl == std::string::npos ? "" : p.substr(0, sl);
      ImGui::PushID((int)i);
      ImVec2 q = ImGui::GetCursorScreenPos();
      const float rw = ImGui::GetContentRegionAvail().x, rh = 38 * s;
      if (ImGui::InvisibleButton("##r", ImVec2(rw, rh))) picked = p;
      if (ImGui::IsItemHovered()) { dl->AddRectFilled(q, ImVec2(q.x + rw, q.y + rh), cHover, 10 * s); ImGui::SetTooltip("%s", p.c_str()); }
      drawIcon(dl, Icon::Cube, ImVec2(q.x + 16 * s, q.y + rh * 0.5f), 16 * s, cFg2);
      dl->AddText(fBold, fBold->FontSize, ImVec2(q.x + 34 * s, q.y + 3 * s), cFg, name.c_str());
      std::string d = dir.size() > 52 ? "…" + dir.substr(dir.size() - 50) : dir;
      dl->AddText(fSmall, fSmall->FontSize, ImVec2(q.x + 34 * s, q.y + 4 * s + fBold->FontSize), cFg2, d.c_str());
      ImGui::PopID();
    }
  }
  glassEnd(28 * s);
  return clicked;
}

void Ui::toast(ImVec2 W, const char* text, bool err, bool spinner) {
  glassBegin("##toast", ImVec2(W.x * 0.5f, 64 * s), ImVec2(0.5f, 0), 16 * s, ImVec2(14 * s, 9 * s));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (spinner) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 7 * s;
    ImVec2 c(p.x + r, p.y + ImGui::GetFontSize() * 0.5f + 1 * s);
    const float a0 = (float)ImGui::GetTime() * 6.f;
    dl->PathArcTo(c, r, a0, a0 + 4.2f, 24);
    dl->PathStroke(kAccentCol, 0, 2.2f * s);
    ImGui::Dummy(ImVec2(2 * r + 4 * s, 1));
    ImGui::SameLine();
  }
  if (err) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.27f, 0.23f, 1.f));
  ImGui::TextUnformatted(text);
  if (err) ImGui::PopStyleColor();
  glassEnd(16 * s);
}

void Ui::infoChip(ImVec2 bl, const char* l1, const char* l2, const char* l3, float width) {
  if (width > 0) ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
  glassBegin("##info", bl, ImVec2(0, 1), 16 * s, ImVec2(14 * s, 9 * s));
  if (width > 0) ImGui::PushTextWrapPos(0.0f);
  ImGui::PushFont(fBold);
  ImGui::TextUnformatted(l1);
  ImGui::PopFont();
  ImGui::PushFont(fSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(cFg2));
  ImGui::TextUnformatted(l2);
  ImGui::PopStyleColor();
  if (l3) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kAccentCol)); ImGui::TextUnformatted(l3); ImGui::PopStyleColor(); }
  ImGui::PopFont();
  if (width > 0) ImGui::PopTextWrapPos();
  const float hNow = ImGui::GetWindowHeight();
  if (std::fabs(hNow - infoH) > 0.5f) { infoH = hNow; heightChanged = true; }
  glassEnd(16 * s);
}

void Ui::tabs(ImVec2 W, const std::vector<std::string>& names, const std::vector<bool>& loading, int& active, int& close, bool& plus) {
  glassBegin("##tabs", ImVec2(W.x * 0.5f, W.y - 12 * s), ImVec2(0.5f, 1), 22 * s, ImVec2(5 * s, 5 * s));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (size_t i = 0; i < names.size(); i++) {
    if (i) ImGui::SameLine();
    ImGui::PushID((int)i);
    std::string n = names[i];
    if (n.size() > 28) n = n.substr(0, 26) + "…";
    ImVec2 ts = ImGui::CalcTextSize(n.c_str());
    const float h = 32 * s, w = ts.x + 20 * s + 22 * s + (loading[i] ? 16 * s : 0);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##tab", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    const bool on = (int)i == active;
    if (on) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), night ? IM_COL32(255, 255, 255, 34) : IM_COL32(255, 255, 255, 240), h * 0.5f);
    else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), cHover, h * 0.5f);
    float x = p.x + 12 * s;
    if (loading[i]) {
      const float a0 = (float)ImGui::GetTime() * 6.f;
      dl->PathArcTo(ImVec2(x + 5 * s, p.y + h * 0.5f), 5 * s, a0, a0 + 4.2f, 16);
      dl->PathStroke(kAccentCol, 0, 1.8f * s);
      x += 16 * s;
    }
    dl->AddText(ImVec2(x, p.y + (h - ts.y) * 0.5f), on ? kAccentCol : cFg, n.c_str());
    // close button
    ImVec2 cc(p.x + w - 16 * s, p.y + h * 0.5f);
    const bool overClose = ImGui::IsMouseHoveringRect(ImVec2(cc.x - 8 * s, cc.y - 8 * s), ImVec2(cc.x + 8 * s, cc.y + 8 * s));
    if (overClose) dl->AddCircleFilled(cc, 9 * s, cSep);
    drawIcon(dl, Icon::Close, cc, 10 * s, (hov || on) ? cFg : cFg2);
    if (clicked) { if (overClose) close = (int)i; else active = (int)i; }
    if (hov) tip(names[i].c_str());
    ImGui::PopID();
  }
  ImGui::SameLine();
  if (iconButton(Icon::Plus, "Open another file (Ctrl+O)")) plus = true;
  glassEnd(22 * s);
}

void Ui::hoverTag(ImVec2 p, const char* text) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImVec2 ts = fBold->CalcTextSizeA(fBold->FontSize * 0.9f, 1e9f, 0, text);
  ImVec2 a(p.x + 16 * s, p.y + 18 * s), b(a.x + ts.x + 18 * s, a.y + ts.y + 10 * s);
  dl->AddRectFilled(ImVec2(a.x, a.y + 3 * s), ImVec2(b.x, b.y + 3 * s), IM_COL32(0, 0, 0, night ? 60 : 22), 10 * s);
  dl->AddRectFilled(a, b, cGlassStrong, 10 * s);
  dl->AddRect(a, b, cRim, 10 * s);
  dl->AddText(fBold, fBold->FontSize * 0.9f, ImVec2(a.x + 9 * s, a.y + 5 * s), cFg, text);
}

// ======================================================================== tree
void Ui::tree(gv::Node* root, gv::Node* selected, gv::Node*& clicked, bool& visChanged, const std::function<ImU32(const gv::Node*)>& dotColor) {
  rightClicked = nullptr;
  treeRow(root, 0, selected, clicked, visChanged, dotColor);
  scrollToSelected = false;
}

void Ui::treeRow(gv::Node* n, int depth, gv::Node* selected, gv::Node*& clicked, bool& visChanged, const std::function<ImU32(const gv::Node*)>& dotColor) {
  const float h = 28 * s;
  const bool hasKids = !n->kids.empty();
  ImGui::PushID(n);
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  ImGui::InvisibleButton("##row", ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  const bool rowClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool sel = n == selected;
  if (sel) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(10, 132, 255, night ? 70 : 46), 9 * s);
  else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), cHover, 9 * s);
  if (sel && scrollToSelected) ImGui::SetScrollHereY(0.4f);
  float x = p.x + 4 * s + depth * 14 * s;
  const float cy = p.y + h * 0.5f;
  // chevron
  ImRect chev(ImVec2(x, p.y), ImVec2(x + 16 * s, p.y + h));
  if (hasKids) drawIcon(dl, n->open ? Icon::ChevronDown : Icon::Chevron, ImVec2(x + 8 * s, cy), 10 * s, cFg2);
  x += 18 * s;
  // colour dot
  dl->AddCircleFilled(ImVec2(x + 5 * s, cy), 5 * s, dotColor(n));
  dl->AddCircle(ImVec2(x + 5 * s, cy), 5 * s, IM_COL32(0, 0, 0, 40));
  x += 16 * s;
  // eye
  ImRect eye(ImVec2(p.x + w - 26 * s, p.y), ImVec2(p.x + w - 2 * s, p.y + h));
  const bool overEye = ImGui::IsMouseHoveringRect(eye.Min, eye.Max);
  const bool overChev = hasKids && ImGui::IsMouseHoveringRect(chev.Min, chev.Max);
  // name
  ImU32 tc = n->visible ? (sel ? kAccentCol : cFg) : cFg2;
  ImGui::PushClipRect(ImVec2(x, p.y), ImVec2(eye.Min.x - 4 * s, p.y + h), true);
  dl->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), tc, n->name.c_str());
  ImGui::PopClipRect();
  if (hasKids) {
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%zu", n->kids.size());
    ImVec2 cs = ImGui::CalcTextSize(cnt);
    dl->AddText(ImVec2(eye.Min.x - cs.x - 6 * s, cy - cs.y * 0.5f), cFg2, cnt);
  }
  drawIcon(dl, n->visible ? Icon::Eye : Icon::EyeOff, ImVec2((eye.Min.x + eye.Max.x) * 0.5f, cy), 15 * s, overEye ? cFg : cFg2);
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) rightClicked = n;
  if (rowClicked) {
    if (overEye) { n->visible = !n->visible; visChanged = true; }
    else if (overChev) n->open = !n->open;
    else clicked = n;
  }
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hasKids && !overEye) n->open = !n->open;
  ImGui::PopID();
  if (hasKids && n->open)
    for (auto& k : n->kids) treeRow(k.get(), depth + 1, selected, clicked, visChanged, dotColor);
}

// ======================================================================== icons (20×20 grid, like the web version's SVGs)
void Ui::drawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float size, ImU32 col) {
  const float k = size / 20.f;
  auto P = [&](float x, float y) { return ImVec2(c.x + (x - 10) * k, c.y + (y - 10) * k); };
  const float lw = std::max(1.2f, 1.5f * k);
  auto line = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), col, lw); };
  auto poly = [&](std::initializer_list<ImVec2> pts, bool closed) {
    std::vector<ImVec2> v;
    for (auto& q : pts) v.push_back(P(q.x, q.y));
    dl->AddPolyline(v.data(), (int)v.size(), col, closed ? ImDrawFlags_Closed : 0, lw);
  };
  switch (ic) {
    case Icon::Open: poly({{2.5f, 5}, {8, 5}, {9.5f, 7}, {17.5f, 7}, {17.5f, 15.5f}, {2.5f, 15.5f}}, true); line(2.5f, 9.5f, 17.5f, 9.5f); break;
    case Icon::Tree:
      dl->AddCircleFilled(P(4, 5), 1.6f * k, col); line(7, 5, 16.5f, 5);
      dl->AddCircleFilled(P(7, 10), 1.6f * k, col); line(10, 10, 16.5f, 10);
      dl->AddCircleFilled(P(7, 15), 1.6f * k, col); line(10, 15, 16.5f, 15);
      line(4, 6.5f, 4, 15); line(4, 10, 5.4f, 10); line(4, 15, 5.4f, 15);
      break;
    case Icon::Fit:
      poly({{3, 7}, {3, 3}, {7, 3}}, false); poly({{13, 3}, {17, 3}, {17, 7}}, false);
      poly({{17, 13}, {17, 17}, {13, 17}}, false); poly({{7, 17}, {3, 17}, {3, 13}}, false);
      dl->AddRect(P(7, 7), P(13, 13), col, 1.5f * k, 0, lw);
      break;
    case Icon::Ortho: dl->AddRect(P(4, 5), P(16, 15), col, 1.5f * k, 0, lw); line(4, 10, 16, 10); line(10, 5, 10, 15); break;
    case Icon::Persp: poly({{6, 5}, {14, 5}, {17.5f, 15}, {2.5f, 15}}, true); line(10, 5, 10, 15); line(4.2f, 10, 15.8f, 10); break;
    case Icon::Shaded: {
      ImVec2 h[6] = {P(10, 2.5f), P(16.5f, 6.25f), P(16.5f, 13.75f), P(10, 17.5f), P(3.5f, 13.75f), P(3.5f, 6.25f)};
      dl->AddConvexPolyFilled(h, 6, col);
      break;
    }
    case Icon::Edges:
    case Icon::Cube:
      poly({{10, 2.5f}, {16.5f, 6.25f}, {16.5f, 13.75f}, {10, 17.5f}, {3.5f, 13.75f}, {3.5f, 6.25f}}, true);
      poly({{3.5f, 6.25f}, {10, 10}, {16.5f, 6.25f}}, false); line(10, 10, 10, 17.5f);
      break;
    case Icon::Wire:
      poly({{10, 2.5f}, {16.5f, 6.25f}, {16.5f, 13.75f}, {10, 17.5f}, {3.5f, 13.75f}, {3.5f, 6.25f}}, true);
      line(3.5f, 6.25f, 10, 10); line(16.5f, 6.25f, 10, 10); line(10, 10, 10, 17.5f);
      line(10, 2.5f, 10, 6); line(3.5f, 13.75f, 6.5f, 12); line(16.5f, 13.75f, 13.5f, 12);
      break;
    case Icon::Mesh:
      poly({{10, 3}, {17, 16}, {3, 16}}, true); line(6.5f, 9.5f, 13.5f, 9.5f); line(6.5f, 9.5f, 10, 16); line(13.5f, 9.5f, 10, 16);
      break;
    case Icon::Xray: dl->AddCircle(P(7.5f, 10), 5 * k, col, 24, lw); dl->AddCircle(P(12.5f, 10), 5 * k, col, 24, lw); break;
    case Icon::Theme:
      dl->AddCircle(P(10, 10), 6.5f * k, col, 32, lw);
      dl->PathArcTo(P(10, 10), 6.5f * k, -IM_PI * 0.5f, IM_PI * 0.5f, 20);
      dl->PathFillConvex(col);
      break;
    case Icon::Settings:
      line(3, 6, 17, 6); line(3, 14, 17, 14);
      dl->AddCircleFilled(P(7, 6), 2.3f * k, col); dl->AddCircleFilled(P(13, 14), 2.3f * k, col);
      break;
    case Icon::Plus: line(10, 4, 10, 16); line(4, 10, 16, 10); break;
    case Icon::Close: line(5, 5, 15, 15); line(15, 5, 5, 15); break;
    case Icon::Eye:
    case Icon::EyeOff: {
      dl->PathArcTo(P(10, 16), 8.5f * k, -IM_PI * 0.82f, -IM_PI * 0.18f, 16);
      dl->PathStroke(col, 0, lw);
      dl->PathArcTo(P(10, 4), 8.5f * k, IM_PI * 0.18f, IM_PI * 0.82f, 16);
      dl->PathStroke(col, 0, lw);
      if (ic == Icon::Eye) dl->AddCircleFilled(P(10, 10), 2.4f * k, col);
      else line(4, 16, 16, 4);
      break;
    }
    case Icon::Chevron: poly({{7, 4}, {13, 10}, {7, 16}}, false); break;
    case Icon::Ruler:
      poly({{2.5f, 12.5f}, {12.5f, 2.5f}, {17.5f, 7.5f}, {7.5f, 17.5f}}, true);
      line(6, 9, 7.8f, 10.8f); line(8.5f, 6.5f, 11.5f, 9.5f); line(11, 4, 12.8f, 5.8f); line(13.5f, 11.5f, 11.5f, 13.5f);
      break;
    case Icon::Cut:
      poly({{3, 12}, {10, 15.5f}, {17, 12}}, false); poly({{3, 8}, {10, 4.5f}, {17, 8}, {10, 11.5f}}, true);
      break;
    case Icon::Move:
      line(10, 2.5f, 10, 17.5f); line(2.5f, 10, 17.5f, 10);
      poly({{8, 4.5f}, {10, 2.5f}, {12, 4.5f}}, false); poly({{8, 15.5f}, {10, 17.5f}, {12, 15.5f}}, false);
      poly({{4.5f, 8}, {2.5f, 10}, {4.5f, 12}}, false); poly({{15.5f, 8}, {17.5f, 10}, {15.5f, 12}}, false);
      break;
    case Icon::Mass:
      poly({{6.2f, 7}, {13.8f, 7}, {16, 16.5f}, {4, 16.5f}}, true); dl->AddCircle(P(10, 4.8f), 1.9f * k, col, 16, lw);
      break;
    case Icon::AM:
      line(3, 16.5f, 17, 16.5f); line(5, 14, 15, 14); line(6.5f, 11.5f, 13.5f, 11.5f); line(8, 9, 12, 9);
      line(10, 2.5f, 10, 6.5f); poly({{8.5f, 5}, {10, 6.5f}, {11.5f, 5}}, false);
      break;
    case Icon::Palette:
      dl->AddCircle(P(10, 10), 7 * k, col, 32, lw);
      dl->AddCircleFilled(P(7, 7.5f), 1.4f * k, col); dl->AddCircleFilled(P(11, 6), 1.4f * k, col); dl->AddCircleFilled(P(14, 9), 1.4f * k, col);
      dl->AddCircleFilled(P(8, 12.5f), 1.4f * k, col);
      break;
    case Icon::Insert:
      poly({{10, 2.5f}, {16.5f, 6.25f}, {16.5f, 10}}, false); poly({{10, 2.5f}, {3.5f, 6.25f}, {3.5f, 13.75f}, {10, 17.5f}}, false);
      line(14.5f, 12, 14.5f, 18); line(11.5f, 15, 17.5f, 15);
      break;
    case Icon::ChevronDown: poly({{4, 7}, {10, 13}, {16, 7}}, false); break;
    case Icon::Explode:  // three blocks flying apart
      poly({{7.5f, 7.5f}, {12.5f, 7.5f}, {12.5f, 12.5f}, {7.5f, 12.5f}}, true);
      poly({{2.5f, 2.5f}, {6, 2.5f}, {6, 6}, {2.5f, 6}}, true); poly({{14, 14}, {17.5f, 14}, {17.5f, 17.5f}, {14, 17.5f}}, true);
      line(14, 6, 17.5f, 2.5f); line(2.5f, 17.5f, 6, 14);
      break;
    case Icon::Compare:  // two overlapping outlines
      poly({{2.5f, 5}, {12, 5}, {12, 15}, {2.5f, 15}}, true);
      dl->AddCircle(P(12.5f, 10), 5 * k, col, 32, lw);
      break;
  }
}
