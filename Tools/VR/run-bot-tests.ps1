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
    Start-Sleep -Seconds 5
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
# The watching half runs minimised, so without this its output goes nowhere -- and in a two-sided test that is the
# half doing the checking. Every failure investigation starts by reading this file.
$hostLog = Join-Path $release 'logs\watcher.log'
$hostBot = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -WindowStyle Minimized `
    -RedirectStandardOutput $hostLog `
    -ArgumentList @("scripts\$hostScriptName", '--server', $Server, '--name', $hostName, '--standalone', '--host-timeout', '8', '--x', '0', '--y', '0', '--max-runtime', $MaxRuntime)
# Touching the handle is what makes ExitCode readable later. Without it the property comes back empty and a passing
# watcher is reported as a failure -- which is the same class of lie as a failing test reported as a pass.
$null = $hostBot.Handle
Start-Sleep -Seconds 14

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

$failures = 0
try {
    for ($run = 1; $run -le $Repeat; $run++) {
        Write-Host "`nRun $run of $Repeat : $Script" -ForegroundColor Cyan
        $proc = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -NoNewWindow -Wait `
            -ArgumentList @("scripts\$Script", '--server', $Server, '--name', "TestBot$run", '--x', '200', '--y', '0', '--max-runtime', $MaxRuntime)
        if ($proc.ExitCode -ne 0) {
            $failures++
            Write-Host "Run $run FAILED (exit $($proc.ExitCode))" -ForegroundColor Red
        }
        else {
            Write-Host "Run $run passed" -ForegroundColor Green
        }
    }
}
finally {
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

Write-Host "`nbot $botVersion : $($Repeat - $failures) of $Repeat runs passed" -ForegroundColor $(if ($failures) { 'Red' } else { 'Green' })
if ($failures) { exit 1 }
exit 0
