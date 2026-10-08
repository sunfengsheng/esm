[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Path
)

$ErrorActionPreference = 'Stop'
$signTool = $env:ESM_NSIS_SIGNTOOL_PATH
$certificate = $env:ESM_NSIS_CERTIFICATE_PATH
$password = $env:ESM_NSIS_CERTIFICATE_PASSWORD
$timestamp = $env:ESM_NSIS_TIMESTAMP_URL
if ([string]::IsNullOrWhiteSpace($signTool) -or
    [string]::IsNullOrWhiteSpace($certificate) -or
    [string]::IsNullOrWhiteSpace($timestamp)) {
    throw 'NSIS uninstaller signing environment is incomplete.'
}
$target = [IO.Path]::GetFullPath($Path)
$arguments = @(
    'sign', '/fd', 'SHA256', '/td', 'SHA256', '/tr', $timestamp,
    '/f', $certificate
)
if (-not [string]::IsNullOrEmpty($password)) {
    $arguments += @('/p', $password)
}
$arguments += $target
& $signTool @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Authenticode signing failed for generated uninstaller: $target"
}
& $signTool verify /pa /all $target
if ($LASTEXITCODE -ne 0) {
    throw "Authenticode verification failed for generated uninstaller: $target"
}
