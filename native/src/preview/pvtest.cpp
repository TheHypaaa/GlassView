// Linux test harness: renders the Explorer preview scene to a PPM.
#include "pvscene.hpp"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "pvtest model out.ppm [yaw pitch zoom dark]\n"); return 1; }
  gv::LoadProgress prog;
  auto m = gv::loadModel(argv[1], gv::Quality::Coarse, prog);
  pv::Scene s;
  if (m->error.empty()) pv::build(*m, s);
  printf("tris %zu hard %zu soft %zu err '%s'\n", s.tris, s.hard.size() / 6, s.soft.size() / 6, (m->error + s.error).c_str());
  glfwInit();
  glfwWindowHint(GLFW_VISIBLE, 0);
  GLFWwindow* w = glfwCreateWindow(480, 360, "pv", nullptr, nullptr);
  glfwMakeContextCurrent(w);
  pv::View v;
  v.w = 480; v.h = 360;
  if (argc > 4) { v.yaw = (float)atof(argv[3]); v.pitch = (float)atof(argv[4]); }
  if (argc > 5) v.zoom = (float)atof(argv[5]);
  if (argc > 6) { v.dark = atoi(argv[6]); if (v.dark) { v.bg[0] = v.bg[1] = v.bg[2] = 25; } }
  pv::render(s, v);
  glFinish();
  std::vector<unsigned char> px(480 * 360 * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, 480, 360, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  FILE* f = fopen(argv[2], "wb");
  fprintf(f, "P6\n480 360\n255\n");
  for (int y = 359; y >= 0; y--) fwrite(&px[y * 480 * 3], 1, 480 * 3, f);
  fclose(f);
  return 0;
}
