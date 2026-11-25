// GlassView 3D preview handler for Windows Explorer's preview pane (Alt+P).
// A COM in-process server (GlassViewPreview.dll) hosted by prevhost.exe: it loads the STEP / IGES /
// STL file on a worker thread and shows it in an OpenGL child window that you can rotate, pan and zoom.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <shlwapi.h>
#include <GL/gl.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

#include "pvscene.hpp"
#include "../version.hpp"

// {7A3F1C2E-5B8D-4E61-9C07-2D4B6A8E91F3}
static const CLSID CLSID_GvPreview = {0x7a3f1c2e, 0x5b8d, 0x4e61, {0x9c, 0x07, 0x2d, 0x4b, 0x6a, 0x8e, 0x91, 0xf3}};
static const wchar_t* kClsidStr = L"{7A3F1C2E-5B8D-4E61-9C07-2D4B6A8E91F3}";

static HINSTANCE gInst = nullptr;
static std::atomic<long> gObjects{0}, gLocks{0}, gThreads{0};

// ======================================================================== loading job (shared with the worker)
struct Job {
  std::mutex m;
  HWND hwnd = nullptr;          // cleared when the preview is unloaded
  gv::LoadProgress prog;
  pv::Scene scene;
  bool done = false;
  std::wstring tempFile;        // deleted by whoever finishes last
};
static const UINT WM_PV_LOADED = WM_APP + 41;

static std::string utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

// ======================================================================== the handler
class Preview : public IPreviewHandler, public IInitializeWithFile, public IInitializeWithStream,
                public IObjectWithSite, public IOleWindow, public IPreviewHandlerVisuals {
public:
  Preview() { gObjects++; }
  ~Preview() { Unload(); if (site) site->Release(); gObjects--; }

  // ---- IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IPreviewHandler) *ppv = static_cast<IPreviewHandler*>(this);
    else if (riid == IID_IInitializeWithFile) *ppv = static_cast<IInitializeWithFile*>(this);
    else if (riid == IID_IInitializeWithStream) *ppv = static_cast<IInitializeWithStream*>(this);
    else if (riid == IID_IObjectWithSite) *ppv = static_cast<IObjectWithSite*>(this);
    else if (riid == IID_IOleWindow) *ppv = static_cast<IOleWindow*>(this);
    else if (riid == IID_IPreviewHandlerVisuals) *ppv = static_cast<IPreviewHandlerVisuals*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { const ULONG r = --refs; if (!r) delete this; return r; }

  // ---- IInitializeWithFile / IInitializeWithStream
  STDMETHODIMP Initialize(LPCWSTR path, DWORD) override { file = path ? path : L""; return S_OK; }
  STDMETHODIMP Initialize(IStream* st, DWORD) override {
    if (!st) return E_INVALIDARG;
    // name → extension (the loader picks the reader by extension)
    STATSTG ss{};
    std::wstring ext = L".step";
    if (SUCCEEDED(st->Stat(&ss, STATFLAG_DEFAULT)) && ss.pwcsName) {
      const wchar_t* dot = wcsrchr(ss.pwcsName, L'.');
      if (dot) ext = dot;
      CoTaskMemFree(ss.pwcsName);
    }
    wchar_t dir[MAX_PATH], tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"gvp", 0, tmp)) return E_FAIL;
    DeleteFileW(tmp);
    std::wstring path = std::wstring(tmp) + ext;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return E_FAIL;
    char buf[1 << 16];
    ULONG got = 0;
    HRESULT hr;
    while (SUCCEEDED(hr = st->Read(buf, sizeof buf, &got)) && got > 0) { DWORD w = 0; WriteFile(h, buf, got, &w, nullptr); }
    CloseHandle(h);
    file = path;
    tempFile = path;
    return S_OK;
  }

  // ---- IObjectWithSite
  STDMETHODIMP SetSite(IUnknown* s) override { if (site) site->Release(); site = s; if (site) site->AddRef(); return S_OK; }
  STDMETHODIMP GetSite(REFIID riid, void** ppv) override { if (!site) { *ppv = nullptr; return E_FAIL; } return site->QueryInterface(riid, ppv); }

  // ---- IOleWindow
  STDMETHODIMP GetWindow(HWND* p) override { if (!p) return E_INVALIDARG; *p = parent; return S_OK; }
  STDMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

  // ---- IPreviewHandlerVisuals
  // Explorer tells us its pane colours and font: use them, so the preview looks like part of Explorer
  STDMETHODIMP SetBackgroundColor(COLORREF c) override {
    gotBg = true;
    setBg(c);
    if (wnd) InvalidateRect(wnd, nullptr, FALSE);
    return S_OK;
  }
  STDMETHODIMP SetFont(const LOGFONTW* lf) override {
    if (lf) { logFont = *lf; gotFont = true; if (rc) makeFonts(); if (wnd) InvalidateRect(wnd, nullptr, FALSE); }
    return S_OK;
  }
  STDMETHODIMP SetTextColor(COLORREF c) override { textColor = c; gotText = true; if (wnd) InvalidateRect(wnd, nullptr, FALSE); return S_OK; }
  void setBg(COLORREF c) {
    view.bg[0] = GetRValue(c); view.bg[1] = GetGValue(c); view.bg[2] = GetBValue(c);
    const int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
    view.dark = lum < 110;
  }

  // ---- IPreviewHandler
  STDMETHODIMP SetWindow(HWND hwnd, const RECT* rc) override {
    parent = hwnd;
    if (rc) bounds = *rc;
    if (wnd) {
      SetParent(wnd, parent);
      SetWindowPos(wnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return S_OK;
  }
  STDMETHODIMP SetRect(const RECT* rc) override {
    if (!rc) return E_INVALIDARG;
    bounds = *rc;
    if (wnd) SetWindowPos(wnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
    return S_OK;
  }
  STDMETHODIMP DoPreview() override;
  STDMETHODIMP Unload() override;
  STDMETHODIMP SetFocus() override { if (wnd) ::SetFocus(wnd); return S_OK; }
  STDMETHODIMP QueryFocus(HWND* p) override { if (!p) return E_INVALIDARG; *p = ::GetFocus(); return *p ? S_OK : HRESULT_FROM_WIN32(GetLastError()); }
  STDMETHODIMP TranslateAccelerator(MSG* msg) override {
    // let Explorer handle its accelerators (Tab, F5 …)
    IPreviewHandlerFrame* fr = nullptr;
    HRESULT hr = S_FALSE;
    if (site && SUCCEEDED(site->QueryInterface(IID_IPreviewHandlerFrame, (void**)&fr))) { hr = fr->TranslateAccelerator(msg); fr->Release(); }
    return hr;
  }

  // ---- window
  static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
  void paint();
  void drawText(float x, float y, const std::wstring& t, bool bold, unsigned char r, unsigned char g, unsigned char b, unsigned char a);

private:
  std::atomic<ULONG> refs{1};
  IUnknown* site = nullptr;
  HWND parent = nullptr, wnd = nullptr;
  RECT bounds{};
  std::wstring file, tempFile;
  std::shared_ptr<Job> job;
  pv::View view;
  HDC dc = nullptr;
  HGLRC rc = nullptr;
  GLuint fontBase = 0, fontBold = 0;
  HFONT font = nullptr, fontB = nullptr;
  LOGFONTW logFont{};
  bool gotFont = false, gotBg = false, gotText = false, hover = false;
  COLORREF textColor = RGB(0, 0, 0);
  UINT dpi = 96;
  void makeFonts();
  float textWidth(const std::wstring& t, bool bold) {
    SIZE sz{};
    HGDIOBJ old = SelectObject(dc, bold ? fontB : font);
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
    SelectObject(dc, old);
    return (float)sz.cx;
  }
  void roundRect(float x0, float y0, float x1, float y1, float r, unsigned char cr, unsigned char cg, unsigned char cb, unsigned char ca);
  int fontH = 15;
};

static const wchar_t* kWndClass = L"GlassViewPreviewWnd";

static void registerClass() {
  static bool done = false;
  if (done) return;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_OWNDC | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = Preview::WndProc;
  wc.hInstance = gInst;
  wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
  wc.lpszClassName = kWndClass;
  RegisterClassExW(&wc);
  done = true;
}

void Preview::makeFonts() {
  if (!rc) return;
  wglMakeCurrent(dc, rc);
  if (fontBase) { glDeleteLists(fontBase, 512); fontBase = 0; }
  if (fontBold) { glDeleteLists(fontBold, 512); fontBold = 0; }
  if (font) { DeleteObject(font); font = nullptr; }
  if (fontB) { DeleteObject(fontB); fontB = nullptr; }
  // Explorer's own font if it gave us one (already DPI-scaled), otherwise Segoe UI 9 pt
  LOGFONTW lf{};
  if (gotFont && logFont.lfHeight) lf = logFont;
  else { lf.lfHeight = -MulDiv(12, dpi, 96); wcscpy(lf.lfFaceName, L"Segoe UI"); }
  lf.lfQuality = CLEARTYPE_QUALITY;
  lf.lfWeight = FW_NORMAL;
  fontH = std::abs(lf.lfHeight);
  if (fontH < 8) fontH = MulDiv(12, dpi, 96);
  HFONT f1 = CreateFontIndirectW(&lf);
  lf.lfWeight = FW_SEMIBOLD;
  HFONT f2 = CreateFontIndirectW(&lf);
  fontBase = glGenLists(512);
  fontBold = glGenLists(512);
  HGDIOBJ old = SelectObject(dc, f1);
  wglUseFontBitmapsW(dc, 0, 512, fontBase);
  SelectObject(dc, f2);
  wglUseFontBitmapsW(dc, 0, 512, fontBold);
  SelectObject(dc, old);
  font = f1;
  fontB = f2;
}

STDMETHODIMP Preview::DoPreview() {
  if (wnd || !parent) return S_OK;
  registerClass();
  wnd = CreateWindowExW(0, kWndClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, bounds.left, bounds.top,
                        bounds.right - bounds.left, bounds.bottom - bounds.top, parent, nullptr, gInst, this);
  if (!wnd) return HRESULT_FROM_WIN32(GetLastError());
  // OpenGL context on the child window
  dc = GetDC(wnd);
  PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
  pfd.cDepthBits = 24;
  pfd.iLayerType = PFD_MAIN_PLANE;
  const int pf = ChoosePixelFormat(dc, &pfd);
  if (pf) SetPixelFormat(dc, pf, &pfd);
  rc = wglCreateContext(dc);
  if (rc) {
    wglMakeCurrent(dc, rc);
    if (auto f = (UINT(WINAPI*)(HWND))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")) dpi = f(wnd);
    makeFonts();
  }
  // Windows dark mode → Explorer's dark pane colour (unless Explorer told us its colour)
  if (!gotBg) {
    DWORD light = 1, sz = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &sz) == ERROR_SUCCESS && light == 0)
      setBg(RGB(25, 25, 25));
    else
      setBg(RGB(255, 255, 255));
  }
  if (!gotText) textColor = view.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
  // the same navigation speeds as in GlassView (the app writes them to %LOCALAPPDATA%\\GlassView\\view.ini)
  {
    wchar_t ini[MAX_PATH * 2], buf[64];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", ini, MAX_PATH);
    if (n && n < MAX_PATH) {
      wcscat(ini, L"\\GlassView\\view.ini");
      GetPrivateProfileStringW(L"view", L"zoomSpeed", L"1", buf, 64, ini);
      view.zoomSpeed = (float)_wtof(buf);
      GetPrivateProfileStringW(L"view", L"rotateSpeed", L"1", buf, 64, ini);
      view.rotateSpeed = (float)_wtof(buf);
      view.reverseWheel = GetPrivateProfileIntW(L"view", L"reverseWheel", 0, ini) != 0;
      if (!(view.zoomSpeed > 0.05f && view.zoomSpeed < 20)) view.zoomSpeed = 1;
      if (!(view.rotateSpeed > 0.05f && view.rotateSpeed < 20)) view.rotateSpeed = 1;
    }
  }

  // load on a worker thread – Explorer must never wait for us
  job = std::make_shared<Job>();
  job->hwnd = wnd;
  job->tempFile = tempFile;
  tempFile.clear();
  std::shared_ptr<Job> j = job;
  const std::string path = utf8(file);
  gThreads++;
  std::thread([j, path]() {
    try {
      auto m = gv::loadModel(path, gv::Quality::Coarse, j->prog);
      pv::Scene s;
      if (!j->prog.cancel) {
        if (m->error.empty()) pv::build(*m, s);
        else s.error = m->error;
      }
      std::lock_guard<std::mutex> lk(j->m);
      j->scene = std::move(s);
      j->done = true;
      if (j->hwnd) PostMessageW(j->hwnd, WM_PV_LOADED, 0, 0);
    } catch (...) {
      std::lock_guard<std::mutex> lk(j->m);
      j->scene.error = "Could not read this file";
      j->done = true;
      if (j->hwnd) PostMessageW(j->hwnd, WM_PV_LOADED, 0, 0);
    }
    if (!j->tempFile.empty()) DeleteFileW(j->tempFile.c_str());
    gThreads--;
  }).detach();
  return S_OK;
}

STDMETHODIMP Preview::Unload() {
  if (job) {
    std::lock_guard<std::mutex> lk(job->m);
    job->hwnd = nullptr;
    job->prog.cancel = true;
    job.reset();
  }
  if (font) { DeleteObject(font); font = nullptr; }
  if (fontB) { DeleteObject(fontB); fontB = nullptr; }
  if (rc) {
    wglMakeCurrent(dc, rc);
    if (fontBase) glDeleteLists(fontBase, 512);
    if (fontBold) glDeleteLists(fontBold, 512);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    rc = nullptr;
  }
  if (wnd) {
    if (dc) ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    wnd = nullptr;
  }
  dc = nullptr;
  if (!tempFile.empty()) { DeleteFileW(tempFile.c_str()); tempFile.clear(); }
  return S_OK;
}

void Preview::drawText(float x, float y, const std::wstring& t, bool bold, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
  if (!fontBase || t.empty()) return;
  glColor4ub(r, g, b, a);
  glRasterPos2f(x, y);
  glListBase(bold ? fontBold : fontBase);
  std::wstring s;
  for (wchar_t c : t) s.push_back(c < 512 ? c : L'?');
  glCallLists((GLsizei)s.size(), GL_UNSIGNED_SHORT, s.data());
}

static std::wstring utf8w(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

void Preview::roundRect(float x0, float y0, float x1, float y1, float r, unsigned char cr, unsigned char cg, unsigned char cb, unsigned char ca) {
  r = std::min(r, std::min(x1 - x0, y1 - y0) / 2);
  glColor4ub(cr, cg, cb, ca);
  glBegin(GL_TRIANGLE_FAN);
  glVertex2f((x0 + x1) / 2, (y0 + y1) / 2);
  const float cxs[4] = {x1 - r, x0 + r, x0 + r, x1 - r}, cys[4] = {y1 - r, y1 - r, y0 + r, y0 + r};
  for (int q = 0; q < 4; q++)
    for (int i = 0; i <= 8; i++) {
      const float a = (q * 90 + i * 11.25f) * 0.01745329f;
      glVertex2f(cxs[q] + std::cos(a) * r, cys[q] + std::sin(a) * r);
    }
  glVertex2f(x1, y1 - r);
  glEnd();
}

static std::wstring wfmt(const char* fmt, double v) {
  char b[64];
  snprintf(b, sizeof b, fmt, v);
  return std::wstring(b, b + strlen(b));
}

void Preview::paint() {
  if (!rc) return;
  wglMakeCurrent(dc, rc);
  RECT r;
  GetClientRect(wnd, &r);
  view.w = std::max(1L, r.right);
  view.h = std::max(1L, r.bottom);
  static pv::Scene empty;
  const pv::Scene* s = &empty;
  std::unique_lock<std::mutex> lk;
  if (job) { lk = std::unique_lock<std::mutex>(job->m); if (job->done) s = &job->scene; }
  pv::render(*s, view);
  // 2D overlay text (pixel coordinates, origin bottom-left)
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, view.w, 0, view.h, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_LIGHTING);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  // plain text in Explorer's font and colours: size bottom-left, nothing else competing with the model
  const unsigned char tr = GetRValue(textColor), tg = GetGValue(textColor), tb = GetBValue(textColor);
  auto mix = [&](unsigned char a, unsigned char b, float t) { return (unsigned char)(a + (b - a) * t); };
  const unsigned char sr = mix(tr, view.bg[0], 0.42f), sg = mix(tg, view.bg[1], 0.42f), sb = mix(tb, view.bg[2], 0.42f);
  const float pad = fontH * 1.0f, lh = fontH * 1.45f;
  if (!job || !job->done) {
    const std::wstring t = L"Loading preview\u2026";
    drawText((view.w - textWidth(t, false)) / 2, view.h / 2.0f - fontH / 2.0f, t, false, sr, sg, sb, 255);
  } else if (!s->error.empty()) {
    const std::wstring t = L"No preview available";
    drawText((view.w - textWidth(t, false)) / 2, view.h / 2.0f - fontH / 2.0f, t, false, sr, sg, sb, 255);
  } else {
    std::vector<std::wstring> lines;
    lines.push_back(wfmt("%.1f", s->size[0]) + L" \u00d7 " + wfmt("%.1f", s->size[1]) + L" \u00d7 " + wfmt("%.1f", s->size[2]) + L" mm");
    if (s->parts > 1) lines.push_back(std::to_wstring(s->parts) + L" bodies");
    for (size_t k = 0; k < lines.size(); k++) {
      const float ty = pad * 0.8f + (lines.size() - 1 - k) * lh;
      drawText(pad, ty, lines[k], false, sr, sg, sb, 255);
    }
    // how to use it, only while the mouse is over the preview
    if (hover) {
      const std::wstring hint = L"Drag to rotate \u00b7 right-drag to pan \u00b7 scroll to zoom \u00b7 double-click to reset";
      const float hw = textWidth(hint, false);
      if (hw + 2 * pad < view.w) drawText((view.w - hw) / 2, view.h - pad - fontH, hint, false, sr, sg, sb, 200);
    }
  }
  glDisable(GL_BLEND);
  if (lk.owns_lock()) lk.unlock();
  SwapBuffers(dc);
}

LRESULT CALLBACK Preview::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Preview* self = (Preview*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (m == WM_NCCREATE) {
    self = (Preview*)((CREATESTRUCTW*)l)->lpCreateParams;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
  }
  if (!self) return DefWindowProcW(h, m, w, l);
  const int x = (short)LOWORD(l), y = (short)HIWORD(l);
  switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); self->paint(); EndPaint(h, &ps); return 0; }
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PV_LOADED: self->view.reset(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
      ::SetFocus(h);
      SetCapture(h);
      self->view.mouseDown(x, y, m == WM_LBUTTONDOWN && !(w & MK_SHIFT));
      return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
      self->view.mouseUp();
      ReleaseCapture();
      return 0;
    case WM_MOUSEMOVE:
      if (!self->hover) {
        self->hover = true;
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        InvalidateRect(h, nullptr, FALSE);
      }
      if (self->view.mouseMove(x, y)) InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_MOUSELEAVE:
      self->hover = false;
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_MOUSEWHEEL: {
      POINT pt{x, y};  // screen coordinates for wheel messages
      ScreenToClient(h, &pt);
      self->view.wheel((short)HIWORD(w) / (float)WHEEL_DELTA, pt.x, pt.y);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONDBLCLK:
      self->view.reset();
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) { SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return TRUE; }
      break;
  }
  return DefWindowProcW(h, m, w, l);
}

// ======================================================================== class factory + exports
class Factory : public IClassFactory {
public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { gLocks++; return 2; }
  STDMETHODIMP_(ULONG) Release() override { gLocks--; return 1; }
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    Preview* p = new (std::nothrow) Preview();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL lock) override { lock ? gLocks++ : gLocks--; return S_OK; }
};
static Factory gFactory;

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) { gInst = h; DisableThreadLibraryCalls(h); }
  return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (clsid != CLSID_GvPreview) return CLASS_E_CLASSNOTAVAILABLE;
  return gFactory.QueryInterface(riid, ppv);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return gObjects == 0 && gLocks == 0 && gThreads == 0 ? S_OK : S_FALSE; }

// ---- per-user registration (no admin rights needed) – also done from GlassView's settings
static bool setStr(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& val) {
  HKEY h;
  if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
  const bool ok = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  RegCloseKey(h);
  return ok;
}
static bool setDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD v) {
  HKEY h;
  if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
  const bool ok = RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v) == ERROR_SUCCESS;
  RegCloseKey(h);
  return ok;
}
static const wchar_t* kExts[] = {L".step", L".stp", L".iges", L".igs", L".stl", L".brep", L".brp", nullptr};
static const wchar_t* kPreviewShellex = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

extern "C" HRESULT __stdcall DllRegisterServer() {
  wchar_t dll[MAX_PATH];
  GetModuleFileNameW(gInst, dll, MAX_PATH);
  const std::wstring cls = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr;
  bool ok = setStr(HKEY_CURRENT_USER, cls, nullptr, L"GlassView 3D preview");
  ok &= setStr(HKEY_CURRENT_USER, cls, L"DisplayName", L"GlassView 3D preview");
  ok &= setStr(HKEY_CURRENT_USER, cls, L"AppID", L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}");  // 64-bit prevhost.exe
  ok &= setDword(HKEY_CURRENT_USER, cls, L"DisableLowILProcess", 1);
  ok &= setStr(HKEY_CURRENT_USER, cls + L"\\InprocServer32", nullptr, dll);
  ok &= setStr(HKEY_CURRENT_USER, cls + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
  for (const wchar_t** e = kExts; *e; e++) {
    ok &= setStr(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + *e + L"\\shellex\\" + kPreviewShellex, nullptr, kClsidStr);
    ok &= setStr(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + *e + L"\\shellex\\" + kPreviewShellex, nullptr, kClsidStr);
  }
  ok &= setStr(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", kClsidStr, L"GlassView 3D preview");
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return ok ? S_OK : E_FAIL;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
  RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr).c_str());
  for (const wchar_t** e = kExts; *e; e++) {
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + *e + L"\\shellex\\" + kPreviewShellex).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + *e + L"\\shellex\\" + kPreviewShellex).c_str());
  }
  HKEY h;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
    RegDeleteValueW(h, kClsidStr);
    RegCloseKey(h);
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return S_OK;
}
