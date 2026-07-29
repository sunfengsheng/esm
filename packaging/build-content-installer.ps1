[CmdletBinding()]
param(
    [string]$BuildDirectory = "build-content-release",
    [string]$Configuration = "Release",
    [string]$Version,
    [string]$MakeNsisPath,
    [switch]$SkipBuild,
    [switch]$AllowDevelopmentPackage
)

$ErrorActionPreference = "Stop"
if (-not $AllowDevelopmentPackage) {
    throw "Content search is statically linked with GPL Xapian. Pass -AllowDevelopmentPackage only for local development packaging; do not redistribute until the repository licensing decision and source-distribution obligations are completed."
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildPath = [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
$distPath = Join-Path $projectRoot "dist"
$nsiPath = Join-Path $PSScriptRoot "content_search.nsi"
$nsisRoot = Join-Path $projectRoot "tools\nsis\nsis-3.12\nsis-3.12"
$bundledMakeNsis = Join-Path $nsisRoot "makensis.exe"
$nsisZip = Join-Path $projectRoot "tools\nsis\nsis-3.12.zip"

$cmakeListsPath = Join-Path $projectRoot "CMakeLists.txt"
if ([string]::IsNullOrWhiteSpace($Version)) {
    $cmakeLists = Get-Content -LiteralPath $cmakeListsPath -Raw
    $match = [regex]::Match($cmakeLists, 'project\s*\([^)]*\bVERSION\s+([0-9]+(?:\.[0-9]+){2,3})', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success) { throw "Unable to read project version from $cmakeListsPath" }
    $Version = $match.Groups[1].Value
}
if ($Version -notmatch '^[0-9]+(?:\.[0-9]+){2,3}$') {
    throw "Invalid installer version '$Version'. Expected numeric x.y.z or x.y.z.w."
}
$versionParts = @($Version.Split('.') | ForEach-Object { [int]$_ })
while ($versionParts.Count -lt 4) { $versionParts += 0 }
if ($versionParts.Count -gt 4 -or ($versionParts | Where-Object { $_ -lt 0 -or $_ -gt 65535 })) {
    throw "Installer version '$Version' cannot be represented as a Windows four-part version."
}
$resourceVersion = ($versionParts -join '.')

$makeNsisCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($MakeNsisPath)) {
    $makeNsisCandidates += [IO.Path]::GetFullPath($MakeNsisPath)
}
$makeNsisCommand = Get-Command makensis.exe -ErrorAction SilentlyContinue
if ($makeNsisCommand) { $makeNsisCandidates += $makeNsisCommand.Source }
$makeNsisCandidates += $bundledMakeNsis
$makeNsis = $makeNsisCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $makeNsis) { $makeNsis = $bundledMakeNsis }
if (-not (Test-Path -LiteralPath $makeNsis)) {
    if (-not (Test-Path -LiteralPath $nsisZip)) {
        New-Item -ItemType Directory -Force -Path (Split-Path $nsisZip) | Out-Null
        Invoke-WebRequest -Uri "https://sourceforge.net/projects/nsis/files/NSIS%203/3.12/nsis-3.12.zip/download" -OutFile $nsisZip
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $nsisRoot) | Out-Null
    Expand-Archive -LiteralPath $nsisZip -DestinationPath (Split-Path -Parent $nsisRoot) -Force
}

if (-not $SkipBuild) {
    & cmake -S $projectRoot -B $buildPath -G "MinGW Makefiles" `
        "-DCMAKE_BUILD_TYPE=$Configuration" -DESM_BUILD_CONTENT_SEARCH=ON `
        -DESM_BUILD_TESTS=OFF -DESM_BUILD_BENCHMARKS=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & cmake --build $buildPath -j 4 --target esm_content esm_content_service esm_content_cli
    if ($LASTEXITCODE -ne 0) { throw "Content release build failed: $LASTEXITCODE" }
}

$required = @("esm_content.exe", "esm_content_service.exe", "esm_content_cli.exe")
foreach ($name in $required) {
    $path = Join-Path $buildPath $name
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing package input: $path" }
}

New-Item -ItemType Directory -Force -Path $distPath | Out-Null
& $makeNsis /V4 /WX "/DPROJECT_ROOT=$projectRoot" "/DBUILD_DIR=$buildPath" `
    "/DOUTPUT_DIR=$distPath" "/DPRODUCT_VERSION=$Version" `
    "/DPRODUCT_VERSION_RESOURCE=$resourceVersion" $nsiPath
if ($LASTEXITCODE -ne 0) { throw "NSIS compilation failed: $LASTEXITCODE" }

$installer = Join-Path $distPath "everything-sm-content-$Version-dev-setup.exe"
if (-not (Test-Path -LiteralPath $installer)) { throw "Installer was not produced: $installer" }
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $installer
$hashFile = "$installer.sha256"
Set-Content -LiteralPath $hashFile -Value "$($hash.Hash)  $([IO.Path]::GetFileName($installer))" -Encoding ascii
Write-Host "Installer: $installer"
Write-Host "SHA256:    $($hash.Hash)"
Write-Host "Hash file: $hashFile"
Write-Host "Size:      $((Get-Item -LiteralPath $installer).Length) bytes"
Write-Warning "Development package only. Do not redistribute before GPL/source-distribution review is complete."
