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
    [string[]]$Pairs = @('relay', 'pvp', 'churn2', 'range', 'equip', 'deadfar', 'cellwalk', 'ownerchurn', 'drops', 'dropmove', 'spell', 'rangeback', 'revive'),
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
#
# $SkipLines is how much of the log predates this run's server. It matters when this script starts the server
# itself: the check runs a moment after the process appears, before the server has written its own version line,
# so without it the "latest" line was the previous run's -- and the check reported a mismatch that did not exist,
# in red, on two runs whose bots then connected and passed (2026-09-28 and 2026-09-30).
function Assert-VersionsMatch($botExe, $release, [int]$SkipLines = 0) {
    # The protocol, which is what the server compares since 2026-10-02 (a digest of Code/encoding). The versions may
    # differ and usually do after a bot-only change; that is no longer a reason for a refusal.
    $botLine = & $botExe --version 2>&1 | Select-String -Pattern 'protocol (\S+)' | Select-Object -First 1
    if (-not $botLine) { return }
    $botProtocol = $botLine.Matches[0].Groups[1].Value

    $log = Join-Path $release 'logs\STServerOut.log'
    if (-not (Test-Path $log)) { return }
    $all = @(Get-Content $log)
    # The log may have rotated at start-up, in which case it is shorter than the line count taken before.
    if ($SkipLines -gt $all.Count) { $SkipLines = 0 }
    $srvLine = $all | Select-Object -Skip $SkipLines | Select-String -Pattern 'Protocol (\S+):' | Select-Object -Last 1
    if (-not $srvLine) { return }
    $srvProtocol = $srvLine.Matches[0].Groups[1].Value

    if ($srvProtocol -ne $botProtocol) {
        Write-Host "The bot and the server speak different protocols; the server will refuse the bot." -ForegroundColor Red
        Write-Host "  bot:    $botProtocol" -ForegroundColor Yellow
        Write-Host "  server: $srvProtocol" -ForegroundColor Yellow
        Write-Host "  Something in Code\encoding changed between the two builds. Run xmake build (everything) once." -ForegroundColor Yellow
    }
}

$release = (Resolve-Path (Join-Path $PSScriptRoot '..\..\build\windows\x64\release')).Path
$bot = Join-Path $release 'STBot.exe'
$serverExe = Join-Path $release 'SkyrimTogetherServer.exe'

# The bot reads its scripts from the release folder, and nothing in the build puts them there. They were copied
# by hand, which drifted both ways: the doorloss pair existed only in the release folder and was never added to
# the repo, so a clean build would have lost it, while the repo's copies could sit unused for a run. Sync them
# here, so the scripts that run are the scripts that are tracked.
$scriptSource = (Resolve-Path (Join-Path $PSScriptRoot '..\..\Code\bot\scripts')).Path
$scriptDest = Join-Path $release 'scripts'
if (-not (Test-Path $scriptDest)) { New-Item -ItemType Directory -Path $scriptDest | Out-Null }
Copy-Item -Path (Join-Path $scriptSource '*.txt') -Destination $scriptDest -Force

$serverLogPath = Join-Path $release 'logs\STServerOut.log'
$logBeforeStart = 0
if (Test-Path $serverLogPath) { $logBeforeStart = @(Get-Content $serverLogPath).Count }

$startedServer = $false
if (-not (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $serverExe -WorkingDirectory $release -WindowStyle Hidden | Out-Null
    $startedServer = $true
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and -not (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue)) {
        Start-Sleep -Milliseconds 200
    }

    # Wait for this server's own start-up line rather than a fixed pause: that line is what the version check
    # reads, and it is also the only sign the server is really up. A server whose STServer.dll and runner were
    # built apart exits at once, code 1, with no log line at all -- which cost an evening on 2026-09-29 before the
    # note under Harness in VR_HISTORY.md was found. Say so here instead of letting every bot time out.
    $upBy = (Get-Date).AddSeconds(20)
    $started = $false
    while ((Get-Date) -lt $upBy) {
        if (Test-Path $serverLogPath) {
            $lines = @(Get-Content $serverLogPath)
            $skip = if ($logBeforeStart -gt $lines.Count) { 0 } else { $logBeforeStart }
            if ($lines | Select-Object -Skip $skip | Select-String -Pattern 'started on port' -Quiet) { $started = $true; break }
        }
        if (-not (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $started) {
        Write-Host "The server did not come up: it wrote no 'started on port' line." -ForegroundColor Red
        if (-not (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue)) {
            Write-Host "  It has already exited. The usual cause is STServer.dll and the runner built apart -- rebuild everything in one go." -ForegroundColor Yellow
        }
        exit 1
    }
}

$serverLogStart = 0
if (Test-Path $serverLogPath) { $serverLogStart = @(Get-Content $serverLogPath).Count }

# 3C is the bot's default. One worldspace per pair, so none of them can see another.
$worldspaces = @('3C', '3D', '3E', '3F', '40', '41', '42', '43')

# Only this run's lines when this run started the server; a server that was already up wrote its line earlier.
Assert-VersionsMatch $bot $release -SkipLines $(if ($startedServer) { $logBeforeStart } else { 0 })

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
        # A failure, not a note. 'doorloss' sat in the default list until 2026-09-28 with no scripts behind it:
        # the run printed one red line, carried on, and reported success, so the suite read as green while the
        # invisible-copy regression it was named for was never run at all. A test that does not exist must not
        # be able to pass.
        Write-Host "Missing scripts for pair '$pair' -- expected $actorScript and $watcherScript" -ForegroundColor Red
        $failures++
        continue
    }

    $watcherLog = Join-Path $release "logs\parallel-$pair-watcher.log"
    $watcher = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -WindowStyle Hidden -RedirectStandardOutput $watcherLog `
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
    $actor = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -WindowStyle Hidden -RedirectStandardOutput $actorLog `
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
