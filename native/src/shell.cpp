// GlassView Native – Windows integration: recent files, "Open with", single instance,
// PNG export of the view, update check.
#include "features.hpp"
#include "version.hpp"

#include <Image_PixMap.hxx>
#include <V3d_ImageDumpOptions.hxx>
#include <GLFW/glfw3.h>
#include <atomic>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

// ======================================================================== recent files
std::vector<std::string> recentFiles(App& a) {
  std::vector<std::string> out;
  const std::string s = a.set.get("recent", "");
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find('|', i);
    if (j == std::string::npos) j = s.size();
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

void addRecent(App& a, const std::string& path) {
  std::vector<std::string> r = recentFiles(a);
  r.erase(std::remove(r.begin(), r.end(), path), r.end());
  r.insert(r.begin(), path);
  if (r.size() > 12) r.resize(12);
  std::string s;
  for (auto& p : r) { if (!s.empty()) s += '|'; s += p; }
  a.set.kv["recent"] = s;
  a.set.save();
}

void clearRecent(App& a) { a.set.kv["recent"] = ""; a.set.save(); }

// ======================================================================== PNG export
static void pngSink(void* ctx, void* data, int size) { fwrite(data, 1, (size_t)size, (FILE*)ctx); }

bool writePng(const std::string& path, int w, int h, int comps, const unsigned char* data, int stride) {
  FILE* f = fopenUtf8(path, "wb");
  if (!f) return false;
  stbi_write_png_compression_level = 6;
  const int ok = stbi_write_png_to_func(pngSink, f, w, h, comps, data, stride);
  fclose(f);
  return ok != 0;
}

/** Renders the 3D view off-screen at `factor` × the window size (no interface, no view cube). */
bool exportViewPng(App& a, const std::string& path, int factor) {
  int fw, fh;
  glfwGetFramebufferSize(a.win, &fw, &fh);
  const int W = std::max(16, fw * factor), H = std::max(16, fh * factor);
  const bool cube = a.ctx->IsDisplayed(a.cube);
  if (cube) a.ctx->Erase(a.cube, false);
  Image_PixMap img;
  V3d_ImageDumpOptions o;
  o.Width = W;
  o.Height = H;
  o.BufferType = Graphic3d_BT_RGB;
  o.ToAdjustAspect = true;
  bool ok = a.view->ToPixMap(img, o);
  if (cube) a.ctx->Display(a.cube, false);
  a.view->Invalidate();
  if (!ok || img.IsEmpty()) return false;
  std::vector<unsigned char> rgb((size_t)img.Width() * img.Height() * 3);
  for (size_t r = 0; r < img.Height(); r++) {
    const size_t src = img.IsTopDown() ? r : img.Height() - 1 - r;
    memcpy(&rgb[r * img.Width() * 3], img.Row(src), img.Width() * 3);
  }
  return writePng(path, (int)img.Width(), (int)img.Height(), 3, rgb.data(), (int)img.Width() * 3);
}

// ======================================================================== update check
struct UpdateState { std::atomic<int> state{0}; std::string latest, url, error; std::thread th; }; // 0 idle 1 busy 2 done 3 error
static UpdateState upd;

#ifdef _WIN32
static std::string httpsGet(const std::wstring& host, const std::wstring& path, std::string& err) {
  std::string body;
  HINTERNET s = WinHttpOpen(L"GlassViewNative/" GV_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) s = WinHttpOpen(L"GlassViewNative", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) { err = "no network"; return body; }
  WinHttpSetTimeouts(s, 5000, 5000, 8000, 8000);
  HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
  if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(r, &avail) && avail > 0 && body.size() < 2000000) {
      std::string chunk(avail, 0);
      DWORD got = 0;
      if (!WinHttpReadData(r, chunk.data(), avail, &got) || got == 0) break;
      body.append(chunk.data(), got);
    }
    if (status != 200) { err = status == 404 ? "no release found" : "server answered " + std::to_string(status); body.clear(); }
  } else err = "could not reach github.com";
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return body;
}
#endif

static std::string jsonString(const std::string& j, const std::string& key) {
  size_t p = j.find("\"" + key + "\"");
  if (p == std::string::npos) return "";
  p = j.find(':', p);
  p = j.find('"', p);
  if (p == std::string::npos) return "";
  size_t e = j.find('"', p + 1);
  return e == std::string::npos ? "" : j.substr(p + 1, e - p - 1);
}

/** "v2.10.1" > "2.9" ? */
static bool newer(const std::string& tag, const std::string& cur) {
  auto parse = [](std::string s) {
    std::vector<int> v;
    size_t i = 0;
    while (i < s.size() && !isdigit((unsigned char)s[i])) i++;
    while (i < s.size()) {
      int n = 0;
      bool any = false;
      while (i < s.size() && isdigit((unsigned char)s[i])) { n = n * 10 + (s[i++] - '0'); any = true; }
      if (any) v.push_back(n);
      if (i < s.size() && s[i] == '.') i++; else break;
    }
    return v;
  };
  auto a = parse(tag), b = parse(cur);
  for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
    const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
    if (x != y) return x > y;
  }
  return false;
}

void checkForUpdates(App& a, bool silent) {
  std::string repo = a.set.get("updateRepo", "");
  if (repo.empty() || upd.state == 1) return;
  if (upd.th.joinable()) upd.th.join();
  upd.state = 1;
  upd.th = std::thread([repo, silent, &a]() {
#ifdef _WIN32
    std::string err;
    std::string body = httpsGet(L"api.github.com", L"/repos/" + std::wstring(repo.begin(), repo.end()) + L"/releases/latest", err);
    if (!body.empty()) {
      upd.latest = jsonString(body, "tag_name");
      upd.url = jsonString(body, "html_url");
      upd.state = upd.latest.empty() ? 3 : 2;
      if (upd.latest.empty()) upd.error = "unexpected answer";
    } else { upd.error = err; upd.state = 3; }
#else
    upd.error = "update check is Windows-only";
    upd.state = 3;
#endif
    (void)silent; (void)a;
    glfwPostEmptyEvent();
  });
  a.set.kv["updateSilent"] = silent ? "1" : "0";
}

/** Called each frame: reports a finished check. */
void pollUpdates(App& a) {
  const int s = upd.state;
  if (s < 2) return;
  if (upd.th.joinable()) upd.th.join();
  const bool silent = a.set.get("updateSilent", "0") == "1";
  if (s == 2) {
    a.set.kv["updateLastCheck"] = std::to_string((long long)time(nullptr));
    a.set.save();
    if (newer(upd.latest, GV_VERSION)) {
      a.toastMsg("Update available: " + upd.latest + " – see Settings › About", false, 8);
      a.set.kv["updateAvailable"] = upd.latest;
      a.set.kv["updateUrl"] = upd.url;
    } else {
      a.set.kv.erase("updateAvailable");
      if (!silent) a.toastMsg(std::string("GlassView ") + GV_VERSION + " is up to date", false, 3);
    }
  } else if (!silent) a.toastMsg("Update check failed: " + upd.error, true, 4);
  upd.state = 0;
}

void updateShutdown() { if (upd.th.joinable()) upd.th.join(); }

bool updateBusy() { return upd.state == 1; }
