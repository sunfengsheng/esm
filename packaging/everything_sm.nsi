Unicode True

!ifndef PROJECT_ROOT
  !define PROJECT_ROOT ".."
!endif
!ifndef BUILD_DIR
  !define BUILD_DIR "${PROJECT_ROOT}\build-release"
!endif
!ifndef OUTPUT_DIR
  !define OUTPUT_DIR "${PROJECT_ROOT}\dist"
!endif

!define PRODUCT_NAME "everything_sm"
!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "0.1.0"
!endif
!ifndef PRODUCT_VERSION_RESOURCE
  !define PRODUCT_VERSION_RESOURCE "0.1.0.0"
!endif
!define PRODUCT_PUBLISHER "everything_sm Project"
!define PRODUCT_WEB_SITE "https://github.com/sunfengsheng/esm"
!define PRODUCT_DIR_REGKEY "Software\everything_sm"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\everything_sm"
!define PRODUCT_PIPE "everything_sm_service"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "${OUTPUT_DIR}\everything_sm-${PRODUCT_VERSION}-setup.exe"
InstallDir "$PROGRAMFILES64\everything_sm"
InstallDirRegKey HKLM "${PRODUCT_DIR_REGKEY}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetDatablockOptimize on
CRCCheck on
BrandingText "everything_sm"
Icon "${PROJECT_ROOT}\assets\icon\everything_sm.ico"
UninstallIcon "${PROJECT_ROOT}\assets\icon\everything_sm.ico"
VIProductVersion "${PRODUCT_VERSION_RESOURCE}"
VIAddVersionKey /LANG=2052 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "FileDescription" "everything_sm 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "Clean-room implementation"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "nsDialogs.nsh"

!define MUI_ABORTWARNING
!define MUI_ICON "${PROJECT_ROOT}\assets\icon\everything_sm.ico"
!define MUI_UNICON "${PROJECT_ROOT}\assets\icon\everything_sm.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\esm_launcher.exe"
!define MUI_FINISHPAGE_RUN_TEXT "启动 everything_sm"
!define MUI_FINISHPAGE_LINK "打开项目说明"
!define MUI_FINISHPAGE_LINK_LOCATION "$INSTDIR\README.md"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
Page custom SearchConfigPage SearchConfigPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

Var ScanRoot
Var InstallService
Var ScanRootField
Var ServiceCheckbox
Var ServiceDataRoot
Var ServiceDataDir
Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "everything_sm 当前安装包仅支持 64 位 Windows。"
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  ${GetRoot} "$WINDIR" $0
  StrCpy $ScanRoot "$0\"
  StrCpy $InstallService "1"
  ReadEnvStr $ServiceDataRoot "ProgramData"
  ${If} $ServiceDataRoot == ""
    StrCpy $ServiceDataRoot "$0\ProgramData"
  ${EndIf}
  StrCpy $ServiceDataDir "$ServiceDataRoot\everything_sm\indexes"
FunctionEnd

Function SearchConfigPage
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 18u "兼容模式索引目录（系统服务不可用时自动使用）："
  Pop $0
  ${NSD_CreateText} 0 19u 100% 13u "$ScanRoot"
  Pop $ScanRootField

  ${NSD_CreateLabel} 0 38u 100% 28u "兼容模式会递归扫描目录并监控变化；全盘首次扫描可能较慢。"
  Pop $0

  ${NSD_CreateCheckbox} 0 70u 100% 12u "自动索引所有本地 NTFS 固定磁盘（推荐）"
  Pop $ServiceCheckbox
  ${NSD_SetState} $ServiceCheckbox $InstallService

  ${NSD_CreateLabel} 0 92u 100% 58u "服务会自动发现 C:、D:、E: 等本地 NTFS 固定磁盘，读取各卷 MFT 并统一搜索。索引快照保存在 C:\ProgramData\everything_sm\indexes，重启后可立即加载；后台每分钟协调一次，每五分钟持久化一次。若服务安装或启动失败，启动器会自动回退到兼容模式。"
  Pop $0

  nsDialogs::Show
FunctionEnd

Function SearchConfigPageLeave
  ${NSD_GetText} $ScanRootField $ScanRoot
  ${NSD_GetState} $ServiceCheckbox $InstallService

  ${If} $ScanRoot == ""
    MessageBox MB_OK|MB_ICONEXCLAMATION "请输入兼容模式索引目录。"
    Abort
  ${EndIf}
  IfFileExists "$ScanRoot\*.*" scan_root_ok
    MessageBox MB_OK|MB_ICONEXCLAMATION "兼容模式索引目录不存在：$ScanRoot"
    Abort
  scan_root_ok:
FunctionEnd

Section "安装 everything_sm" SEC_MAIN
  SectionIn RO
  SetRegView 64
  SetShellVarContext all

  ; Stop an older installed copy before replacing binaries.
  IfFileExists "$INSTDIR\esm_service.exe" 0 +3
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" stop'
    Pop $0
  IfFileExists "$INSTDIR\esm_service.exe" 0 +3
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" uninstall'
    Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_gui.exe'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_server.exe'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_launcher.exe'
  Pop $0

  SetOutPath "$INSTDIR"
  File /oname=esm_gui.exe "${BUILD_DIR}\esm_gui.exe"
  File /oname=esm_launcher.exe "${BUILD_DIR}\esm_launcher.exe"
  File /oname=esm_server.exe "${BUILD_DIR}\esm_server.exe"
  File /oname=esm_service.exe "${BUILD_DIR}\esm_service.exe"
  File /oname=esm_cli.exe "${BUILD_DIR}\esm_cli.exe"
  File /oname=README.md "${PROJECT_ROOT}\README.md"

  WriteINIStr "$INSTDIR\everything_sm.ini" "search" "scan_root" "$ScanRoot"
  WriteINIStr "$INSTDIR\everything_sm.ini" "search" "pipe_name" "${PRODUCT_PIPE}"

  ${If} $InstallService == ${BST_CHECKED}
    CreateDirectory "$ServiceDataRoot\everything_sm"
    CreateDirectory "$ServiceDataDir"
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" install-mft-auto "$ServiceDataDir" "${PRODUCT_PIPE}"'
    Pop $0
    ${If} $0 == "0"
      nsExec::ExecToLog '"$SYSDIR\sc.exe" config everything_sm start= delayed-auto'
      Pop $1
      nsExec::ExecToLog '"$INSTDIR\esm_service.exe" start'
      Pop $1
      ${If} $1 != "0"
        MessageBox MB_OK|MB_ICONEXCLAMATION "高性能 NTFS MFT 服务已安装，但启动失败（错误码 $1）。启动器将自动使用兼容模式。"
      ${EndIf}
    ${Else}
      MessageBox MB_OK|MB_ICONEXCLAMATION "高性能 NTFS MFT 服务安装失败（错误码 $0）。核心程序仍已安装，将自动使用兼容模式。"
    ${EndIf}
  ${EndIf}

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "ScanRoot" "$ScanRoot"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\esm_gui.exe"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoRepair" 1

  CreateDirectory "$SMPROGRAMS\everything_sm"
  CreateShortcut "$SMPROGRAMS\everything_sm\everything_sm.lnk" "$INSTDIR\esm_launcher.exe" "" "$INSTDIR\esm_gui.exe" 0 SW_SHOWNORMAL "" "快速文件搜索"
  CreateShortcut "$SMPROGRAMS\everything_sm\卸载 everything_sm.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\everything_sm.lnk" "$INSTDIR\esm_launcher.exe" "" "$INSTDIR\esm_gui.exe" 0 SW_SHOWNORMAL "" "快速文件搜索"
SectionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
  ReadEnvStr $ServiceDataRoot "ProgramData"
  ${If} $ServiceDataRoot == ""
    ${GetRoot} "$WINDIR" $0
    StrCpy $ServiceDataRoot "$0\ProgramData"
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all

  IfFileExists "$INSTDIR\esm_service.exe" 0 +3
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" stop'
    Pop $0
  IfFileExists "$INSTDIR\esm_service.exe" 0 +3
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" uninstall'
    Pop $0

  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_gui.exe'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_server.exe'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_launcher.exe'
  Pop $0

  Delete "$DESKTOP\everything_sm.lnk"
  RMDir /r "$SMPROGRAMS\everything_sm"

  DeleteRegKey HKLM "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKLM "${PRODUCT_DIR_REGKEY}"

  MessageBox MB_YESNO|MB_ICONQUESTION "是否同时删除机器级索引快照和当前用户设置？" IDNO keep_data
    RMDir /r "$ServiceDataRoot\everything_sm"
    RMDir /r "$LOCALAPPDATA\everything_sm"
  keep_data:

  Delete /REBOOTOK "$INSTDIR\esm_gui.exe"
  Delete /REBOOTOK "$INSTDIR\esm_launcher.exe"
  Delete /REBOOTOK "$INSTDIR\esm_server.exe"
  Delete /REBOOTOK "$INSTDIR\esm_service.exe"
  Delete /REBOOTOK "$INSTDIR\esm_cli.exe"
  Delete /REBOOTOK "$INSTDIR\everything_sm.ini"
  Delete /REBOOTOK "$INSTDIR\README.md"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"
SectionEnd

