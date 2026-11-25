#include "platform.hpp"
#include <cstdlib>
#include <fstream>
#include <functional>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#endif

namespace gv {

#ifdef _WIN32
std::string toUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::vector<std::string> openFileDialog(void* hwnd) {
  std::vector<wchar_t> buf(32768, 0);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = (HWND)hwnd;
  ofn.lpstrFilter = L"3D models (STEP, IGES, STL, BREP)\0*.step;*.stp;*.iges;*.igs;*.stl;*.brep;*.brp\0All files\0*.*\0";
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = (DWORD)buf.size();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
  std::vector<std::string> out;
  if (!GetOpenFileNameW(&ofn)) return out;
  std::wstring dir(buf.data());
  const wchar_t* p = buf.data() + dir.size() + 1;
  if (!*p) { out.push_back(toUtf8(dir)); return out; }
  while (*p) {
    std::wstring f(p);
    out.push_back(toUtf8(dir + L"\\" + f));
    p += f.size() + 1;
  }
  return out;
}

static std::wstring toWide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string saveFileDialog(void* hwnd, const std::string& defaultName, const std::string& ext) {
  std::vector<wchar_t> buf(4096, 0);
  std::wstring def = toWide(defaultName);
  for (size_t i = 0; i < def.size() && i < buf.size() - 1; i++) buf[i] = def[i];
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = (HWND)hwnd;
  ofn.lpstrFilter = ext == "stl" ? L"STL (binary)\0*.stl\0" : ext == "3mf" ? L"3MF (3D printing)\0*.3mf\0" : ext == "png" ? L"PNG image\0*.png\0" : L"STEP\0*.step;*.stp\0";
  static std::wstring defExt;
  defExt = toWide(ext);
  ofn.lpstrDefExt = defExt.c_str();
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = (DWORD)buf.size();
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&ofn)) return "";
  return toUtf8(buf.data());
}


// ------------------------------------------------------------------ file types ("Open with")
static const wchar_t* kProgId = L"GlassView.CADFile";
static const wchar_t* kExts[] = {L".step", L".stp", L".iges", L".igs", L".stl", L".brep", L".brp", nullptr};

static bool setKey(const std::wstring& key, const wchar_t* name, const std::wstring& val) {
  HKEY h;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
  const bool ok = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  RegCloseKey(h);
  return ok;
}

bool fileTypesRegistered() {
  std::wstring want = L"\"" + toWide(exePath()) + L"\" \"%1\"";
  wchar_t buf[2048];
  DWORD sz = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + kProgId + L"\\shell\\open\\command").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
  return _wcsicmp(buf, want.c_str()) == 0;
}

bool registerFileTypes(bool on) {
  const std::wstring base = std::wstring(L"Software\\Classes\\") + kProgId;
  const std::wstring exe = toWide(exePath());
  bool ok = true;
  if (on) {
    ok &= setKey(base, nullptr, L"CAD model (GlassView)");
    ok &= setKey(base + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
    ok &= setKey(base + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"");
    const std::wstring app = L"Software\\Classes\\Applications\\GlassViewNative.exe";
    ok &= setKey(app, L"FriendlyAppName", L"GlassView");
    ok &= setKey(app + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"");
    for (const wchar_t** e = kExts; *e; e++) {
      ok &= setKey(std::wstring(L"Software\\Classes\\") + *e + L"\\OpenWithProgids", kProgId, L"");
      ok &= setKey(app + L"\\SupportedTypes", *e, L"");
    }
  } else {
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\GlassViewNative.exe");
    for (const wchar_t** e = kExts; *e; e++) {
      HKEY h;
      if (RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + *e + L"\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, kProgId);
        RegCloseKey(h);
      }
    }
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return ok;
}

// ------------------------------------------------------------------ Explorer 3D preview (GlassViewPreview.dll)
static std::wstring previewDll() {
  std::wstring e = toWide(exePath());
  return e.substr(0, e.find_last_of(L"\\/") + 1) + L"GlassViewPreview.dll";
}
bool previewDllPresent() { return GetFileAttributesW(previewDll().c_str()) != INVALID_FILE_ATTRIBUTES; }
bool previewRegistered() {
  wchar_t buf[2048];
  DWORD sz = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{7A3F1C2E-5B8D-4E61-9C07-2D4B6A8E91F3}\\InprocServer32", nullptr, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
  return _wcsicmp(buf, previewDll().c_str()) == 0;
}
bool registerPreview(bool on) {
  // the DLL registers itself per user (HKCU) – no admin rights needed
  HMODULE m = LoadLibraryW(previewDll().c_str());
  if (!m) return false;
  typedef HRESULT(__stdcall * Fn)();
  Fn f = (Fn)GetProcAddress(m, on ? "DllRegisterServer" : "DllUnregisterServer");
  const bool ok = f && SUCCEEDED(f());
  FreeLibrary(m);
  return ok;
}

void openDefaultAppsSettings() { ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL); }
void openUrl(const std::string& url) { ShellExecuteW(nullptr, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ------------------------------------------------------------------ single instance
static const wchar_t* kMutex = L"GlassViewNative.SingleInstance";
static const ULONG_PTR kCopyId = 0x47564E31; // "GVN1"
static HANDLE gMutex = nullptr;

struct FindCtx { HWND found = nullptr; };
static BOOL CALLBACK findGv(HWND h, LPARAM lp) {
  wchar_t cls[64], title[256];
  GetClassNameW(h, cls, 64);
  GetWindowTextW(h, title, 256);
  if (wcscmp(cls, L"GLFW30") == 0 && wcsstr(title, L"GlassView")) { ((FindCtx*)lp)->found = h; return FALSE; }
  return TRUE;
}

bool forwardToRunningInstance(const std::vector<std::string>& files) {
  gMutex = CreateMutexW(nullptr, FALSE, kMutex);
  if (GetLastError() != ERROR_ALREADY_EXISTS || files.empty()) return false;
  FindCtx c;
  for (int tries = 0; tries < 20 && !c.found; tries++) { EnumWindows(findGv, (LPARAM)&c); if (!c.found) Sleep(100); }
  if (!c.found) return false;
  std::string blob;
  for (auto& f : files) { blob += f; blob.push_back('\0'); }
  COPYDATASTRUCT cd{kCopyId, (DWORD)blob.size(), (void*)blob.data()};
  DWORD_PTR res = 0;
  if (!SendMessageTimeoutW(c.found, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &res)) return false;
  if (IsIconic(c.found)) ShowWindow(c.found, SW_RESTORE);
  AllowSetForegroundWindow(ASFW_ANY);
  SetForegroundWindow(c.found);
  return true;
}

static WNDPROC gPrevProc = nullptr;
static std::function<void(const std::vector<std::string>&)> gOnFiles;
static LRESULT CALLBACK gvProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_COPYDATA) {
    auto* cd = (COPYDATASTRUCT*)l;
    if (cd && cd->dwData == kCopyId) {
      std::vector<std::string> files;
      const char* p = (const char*)cd->lpData;
      size_t i = 0;
      while (i < cd->cbData) { std::string s(p + i); i += s.size() + 1; if (!s.empty()) files.push_back(s); }
      if (gOnFiles) gOnFiles(files);
      SetForegroundWindow(h);
      return TRUE;
    }
  }
  return CallWindowProcW(gPrevProc, h, m, w, l);
}

void receiveForwardedFiles(void* hwnd, std::function<void(const std::vector<std::string>&)> cb) {
  gOnFiles = std::move(cb);
  gPrevProc = (WNDPROC)SetWindowLongPtrW((HWND)hwnd, GWLP_WNDPROC, (LONG_PTR)gvProc);
}

FILE* fopenUtf8(const std::string& path, const char* mode) {
  return _wfopen(toWide(path).c_str(), toWide(mode).c_str());
}

std::string uiFontPath(bool semibold) {
  const char* win = getenv("WINDIR");
  std::string d = std::string(win ? win : "C:\\Windows") + "\\Fonts\\";
  static const char* bold[] = {"seguisb.ttf", "segoeuib.ttf", nullptr};
  static const char* reg[] = {"segoeui.ttf", nullptr};
  for (const char** f = semibold ? bold : reg; *f; f++) {
    std::ifstream t(d + *f);
    if (t.good()) return d + *f;
  }
  return "";
}

std::string exePath() {
  wchar_t p[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
  return toUtf8(std::wstring(p, n));
}

bool portableMode() {
  // portable: a GlassView.ini next to the exe is used instead of %APPDATA%
  std::wstring e = toWide(exePath());
  e = e.substr(0, e.find_last_of(L"\\/") + 1) + L"GlassView.ini";
  return GetFileAttributesW(e.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string settingsPath() {
  if (portableMode()) { std::string e = exePath(); return e.substr(0, e.find_last_of("\\/") + 1) + "GlassView.ini"; }
  wchar_t path[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
    std::wstring d = std::wstring(path) + L"\\GlassView";
    CreateDirectoryW(d.c_str(), nullptr);
    return toUtf8(d + L"\\native.ini");
  }
  return "glassview-native.ini";
}

bool systemPrefersDark() {
  DWORD v = 1, sz = sizeof(v);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
    return v == 0;
  return false;
}
#else
std::string toUtf8(const std::wstring& w) { return std::string(w.begin(), w.end()); }
std::vector<std::string> openFileDialog(void*) { return {}; }
std::string saveFileDialog(void*, const std::string& defaultName, const std::string&) {
  const char* d = getenv("GV_SAVE_DIR");
  return d ? std::string(d) + "/" + defaultName : "";
}
FILE* fopenUtf8(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }
std::string exePath() { return "/proc/self/exe"; }
bool portableMode() { return false; }
bool fileTypesRegistered() { return false; }
bool previewDllPresent() { return false; }
bool previewRegistered() { return false; }
bool registerPreview(bool) { return false; }
bool registerFileTypes(bool) { return false; }
void openDefaultAppsSettings() {}
void openUrl(const std::string&) {}
bool forwardToRunningInstance(const std::vector<std::string>&) { return false; }
void receiveForwardedFiles(void*, std::function<void(const std::vector<std::string>&)>) {}
std::string uiFontPath(bool semibold) {
  static const char* bold[] = {"/usr/share/fonts/opentype/inter/Inter-SemiBold.otf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", nullptr};
  static const char* reg[] = {"/usr/share/fonts/opentype/inter/Inter-Regular.otf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", nullptr};
  for (const char** f = semibold ? bold : reg; *f; f++) {
    std::ifstream t(*f);
    if (t.good()) return *f;
  }
  return "";
}
std::string settingsPath() {
  const char* h = getenv("HOME");
  return std::string(h ? h : ".") + "/.glassview-native.ini";
}
bool systemPrefersDark() { return false; }
#endif

} // namespace gv
