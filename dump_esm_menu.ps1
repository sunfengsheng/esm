Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NativeMenu2 {
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr hMenu);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetMenuString(IntPtr hMenu, uint uIDItem, StringBuilder lpString, int cchMax, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr hMenu, int nPos);
  [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr hMenu, int nPos);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr hMenu, uint uId, uint flags);
}
"@
$MF_BYPOSITION=0x400
function Dump-Menu([IntPtr]$menu,[int]$depth=0) {
  $count=[NativeMenu2]::GetMenuItemCount($menu)
  for($i=0;$i -lt $count;$i++){
    $sb=New-Object Text.StringBuilder 512
    [void][NativeMenu2]::GetMenuString($menu,[uint32]$i,$sb,$sb.Capacity,$MF_BYPOSITION)
    $sub=[NativeMenu2]::GetSubMenu($menu,$i)
    $id=[NativeMenu2]::GetMenuItemID($menu,$i)
    $state=[NativeMenu2]::GetMenuState($menu,[uint32]$i,$MF_BYPOSITION)
    $flags=@(); if($state -band 0x800){$flags+='SEP'}; if($state -band 0x1){$flags+='GRAY'}; if($state -band 0x2){$flags+='DIS'}; if($state -band 0x8){$flags+='CHECK'}
    '{0}{1} [id={2} {3}]' -f ('  '*$depth),$sb.ToString(),$id,($flags -join ',')
    if($sub -ne [IntPtr]::Zero){Dump-Menu $sub ($depth+1)}
  }
}
$proc=Get-Process esm_gui -ErrorAction Stop | Where-Object { $_.Path -eq 'D:\work\everything_sm\build\esm_gui.exe' -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
"Process=$($proc.Id) HWND=$($proc.MainWindowHandle)"
Dump-Menu ([NativeMenu2]::GetMenu($proc.MainWindowHandle))
