# Every real-game test in one go: the game brought up with nobody in the headset, every live-*.txt script run
# against it, one table per batch, and the game taken down again whatever happened.
#
#   powershell -ExecutionPolicy Bypass -File Tools\VR\run-live.ps1                  # all of them, in live-check.py's order
#   powershell -ExecutionPolicy Bypass -File Tools\VR\run-live.ps1 live-copy live-npc
#   powershell -ExecutionPolicy Bypass -File Tools\VR\run-live.ps1 -Batch 0         # one game for the whole run
#
# The game is started afresh every -Batch scripts (default 6). Emma's modlist grows the game with every loading
# screen -- DynDOLOD DLL NG Alpha-32 leaves twelve threads behind per load, and the process gains about 0.4 GB per
# round trip, connected or not (measured 2026-10-03) -- and the travelling scripts do two loads each: one game for
# the whole suite froze in the game's own allocator after about thirteen scripts (12:07) and crashed in the AMD
# driver after eight (12:32). About ten minutes per batch. It takes over the screen while it runs, and the away
# scripts move the player through loading screens. Nothing is saved.

[CmdletBinding(PositionalBinding = $false)]
param(
    [int]$Batch = 6,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Scripts
)

$ErrorActionPreference = 'Stop'
$headless = Join-Path $PSScriptRoot 'headless.ps1'
$check = Join-Path $PSScriptRoot 'live-check.py'

if (-not $Scripts -or $Scripts.Count -eq 0) { $Scripts = @(& python $check --list | Where-Object { $_ }) }
if ($Batch -le 0) { $Batch = $Scripts.Count }

$code = 0
for ($i = 0; $i -lt $Scripts.Count; $i += $Batch) {
    $these = $Scripts[$i..([Math]::Min($i + $Batch, $Scripts.Count) - 1)]
    Write-Host ("Batch {0} of {1}: {2}" -f ([int]($i / $Batch) + 1), [Math]::Ceiling($Scripts.Count / $Batch), ($these -join ' ')) -ForegroundColor Cyan

    & powershell -ExecutionPolicy Bypass -File $headless up
    if ($LASTEXITCODE -ne 0) { Write-Host 'The game did not come up; this batch was not run.' -ForegroundColor Red; $code = 2; continue }
    try {
        & python $check @these
        if ($LASTEXITCODE -ne 0) { $code = 1 }
    }
    finally {
        & powershell -ExecutionPolicy Bypass -File $headless down
    }
}
exit $code
