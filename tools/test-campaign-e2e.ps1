param()

$ErrorActionPreference = "Stop"
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot "e2e-levels.json") -Raw |
    ConvertFrom-Json
foreach ($case in $manifest.levels | Sort-Object index) {
    & (Join-Path $PSScriptRoot "test-level-e2e.ps1") -Level $case.name
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
