[CmdletBinding()]
param(
    [string]$BuildDirectory = "build-release",
    [string]$Configuration = "Release",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildPath = [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
$distPath = Join-Path $projectRoot "dist"
$nsiPath = Join-Path $PSScriptRoot "everything_sm.nsi"
$nsisRoot = Join-Path $projectRoot "tools\nsis\nsis-3.12\nsis-3.12"
$makeNsis = Join-Path $nsisRoot "makensis.exe"
$nsisZip = Join-Path $projectRoot "tools\nsis\nsis-3.12.zip"

if (-not (Test-Path -LiteralPath $makeNsis)) {
    if (-not (Test-Path -LiteralPath $nsisZip)) {
        New-Item -ItemType Directory -Force -Path (Split-Path $nsisZip) | Out-Null
        Invoke-WebRequest `
            -Uri "https://sourceforge.net/projects/nsis/files/NSIS%203/3.12/nsis-3.12.zip/download" `
            -OutFile $nsisZip
    }
    $extractRoot = Split-Path -Parent $nsisRoot
    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
    Expand-Archive -LiteralPath $nsisZip -DestinationPath $extractRoot -Force
}

if (-not $SkipBuild) {
    & cmake -S $projectRoot -B $buildPath -G "MinGW Makefiles" `
        "-DCMAKE_BUILD_TYPE=$Configuration" `
        -DESM_BUILD_TESTS=OFF -DESM_BUILD_BENCHMARKS=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

    & cmake --build $buildPath -j 4
    if ($LASTEXITCODE -ne 0) { throw "Release build failed: $LASTEXITCODE" }
}

$required = @(
    "esm_gui.exe",
    "esm_launcher.exe",
    "esm_server.exe",
    "esm_service.exe",
    "esm_cli.exe"
)
foreach ($name in $required) {
    $path = Join-Path $buildPath $name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing package input: $path"
    }
}

New-Item -ItemType Directory -Force -Path $distPath | Out-Null
& $makeNsis /V4 /WX "/DPROJECT_ROOT=$projectRoot" "/DBUILD_DIR=$buildPath" `
    "/DOUTPUT_DIR=$distPath" $nsiPath
if ($LASTEXITCODE -ne 0) { throw "NSIS compilation failed: $LASTEXITCODE" }

$installer = Join-Path $distPath "everything_sm-0.1.0-setup.exe"
if (-not (Test-Path -LiteralPath $installer)) {
    throw "Installer was not produced: $installer"
}

$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $installer
$hashFile = "$installer.sha256"
Set-Content -LiteralPath $hashFile -Value "$($hash.Hash)  $([IO.Path]::GetFileName($installer))" -Encoding ascii
Write-Host ""
Write-Host "Installer: $installer"
Write-Host "SHA256:    $($hash.Hash)"
Write-Host "Hash file: $hashFile"
Write-Host "Size:      $((Get-Item -LiteralPath $installer).Length) bytes"

