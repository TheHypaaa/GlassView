; GlassView setup wizard (NSIS 3). Built by scripts/build.sh:  makensis -DVERSION=x.y.z -DSRC=<files> -DOUTDIR=<out> GlassView.nsi
; Per-user ("only me", no admin) or all users (admin), file types, default app, Explorer preview, uninstaller.
; Silent:  GlassView-Setup.exe /S [/AllUsers | /CurrentUser]

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!ifndef VERSION
  !define VERSION "10.2.0"
!endif
!ifndef SRC
  !define SRC "files"
!endif
!ifndef OUTDIR
  !define OUTDIR "."
!endif

!define APP        "GlassView"
!define EXE        "GlassView.exe"
!define PROGID     "GlassView.Model"
!define REGAPP     "Software\GlassView"
!define UNINSTKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\GlassView"
!define CLSID      "{7A3F1C2E-5B8D-4E61-9C07-2D4B6A8E91F3}"
!define PVSHELLEX  "{8895b1c6-b41f-4c1c-a562-0d564250836f}"
!define POLICYKEY  "SOFTWARE\Policies\Microsoft\Windows\System"
!define POLICYVAL  "DefaultAssociationsConfiguration"

Name "${APP}"
OutFile "${OUTDIR}\GlassView-Setup-${VERSION}.exe"
BrandingText "GlassView ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "GlassView"
VIAddVersionKey "FileDescription" "GlassView Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" ""

; ---------------------------------------------------------------- per-user / all-users
!define MULTIUSER_EXECUTIONLEVEL Highest
!define MULTIUSER_MUI
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_USE_PROGRAMFILES64
!define MULTIUSER_INSTALLMODE_INSTDIR "GlassView"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_KEY "${REGAPP}"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_VALUENAME "InstallDir"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_KEY "${REGAPP}"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_VALUENAME "InstallDir"
!define MULTIUSER_INSTALLMODE_DEFAULT_CURRENTUSER
!define MULTIUSER_INSTALLMODE_FUNCTION onModeChange
!define MULTIUSER_INSTALLMODEPAGE_TEXT_TOP "GlassView can be installed just for you (no administrator rights needed) or for everyone who uses this PC.$\r$\n$\r$\nEveryone keeps their own settings, recent files and cache."
!define MULTIUSER_INSTALLMODEPAGE_TEXT_ALLUSERS "For everyone who uses this PC (administrator)"
!define MULTIUSER_INSTALLMODEPAGE_TEXT_CURRENTUSER "Only for me"
!include MultiUser.nsh
!include MUI2.nsh
!include LogicLib.nsh
!include Sections.nsh
!include FileFunc.nsh
!include x64.nsh

; ---------------------------------------------------------------- look
!define MUI_ICON "..\app\icon.ico"
!define MUI_UNICON "..\app\icon.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "header.bmp"
!define MUI_COMPONENTSPAGE_SMALLDESC

!define MUI_WELCOMEPAGE_TITLE "GlassView ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "A fast viewer for STEP, IGES, STL, 3MF and other 3D files.$\r$\n$\r$\nThis wizard installs GlassView, can open your 3D files with it and adds a 3D preview to Windows Explorer.$\r$\n$\r$\nIf GlassView is running, close it before you continue."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start GlassView"
!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Choose GlassView as the default app in Windows Settings"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION openDefaultApps
!define MUI_PAGE_CUSTOMFUNCTION_SHOW finishShow
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Var DefaultNeedsSettings ; 1 when "make default" was chosen for a per-user install

; ---------------------------------------------------------------- helpers
!macro FileTypes MACRO
  !insertmacro ${MACRO} ".step"
  !insertmacro ${MACRO} ".stp"
  !insertmacro ${MACRO} ".iges"
  !insertmacro ${MACRO} ".igs"
  !insertmacro ${MACRO} ".brep"
  !insertmacro ${MACRO} ".brp"
  !insertmacro ${MACRO} ".stl"
  !insertmacro ${MACRO} ".obj"
  !insertmacro ${MACRO} ".ply"
  !insertmacro ${MACRO} ".3mf"
  !insertmacro ${MACRO} ".glb"
  !insertmacro ${MACRO} ".gltf"
  !insertmacro ${MACRO} ".gva"
!macroend
!macro PreviewTypes MACRO
  !insertmacro ${MACRO} ".step"
  !insertmacro ${MACRO} ".stp"
  !insertmacro ${MACRO} ".iges"
  !insertmacro ${MACRO} ".igs"
  !insertmacro ${MACRO} ".stl"
  !insertmacro ${MACRO} ".brep"
  !insertmacro ${MACRO} ".brp"
!macroend

!macro AssocAdd EXT
  WriteRegStr SHCTX "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}" ""
  WriteRegStr SHCTX "Software\Classes\Applications\${EXE}\SupportedTypes" "${EXT}" ""
  WriteRegStr SHCTX "${REGAPP}\Capabilities\FileAssociations" "${EXT}" "${PROGID}"
!macroend
!macro AssocDel EXT
  DeleteRegValue SHCTX "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}"
  DeleteRegKey /ifempty SHCTX "Software\Classes\${EXT}\OpenWithProgids"
  DeleteRegKey /ifempty SHCTX "Software\Classes\${EXT}"
!macroend
!macro PvAdd EXT
  WriteRegStr SHCTX "Software\Classes\${EXT}\shellex\${PVSHELLEX}" "" "${CLSID}"
  WriteRegStr SHCTX "Software\Classes\SystemFileAssociations\${EXT}\shellex\${PVSHELLEX}" "" "${CLSID}"
!macroend
!macro PvDel EXT
  ReadRegStr $0 SHCTX "Software\Classes\${EXT}\shellex\${PVSHELLEX}" ""
  ${If} $0 == "${CLSID}"
    DeleteRegKey SHCTX "Software\Classes\${EXT}\shellex\${PVSHELLEX}"
    DeleteRegKey /ifempty SHCTX "Software\Classes\${EXT}\shellex"
  ${EndIf}
  ReadRegStr $0 SHCTX "Software\Classes\SystemFileAssociations\${EXT}\shellex\${PVSHELLEX}" ""
  ${If} $0 == "${CLSID}"
    DeleteRegKey SHCTX "Software\Classes\SystemFileAssociations\${EXT}\shellex\${PVSHELLEX}"
    DeleteRegKey /ifempty SHCTX "Software\Classes\SystemFileAssociations\${EXT}\shellex"
  ${EndIf}
!macroend

!macro RefreshShell
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
!macroend

; Replace a file that may be in use (Explorer's preview host keeps the DLL loaded)
!macro ReplaceLocked NAME
  ${If} ${FileExists} "$INSTDIR\${NAME}"
    ClearErrors
    Delete "$INSTDIR\${NAME}"
    ${If} ${Errors}
      Delete "$INSTDIR\${NAME}.old"
      Rename "$INSTDIR\${NAME}" "$INSTDIR\${NAME}.old"
      Delete /REBOOTOK "$INSTDIR\${NAME}.old"
    ${EndIf}
  ${EndIf}
!macroend

; ---------------------------------------------------------------- sections
Section "GlassView" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"

  ; the app must not be running while its files are replaced
  retry:
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    ClearErrors
    Delete "$INSTDIR\${EXE}"
    ${If} ${Errors}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "GlassView is still running.$\r$\n$\r$\nClose all GlassView windows, then click Retry." /SD IDCANCEL IDRETRY retry
      Abort "GlassView is running."
    ${EndIf}
  ${EndIf}
  nsExec::Exec 'taskkill /f /im gvkernel.exe'
  !insertmacro ReplaceLocked "GlassViewPreview.dll"

  File "${SRC}\${EXE}"
  File "${SRC}\GlassViewOpen.exe"
  File "${SRC}\gvkernel.exe"
  File "${SRC}\GlassViewPreview.dll"
  File "${SRC}\README.txt"
  WriteUninstaller "$INSTDIR\Uninstall GlassView.exe"

  ; Start menu + App Paths (Win+R "glassview")
  CreateShortcut "$SMPROGRAMS\GlassView.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\App Paths\${EXE}" "" "$INSTDIR\${EXE}"
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\App Paths\${EXE}" "Path" "$INSTDIR"

  ; "Installed apps" entry
  WriteRegStr SHCTX "${UNINSTKEY}" "DisplayName" "GlassView"
  WriteRegStr SHCTX "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr SHCTX "${UNINSTKEY}" "Publisher" "GlassView"
  WriteRegStr SHCTX "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\${EXE},0"
  WriteRegStr SHCTX "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr SHCTX "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall GlassView.exe" /$MultiUser.InstallMode'
  WriteRegStr SHCTX "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall GlassView.exe" /$MultiUser.InstallMode /S'
  WriteRegDWORD SHCTX "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD SHCTX "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD SHCTX "${UNINSTKEY}" "EstimatedSize" "$0"

  WriteRegStr SHCTX "${REGAPP}" "InstallDir" "$INSTDIR"
  WriteRegStr SHCTX "${REGAPP}" "Version" "${VERSION}"
  WriteRegStr SHCTX "${REGAPP}" "Mode" "$MultiUser.InstallMode"
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\GlassView.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
SectionEnd

Section "Open 3D files with GlassView" SecAssoc
  ; ProgID used by every file type
  WriteRegStr SHCTX "Software\Classes\${PROGID}" "" "3D model"
  WriteRegStr SHCTX "Software\Classes\${PROGID}" "FriendlyTypeName" "3D model (GlassView)"
  WriteRegStr SHCTX "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\${EXE},0"
  ; double-click goes through GlassViewOpen.exe: it passes the file to an open GlassView window
  WriteRegStr SHCTX "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\GlassViewOpen.exe" "%1"'
  ; "Open with" entry
  WriteRegStr SHCTX "Software\Classes\Applications\${EXE}" "FriendlyAppName" "GlassView"
  WriteRegStr SHCTX "Software\Classes\Applications\${EXE}\shell\open\command" "" '"$INSTDIR\GlassViewOpen.exe" "%1"'
  ; listed under Settings > Apps > Default apps
  WriteRegStr SHCTX "${REGAPP}\Capabilities" "ApplicationName" "GlassView"
  WriteRegStr SHCTX "${REGAPP}\Capabilities" "ApplicationDescription" "Fast viewer for STEP, IGES, STL and other 3D files"
  WriteRegStr SHCTX "${REGAPP}\Capabilities" "ApplicationIcon" "$INSTDIR\${EXE},0"
  WriteRegStr SHCTX "Software\RegisteredApplications" "GlassView" "${REGAPP}\Capabilities"
  !insertmacro FileTypes AssocAdd
  !insertmacro RefreshShell
SectionEnd

Section "Make GlassView the default app" SecDefault
  ${If} $MultiUser.InstallMode == "AllUsers"
    ; Windows lets no program set itself as default; for all users this goes through the
    ; "default associations" policy (applied at every sign-in).
    ReadRegStr $0 HKLM "${POLICYKEY}" "${POLICYVAL}"
    ${If} $0 != ""
    ${AndIf} $0 != "$COMMONPROGRAMDATA\GlassView\DefaultApps.xml"
      MessageBox MB_OK|MB_ICONINFORMATION "This PC already uses a default-apps policy ($0), probably set by your organisation. GlassView leaves it unchanged; ask your administrator to add GlassView to it." /SD IDOK
    ${Else}
      CreateDirectory "$COMMONPROGRAMDATA\GlassView"
      FileOpen $1 "$COMMONPROGRAMDATA\GlassView\DefaultApps.xml" w
      FileWrite $1 '<?xml version="1.0" encoding="UTF-8"?>$\r$\n<DefaultAssociations>$\r$\n'
      FileWrite $1 '  <Association Identifier=".step" ProgId="${PROGID}" ApplicationName="GlassView" />$\r$\n'
      FileWrite $1 '  <Association Identifier=".stp" ProgId="${PROGID}" ApplicationName="GlassView" />$\r$\n'
      FileWrite $1 '  <Association Identifier=".iges" ProgId="${PROGID}" ApplicationName="GlassView" />$\r$\n'
      FileWrite $1 '  <Association Identifier=".igs" ProgId="${PROGID}" ApplicationName="GlassView" />$\r$\n'
      FileWrite $1 '  <Association Identifier=".brep" ProgId="${PROGID}" ApplicationName="GlassView" />$\r$\n'
      FileWrite $1 '  <Association Identifier=".stl" ProgId="${PROGID}" ApplicationName="GlassView" />$\r$\n'
      FileWrite $1 '  <Association Identifier=".gva" ProgId="${PROGID}" ApplicationName="GlassView" />$\r$\n'
      FileWrite $1 '</DefaultAssociations>$\r$\n'
      FileClose $1
      WriteRegStr HKLM "${POLICYKEY}" "${POLICYVAL}" "$COMMONPROGRAMDATA\GlassView\DefaultApps.xml"
      WriteRegDWORD SHCTX "${REGAPP}" "DefaultPolicy" 1
    ${EndIf}
  ${Else}
    StrCpy $DefaultNeedsSettings 1
  ${EndIf}
SectionEnd

Section "3D preview in Explorer" SecPreview
  WriteRegStr SHCTX "Software\Classes\CLSID\${CLSID}" "" "GlassView 3D preview"
  WriteRegStr SHCTX "Software\Classes\CLSID\${CLSID}" "DisplayName" "GlassView 3D preview"
  WriteRegStr SHCTX "Software\Classes\CLSID\${CLSID}" "AppID" "{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}"
  WriteRegDWORD SHCTX "Software\Classes\CLSID\${CLSID}" "DisableLowILProcess" 1
  WriteRegStr SHCTX "Software\Classes\CLSID\${CLSID}\InprocServer32" "" "$INSTDIR\GlassViewPreview.dll"
  WriteRegStr SHCTX "Software\Classes\CLSID\${CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  !insertmacro PreviewTypes PvAdd
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\PreviewHandlers" "${CLSID}" "GlassView 3D preview"
  nsExec::Exec 'taskkill /f /im prevhost.exe'
  !insertmacro RefreshShell
SectionEnd

; options that are switched off on a re-run are removed again
Section "-cleanup"
  ${IfNot} ${SectionIsSelected} ${SecDesktop}
    Delete "$DESKTOP\GlassView.lnk"
  ${EndIf}
  ${IfNot} ${SectionIsSelected} ${SecPreview}
    Call RemovePreview
  ${EndIf}
  ${IfNot} ${SectionIsSelected} ${SecDefault}
    Call RemovePolicy
  ${EndIf}
  ${IfNot} ${SectionIsSelected} ${SecAssoc}
    Call RemoveAssoc
  ${EndIf}
  ; remember the choices for the next run
  ${If} ${SectionIsSelected} ${SecDesktop}
    WriteRegDWORD SHCTX "${REGAPP}" "OptDesktop" 1
  ${Else}
    WriteRegDWORD SHCTX "${REGAPP}" "OptDesktop" 0
  ${EndIf}
  ${If} ${SectionIsSelected} ${SecAssoc}
    WriteRegDWORD SHCTX "${REGAPP}" "OptAssoc" 1
  ${Else}
    WriteRegDWORD SHCTX "${REGAPP}" "OptAssoc" 0
  ${EndIf}
  ${If} ${SectionIsSelected} ${SecDefault}
    WriteRegDWORD SHCTX "${REGAPP}" "OptDefault" 1
  ${Else}
    WriteRegDWORD SHCTX "${REGAPP}" "OptDefault" 0
  ${EndIf}
  ${If} ${SectionIsSelected} ${SecPreview}
    WriteRegDWORD SHCTX "${REGAPP}" "OptPreview" 1
  ${Else}
    WriteRegDWORD SHCTX "${REGAPP}" "OptPreview" 0
  ${EndIf}
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "The app, the native geometry engine and the Explorer preview component."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A GlassView icon on the desktop."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAssoc} "Adds GlassView to 'Open with' for STEP, IGES, BREP, STL, OBJ, PLY, 3MF, glTF and GlassView assemblies."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDefault} "Double-click opens STEP, IGES, BREP, STL and .gva files in GlassView. For everyone: set by a Windows policy at each sign-in. Only for you: Windows Settings opens at the end, click 'Set default'."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPreview} "Rotatable 3D preview of STEP, IGES, STL and BREP files in the Explorer preview pane (Alt+P)."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------- shared removal (installer + uninstaller)
!macro RemoveFns UN
Function ${UN}RemovePreview
  ReadRegStr $0 SHCTX "Software\Classes\CLSID\${CLSID}\InprocServer32" ""
  ${If} $0 != ""
    DeleteRegKey SHCTX "Software\Classes\CLSID\${CLSID}"
    DeleteRegValue SHCTX "Software\Microsoft\Windows\CurrentVersion\PreviewHandlers" "${CLSID}"
    !insertmacro PreviewTypes PvDel
    nsExec::Exec 'taskkill /f /im prevhost.exe'
    !insertmacro RefreshShell
  ${EndIf}
FunctionEnd
Function ${UN}RemovePolicy
  ReadRegStr $0 HKLM "${POLICYKEY}" "${POLICYVAL}"
  ${If} $MultiUser.InstallMode == "AllUsers"
  ${AndIf} $0 == "$COMMONPROGRAMDATA\GlassView\DefaultApps.xml"
    DeleteRegValue HKLM "${POLICYKEY}" "${POLICYVAL}"
    Delete "$COMMONPROGRAMDATA\GlassView\DefaultApps.xml"
    RMDir "$COMMONPROGRAMDATA\GlassView"
  ${EndIf}
  DeleteRegValue SHCTX "${REGAPP}" "DefaultPolicy"
FunctionEnd
Function ${UN}RemoveAssoc
  !insertmacro FileTypes AssocDel
  DeleteRegKey SHCTX "Software\Classes\${PROGID}"
  DeleteRegKey SHCTX "Software\Classes\Applications\${EXE}"
  DeleteRegKey SHCTX "${REGAPP}\Capabilities"
  DeleteRegValue SHCTX "Software\RegisteredApplications" "GlassView"
  !insertmacro RefreshShell
FunctionEnd
!macroend
!insertmacro RemoveFns ""
!insertmacro RemoveFns "un."

; ---------------------------------------------------------------- installer callbacks
Function .onInit
  SetRegView 64
  !insertmacro MULTIUSER_INIT
  StrCpy $DefaultNeedsSettings 0
  Call restoreOptions
FunctionEnd

Function onModeChange
  SetRegView 64
FunctionEnd

; previous choices (re-run = change options / update)
Function restoreOptions
  ReadRegDWORD $0 SHCTX "${REGAPP}" "OptDesktop"
  ReadRegStr $1 SHCTX "${REGAPP}" "Version"
  ${If} $1 != ""
    ${If} $0 == 1
      !insertmacro SelectSection ${SecDesktop}
    ${Else}
      !insertmacro UnselectSection ${SecDesktop}
    ${EndIf}
    ReadRegDWORD $0 SHCTX "${REGAPP}" "OptAssoc"
    ${If} $0 == 0
      !insertmacro UnselectSection ${SecAssoc}
    ${EndIf}
    ReadRegDWORD $0 SHCTX "${REGAPP}" "OptDefault"
    ${If} $0 == 0
      !insertmacro UnselectSection ${SecDefault}
    ${EndIf}
    ReadRegDWORD $0 SHCTX "${REGAPP}" "OptPreview"
    ${If} $0 == 0
      !insertmacro UnselectSection ${SecPreview}
    ${EndIf}
  ${EndIf}
FunctionEnd

; "make default" needs the file types
Function .onSelChange
  ${If} ${SectionIsSelected} ${SecDefault}
    !insertmacro SelectSection ${SecAssoc}
  ${EndIf}
FunctionEnd

Function finishShow
  ${If} $DefaultNeedsSettings != 1
    ShowWindow $mui.FinishPage.ShowReadme ${SW_HIDE}
  ${Else}
    ${NSD_Check} $mui.FinishPage.ShowReadme
  ${EndIf}
FunctionEnd

Function openDefaultApps
  ; Windows 11 opens GlassView's own page; Windows 10 shows the general default-apps page
  ExecShell "open" "ms-settings:defaultapps?registeredAppUser=GlassView"
FunctionEnd

; ---------------------------------------------------------------- uninstaller
Function un.onInit
  SetRegView 64
  !insertmacro MULTIUSER_UNINIT
FunctionEnd

Section "Uninstall"
  ClearErrors
  Delete "$INSTDIR\${EXE}"
  ${If} ${Errors}
    MessageBox MB_OK|MB_ICONEXCLAMATION "GlassView is still running. Close it and run the uninstaller again." /SD IDOK
    Abort
  ${EndIf}
  nsExec::Exec 'taskkill /f /im gvkernel.exe'
  Call un.RemovePreview
  Call un.RemovePolicy
  Call un.RemoveAssoc

  Delete "$INSTDIR\gvkernel.exe"
  Delete "$INSTDIR\GlassViewOpen.exe"
  Delete /REBOOTOK "$INSTDIR\GlassViewPreview.dll"
  Delete /REBOOTOK "$INSTDIR\GlassViewPreview.dll.old"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\Uninstall GlassView.exe"
  RMDir /REBOOTOK "$INSTDIR"

  Delete "$SMPROGRAMS\GlassView.lnk"
  Delete "$DESKTOP\GlassView.lnk"
  DeleteRegKey SHCTX "Software\Microsoft\Windows\CurrentVersion\App Paths\${EXE}"
  DeleteRegKey SHCTX "${UNINSTKEY}"
  DeleteRegKey SHCTX "${REGAPP}"

  ; your own thumbnails / tessellation cache (other users keep theirs)
  ReadEnvStr $0 LOCALAPPDATA
  ${If} ${FileExists} "$0\GlassView\cache\*.*"
    MessageBox MB_YESNO|MB_ICONQUESTION "Also delete your GlassView thumbnails and tessellation cache ($0\GlassView)?" /SD IDNO IDNO keep
      RMDir /r "$0\GlassView"
    keep:
  ${EndIf}
  !insertmacro RefreshShell
SectionEnd
