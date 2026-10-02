# Every real-game test in one go: the game brought up with nobody in the headset, every live-*.txt script run
# against it, one table at the end, and the game taken down again whatever happened.
#
#   powershell -ExecutionPolicy Bypass -File Tools\VR\run-live.ps1                  # all of them, in live-check.py's order
#   powershell -ExecutionPolicy Bypass -File Tools\VR\run-live.ps1 live-copy live-npc
#
# About ten minutes for the full set. It takes over the screen while it runs, and the last two scripts move the
# player through a load door. Nothing is saved.

param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Scripts)

$ErrorActionPreference = 'Stop'
$headless = Join-Path $PSScriptRoot 'headless.ps1'
$check = Join-Path $PSScriptRoot 'live-check.py'

& powershell -ExecutionPolicy Bypass -File $headless up
if ($LASTEXITCODE -ne 0) { Write-Host 'The game did not come up; nothing was run.' -ForegroundColor Red; exit 2 }

$code = 1
try {
    if ($Scripts -and $Scripts.Count -gt 0) { & python $check @Scripts } else { & python $check --all }
    $code = $LASTEXITCODE
}
finally {
    & powershell -ExecutionPolicy Bypass -File $headless down
}
exit $code
