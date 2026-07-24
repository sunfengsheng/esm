[CmdletBinding()]
param(
    [switch]$Staged,
    [string]$BaseRef,
    [string]$HeadRef
)

$ErrorActionPreference = 'Stop'

function Invoke-GitLines {
    param([Parameter(Mandatory)][string[]]$Arguments)

    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & git @Arguments 2>$null
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0) {
        throw "git $($Arguments -join ' ') 执行失败。"
    }
    @($output | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

function Test-GitRef {
    param([string]$Ref)
    if ([string]::IsNullOrWhiteSpace($Ref)) { return $false }

    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & git rev-parse --verify --quiet "$Ref^{commit}" *> $null
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    return $exitCode -eq 0
}

function Get-HeadChangeFiles {
    if (Test-GitRef 'HEAD^') {
        return Invoke-GitLines @('diff', '--name-only', '--diff-filter=ACDMRTUXB', 'HEAD^', 'HEAD', '--')
    }
    return Invoke-GitLines @('diff-tree', '--no-commit-id', '--name-only', '-r', 'HEAD')
}

$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $root = (& git rev-parse --show-toplevel 2>$null).Trim()
    $rootExitCode = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $previousPreference
}
if ($rootExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($root)) {
    throw '当前目录不在 Git 仓库中。'
}

Push-Location $root
try {
    $files = @()

    if ($Staged) {
        $files = Invoke-GitLines @('diff', '--cached', '--name-only', '--diff-filter=ACDMRTUXB', '--')
    }
    elseif ($HeadRef -eq 'WORKTREE') {
        $files = @(
            Invoke-GitLines @('diff', '--name-only', '--diff-filter=ACDMRTUXB', 'HEAD', '--')
            Invoke-GitLines @('ls-files', '--others', '--exclude-standard')
        )
    }
    elseif (-not [string]::IsNullOrWhiteSpace($BaseRef) -or
            -not [string]::IsNullOrWhiteSpace($HeadRef)) {
        $head = if (Test-GitRef $HeadRef) { $HeadRef } else { 'HEAD' }
        $allZeroBase = $BaseRef -match '^0+$'
        if (-not $allZeroBase -and (Test-GitRef $BaseRef)) {
            $files = Invoke-GitLines @('diff', '--name-only', '--diff-filter=ACDMRTUXB', $BaseRef, $head, '--')
        }
        else {
            $files = Get-HeadChangeFiles
        }
    }
    else {
        $files = @(
            Invoke-GitLines @('diff', '--name-only', '--diff-filter=ACDMRTUXB', 'HEAD', '--')
            Invoke-GitLines @('ls-files', '--others', '--exclude-standard')
        )
    }

    $files = @($files | ForEach-Object { $_ -replace '\\', '/' } | Sort-Object -Unique)
    if ($files.Count -eq 0) {
        Write-Host '文档同步检查：没有需要检查的修改。'
        exit 0
    }

    $governed = @($files | Where-Object {
        $_ -match '^(include|src|tests|benchmarks|packaging|tools|assets|\.github|\.githooks)/' -or
        $_ -match '^(CMakeLists\.txt|\.gitignore|\.gitattributes|\.editorconfig)$' -or
        $_ -match '^[^/]+\.(c|cc|cpp|cxx|h|hpp|ps1|cmake|nsi|rc|json|ya?ml|toml|ini)$'
    })

    if ($governed.Count -eq 0) {
        Write-Host '文档同步检查：仅文档或非受控文件发生变化，检查通过。'
        exit 0
    }

    $hasChangelog = ($files -contains 'CHANGELOG.md') -and (Test-Path -LiteralPath 'CHANGELOG.md')
    $longLivedDocs = @($files | Where-Object {
        ($_ -eq 'README.md' -or
         $_ -eq 'CONTRIBUTING.md' -or
         $_ -match '^docs/.+\.md$') -and
        (Test-Path -LiteralPath $_)
    })

    if ($hasChangelog -and $longLivedDocs.Count -gt 0) {
        Write-Host '文档同步检查通过。'
        Write-Host ('  受控修改：' + ($governed -join ', '))
        Write-Host ('  长期文档：' + ($longLivedDocs -join ', '))
        exit 0
    }

    Write-Error @"
文档同步检查失败。

本次包含源代码、测试、构建、安装、CI、工具或资源修改：
  $($governed -join "`n  ")

同一变更必须同时包含：
  1. CHANGELOG.md（更新 Unreleased）；
  2. 至少一份长期文档：README.md、CONTRIBUTING.md 或 docs/*.md。

详见 CONTRIBUTING.md 和 docs/README.md。
"@
    exit 1
}
finally {
    Pop-Location
}
