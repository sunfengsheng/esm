[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallerPath,
    [string]$InstallDirectory = (Join-Path $env:ProgramFiles 'everything_sm-ci'),
    [int]$HealthTimeoutMilliseconds = 180000
)

$ErrorActionPreference = 'Stop'
$installer = [IO.Path]::GetFullPath($InstallerPath)
$install = [IO.Path]::GetFullPath($InstallDirectory)
$serviceName = 'everything_sm'
$programData = Join-Path $env:ProgramData 'everything_sm'

function Invoke-CheckedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int[]]$AllowedExitCodes = @(0)
    )
    $process = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -Wait -PassThru -NoNewWindow
    if ($AllowedExitCodes -notcontains $process.ExitCode) {
        throw "$FilePath failed with exit code $($process.ExitCode)"
    }
}

function Assert-ServiceReady {
    $serviceExe = Join-Path $install 'esm_service.exe'
    if (-not (Test-Path -LiteralPath $serviceExe)) {
        throw "Missing installed service executable: $serviceExe"
    }
    Invoke-CheckedProcess -FilePath $serviceExe -ArgumentList @(
        'health', 'everything_sm_service', [string]$HealthTimeoutMilliseconds)
    $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'" -ErrorAction Stop
    if ($service.State -ne 'Running') {
        throw "Service is not running: $($service.State)"
    }
    $imagePath = [Environment]::ExpandEnvironmentVariables($service.PathName).Trim('"')
    if (-not $imagePath.StartsWith($install, [StringComparison]::OrdinalIgnoreCase)) {
        throw "SCM ImagePath is outside the smoke-test install directory: $($service.PathName)"
    }
}

if (-not (Test-Path -LiteralPath $installer)) {
    throw "Installer not found: $installer"
}
if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
    throw "The smoke runner already contains the $serviceName service"
}

try {
    Write-Host "Fresh silent install: $install"
    Invoke-CheckedProcess -FilePath $installer -ArgumentList @('/S', "/D=$install")
    Assert-ServiceReady

    Write-Host 'Same-version silent upgrade'
    Invoke-CheckedProcess -FilePath $installer -ArgumentList @('/S', "/D=$install")
    Assert-ServiceReady

    Write-Host 'Silent uninstall'
    $uninstaller = Join-Path $install 'Uninstall.exe'
    Invoke-CheckedProcess -FilePath $uninstaller -ArgumentList @('/S')

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if (-not (Get-Service -Name $serviceName -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
        throw 'Service still exists after uninstall'
    }
    foreach ($name in 'esm_gui','esm_launcher','esm_server','esm_service','esm_cli') {
        if (Get-Process -Name $name -ErrorAction SilentlyContinue) {
            throw "Process still exists after uninstall: $name"
        }
    }
    Write-Host 'Installer smoke test passed.'
} finally {
    if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
        $serviceExe = Join-Path $install 'esm_service.exe'
        if (Test-Path -LiteralPath $serviceExe) {
            & $serviceExe stop 2>$null | Out-Null
            & $serviceExe uninstall 2>$null | Out-Null
        } else {
            sc.exe stop $serviceName | Out-Null
            sc.exe delete $serviceName | Out-Null
        }
    }
    if (Test-Path -LiteralPath $programData) {
        Remove-Item -LiteralPath $programData -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $install) {
        Remove-Item -LiteralPath $install -Recurse -Force -ErrorAction SilentlyContinue
    }
}
