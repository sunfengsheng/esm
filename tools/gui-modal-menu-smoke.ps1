param([string]$BuildDir = (Join-Path $PSScriptRoot '..\build'))
$exePath = [IO.Path]::GetFullPath((Join-Path $BuildDir 'esm_gui.exe'))
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ModalSmokeNative {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hWnd, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr hWnd, string text);
  public static void Command(IntPtr hWnd, int id) { PostMessage(hWnd, 0x111, new IntPtr(id), IntPtr.Zero); }
  public static string Title(IntPtr h) { var b=new StringBuilder(512); GetWindowText(h,b,b.Capacity); return b.ToString(); }
}
"@
function Get-OwnedProcessWindows([uint32]$processId,[IntPtr]$main) {
  $result = [System.Collections.Generic.List[object]]::new()
  $cb = [ModalSmokeNative+EnumWindowsProc]{ param($h,$lp)
    [uint32]$windowPid=0; [void][ModalSmokeNative]::GetWindowThreadProcessId($h,[ref]$windowPid)
    if($windowPid -eq $processId -and $h -ne $main -and [ModalSmokeNative]::IsWindowVisible($h)) {
      $result.Add([pscustomobject]@{Handle=$h;Title=[ModalSmokeNative]::Title($h)})
    }
    return $true
  }
  [void][ModalSmokeNative]::EnumWindows($cb,[IntPtr]::Zero)
  return $result
}
function Test-Modal([IntPtr]$main,[uint32]$processId,[int]$id,[string]$label) {
  [ModalSmokeNative]::Command($main,$id)
  $found=@()
  for($i=0;$i -lt 40;$i++) {
    Start-Sleep -Milliseconds 100
    $found=@(Get-OwnedProcessWindows $processId $main)
    if($found.Count -gt 0){break}
  }
  if($found.Count -eq 0){throw "$label ($id) did not open a modal window"}
  $titles=($found | ForEach-Object {$_.Title}) -join ' | '
  foreach($window in $found){[void][ModalSmokeNative]::PostMessage($window.Handle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)}
  Start-Sleep -Milliseconds 150
  "PASS $label [$titles]"
}
$p=Get-Process esm_gui -ErrorAction Stop | Where-Object {$_.Path -eq $exePath} | Select-Object -First 1
$p.Refresh(); $main=[IntPtr]$p.MainWindowHandle
$search=[ModalSmokeNative]::GetDlgItem($main,100)
[void][ModalSmokeNative]::SetWindowText($search,'menu-smoke-query')
$tests=@(
  @(40066,'advanced-search'), @(40067,'add-filter'), @(40068,'manage-filters'),
  @(40069,'add-bookmark'), @(40071,'manage-bookmarks'), @(40072,'connect-service'),
  @(40074,'file-list-editor'), @(40078,'options'), @(40080,'search-syntax'),
  @(40081,'regex-syntax'), @(40084,'command-line'), @(40086,'check-updates'), @(40087,'about')
)
foreach($t in $tests){Test-Modal $main ([uint32]$p.Id) ([int]$t[0]) ([string]$t[1])}
[void][ModalSmokeNative]::SetWindowText($search,'')
'Modal menu smoke test passed.'

