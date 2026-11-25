// GlassView Native – additive-manufacturing check (overhang, wall thickness, build orientation).
#pragma once
#include "features.hpp"

void amInit(App& a);
void amSetActive(App& a, bool on);
void amDocShown(App& a);
void amPanel(App& a, float& y);
void amOverlay(App& a);
void amSetMapMode(int m);
bool amPick(App& a, const Graphic3d_Vec2i& px);
void exportBuild3mf(App& a, const gp_Dir& build);
