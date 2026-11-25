#include "blur.hpp"
#include <GLFW/glfw3.h>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>

// ---- the few GL 3 entry points we need (opengl32.dll on Windows only exports GL 1.1)
#ifndef APIENTRY
#define APIENTRY
#endif
typedef char GLch;
#define GLB_FRAMEBUFFER 0x8D40
#define GLB_READ_FRAMEBUFFER 0x8CA8
#define GLB_DRAW_FRAMEBUFFER 0x8CA9
#define GLB_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GLB_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GLB_COLOR_ATTACHMENT0 0x8CE0
#define GLB_FRAMEBUFFER_COMPLETE 0x8CD5
#define GLB_VERTEX_SHADER 0x8B31
#define GLB_FRAGMENT_SHADER 0x8B30
#define GLB_COMPILE_STATUS 0x8B81
#define GLB_LINK_STATUS 0x8B82
#define GLB_CURRENT_PROGRAM 0x8B8D
#define GLB_ACTIVE_TEXTURE 0x84E0
#define GLB_TEXTURE0 0x84C0
#define GLB_VERTEX_ARRAY_BINDING 0x85B5
#define GLB_SAMPLER_BINDING 0x8919
#define GLB_FRAMEBUFFER_SRGB 0x8DB9
#define GLB_CLAMP_TO_EDGE 0x812F
#define GLB_RGBA8 0x8058
#define GLB_SRGB8_ALPHA8 0x8C43
#define GLB_SRGB 0x8C40
#define GLB_BACK_LEFT 0x0402
#define GLB_COLOR_ENCODING 0x8210

typedef void(APIENTRY* PGenFramebuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void(APIENTRY* PBindFramebuffer)(GLenum, GLuint);
typedef void(APIENTRY* PFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY* PCheckFramebufferStatus)(GLenum);
typedef void(APIENTRY* PBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef GLuint(APIENTRY* PCreateShader)(GLenum);
typedef void(APIENTRY* PShaderSource)(GLuint, GLsizei, const GLch* const*, const GLint*);
typedef void(APIENTRY* PCompileShader)(GLuint);
typedef void(APIENTRY* PGetShaderiv)(GLuint, GLenum, GLint*);
typedef GLuint(APIENTRY* PCreateProgram)();
typedef void(APIENTRY* PAttachShader)(GLuint, GLuint);
typedef void(APIENTRY* PLinkProgram)(GLuint);
typedef void(APIENTRY* PGetProgramiv)(GLuint, GLenum, GLint*);
typedef void(APIENTRY* PDeleteShader)(GLuint);
typedef void(APIENTRY* PDeleteProgram)(GLuint);
typedef void(APIENTRY* PUseProgram)(GLuint);
typedef GLint(APIENTRY* PGetUniformLocation)(GLuint, const GLch*);
typedef void(APIENTRY* PUniform1i)(GLint, GLint);
typedef void(APIENTRY* PUniform2f)(GLint, GLfloat, GLfloat);
typedef void(APIENTRY* PGenVertexArrays)(GLsizei, GLuint*);
typedef void(APIENTRY* PDeleteVertexArrays)(GLsizei, const GLuint*);
typedef void(APIENTRY* PBindVertexArray)(GLuint);
typedef void(APIENTRY* PActiveTexture)(GLenum);
typedef void(APIENTRY* PBindSampler)(GLuint, GLuint);
typedef void(APIENTRY* PGetFBAttachParam)(GLenum, GLenum, GLenum, GLint*);

static PGenFramebuffers glGenFramebuffers_;
static PDeleteFramebuffers glDeleteFramebuffers_;
static PBindFramebuffer glBindFramebuffer_;
static PFramebufferTexture2D glFramebufferTexture2D_;
static PCheckFramebufferStatus glCheckFramebufferStatus_;
static PBlitFramebuffer glBlitFramebuffer_;
static PCreateShader glCreateShader_;
static PShaderSource glShaderSource_;
static PCompileShader glCompileShader_;
static PGetShaderiv glGetShaderiv_;
static PCreateProgram glCreateProgram_;
static PAttachShader glAttachShader_;
static PLinkProgram glLinkProgram_;
static PGetProgramiv glGetProgramiv_;
static PDeleteShader glDeleteShader_;
static PDeleteProgram glDeleteProgram_;
static PUseProgram glUseProgram_;
static PGetUniformLocation glGetUniformLocation_;
static PUniform1i glUniform1i_;
static PUniform2f glUniform2f_;
static PGenVertexArrays glGenVertexArrays_;
static PDeleteVertexArrays glDeleteVertexArrays_;
static PBindVertexArray glBindVertexArray_;
static PActiveTexture glActiveTexture_;
static PBindSampler glBindSampler_;
static PGetFBAttachParam glGetFramebufferAttachmentParameteriv_;

template <class T> static bool load(T& f, const char* name) { f = (T)glfwGetProcAddress(name); return f != nullptr; }

// separable Gaussian, 9 taps folded into 5 bilinear fetches
static const char* kVS =
    "#version 130\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char* kFS =
    "#version 130\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 dir;\n"
    "void main() {\n"
    "  vec4 c = texture(tex, uv) * 0.2270270270;\n"
    "  c += texture(tex, uv + dir * 1.3846153846) * 0.3162162162;\n"
    "  c += texture(tex, uv - dir * 1.3846153846) * 0.3162162162;\n"
    "  c += texture(tex, uv + dir * 3.2307692308) * 0.0702702703;\n"
    "  c += texture(tex, uv - dir * 3.2307692308) * 0.0702702703;\n"
    "  color = vec4(c.rgb, 1.0);\n"
    "}\n";

namespace gv {

static GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader_(type);
  glShaderSource_(s, 1, &src, nullptr);
  glCompileShader_(s);
  GLint ok = 0;
  glGetShaderiv_(s, GLB_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader_(s); return 0; }
  return s;
}

bool Blur::init() {
  tried = true;
  bool all = load(glGenFramebuffers_, "glGenFramebuffers") && load(glDeleteFramebuffers_, "glDeleteFramebuffers") &&
             load(glBindFramebuffer_, "glBindFramebuffer") && load(glFramebufferTexture2D_, "glFramebufferTexture2D") &&
             load(glCheckFramebufferStatus_, "glCheckFramebufferStatus") && load(glBlitFramebuffer_, "glBlitFramebuffer") &&
             load(glCreateShader_, "glCreateShader") && load(glShaderSource_, "glShaderSource") && load(glCompileShader_, "glCompileShader") &&
             load(glGetShaderiv_, "glGetShaderiv") && load(glCreateProgram_, "glCreateProgram") && load(glAttachShader_, "glAttachShader") &&
             load(glLinkProgram_, "glLinkProgram") && load(glGetProgramiv_, "glGetProgramiv") && load(glDeleteShader_, "glDeleteShader") &&
             load(glDeleteProgram_, "glDeleteProgram") && load(glUseProgram_, "glUseProgram") && load(glGetUniformLocation_, "glGetUniformLocation") &&
             load(glUniform1i_, "glUniform1i") && load(glUniform2f_, "glUniform2f") && load(glGenVertexArrays_, "glGenVertexArrays") &&
             load(glDeleteVertexArrays_, "glDeleteVertexArrays") && load(glBindVertexArray_, "glBindVertexArray") &&
             load(glActiveTexture_, "glActiveTexture");
  load(glBindSampler_, "glBindSampler"); // optional (GL 3.3)
  load(glGetFramebufferAttachmentParameteriv_, "glGetFramebufferAttachmentParameteriv");
  if (!all) return false;
  GLuint vs = compile(GLB_VERTEX_SHADER, kVS), fs = compile(GLB_FRAGMENT_SHADER, kFS);
  if (!vs || !fs) return false;
  prog = glCreateProgram_();
  glAttachShader_(prog, vs);
  glAttachShader_(prog, fs);
  glLinkProgram_(prog);
  glDeleteShader_(vs);
  glDeleteShader_(fs);
  GLint linked = 0;
  glGetProgramiv_(prog, GLB_LINK_STATUS, &linked);
  if (!linked) { glDeleteProgram_(prog); prog = 0; return false; }
  locTex = glGetUniformLocation_(prog, "tex");
  locDir = glGetUniformLocation_(prog, "dir");
  glGenVertexArrays_(1, &vao);
  glGenFramebuffers_(3, fbo);
  GLuint t[3];
  glGenTextures(3, t);
  tex[0] = t[0]; tex[1] = t[1]; half = t[2];
  return true;
}

static void allocTex(GLuint t, int w, int h, bool srgb) {
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GLB_SRGB8_ALPHA8 : GLB_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLB_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLB_CLAMP_TO_EDGE);
}

void Blur::update(int fbW, int fbH) {
  if (fbW <= 0 || fbH <= 0) return;
  if (!tried && !init()) { ok = false; return; }
  if (!prog) return;

  // ---- save state (OCCT and ImGui both cache GL state)
  GLint oProg, oActive, oTex, oVao, oRead, oDraw, oVp[4], oReadBuf, oSampler = 0;
  glGetIntegerv(GLB_CURRENT_PROGRAM, &oProg);
  glGetIntegerv(GLB_ACTIVE_TEXTURE, &oActive);
  glActiveTexture_(GLB_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &oTex);
  if (glBindSampler_) glGetIntegerv(GLB_SAMPLER_BINDING, &oSampler);
  glGetIntegerv(GLB_VERTEX_ARRAY_BINDING, &oVao);
  glGetIntegerv(GLB_READ_FRAMEBUFFER_BINDING, &oRead);
  glGetIntegerv(GLB_DRAW_FRAMEBUFFER_BINDING, &oDraw);
  glGetIntegerv(GL_VIEWPORT, oVp);
  glGetIntegerv(GL_READ_BUFFER, &oReadBuf);
  const GLboolean eBlend = glIsEnabled(GL_BLEND), eDepth = glIsEnabled(GL_DEPTH_TEST), eScissor = glIsEnabled(GL_SCISSOR_TEST),
                  eCull = glIsEnabled(GL_CULL_FACE), eStencil = glIsEnabled(GL_STENCIL_TEST), eSrgb = glIsEnabled(GLB_FRAMEBUFFER_SRGB);
  GLboolean oMask[4];
  glGetBooleanv(GL_COLOR_WRITEMASK, oMask);

  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
  glDisable(GLB_FRAMEBUFFER_SRGB);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  if (glBindSampler_) glBindSampler_(0, 0);

  // ---- (re)allocate: half and quarter resolution
  const int nhw = std::max(1, fbW / 2), nhh = std::max(1, fbH / 2), nw = std::max(1, fbW / 4), nh = std::max(1, fbH / 4);
  // Does the interface get sRGB-encoded on output? Then keep the copy in sRGB textures so it
  // round-trips unchanged (and blur in linear light); otherwise plain RGBA8.
  bool encode = false;
  if (eSrgb && glGetFramebufferAttachmentParameteriv_) {
    GLint enc = 0;
    glBindFramebuffer_(GLB_FRAMEBUFFER, 0);
    glGetFramebufferAttachmentParameteriv_(GLB_FRAMEBUFFER, GLB_BACK_LEFT, GLB_COLOR_ENCODING, &enc);
    while (glGetError() != GL_NO_ERROR) {}
    encode = enc == GLB_SRGB;
  }
  bool good = true;
  if (nw != w || nh != h || encode != srgb) {
    w = nw; h = nh; hw = nhw; hh = nhh; srgb = encode;
    allocTex(half, hw, hh, srgb);
    allocTex(tex[0], w, h, srgb);
    allocTex(tex[1], w, h, srgb);
    const GLuint ts[3] = {tex[0], tex[1], half};
    for (int i = 0; i < 3; i++) {
      glBindFramebuffer_(GLB_FRAMEBUFFER, fbo[i]);
      glFramebufferTexture2D_(GLB_FRAMEBUFFER, GLB_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ts[i], 0);
      good = good && glCheckFramebufferStatus_(GLB_FRAMEBUFFER) == GLB_FRAMEBUFFER_COMPLETE;
    }
  }

  if (good) {
    // ---- downsample the back buffer twice (bilinear), then blur at quarter size
    glBindFramebuffer_(GLB_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindFramebuffer_(GLB_DRAW_FRAMEBUFFER, fbo[2]);
    glBlitFramebuffer_(0, 0, fbW, fbH, 0, 0, hw, hh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer_(GLB_READ_FRAMEBUFFER, fbo[2]);
    glBindFramebuffer_(GLB_DRAW_FRAMEBUFFER, fbo[0]);
    glBlitFramebuffer_(0, 0, hw, hh, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    if (srgb) glEnable(GLB_FRAMEBUFFER_SRGB);
    glUseProgram_(prog);
    glUniform1i_(locTex, 0);
    glBindVertexArray_(vao);
    glViewport(0, 0, w, h);
    const float spread[3] = {1.0f, 1.6f, 2.4f}; // three passes of growing radius ≈ 22 px at full size
    for (float k : spread) {
      glBindFramebuffer_(GLB_DRAW_FRAMEBUFFER, fbo[1]);
      glBindTexture(GL_TEXTURE_2D, tex[0]);
      glUniform2f_(locDir, k / w, 0.f);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glBindFramebuffer_(GLB_DRAW_FRAMEBUFFER, fbo[0]);
      glBindTexture(GL_TEXTURE_2D, tex[1]);
      glUniform2f_(locDir, 0.f, k / h);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    ok = true;
    if (const char* dump = getenv("GV_BLURDUMP")) {
      std::vector<unsigned char> px((size_t)w * h * 3);
      glBindTexture(GL_TEXTURE_2D, tex[0]);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, px.data());
      if (FILE* f = fopen(dump, "wb")) { fprintf(f, "P6\n%d %d\n255\n", w, h); for (int y = h - 1; y >= 0; y--) fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f); fclose(f); }
    }
  } else ok = false;

  // ---- restore
  glUseProgram_(oProg);
  glBindVertexArray_(oVao);
  glBindTexture(GL_TEXTURE_2D, oTex);
  if (glBindSampler_) glBindSampler_(0, oSampler);
  glActiveTexture_(oActive);
  glBindFramebuffer_(GLB_READ_FRAMEBUFFER, oRead);
  glBindFramebuffer_(GLB_DRAW_FRAMEBUFFER, oDraw);
  glReadBuffer(oReadBuf);
  glViewport(oVp[0], oVp[1], oVp[2], oVp[3]);
  glColorMask(oMask[0], oMask[1], oMask[2], oMask[3]);
  auto set = [](GLenum cap, GLboolean on) { if (on) glEnable(cap); else glDisable(cap); };
  set(GL_BLEND, eBlend); set(GL_DEPTH_TEST, eDepth); set(GL_SCISSOR_TEST, eScissor); set(GL_CULL_FACE, eCull);
  set(GL_STENCIL_TEST, eStencil); set(GLB_FRAMEBUFFER_SRGB, eSrgb);
}

void Blur::release() {
  if (!prog) return;
  glDeleteProgram_(prog);
  glDeleteVertexArrays_(1, &vao);
  glDeleteFramebuffers_(3, fbo);
  GLuint t[3] = {tex[0], tex[1], half};
  glDeleteTextures(3, t);
  prog = 0; ok = false;
}

} // namespace gv
