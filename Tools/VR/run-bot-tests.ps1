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
    [switch]$KeepServer
)

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
$hostScript = Join-Path $release 'scripts\host.txt'
if (-not (Test-Path $hostScript)) {
    @('log standalone host, holds the world open', 'waitfor connected 40', 'collect none', 'wait 600', 'stop') |
        Set-Content -Path $hostScript -Encoding utf8
}
$hostBot = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -WindowStyle Minimized `
    -ArgumentList @('scripts\host.txt', '--server', $Server, '--name', 'HostBot', '--standalone', '--host-timeout', '8', '--x', '0', '--y', '0')
Start-Sleep -Seconds 14

$failures = 0
try {
    for ($run = 1; $run -le $Repeat; $run++) {
        Write-Host "`nRun $run of $Repeat : $Script" -ForegroundColor Cyan
        $proc = Start-Process -FilePath $bot -WorkingDirectory $release -PassThru -NoNewWindow -Wait `
            -ArgumentList @("scripts\$Script", '--server', $Server, '--name', "TestBot$run", '--x', '200', '--y', '0')
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
    if ($hostBot -and -not $hostBot.HasExited) { Stop-Process -Id $hostBot.Id -Force -ErrorAction SilentlyContinue }
    if ($startedServer -and -not $KeepServer) {
        Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Write-Host "Server stopped" -ForegroundColor DarkGray
    }
}

Write-Host "`nbot $botVersion : $($Repeat - $failures) of $Repeat runs passed" -ForegroundColor $(if ($failures) { 'Red' } else { 'Green' })
if ($failures) { exit 1 }
exit 0
