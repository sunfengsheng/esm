[CmdletBinding()]
param(
    [string]$OutputPath = (Join-Path ([Environment]::GetFolderPath('Desktop')) ('everything_sm-diagnostics-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.zip'))
)

$ErrorActionPreference = 'Stop'
$root = Join-Path $env:TEMP ('everything_sm-diagnostics-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$report = New-Object System.Collections.Generic.List[string]
function Add-Line([string]$Text = '') { $report.Add($Text) }
function Add-Command([string]$Title, [scriptblock]$Command) {
    Add-Line; Add-Line ('## ' + $Title)
    try { (& $Command 2>&1 | Out-String).TrimEnd() -split "\r?\n" | ForEach-Object { Add-Line $_ } }
    catch { Add-Line ('ERROR: ' + $_.Exception.Message) }
}
function Redact([string]$Text) {
    if ([string]::IsNullOrEmpty($Text)) { return $Text }
    $value = $Text
    foreach ($candidate in @($env:USERPROFILE, $env:USERNAME)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            $value = $value.Replace($candidate, '<redacted-user>')
        }
    }
    return $value
}
try {
    Add-Line '# everything_sm 诊断报告'
    Add-Line ('generated_utc=' + [DateTime]::UtcNow.ToString('o'))
    Add-Line ('computer=' + $env:COMPUTERNAME)
    Add-Line ('architecture=' + [Runtime.InteropServices.RuntimeInformation]::OSArchitecture)
    Add-Line ('os=' + [Runtime.InteropServices.RuntimeInformation]::OSDescription)
    Add-Line ('powershell=' + $PSVersionTable.PSVersion)

    $installDir = $null
    try { $installDir = (Get-ItemProperty 'HKLM:\Software\everything_sm' -ErrorAction Stop).InstallDir } catch {}
    Add-Line ('install_dir=' + (Redact $installDir))

    Add-Command '服务状态' { sc.exe queryex everything_sm }
    Add-Command '服务配置' { sc.exe qc everything_sm }
    Add-Command '服务恢复策略' { sc.exe qfailure everything_sm; sc.exe qfailureflag everything_sm }
    Add-Command '服务安全描述符' { sc.exe sdshow everything_sm }
    Add-Command '相关进程资源' {
        Get-Process esm_gui,esm_launcher,esm_server,esm_service -ErrorAction SilentlyContinue |
            Select-Object Name,Id,StartTime,CPU,WorkingSet64,PrivateMemorySize64,HandleCount,Threads |
            Format-List
    }

    if ($installDir -and (Test-Path -LiteralPath $installDir)) {
        Add-Command '安装文件版本与 SHA-256' {
            Get-ChildItem -LiteralPath $installDir -File -ErrorAction Stop |
                Where-Object { $_.Extension -in '.exe','.dll' } |
                ForEach-Object {
                    $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
                    [pscustomobject]@{
                        Name=$_.Name; Version=$_.VersionInfo.FileVersion
                        Bytes=$_.Length; SHA256=$hash.Hash
                    }
                } | Format-Table -AutoSize
        }
        $ini = Join-Path $installDir 'everything_sm.ini'
        if (Test-Path -LiteralPath $ini) {
            (Get-Content -LiteralPath $ini -Raw) -split "\r?\n" |
                ForEach-Object { Add-Line ('config: ' + (Redact $_)) }
        }
    }

    $dataRoot = Join-Path $env:ProgramData 'everything_sm'
    Add-Command '索引和转储文件清单（不包含文件内容）' {
        if (Test-Path -LiteralPath $dataRoot) {
            Get-ChildItem -LiteralPath $dataRoot -Recurse -File -ErrorAction SilentlyContinue |
                Select-Object @{n='Path';e={Redact $_.FullName}},Length,LastWriteTimeUtc |
                Format-Table -AutoSize
        } else { 'ProgramData 数据目录不存在' }
    }
    Add-Command '最近 200 条 Application 事件' {
        Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='everything_sm'} -MaxEvents 200 -ErrorAction Stop |
            Select-Object TimeCreated,LevelDisplayName,Id,Message | Format-List
    }

    $report | ForEach-Object { Redact $_ } | Set-Content -LiteralPath (Join-Path $root 'diagnostics.txt') -Encoding UTF8
    $logs = Join-Path $dataRoot 'logs'
    if (Test-Path -LiteralPath $logs) {
        $dest = Join-Path $root 'logs'
        New-Item -ItemType Directory -Path $dest | Out-Null
        Get-ChildItem -LiteralPath $logs -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 10 |
            Copy-Item -Destination $dest -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $OutputPath) { Remove-Item -LiteralPath $OutputPath -Force }
    Compress-Archive -Path (Join-Path $root '*') -DestinationPath $OutputPath -CompressionLevel Optimal
    Write-Output $OutputPath
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
