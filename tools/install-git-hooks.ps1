[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (& git rev-parse --show-toplevel 2>$null).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($root)) {
    throw '当前目录不在 Git 仓库中。'
}

Push-Location $root
try {
    git config core.hooksPath .githooks
    if ($LASTEXITCODE -ne 0) { throw '无法设置 core.hooksPath。' }
    Write-Host "已启用仓库 Hook：$root\.githooks"
    Write-Host '以后提交受控修改时，必须同时更新 CHANGELOG.md 和长期文档。'
}
finally {
    Pop-Location
}
