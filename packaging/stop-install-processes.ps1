[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory,
    [int]$GraceMilliseconds = 3000
)

$ErrorActionPreference = 'Stop'
$resolvedInstall = [System.IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$names = @('esm_gui.exe', 'esm_server.exe', 'esm_launcher.exe', 'esm_cli.exe')
$targets = @(Get-CimInstance Win32_Process | Where-Object { $names -contains $_.Name })
$matched = @()

foreach ($process in $targets) {
    if ([string]::IsNullOrWhiteSpace($process.ExecutablePath)) {
        throw "无法确认进程 $($process.ProcessId) ($($process.Name)) 的可执行文件路径；为避免误杀其他目录中的程序，停止升级。"
    }
    $parent = [System.IO.Path]::GetDirectoryName(
        [System.IO.Path]::GetFullPath($process.ExecutablePath)).TrimEnd('\')
    if ($parent.Equals($resolvedInstall, [System.StringComparison]::OrdinalIgnoreCase)) {
        $matched += $process
    }
}

foreach ($process in $matched) {
    try {
        $live = Get-Process -Id $process.ProcessId -ErrorAction Stop
        if ($live.MainWindowHandle -ne [IntPtr]::Zero) {
            [void]$live.CloseMainWindow()
        }
    } catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
        # Process already exited.
    }
}

if ($GraceMilliseconds -gt 0 -and $matched.Count -gt 0) {
    Start-Sleep -Milliseconds $GraceMilliseconds
}

foreach ($process in $matched) {
    $live = Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
    if ($null -ne $live) {
        Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
        $live.WaitForExit(5000)
    }
}

$remaining = @(Get-CimInstance Win32_Process | Where-Object {
    $names -contains $_.Name -and
    -not [string]::IsNullOrWhiteSpace($_.ExecutablePath) -and
    [System.IO.Path]::GetDirectoryName(
        [System.IO.Path]::GetFullPath($_.ExecutablePath)).TrimEnd('\').Equals(
            $resolvedInstall, [System.StringComparison]::OrdinalIgnoreCase)
})
if ($remaining.Count -ne 0) {
    throw "安装目录中仍有 everything_sm 进程正在运行：$($remaining.ProcessId -join ', ')"
}

Write-Output "Stopped $($matched.Count) process(es) from $resolvedInstall"
