# Shared mechanics for deterministic gameplay routes.  Keep build, symbol
# resolution and report execution here so focused and full E2E cases cannot
# quietly drift into different runner contracts.

function Get-MegalDoomRoot {
    return [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
}

function Invoke-MegalDoomDebugBuild {
    param([Parameter(Mandatory = $true)][string]$ExtraFlags)

    $previousExtraFlags = $env:EXTRA_FLAGS
    $env:EXTRA_FLAGS = $ExtraFlags
    try {
        & (Join-Path $PSScriptRoot "build-windows.ps1") -Clean
        if ($LASTEXITCODE -ne 0) {
            throw "Deterministic E2E build failed."
        }
    } finally {
        if ($null -eq $previousExtraFlags) {
            Remove-Item Env:\EXTRA_FLAGS -ErrorAction SilentlyContinue
        } else {
            $env:EXTRA_FLAGS = $previousExtraFlags
        }
    }
}

function Resolve-MegalDoomMailbox {
    param(
        [Parameter(Mandatory = $true)][string]$Symbol,
        [Parameter(Mandatory = $true)][int]$Bytes
    )

    $root = Get-MegalDoomRoot
    $mailbox = (& python (Join-Path $PSScriptRoot "resolve-symbol.py") `
        (Join-Path $root "out\symbol.txt") $Symbol --bytes $Bytes).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $mailbox) {
        throw "Could not resolve mailbox symbol $Symbol."
    }
    return $mailbox
}

function Invoke-MegalDoomRoute {
    param(
        [string]$Route = "",
        [string]$Waypoints = "",
        [Parameter(Mandatory = $true)][int]$Frames,
        [Parameter(Mandatory = $true)][string]$Report,
        [Parameter(Mandatory = $true)][string]$Mailbox,
        [string]$RequireCheckpoints = ""
    )

    $runnerParams = @{
        Frames = $Frames
        Report = $Report
        Mailbox = $Mailbox
    }
    if ($Route) { $runnerParams.Route = $Route }
    if ($Waypoints) { $runnerParams.Waypoints = $Waypoints }
    if ($RequireCheckpoints) {
        $runnerParams.RequireCheckpoints = $RequireCheckpoints
    }
    & (Join-Path $PSScriptRoot "run-blastem-route.ps1") @runnerParams
}
