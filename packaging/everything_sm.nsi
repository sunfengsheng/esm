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
!ifdef SIGN_UNINSTALLER
  !uninstfinalize 'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "${PROJECT_ROOT}\packaging\sign-nsis-uninstaller.ps1" "%1"' = 0
!endif
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
Var ServiceInstallState
Var UpgradeBackupDir
Var HadPreviousInstall
Var HadPreviousService
Var PreviousServiceMode
Var PreviousScanRoot
Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "everything_sm 当前安装包仅支持 64 位 Windows。" /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  ${GetRoot} "$WINDIR" $0
  StrCpy $ScanRoot "$0\"
  StrCpy $InstallService "1"
  StrCpy $ServiceInstallState "compatibility"
  StrCpy $HadPreviousInstall "0"
  StrCpy $HadPreviousService "0"
  StrCpy $PreviousServiceMode "compatibility"
  StrCpy $PreviousScanRoot $ScanRoot
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
    MessageBox MB_OK|MB_ICONEXCLAMATION "请输入兼容模式索引目录。" /SD IDOK
    Abort
  ${EndIf}
  IfFileExists "$ScanRoot\*.*" scan_root_ok
    MessageBox MB_OK|MB_ICONEXCLAMATION "兼容模式索引目录不存在：$ScanRoot" /SD IDOK
    Abort
  scan_root_ok:
FunctionEnd

Section "安装 everything_sm" SEC_MAIN
  SectionIn RO
  SetRegView 64

  ; Back up the old installation before stopping processes or touching SCM.
  ReadRegStr $PreviousServiceMode HKLM "${PRODUCT_DIR_REGKEY}" "ServiceMode"
  ${If} $PreviousServiceMode == ""
    StrCpy $PreviousServiceMode "compatibility"
  ${EndIf}
  ReadRegStr $PreviousScanRoot HKLM "${PRODUCT_DIR_REGKEY}" "ScanRoot"
  ${If} $PreviousScanRoot == ""
    StrCpy $PreviousScanRoot $ScanRoot
  ${EndIf}
  InitPluginsDir
  StrCpy $UpgradeBackupDir "$INSTDIR\.everything_sm-upgrade-backup"
  IfFileExists "$INSTDIR\esm_gui.exe" 0 no_previous_install
    StrCpy $HadPreviousInstall "1"
    IfFileExists "$UpgradeBackupDir\*.*" 0 create_upgrade_backup
      MessageBox MB_OK|MB_ICONSTOP "检测到上次失败升级留下的恢复目录：$UpgradeBackupDir。请先备份并处理该目录，本次安装不会覆盖它。" /SD IDOK
      SetErrorLevel 1
      Quit
    create_upgrade_backup:
    CreateDirectory "$UpgradeBackupDir"
    ClearErrors
    CopyFiles /SILENT "$INSTDIR\esm_gui.exe" "$UpgradeBackupDir\esm_gui.exe"
    CopyFiles /SILENT "$INSTDIR\esm_launcher.exe" "$UpgradeBackupDir\esm_launcher.exe"
    CopyFiles /SILENT "$INSTDIR\esm_server.exe" "$UpgradeBackupDir\esm_server.exe"
    CopyFiles /SILENT "$INSTDIR\esm_service.exe" "$UpgradeBackupDir\esm_service.exe"
    CopyFiles /SILENT "$INSTDIR\esm_cli.exe" "$UpgradeBackupDir\esm_cli.exe"
    IfFileExists "$INSTDIR\everything_sm.ini" 0 +2
      CopyFiles /SILENT "$INSTDIR\everything_sm.ini" "$UpgradeBackupDir\everything_sm.ini"
    IfFileExists "$INSTDIR\README.md" 0 +2
      CopyFiles /SILENT "$INSTDIR\README.md" "$UpgradeBackupDir\README.md"
    IfFileExists "$INSTDIR\Uninstall.exe" 0 +2
      CopyFiles /SILENT "$INSTDIR\Uninstall.exe" "$UpgradeBackupDir\Uninstall.exe"
    IfFileExists "$INSTDIR\export-diagnostics.ps1" 0 +2
      CopyFiles /SILENT "$INSTDIR\export-diagnostics.ps1" "$UpgradeBackupDir\export-diagnostics.ps1"
    ${If} ${Errors}
      RMDir /r "$UpgradeBackupDir"
      MessageBox MB_OK|MB_ICONSTOP "无法完整备份旧版本，升级尚未修改现有安装。请关闭占用文件的程序后重试。" /SD IDOK
      SetErrorLevel 1
      Quit
    ${EndIf}
  no_previous_install:

  ; Stop only processes whose image path belongs to this installation.
  SetOutPath "$PLUGINSDIR"
  File /oname=stop-install-processes.ps1 "${PROJECT_ROOT}\packaging\stop-install-processes.ps1"
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\stop-install-processes.ps1" -InstallDirectory "$INSTDIR"'
  Pop $0
  ${If} $0 != "0"
    RMDir /r "$UpgradeBackupDir"
    MessageBox MB_OK|MB_ICONSTOP "无法安全停止安装目录中的旧程序（错误码 $0）。现有版本未被覆盖。" /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}

  nsExec::ExecToLog '"$SYSDIR\sc.exe" query everything_sm'
  Pop $0
  ${If} $0 == "0"
    StrCpy $HadPreviousService "1"
    IfFileExists "$INSTDIR\esm_service.exe" upgrade_stop_retry 0
      RMDir /r "$UpgradeBackupDir"
      MessageBox MB_OK|MB_ICONSTOP "SCM 中存在 everything_sm 服务，但安装目录缺少 esm_service.exe。本次升级停止。" /SD IDOK
      SetErrorLevel 1
      Quit
    upgrade_stop_retry:
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" stop'
    Pop $0
    ${If} $0 != "0"
      MessageBox MB_RETRYCANCEL|MB_ICONSTOP "无法停止旧版 everything_sm 服务（错误码 $0）。请选择“重试”，或取消安装以保留现有版本。" /SD IDCANCEL IDRETRY upgrade_stop_retry
      RMDir /r "$UpgradeBackupDir"
      SetErrorLevel 1
      Quit
    ${EndIf}
    upgrade_uninstall_retry:
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" uninstall'
    Pop $0
    ${If} $0 != "0"
      MessageBox MB_RETRYCANCEL|MB_ICONSTOP "无法卸载旧版 everything_sm 服务（错误码 $0）。请选择“重试”，或取消安装以保留现有版本。" /SD IDCANCEL IDRETRY upgrade_uninstall_retry
      RMDir /r "$UpgradeBackupDir"
      SetErrorLevel 1
      Quit
    ${EndIf}
  ${ElseIf} $0 != "1060"
    RMDir /r "$UpgradeBackupDir"
    MessageBox MB_OK|MB_ICONSTOP "无法确认旧版 everything_sm 服务状态（SCM 查询错误码 $0）。本次升级停止。" /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}

  ClearErrors
  SetOutPath "$INSTDIR"
  File /oname=esm_gui.exe "${BUILD_DIR}\esm_gui.exe"
  File /oname=esm_launcher.exe "${BUILD_DIR}\esm_launcher.exe"
  File /oname=esm_server.exe "${BUILD_DIR}\esm_server.exe"
  File /oname=esm_service.exe "${BUILD_DIR}\esm_service.exe"
  File /oname=esm_cli.exe "${BUILD_DIR}\esm_cli.exe"
  File /oname=README.md "${PROJECT_ROOT}\README.md"
  File /oname=export-diagnostics.ps1 "${PROJECT_ROOT}\tools\export-diagnostics.ps1"
  ${If} ${Errors}
    ${If} $HadPreviousInstall == "1"
      Goto rollback_upgrade
    ${EndIf}
    MessageBox MB_OK|MB_ICONSTOP "安装文件解压失败。安装器将清理本次写入的文件。" /SD IDOK
    Goto cleanup_failed_fresh_files
  ${EndIf}

  StrCpy $ServiceInstallState "compatibility"
  ${If} $InstallService == ${BST_CHECKED}
    CreateDirectory "$ServiceDataRoot\everything_sm"
    CreateDirectory "$ServiceDataDir"
    service_install_retry:
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" install-mft-auto "$ServiceDataDir" "${PRODUCT_PIPE}"'
    Pop $0
    ${If} $0 != "0"
      ${If} $HadPreviousInstall == "1"
        Goto rollback_upgrade
      ${EndIf}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "高性能 NTFS MFT 服务安装失败（错误码 $0）。请选择“重试”；取消后会清理残留并使用兼容模式。" /SD IDCANCEL IDRETRY service_install_retry
      Goto cleanup_failed_fresh_service
    ${EndIf}

    service_start_retry:
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" start'
    Pop $0
    ${If} $0 != "0"
      ${If} $HadPreviousInstall == "1"
        Goto rollback_upgrade
      ${EndIf}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "高性能 NTFS MFT 服务启动失败（错误码 $0）。请选择“重试”；取消后会移除失败服务并使用兼容模式。" /SD IDCANCEL IDRETRY service_start_retry
      Goto cleanup_failed_fresh_service
    ${EndIf}

    ; SERVICE_RUNNING is not sufficient: require one successful Pipe query.
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" health "${PRODUCT_PIPE}" 120000'
    Pop $0
    ${If} $0 != "0"
      ${If} $HadPreviousInstall == "1"
        Goto rollback_upgrade
      ${EndIf}
      MessageBox MB_OK|MB_ICONEXCLAMATION "服务已启动但健康检查失败（错误码 $0）；安装器将移除服务并使用兼容模式。" /SD IDOK
      Goto cleanup_failed_fresh_service
    ${EndIf}
    StrCpy $ServiceInstallState "mft-auto"
    Goto service_configuration_done

    cleanup_failed_fresh_service:
      nsExec::ExecToLog '"$INSTDIR\esm_service.exe" uninstall'
      Pop $1
      ${If} $1 != "0"
        MessageBox MB_OK|MB_ICONSTOP "失败服务清理未完成（错误码 $1）。本次安装停止。" /SD IDOK
        SetErrorLevel 1
        Quit
      ${EndIf}
      StrCpy $ServiceInstallState "compatibility"
  ${EndIf}
  service_configuration_done:

  WriteINIStr "$INSTDIR\everything_sm.ini" "search" "scan_root" "$ScanRoot"
  WriteINIStr "$INSTDIR\everything_sm.ini" "search" "pipe_name" "${PRODUCT_PIPE}"
  WriteINIStr "$INSTDIR\everything_sm.ini" "search" "service_mode" "$ServiceInstallState"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "ScanRoot" "$ScanRoot"
  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "ServiceMode" "$ServiceInstallState"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\esm_gui.exe"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoRepair" 1

  SetShellVarContext all
  Delete "$DESKTOP\everything_sm.lnk"
  RMDir /r "$SMPROGRAMS\everything_sm"
  SetShellVarContext current
  CreateDirectory "$SMPROGRAMS\everything_sm"
  CreateShortcut "$SMPROGRAMS\everything_sm\everything_sm.lnk" "$INSTDIR\esm_launcher.exe" "" "$INSTDIR\esm_gui.exe" 0 SW_SHOWNORMAL "" "快速文件搜索"
  CreateShortcut "$SMPROGRAMS\everything_sm\导出诊断信息.lnk" "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\export-diagnostics.ps1"' "$INSTDIR\esm_gui.exe" 0 SW_SHOWNORMAL "" "导出服务、版本、事件和资源诊断包（分享前请检查）"
  CreateShortcut "$SMPROGRAMS\everything_sm\卸载 everything_sm.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\everything_sm.lnk" "$INSTDIR\esm_launcher.exe" "" "$INSTDIR\esm_gui.exe" 0 SW_SHOWNORMAL "" "快速文件搜索"
  RMDir /r "$UpgradeBackupDir"
  Goto install_section_done

  cleanup_failed_fresh_files:
    Delete /REBOOTOK "$INSTDIR\esm_gui.exe"
    Delete /REBOOTOK "$INSTDIR\esm_launcher.exe"
    Delete /REBOOTOK "$INSTDIR\esm_server.exe"
    Delete /REBOOTOK "$INSTDIR\esm_service.exe"
    Delete /REBOOTOK "$INSTDIR\esm_cli.exe"
    Delete /REBOOTOK "$INSTDIR\README.md"
    Delete /REBOOTOK "$INSTDIR\export-diagnostics.ps1"
    RMDir /r "$UpgradeBackupDir"
    SetErrorLevel 1
    Quit

  rollback_upgrade:
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" uninstall'
    Pop $1
    nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\stop-install-processes.ps1" -InstallDirectory "$INSTDIR" -GraceMilliseconds 0'
    Pop $1
    ClearErrors
    CopyFiles /SILENT "$UpgradeBackupDir\esm_gui.exe" "$INSTDIR\esm_gui.exe"
    CopyFiles /SILENT "$UpgradeBackupDir\esm_launcher.exe" "$INSTDIR\esm_launcher.exe"
    CopyFiles /SILENT "$UpgradeBackupDir\esm_server.exe" "$INSTDIR\esm_server.exe"
    CopyFiles /SILENT "$UpgradeBackupDir\esm_service.exe" "$INSTDIR\esm_service.exe"
    CopyFiles /SILENT "$UpgradeBackupDir\esm_cli.exe" "$INSTDIR\esm_cli.exe"
    IfFileExists "$UpgradeBackupDir\everything_sm.ini" 0 +2
      CopyFiles /SILENT "$UpgradeBackupDir\everything_sm.ini" "$INSTDIR\everything_sm.ini"
    IfFileExists "$UpgradeBackupDir\README.md" 0 +2
      CopyFiles /SILENT "$UpgradeBackupDir\README.md" "$INSTDIR\README.md"
    IfFileExists "$UpgradeBackupDir\Uninstall.exe" 0 +2
      CopyFiles /SILENT "$UpgradeBackupDir\Uninstall.exe" "$INSTDIR\Uninstall.exe"
    IfFileExists "$UpgradeBackupDir\export-diagnostics.ps1" restore_diagnostics delete_new_diagnostics
    restore_diagnostics:
      CopyFiles /SILENT "$UpgradeBackupDir\export-diagnostics.ps1" "$INSTDIR\export-diagnostics.ps1"
      Goto diagnostics_restored
    delete_new_diagnostics:
      Delete "$INSTDIR\export-diagnostics.ps1"
    diagnostics_restored:
    ${If} ${Errors}
      MessageBox MB_OK|MB_ICONSTOP "新版本健康检查失败，而且旧文件恢复不完整。备份仍位于 $UpgradeBackupDir，请手动恢复。" /SD IDOK
      SetErrorLevel 1
      Quit
    ${EndIf}
    ${If} $HadPreviousService == "1"
      nsExec::ExecToLog '"$INSTDIR\esm_service.exe" install-mft-auto "$ServiceDataDir" "${PRODUCT_PIPE}"'
      Pop $1
      ${If} $1 == "0"
        nsExec::ExecToLog '"$INSTDIR\esm_service.exe" start'
        Pop $1
      ${EndIf}
      ${If} $1 != "0"
        MessageBox MB_OK|MB_ICONSTOP "旧文件已恢复，但旧服务重新启动失败（错误码 $1）。请重新安装旧版本。" /SD IDOK
        SetErrorLevel 1
        Quit
      ${EndIf}
    ${EndIf}
    MessageBox MB_OK|MB_ICONSTOP "新版本未通过服务健康检查，安装器已回滚到旧版本。" /SD IDOK
    RMDir /r "$UpgradeBackupDir"
    SetErrorLevel 1
    Quit

  install_section_done:
SectionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext current
  ReadEnvStr $ServiceDataRoot "ProgramData"
  ${If} $ServiceDataRoot == ""
    ${GetRoot} "$WINDIR" $0
    StrCpy $ServiceDataRoot "$0\ProgramData"
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext current

  nsExec::ExecToLog '"$SYSDIR\sc.exe" query everything_sm'
  Pop $0
  ${If} $0 == "0"
    IfFileExists "$INSTDIR\esm_service.exe" uninstall_stop_retry 0
      MessageBox MB_OK|MB_ICONSTOP "SCM 中仍存在 everything_sm 服务，但卸载程序找不到 esm_service.exe。为避免留下不可恢复的服务，本次卸载已停止。" /SD IDOK
      SetErrorLevel 1
      Quit
    uninstall_stop_retry:
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" stop'
    Pop $0
    ${If} $0 != "0"
      MessageBox MB_RETRYCANCEL|MB_ICONSTOP "无法停止 everything_sm 服务（错误码 $0）。请选择“重试”，或取消卸载。" /SD IDCANCEL IDRETRY uninstall_stop_retry
      SetErrorLevel 1
      Quit
    ${EndIf}
    uninstall_service_retry:
    nsExec::ExecToLog '"$INSTDIR\esm_service.exe" uninstall'
    Pop $0
    ${If} $0 != "0"
      MessageBox MB_RETRYCANCEL|MB_ICONSTOP "无法删除 everything_sm 服务（错误码 $0）。请选择“重试”，或取消卸载。" /SD IDCANCEL IDRETRY uninstall_service_retry
      SetErrorLevel 1
      Quit
    ${EndIf}
  ${ElseIf} $0 != "1060"
    MessageBox MB_OK|MB_ICONSTOP "无法确认 everything_sm 服务状态（SCM 查询错误码 $0）。本次卸载已停止。" /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}

  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File /oname=stop-install-processes.ps1 "${PROJECT_ROOT}\packaging\stop-install-processes.ps1"
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\stop-install-processes.ps1" -InstallDirectory "$INSTDIR"'
  Pop $0
  ${If} $0 != "0"
    MessageBox MB_OK|MB_ICONSTOP "无法安全停止安装目录中的程序（错误码 $0）。本次卸载已停止。" /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}

  Delete "$DESKTOP\everything_sm.lnk"
  RMDir /r "$SMPROGRAMS\everything_sm"
  SetShellVarContext all
  Delete "$DESKTOP\everything_sm.lnk"
  RMDir /r "$SMPROGRAMS\everything_sm"
  SetShellVarContext current

  DeleteRegKey HKLM "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKLM "${PRODUCT_DIR_REGKEY}"
  DeleteRegKey HKLM "Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\esm_service.exe"
  DeleteRegKey HKLM "Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\esm_gui.exe"
  DeleteRegKey HKLM "Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\esm_server.exe"

  MessageBox MB_YESNO|MB_ICONQUESTION "是否同时删除机器级索引快照和当前用户设置？" /SD IDNO IDNO keep_data
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
  Delete /REBOOTOK "$INSTDIR\export-diagnostics.ps1"
  RMDir /r "$INSTDIR\.everything_sm-upgrade-backup"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"
SectionEnd

