// GlassView 3D preview – a tiny OpenGL 1.1 viewer for Explorer's preview pane.
// Mesh data comes from gv::loadModel; rendering is fixed-function so it works in any GL context.
#pragma once
#include "../model.hpp"
#include <string>
#include <vector>

namespace pv {

struct Scene {
  // flat triangle soup (positions, normals, colours) – world coordinates
  std::vector<float> pos, nrm;
  std::vector<unsigned char> col;
  std::vector<float> hard, soft;   // edge segments (x y z x y z …)
  float c[3] = {0, 0, 0};          // bounding-sphere centre
  float radius = 1;
  float size[3] = {0, 0, 0};       // bounding box extent
  size_t tris = 0;
  int parts = 0;
  std::string name, error;
  bool ready = false;
};

/** Converts a loaded model into GL arrays (call on the loader thread). */
void build(const gv::Model& m, Scene& s);

struct View {
  float yaw = -0.785398f, pitch = 0.615480f;   // isometric
  float zoom = 1, panX = 0, panY = 0;
  bool dark = false;
  unsigned char bg[3] = {255, 255, 255};       // Explorer's preview-pane colour
  // navigation, same feel as GlassView (OrbitControls); speeds come from the app's settings
  float rotateSpeed = 1, zoomSpeed = 1;
  bool reverseWheel = false;
  int w = 1, h = 1;
  // input
  bool rot = false, pan = false;
  int lx = 0, ly = 0;
  void reset() { yaw = -0.785398f; pitch = 0.615480f; zoom = 1; panX = panY = 0; }
  void mouseDown(int x, int y, bool rotate) { (rotate ? rot : pan) = true; lx = x; ly = y; }
  void mouseUp() { rot = pan = false; }
  bool mouseMove(int x, int y);   // true → redraw
  /** notches = wheel delta / 120 (fractional on touchpads); x, y = cursor in client pixels (zoom to cursor). */
  void wheel(float notches, int x, int y);
};

/** Draws the scene (or the background only, if not ready). GL context must be current. */
void render(const Scene& s, const View& v);

} // namespace pv
