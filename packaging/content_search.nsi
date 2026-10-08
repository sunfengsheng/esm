Unicode True

!ifndef PROJECT_ROOT
  !define PROJECT_ROOT ".."
!endif
!ifndef BUILD_DIR
  !define BUILD_DIR "${PROJECT_ROOT}\build-content-release"
!endif
!ifndef OUTPUT_DIR
  !define OUTPUT_DIR "${PROJECT_ROOT}\dist"
!endif
!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "0.1.0"
!endif
!ifndef PRODUCT_VERSION_RESOURCE
  !define PRODUCT_VERSION_RESOURCE "0.1.0.0"
!endif

!define PRODUCT_NAME "Everything SM Content Search"
!define PRODUCT_PUBLISHER "everything_sm Project"
!define PRODUCT_WEB_SITE "https://github.com/sunfengsheng/esm"
!define PRODUCT_DIR_REGKEY "Software\everything_sm_content"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\everything_sm_content"
!define PRODUCT_PIPE "everything_sm_content_service"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION} (Preview)"
OutFile "${OUTPUT_DIR}\everything-sm-content-${PRODUCT_VERSION}-preview-setup.exe"
InstallDir "$LOCALAPPDATA\Programs\Everything SM Content Search"
InstallDirRegKey HKCU "${PRODUCT_DIR_REGKEY}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetDatablockOptimize on
CRCCheck on
BrandingText "Everything SM Content Search"
Icon "${PROJECT_ROOT}\assets\icon\content_search.ico"
UninstallIcon "${PROJECT_ROOT}\assets\icon\content_search.ico"
VIProductVersion "${PRODUCT_VERSION_RESOURCE}"
VIAddVersionKey /LANG=2052 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "FileDescription" "独立文件内容搜索预览安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "GPL-2.0-or-later; see CONTENT_SEARCH_LICENSE.md"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define MUI_ABORTWARNING
!define MUI_ICON "${PROJECT_ROOT}\assets\icon\content_search.ico"
!define MUI_UNICON "${PROJECT_ROOT}\assets\icon\content_search.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\esm_content.exe"
!define MUI_FINISHPAGE_RUN_PARAMETERS "--config $\"$LOCALAPPDATA\everything_sm_content\content.ini$\""
!define MUI_FINISHPAGE_RUN_TEXT "启动 Everything SM 内容搜索"
!define MUI_FINISHPAGE_LINK "查看内容搜索说明"
!define MUI_FINISHPAGE_LINK_LOCATION "$INSTDIR\CONTENT_SEARCH.md"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "内容搜索当前仅支持 64 位 Windows。"
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext current
FunctionEnd

Section "安装独立内容搜索" SEC_MAIN
  SectionIn RO
  SetRegView 64
  SetShellVarContext current

  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_content.exe'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_content_service.exe'
  Pop $0

  SetOutPath "$INSTDIR"
  File /oname=esm_content.exe "${BUILD_DIR}\esm_content.exe"
  File /oname=esm_content_service.exe "${BUILD_DIR}\esm_content_service.exe"
  File /oname=esm_content_cli.exe "${BUILD_DIR}\esm_content_cli.exe"
  File /oname=CONTENT_SEARCH.md "${PROJECT_ROOT}\docs\CONTENT_SEARCH.md"
  File /oname=CONTENT_SEARCH_LICENSE.md "${PROJECT_ROOT}\CONTENT_SEARCH_LICENSE.md"
  File /oname=GPL-2.0-or-later.txt "${PROJECT_ROOT}\LICENSES\GPL-2.0-or-later.txt"
  File /oname=XAPIAN-COPYING "${PROJECT_ROOT}\third_party\xapian-core\COPYING"
  FileOpen $0 "$INSTDIR\SOURCE-CODE.txt" w
  FileWrite $0 "The complete corresponding source for this version is available at:$\r$\n"
  FileWrite $0 "https://github.com/sunfengsheng/esm/releases/download/v${PRODUCT_VERSION}/everything-sm-content-${PRODUCT_VERSION}-source.zip$\r$\n$\r$\n"
  FileWrite $0 "Verify it with everything-sm-content-${PRODUCT_VERSION}-source.zip.sha256 on the same release page.$\r$\n"
  FileClose $0

  CreateDirectory "$LOCALAPPDATA\everything_sm_content"
  CreateDirectory "$LOCALAPPDATA\everything_sm_content\index"
  IfFileExists "$LOCALAPPDATA\everything_sm_content\content.ini" config_ready
    WriteINIStr "$LOCALAPPDATA\everything_sm_content\content.ini" "content" "version" "1"
    WriteINIStr "$LOCALAPPDATA\everything_sm_content\content.ini" "content" "database_root" "$LOCALAPPDATA\everything_sm_content\index"
    WriteINIStr "$LOCALAPPDATA\everything_sm_content\content.ini" "content" "pipe_name" "${PRODUCT_PIPE}"
    WriteINIStr "$LOCALAPPDATA\everything_sm_content\content.ini" "content" "maximum_bytes" "4194304"
    WriteINIStr "$LOCALAPPDATA\everything_sm_content\content.ini" "content" "all_fixed" "0"
    WriteINIStr "$LOCALAPPDATA\everything_sm_content\content.ini" "content" "use_default_excludes" "1"
    WriteINIStr "$LOCALAPPDATA\everything_sm_content\content.ini" "content" "root_count" "1"
    WriteINIStr "$LOCALAPPDATA\everything_sm_content\content.ini" "content" "root0" "$PROFILE"
    WriteINIStr "$LOCALAPPDATA\everything_sm_content\content.ini" "content" "exclude_count" "0"
  config_ready:

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${PRODUCT_DIR_REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME} (Preview)"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\esm_content.exe"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${PRODUCT_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${PRODUCT_UNINST_KEY}" "NoRepair" 1

  CreateDirectory "$SMPROGRAMS\Everything SM Content Search"
  CreateShortcut "$SMPROGRAMS\Everything SM Content Search\内容搜索.lnk" "$INSTDIR\esm_content.exe" '--config "$LOCALAPPDATA\everything_sm_content\content.ini"' "$INSTDIR\esm_content.exe" 0
  CreateShortcut "$SMPROGRAMS\Everything SM Content Search\卸载内容搜索.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\Everything SM 内容搜索.lnk" "$INSTDIR\esm_content.exe" '--config "$LOCALAPPDATA\everything_sm_content\content.ini"' "$INSTDIR\esm_content.exe" 0
SectionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext current
FunctionEnd

Section "Uninstall"
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_content.exe'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM esm_content_service.exe'
  Pop $0

  Delete "$DESKTOP\Everything SM 内容搜索.lnk"
  RMDir /r "$SMPROGRAMS\Everything SM Content Search"
  DeleteRegKey HKCU "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKCU "${PRODUCT_DIR_REGKEY}"

  MessageBox MB_YESNO|MB_ICONQUESTION "是否同时删除当前用户的内容索引和配置？" IDNO keep_data
    RMDir /r "$LOCALAPPDATA\everything_sm_content"
  keep_data:

  Delete /REBOOTOK "$INSTDIR\esm_content.exe"
  Delete /REBOOTOK "$INSTDIR\esm_content_service.exe"
  Delete /REBOOTOK "$INSTDIR\esm_content_cli.exe"
  Delete /REBOOTOK "$INSTDIR\CONTENT_SEARCH.md"
  Delete /REBOOTOK "$INSTDIR\CONTENT_SEARCH_LICENSE.md"
  Delete /REBOOTOK "$INSTDIR\GPL-2.0-or-later.txt"
  Delete /REBOOTOK "$INSTDIR\XAPIAN-COPYING"
  Delete /REBOOTOK "$INSTDIR\SOURCE-CODE.txt"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
