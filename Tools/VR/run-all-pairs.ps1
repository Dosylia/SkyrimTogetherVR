# Every two-sided test at once, each pair in its own worldspace.
#
#   powershell -ExecutionPolicy Bypass -File Tools\VR\run-all-pairs.ps1
#
# The pairs used to run one after another because a watcher resolves "other" to whoever else is in the world, so
# two pairs sharing a server would see four bots and assert about the wrong one. They do not have to share a
# world, though: the server's range check (CellIdComponent::IsInRange) returns false for a different worldspace
# before it looks at anything else, so a pair in its own worldspace is invisible to the others. One server, five
# pairs, ten bots, all at the same time.
#
# Coverage is identical -- the same scripts, the same assertions -- so this is wall time and nothing else.

param(
    [string]$Server = '127.0.0.1:10578',
    [string[]]$Pairs = @('relay', 'pvp', 'churn2', 'range', 'equip', 'deadfar'),
    [int]$MaxRuntime = 300,
    # Two bots per pair, and the server refuses the ninth player: GameServer:uMaxPlayerCount defaults to 8 and its
    # own description says going above that is not recommended. That setting belongs to the server people actually
    # play on, so the batch is sized to it rather than the other way round.
    [int]$PairsAtOnce = 3,
    [switch]$KeepServer
)

$ErrorActionPreference = 'Stop'

# Compare the bot against the server before running anything. A refused version shows up as "the script ran no
# checks" or exit 2, which both read as a broken test and are in fact binaries built at different moments. That has
# now cost time three times (2026-09-25, and twice on 2026-09-26), always the same way: a client or server built
# mid-change while the working tree moved on. The version is a hash of the uncommitted diff, so the only safe habit
# is to build all of them in one go -- and to say so loudly when they disagree.
function Assert-VersionsMatch($botExe, $release) {
    $botLine = & $botExe 2>&1 | Select-String -Pattern 'STBot (\S+)' | Select-Object -First 1
    if (-not $botLine) { return }
    $botVer = $botLine.Matches[0].Groups[1].Value

    $log = Join-Path $release 'logs\STServerOut.log'
    if (-not (Test-Path $log)) { return }
    $srvLine = Get-Content $log -Tail 3000 | Select-String -Pattern 'server v(\S+)|version .v(\S+)\.' | Select-Object -Last 1
    if (-not $srvLine) { return }

    if ($srvLine.Line -notmatch [regex]::Escape($botVer)) {
        Write-Host "The bot and the server were built at different times." -ForegroundColor Red
        Write-Host "  bot:    $botVer" -ForegroundColor Yellow
        Write-Host "  server: $($srvLine.Line.Trim())" -ForegroundColor Yellow
        Write-Host "  Rebuild SkyrimTogetherClientVR, SkyrimImmersiveLauncherVR, SkyrimTogetherServer, SkyrimServerRunner and STBot in one go." -ForegroundColor Yellow
    }
}

$release = (Resolve-Path (Join-Path $PSScriptRoot '..\..\build\windows\x64\release')).Path
$bot = Join-Path $release 'STBot.exe'
$serverExe = Join-Path $release 'SkyrimTogetherServer.exe'

$startedServer = $false
if (-not (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $serverExe -WorkingDirectory $release -WindowStyle Minimized | Out-Null
    $startedServer = $true
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and -not (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue)) {
        Start-Sleep -Milliseconds 200
    }
    Start-Sleep -Milliseconds 800
}

$serverLogPath = Join-Path $release 'logs\STServerOut.log'
$serverLogStart = 0
if (Test-Path $serverLogPath) { $serverLogStart = (Get-Content $serverLogPath | Measure-Object -Line).Lines }

# 3C is the bot's default. One worldspace per pair, so none of them can see another.
$worldspaces = @('3C', '3D', '3E', '3F', '40', '41', '42', '43')

Assert-VersionsMatch $bot $release

$failures = 0
$batchNumber = 0
for ($offset = 0; $offset -lt $Pairs.Count; $offset += $PairsAtOnce) {
$batch = $Pairs[$offset..([Math]::Min($offset + $PairsAtOnce, $Pairs.Count) - 1)]
$batchNumber++
Write-Host "`nbatch $batchNumber : $($batch -join ', ')" -ForegroundColor Cyan

$running = @()
for ($i = 0; $i -lt $batch.Count; $i++) {
    $pair = $batch[$i]
    $ws = $worldspaces[$i]
    $watcherScript = Join-Path $release "scripts\$pair-watcher.txt"
    $actorScript = Join-Path $release "scripts\$pair-actor.txt"
    if (-not (Test-Path $watcherScript) -or -not (Test-Path $actorScript)) {
        Write-Host "Missing scripts for pair '$pair'" -ForegroundColor Red
        continue
    }

    $watcherLog = Join-Path $release "logs\parallel-$pair-watcher.log"
    $watcher = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -WindowStyle Minimized -RedirectStandardOutput $watcherLog `
        -ArgumentList @("scripts\$pair-watcher.txt", '--server', $Server, '--name', "W-$pair", '--standalone', '--host-timeout', '2',
                        '--worldspace', $ws, '--x', '0', '--y', '0', '--max-runtime', $MaxRuntime)
    $null = $watcher.Handle

    # The watcher has to be in the world before its partner arrives, or the partner is the one waiting.
    $ready = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $ready) {
        if ($watcher.HasExited) { break }
        if ((Test-Path $watcherLog) -and (Select-String -Path $watcherLog -Pattern 'In the world: our character is' -Quiet -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 200
    }

    $actorLog = Join-Path $release "logs\parallel-$pair-actor.log"
    $actor = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -WindowStyle Minimized -RedirectStandardOutput $actorLog `
        -ArgumentList @("scripts\$pair-actor.txt", '--server', $Server, '--name', "A-$pair", '--worldspace', $ws,
                        '--x', '200', '--y', '0', '--max-runtime', $MaxRuntime)
    $null = $actor.Handle

    $running += [pscustomobject]@{ Pair = $pair; Watcher = $watcher; Actor = $actor }
    Write-Host "started $pair in worldspace $ws" -ForegroundColor DarkGray
}

foreach ($entry in $running) {
    foreach ($half in @(@{ N = 'actor'; P = $entry.Actor }, @{ N = 'watcher'; P = $entry.Watcher })) {
        if (-not $half.P.WaitForExit(($MaxRuntime + 120) * 1000)) {
            Write-Host "$($entry.Pair) $($half.N) never finished" -ForegroundColor Red
            Stop-Process -Id $half.P.Id -Force -ErrorAction SilentlyContinue
            $failures++
            continue
        }
        if ($half.P.ExitCode -ne 0) {
            Write-Host "$($entry.Pair) $($half.N) FAILED (exit $($half.P.ExitCode))" -ForegroundColor Red
            $failures++
        }
    }
}
}

if (Test-Path $serverLogPath) {
    $dropped = Get-Content $serverLogPath | Select-Object -Skip $serverLogStart | Select-String -Pattern 'Dropped (an|a) .* ownership epoch'
    if ($dropped) {
        $failures++
        Write-Host "`nThe server threw messages away during this run:" -ForegroundColor Red
        $dropped | Select-Object -First 3 | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Red }
    }
}

if ($startedServer -and -not $KeepServer) {
    Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($failures) { Write-Host "`n$($Pairs.Count) pairs, $failures halves failed" -ForegroundColor Red; exit 1 }
Write-Host "`n$($Pairs.Count) pairs, all passed" -ForegroundColor Green
