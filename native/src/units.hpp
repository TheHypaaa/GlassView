// Display units. Geometry is always millimetres / grams internally; only formatting changes.
#pragma once
#include <cmath>
#include <cstdio>
#include <string>

namespace gv {
struct Units {
  bool inch = false;
  int density = 0; // 0 g/cm³, 1 kg/m³, 2 lb/in³, 3 lb/ft³
};
inline Units& units() { static Units u; return u; }

inline std::string fixd(double v, int d) {
  if (std::fabs(v) < 0.5 * std::pow(10.0, -d)) v = 0;
  char b[48];
  snprintf(b, sizeof b, "%.*f", d, v);
  return b;
}
inline std::string fLen(double mm, bool unit = true, int dec = -1) {
  const bool in = units().inch;
  std::string s = fixd(in ? mm / 25.4 : mm, dec >= 0 ? dec : (in ? 4 : 3));
  return unit ? s + (in ? " in" : " mm") : s;
}
inline const char* lenUnit() { return units().inch ? "in" : "mm"; }
inline std::string fArea(double mm2, int dec = -1) { return units().inch ? fixd(mm2 / 645.16, dec >= 0 ? dec : 3) + " in²" : fixd(mm2 / 100, dec >= 0 ? dec : 2) + " cm²"; }
inline std::string fVol(double mm3) { return units().inch ? fixd(mm3 / 16387.064, 4) + " in³" : fixd(mm3 / 1000, 3) + " cm³"; }
inline std::string fMass(double g) {
  if (units().inch) { double lb = g / 453.59237; return lb < 1 ? fixd(lb * 16, 3) + " oz" : fixd(lb, 4) + " lb"; }
  return g >= 1000 ? fixd(g / 1000, 4) + " kg" : fixd(g, 2) + " g";
}
inline double inertiaFactor() { return units().inch ? 1.0 / (453.59237 * 645.16) : 1.0 / 1000.0; }
inline const char* inertiaUnit() { return units().inch ? "lb·in²" : "kg·mm²"; }
inline const char* densityLabel(int i = -1) { static const char* l[] = {"g/cm³", "kg/m³", "lb/in³", "lb/ft³"}; return l[i < 0 ? units().density : i]; }
inline double densityFactor(int i = -1) { static const double f[] = {1, 1000, 0.0361272920, 62.4279606}; return f[i < 0 ? units().density : i]; }
inline int densityDec() { static const int d[] = {3, 0, 5, 2}; return d[units().density]; }
inline std::string fDensity(double gcm3) { return fixd(gcm3 * densityFactor(), densityDec()) + " " + densityLabel(); }
inline std::string fAng(double deg) { return fixd(deg, 2) + "°"; }
} // namespace gv
