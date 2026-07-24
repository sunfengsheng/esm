param([string]$BuildDir = (Join-Path $PSScriptRoot '..\build'))
$exePath = [IO.Path]::GetFullPath((Join-Path $BuildDir 'esm_gui.exe'))
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class MenuSmokeNative {
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr hMenu);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr hMenu, int pos);
  [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr hMenu, int pos);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr hMenu, uint id, uint flags);
  public static void Command(IntPtr hWnd, int id) { SendMessage(hWnd, 0x111, new IntPtr(id), IntPtr.Zero); }
}
"@

function Find-Menu([IntPtr]$menu, [uint32]$id) {
  $count = [MenuSmokeNative]::GetMenuItemCount($menu)
  for ($i = 0; $i -lt $count; $i++) {
    if ([MenuSmokeNative]::GetMenuItemID($menu, $i) -eq $id) { return $menu }
    $sub = [MenuSmokeNative]::GetSubMenu($menu, $i)
    if ($sub -ne [IntPtr]::Zero) {
      $found = Find-Menu $sub $id
      if ($found -ne [IntPtr]::Zero) { return $found }
    }
  }
  return [IntPtr]::Zero
}

function Get-Checked([IntPtr]$root, [uint32]$id) {
  $menu = Find-Menu $root $id
  if ($menu -eq [IntPtr]::Zero) { throw "command $id missing" }
  return (([MenuSmokeNative]::GetMenuState($menu, $id, 0) -band 8) -ne 0)
}

function Invoke-MenuCommand([IntPtr]$hwnd, [uint32]$id) {
  [MenuSmokeNative]::Command($hwnd, [int]$id)
  Start-Sleep -Milliseconds 120
}

function Test-Toggle([IntPtr]$hwnd, [IntPtr]$root, [uint32]$id) {
  $before = Get-Checked $root $id
  Invoke-MenuCommand $hwnd $id
  $after = Get-Checked $root $id
  if ($after -eq $before) { throw "toggle command $id did not change state" }
  Invoke-MenuCommand $hwnd $id
  $restored = Get-Checked $root $id
  if ($restored -ne $before) { throw "toggle command $id did not restore state" }
}

function Test-RadioPair([IntPtr]$hwnd, [IntPtr]$root, [uint32]$first, [uint32]$second) {
  Invoke-MenuCommand $hwnd $first
  if (-not (Get-Checked $root $first)) { throw "radio command $first is not checked" }
  Invoke-MenuCommand $hwnd $second
  if (-not (Get-Checked $root $second)) { throw "radio command $second is not checked" }
}

$p = Get-Process esm_gui -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -eq $exePath } |
  Select-Object -First 1
if (!$p) { throw "GUI is not running: $exePath" }
$p.Refresh()
$hwnd = [IntPtr]$p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw 'debug GUI has no main window' }
$root = [MenuSmokeNative]::GetMenu($hwnd)
if ($root -eq [IntPtr]::Zero) { throw 'debug GUI has no menu' }

# View and search option toggles.
@(40024, 40026, 40025, 40061, 40062, 40063, 40064, 40065) |
  ForEach-Object { Test-Toggle $hwnd $root $_ }

# Radio groups, ending at stable defaults.
Test-RadioPair $hwnd $root 40028 40030
Invoke-MenuCommand $hwnd 40037
if (-not (Get-Checked $root 40037)) { throw "normal font command is not checked" }
Invoke-MenuCommand $hwnd 40035
if (Get-Checked $root 40037) { throw "font increase did not leave normal size" }
Invoke-MenuCommand $hwnd 40037
if (-not (Get-Checked $root 40037)) { throw "normal font command did not restore size" }
Test-RadioPair $hwnd $root 40041 40039
Test-RadioPair $hwnd $root 40053 40052
Test-RadioPair $hwnd $root 40057 40056
Test-RadioPair $hwnd $root 40501 40500

'GUI menu toggle smoke test passed.'

