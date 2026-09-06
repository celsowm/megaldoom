param(
    [Parameter(Mandatory = $true)][string]$Level
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "e2e-common.ps1")

$root = Get-MegalDoomRoot
$manifestPath = Join-Path $PSScriptRoot "e2e-levels.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1) { throw "Unsupported E2E manifest schema." }
$cases = @($manifest.levels | Where-Object { $_.name -eq $Level })
if ($cases.Count -ne 1) { throw "Unknown or duplicate E2E level: $Level" }
$case = $cases[0]
$waypoints = Join-Path $root $case.waypoints
& python (Join-Path $PSScriptRoot "generate-e2e-routes.py") `
    --map $case.map --out $waypoints
if ($LASTEXITCODE -ne 0) { throw "Could not generate certified waypoints for $Level." }

$eventBits = @{
    started = 0x01; moved = 0x02; combat_hit = 0x04; interaction = 0x08
    key = 0x10; locked = 0x20; unlocked = 0x40; exit = 0x80
}
$requiredEvents = 0
foreach ($event in $case.requiredEvents) {
    if (-not $eventBits.ContainsKey([string]$event)) {
        throw "Unknown E2E event '$event' in $Level."
    }
    $requiredEvents = $requiredEvents -bor $eventBits[[string]$event]
}
if ([int]$case.requiredKeys -ne 0) {
    $requiredEvents = $requiredEvents -bor $eventBits.key
    if ($case.requiresLockedDoor) {
        $requiredEvents = $requiredEvents -bor $eventBits.locked -bor $eventBits.unlocked
    }
}

Invoke-MegalDoomDebugBuild `
    "-DDEBUG_BLASTEM_CHECKPOINT=1 -DDEBUG_E2E_START_LEVEL=$($case.index) -DDEBUG_E2E_GOD=1"
$mailbox = Resolve-MegalDoomMailbox -Symbol "g_debug_e2e_state" -Bytes 20
$report = Join-Path $root ("out\{0}-e2e-report.json" -f $case.name.ToLowerInvariant())
Invoke-MegalDoomRoute -Waypoints $waypoints -Frames ([int]$case.frames) `
    -Report $report -Mailbox $mailbox

$data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
if (-not $data.waypoints.enabled -or -not $data.waypoints.complete -or
    $data.waypoints.failed -or [int]$data.waypoints.current -ne [int]$data.waypoints.total) {
    throw ("{0}: waypoint run did not complete (current={1}/{2}, reason='{3}')." -f
           $Level, $data.waypoints.current, $data.waypoints.total, $data.waypoints.reason)
}
$hex = [string]$data.mailbox
if ($hex.Length -ne 40 -or $hex -notmatch '^[0-9a-fA-F]{40}$') {
    throw "Malformed E2E mailbox for ${Level}: '$hex'"
}
[byte[]]$state = for ($i = 0; $i -lt 20; $i++) {
    [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
}
$events = [int]$state[0]
$keysCollected = [int]$state[1]
$keysLocked = [int]$state[2]
$keysUnlocked = [int]$state[3]
$startLevel = [int]$state[4]
$exitLevel = [int]$state[5]
$godEnabled = [int]$state[6]
$godHits = [int]$state[7]
$deaths = [int]$state[8]
$useSerial = [int]$state[9]
$requiredKeys = [int]$case.requiredKeys

if (($events -band $requiredEvents) -ne $requiredEvents) {
    throw ("{0}: missing events (seen=0x{1:X2}, required=0x{2:X2})" -f
           $Level, $events, $requiredEvents)
}
if ($startLevel -ne [int]$case.index -or $exitLevel -ne [int]$case.index) {
    throw "${Level}: wrong level mailbox (start=$startLevel exit=$exitLevel)."
}
if ($godEnabled -ne 1) { throw "${Level}: E2E God mode was not active." }
if ($godHits -eq 0) { throw "${Level}: E2E God mode never absorbed hostile damage." }
if ($useSerial -eq 0) { throw "${Level}: no C use pulse was acknowledged by gameplay." }
if ($deaths -ne 0) { throw "${Level}: player died $deaths time(s) under E2E God mode." }
if (($keysCollected -band $requiredKeys) -ne $requiredKeys) {
    throw ("{0}: required key missing (collected=0x{1:X2}, required=0x{2:X2})" -f
           $Level, $keysCollected, $requiredKeys)
}
if ($case.requiresLockedDoor -and
    ((($keysLocked -band $requiredKeys) -ne $requiredKeys) -or
     (($keysUnlocked -band $requiredKeys) -ne $requiredKeys))) {
    throw ("{0}: required key was not both blocked and unlocked " +
           "(locked=0x{1:X2}, unlocked=0x{2:X2}, required=0x{3:X2})" -f
           $Level, $keysLocked, $keysUnlocked, $requiredKeys)
}

Write-Host ("{0} E2E OK: events=0x{1:X2}, keys=0x{2:X2}, god hits={3}" -f
            $Level, $events, $keysCollected, $godHits) -ForegroundColor Green
