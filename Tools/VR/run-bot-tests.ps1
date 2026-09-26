# Runs the bot test suite with nobody present: starts a server, puts a standalone bot in the world to hold it
# open, then runs a test script against it and reports pass or fail.
#
#   powershell -ExecutionPolicy Bypass -File Tools\VR\run-bot-tests.ps1
#   powershell -ExecutionPolicy Bypass -File Tools\VR\run-bot-tests.ps1 -Script auto-suite.txt -Repeat 5
#
# Exit code 0 means every check in every run passed. Anything else means read the report.
#
# This needs no headset and no game. The bot speaks the protocol, so it can check health, death, ownership,
# spawns, party and the connection, and it cannot check anything visual: invisible bodies, dragged corpses and
# hand positions still need a person in a headset.

param(
    [string]$Script = 'auto-suite.txt',
    [int]$Repeat = 1,
    [string]$Server = '127.0.0.1:10578',
    [switch]$KeepServer,
    # Prove the harness can report failure before trusting it to report success. Runs two suites that must fail:
    # one against a dead server, one with an impossible assertion. If either "passes", the harness is lying and
    # every green result is worthless -- which is exactly what happened on 2026-09-25, when a version mismatch
    # refused the bot at the door and four suites reported success having never connected.
    [switch]$SelfCheck,
    # Two bots that test each other rather than themselves. The watching half asserts only values that arrived from
    # the other client, which is the only way to tell a working relay from a bot talking to itself.
    [switch]$Relay,
    # A two-sided test by name: -Pair pvp runs pvp-watcher.txt against pvp-actor.txt.
    [string]$Pair = '',
    [int]$MaxRuntime = 300
)

$hostScriptName = 'host.txt'
$hostName = 'HostBot'
if ($Relay -and -not $Pair) { $Pair = 'relay' }
if ($Pair) {
    $Script = "$Pair-actor.txt"
    $hostScriptName = "$Pair-watcher.txt"
    $hostName = 'Watcher'
    if ($MaxRuntime -lt 300) { $MaxRuntime = 300 }
}

$ErrorActionPreference = 'Stop'

# Compare the bot against the server before running anything. A refused version shows up as "the script ran no
# checks" or exit 2, which both read as a broken test and are in fact binaries built at different moments. That has
# now cost time three times (2026-09-25, and twice on 2026-09-26), always the same way: a client or server built
# mid-change while the working tree moved on. The version is a hash of the uncommitted diff, so the only safe habit
# is to build all of them in one go -- and to say so loudly when they disagree.
function Assert-VersionsMatch($botExe, $release) {
    $botLine = & $botExe --version 2>&1 | Select-String -Pattern 'STBot (v\S+)' | Select-Object -First 1
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

$release = Join-Path $PSScriptRoot '..\..\build\windows\x64\release'
$release = (Resolve-Path $release).Path

$bot = Join-Path $release 'STBot.exe'
$serverExe = Join-Path $release 'SkyrimTogetherServer.exe'
foreach ($needed in @($bot, $serverExe)) {
    if (-not (Test-Path $needed)) { throw "Not built: $needed. Run: xmake build STBot SkyrimTogetherServer SkyrimServerRunner" }
}

# The bot, the server and the client compare a version string on connect, so they have to come from the same
# build. Code/bot is excluded from that string (modules/version.lua), so editing the bot alone will not lock it
# out of a running server, but editing anything else will.
$versionFile = Join-Path $release '..\..\..\BuildVersion.txt'
$botVersion = if (Test-Path $versionFile) { (Get-Content $versionFile -Raw).Trim() } else { 'unknown' }

$startedServer = $false
if (-not (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue)) {
    Write-Host "Starting a server" -ForegroundColor Cyan
    Start-Process -FilePath $serverExe -WorkingDirectory $release -WindowStyle Minimized | Out-Null
    $startedServer = $true
    # Wait for the port rather than for a guessed number of seconds: the server is usually up in well under a
    # second, and on a slow machine five would not have been enough anyway.
    $serverHost, $serverPort = $Server.Split(':')
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        try {
            $probe = New-Object System.Net.Sockets.UdpClient
            $probe.Connect($serverHost, [int]$serverPort)
            $probe.Close()
            if (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue) { break }
        } catch { }
        Start-Sleep -Milliseconds 200
    }
}
else {
    Write-Host "Using the server that is already running" -ForegroundColor Cyan
}

# A standalone bot holds the world open so the test bot has someone to find. Without it the test bot waits for a
# host forever, which is what made every test need a person in a headset.
Write-Host "Starting the standalone host bot" -ForegroundColor Cyan
$hostScript = Join-Path $release "scripts\$hostScriptName"
if (-not (Test-Path $hostScript)) {
    if ($Pair) { Write-Host "Missing $hostScript" -ForegroundColor Red; exit 2 }
    @('log standalone host, holds the world open', 'waitfor connected 40', 'collect none', 'wait 600', 'stop') |
        Set-Content -Path $hostScript -Encoding utf8
}
# --host-timeout 2, not 8: that timeout is how long a bot looks for a *human* host before giving up and going in
# standalone, and in a harness run there is never a human. Eight seconds of looking, on every run, for something
# that cannot be there.
# The watching half runs minimised, so without this its output goes nowhere -- and in a two-sided test that is the
# half doing the checking. Every failure investigation starts by reading this file.
$hostLog = Join-Path $release 'logs\watcher.log'
$hostBot = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -WindowStyle Minimized `
    -RedirectStandardOutput $hostLog `
    -ArgumentList @("scripts\$hostScriptName", '--server', $Server, '--name', $hostName, '--standalone', '--host-timeout', '2', '--x', '0', '--y', '0', '--max-runtime', $MaxRuntime)
# Touching the handle is what makes ExitCode readable later. Without it the property comes back empty and a passing
# watcher is reported as a failure -- which is the same class of lie as a failing test reported as a pass.
$null = $hostBot.Handle

# Wait for the watching bot to actually be in the world, rather than for a fixed fourteen seconds. It gives up
# looking for a human host after --host-timeout and goes standalone, so the wait was always "8 plus margin"; the
# margin was pure cost on every single run. Reading its own words is both quicker and honest about what we waited
# for. The deadline is generous because a bot that never arrives is a failure the run should report, not hang on.
$hostReady = $false
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
    if ($hostBot.HasExited) { break }
    if ((Test-Path $hostLog) -and (Select-String -Path $hostLog -Pattern 'In the world: our character is' -Quiet -ErrorAction SilentlyContinue)) {
        $hostReady = $true
        break
    }
    Start-Sleep -Milliseconds 250
}
if (-not $hostReady) { Write-Host "The watching bot never reached the world; running anyway" -ForegroundColor Yellow }

if ($SelfCheck) {
    Write-Host "`nSelf-check: these two runs MUST fail" -ForegroundColor Cyan
    $selfOk = $true

    $dead = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -NoNewWindow -Wait `
        -ArgumentList @('scripts\selfcheck-noserver.txt', '--server', '127.0.0.1:1', '--max-runtime', '20')
    if ($dead.ExitCode -eq 0) { Write-Host "  BROKEN: an unreachable server reported success" -ForegroundColor Red; $selfOk = $false }
    else { Write-Host "  ok: unreachable server fails (exit $($dead.ExitCode))" -ForegroundColor Green }

    $bad = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -NoNewWindow -Wait `
        -ArgumentList @('scripts\selfcheck-fail.txt', '--server', $Server, '--name', 'SelfCheckBot', '--x', '200', '--y', '0', '--max-runtime', '60')
    if ($bad.ExitCode -eq 0) { Write-Host "  BROKEN: an impossible assertion reported success" -ForegroundColor Red; $selfOk = $false }
    else { Write-Host "  ok: a failed assertion fails (exit $($bad.ExitCode))" -ForegroundColor Green }

    if (-not $selfOk) {
        if ($hostBot -and -not $hostBot.HasExited) { Stop-Process -Id $hostBot.Id -Force -ErrorAction SilentlyContinue }
        if ($startedServer -and -not $KeepServer) { Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
        Write-Host "`nHarness self-check FAILED: do not trust any green result until this is fixed" -ForegroundColor Red
        exit 2
    }
}

# -Script takes a list: "a.txt,b.txt" runs both against the same server and the same watching bot. Standing all
# that up again for each script was costing more than the scripts themselves.
$scriptList = @($Script -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })

# A bot whose version does not match the server's is refused at the door and exits 2 with no checks run, which the
# summary then reports as "0 of 1 passed" -- a failure that looks like a broken test and is actually a stale build.
# It has happened twice (2026-09-25, 2026-09-26). Name it before running anything.
$botStamp = & $bot --version 2>&1 | Select-String -Pattern 'STBot (v\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1
if (-not $botStamp) {
    $usage = & $bot 2>&1 | Select-Object -First 1
    if ($usage -notmatch 'Usage') { Write-Host "Could not read the bot's version" -ForegroundColor Yellow }
}
$serverStamp = $null
$serverLog = Join-Path $release 'logs\STServerOut.log'
if (Test-Path $serverLog) {
    $serverStamp = Get-Content $serverLog -Tail 400 | Select-String -Pattern 'server v(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1
}

# Where the server's log ends before anything runs, so only this run's lines are read afterwards.
$serverLogPath = Join-Path $release 'logs\STServerOut.log'
$serverLogStart = 0
if (Test-Path $serverLogPath) { $serverLogStart = (Get-Content $serverLogPath | Measure-Object -Line).Lines }

Assert-VersionsMatch $bot $release

$failures = 0
$run = 0
try {
    foreach ($scriptName in $scriptList) {
        for ($pass = 1; $pass -le $Repeat; $pass++) {
            $run++
            $label = if ($Repeat -gt 1) { "$scriptName (pass $pass of $Repeat)" } else { $scriptName }
            Write-Host "`n$label" -ForegroundColor Cyan
            $proc = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -NoNewWindow -Wait `
                -ArgumentList @("scripts\$scriptName", '--server', $Server, '--name', "TestBot$run", '--x', '200', '--y', '0', '--max-runtime', $MaxRuntime)
            if ($proc.ExitCode -eq 2) {
                $failures++
                Write-Host "$scriptName could not run (exit 2)." -ForegroundColor Red
                Write-Host "  Exit 2 means the bot never got in: a refused version, a missing script or a bad option." -ForegroundColor Yellow
                Write-Host "  If the version was refused, the binaries were built at different times. Rebuild the server, the runner and the bot together." -ForegroundColor Yellow
            }
            elseif ($proc.ExitCode -ne 0) {
                $failures++
                Write-Host "$scriptName FAILED (exit $($proc.ExitCode))" -ForegroundColor Red
            }
            else {
                Write-Host "$scriptName passed" -ForegroundColor Green
            }
        }
    }
}
finally {
    # A message the server threw away is the failure that does not look like one: on 2026-09-26 health-sign.txt
    # reported "1 of 1 runs passed" while the server was rejecting every health change the bot sent, because the
    # script asserts on the bot's own bookkeeping. The server now says when it drops something, so the harness
    # reads that back and calls it what it is. Any silently discarded message fails the run from here on.
    if (Test-Path $serverLogPath) {
        $dropped = Get-Content $serverLogPath | Select-Object -Skip $serverLogStart | Select-String -Pattern 'Dropped (an|a) .* ownership epoch'
        if ($dropped) {
            $failures++
            Write-Host "`nThe server threw messages away during this run:" -ForegroundColor Red
            $dropped | Select-Object -First 3 | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Red }
            Write-Host "  A test can pass while this happens, because a script can assert on what the bot believes rather than on what arrived." -ForegroundColor Yellow
        }
    }

    if ($Pair -and $hostBot) {
        if (-not $hostBot.WaitForExit(90000)) {
            Write-Host "Watcher never finished" -ForegroundColor Red
            $failures++
        }
        elseif ($hostBot.ExitCode -ne 0) {
            Write-Host "Watcher FAILED (exit $($hostBot.ExitCode)): the values never crossed" -ForegroundColor Red
            $failures++
        }
        else {
            Write-Host "Watcher passed: every value it checked came from the other client" -ForegroundColor Green
        }
    }
    if ($hostBot -and -not $hostBot.HasExited) { Stop-Process -Id $hostBot.Id -Force -ErrorAction SilentlyContinue }
    if ($startedServer -and -not $KeepServer) {
        Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Write-Host "Server stopped" -ForegroundColor DarkGray
    }
}

Write-Host "`nbot $botVersion : $($run - $failures) of $run runs passed" -ForegroundColor $(if ($failures) { 'Red' } else { 'Green' })
if ($failures) { exit 1 }
exit 0
