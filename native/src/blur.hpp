// GlassView Native – frosted-glass backdrop: blurred copy of the 3D view for the panels.
#pragma once
#include <cstdint>

namespace gv {

class Blur {
public:
  /** Copy the current back buffer (the 3D view) and blur it. Call after the 3D
      redraw and before the interface is drawn. Saves and restores the GL state. */
  void update(int fbW, int fbH);
  /** Blurred texture (0 until the first update or if unsupported). */
  uint64_t texture() const { return ok ? (uint64_t)tex[0] : 0; }
  void release();

private:
  bool init();
  bool tried = false, ok = false, srgb = false;
  unsigned prog = 0, vao = 0, fbo[3] = {0, 0, 0}, tex[2] = {0, 0}, half = 0;
  int locTex = -1, locDir = -1;
  int w = 0, h = 0, hw = 0, hh = 0;
};

} // namespace gv
