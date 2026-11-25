// GlassViewOpen.exe – what Explorer runs when you double-click a 3D file.
// If GlassView is already open, the files go to that window (and it comes to the front) instead of
// starting a second copy of the app. Otherwise it simply starts GlassView.exe with the files.
// Tiny, no console window, starts in a few milliseconds.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

static std::wstring dirOf(const std::wstring& p) { size_t i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? L"." : p.substr(0, i); }
static std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

static std::wstring exePath() {
  wchar_t b[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 4);
  return std::wstring(b, n);
}

static std::wstring localAppData() {
  wchar_t b[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH * 2);
  return n ? std::wstring(b, n) : L"";
}

struct Find { std::wstring target; HWND hwnd = nullptr; DWORD pid = 0; };

static BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
  Find* f = (Find*)lp;
  if (!IsWindowVisible(h) && !IsIconic(h)) return TRUE;
  if (GetWindow(h, GW_OWNER)) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return TRUE;
  wchar_t img[MAX_PATH * 4];
  DWORD n = MAX_PATH * 4;
  bool match = QueryFullProcessImageNameW(p, 0, img, &n) && lower(std::wstring(img, n)) == f->target;
  CloseHandle(p);
  if (!match) return TRUE;
  // the main window has a title; skip helper windows
  if (GetWindowTextLengthW(h) == 0) return TRUE;
  f->hwnd = h;
  f->pid = pid;
  return FALSE;
}

static std::string utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const std::wstring app = dirOf(exePath()) + L"\\GlassView.exe";
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> files;
  for (int i = 1; i < argc; i++) if (argv[i][0] != L'-') files.push_back(argv[i]);

  const std::wstring lad = localAppData();
  bool single = true;
  if (!lad.empty()) single = GetPrivateProfileIntW(L"view", L"singleInstance", 1, (lad + L"\\GlassView\\view.ini").c_str()) != 0;

  if (single && !files.empty() && !lad.empty()) {
    Find f;
    f.target = lower(app);
    EnumWindows(enumProc, (LPARAM)&f);
    if (f.hwnd) {
      // hand the files over through the app's inbox folder
      const std::wstring dir = lad + L"\\GlassView\\cache";
      CreateDirectoryW((lad + L"\\GlassView").c_str(), nullptr);
      CreateDirectoryW(dir.c_str(), nullptr);
      std::string txt;
      for (auto& p : files) { txt += utf8(p); txt += "\n"; }
      wchar_t name[96];
      wsprintfW(name, L"\\inbox-%lu-%lu.txt", GetTickCount(), GetCurrentProcessId());
      const std::wstring tmp = dir + name + L".part", fin = dir + name;
      HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, txt.data(), (DWORD)txt.size(), &w, nullptr);
        CloseHandle(h);
        if (MoveFileExW(tmp.c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING)) {
          // bring the window forward (we were started by a click, so Windows lets us)
          AllowSetForegroundWindow(f.pid);
          if (IsIconic(f.hwnd)) ShowWindow(f.hwnd, SW_RESTORE);
          SetForegroundWindow(f.hwnd);
          LocalFree(argv);
          return 0;
        }
        DeleteFileW(tmp.c_str());
      }
    }
  }

  // start GlassView with the same arguments
  std::wstring cmd = L"\"" + app + L"\"";
  for (int i = 1; i < argc; i++) cmd += L" \"" + std::wstring(argv[i]) + L"\"";
  LocalFree(argv);
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  const std::wstring wd = dirOf(app);
  if (CreateProcessW(app.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, wd.c_str(), &si, &pi)) {
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
  }
  MessageBoxW(nullptr, (L"GlassView.exe was not found next to GlassViewOpen.exe:\n" + app).c_str(), L"GlassView", MB_ICONERROR);
  return 1;
}
