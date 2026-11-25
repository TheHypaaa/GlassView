// GlassView Native – small OS helpers (file dialog, fonts, settings path, theme).
#pragma once
#include <string>
#include <vector>
#include <functional>

namespace gv {
std::vector<std::string> openFileDialog(void* nativeWindow);
std::string uiFontPath(bool semibold);
std::string settingsPath();
bool systemPrefersDark();
std::string toUtf8(const std::wstring& w);
std::string saveFileDialog(void* nativeWindow, const std::string& defaultName, const std::string& ext); // ext "step" | "stl"
#include <cstdio>
FILE* fopenUtf8(const std::string& path, const char* mode);
std::string exePath();
bool portableMode();                          // GlassView.ini next to the exe
bool fileTypesRegistered();
bool registerFileTypes(bool on);              // HKCU "Open with" entries for STEP/IGES/STL/BREP
void openDefaultAppsSettings();
bool previewDllPresent();                     // GlassViewPreview.dll next to the exe
bool previewRegistered();
bool registerPreview(bool on);                // Explorer preview pane: 3D, rotatable
void openUrl(const std::string& url);
bool forwardToRunningInstance(const std::vector<std::string>& files);  // true → this process should exit
void receiveForwardedFiles(void* hwnd, std::function<void(const std::vector<std::string>&)> cb);
} // namespace gv
