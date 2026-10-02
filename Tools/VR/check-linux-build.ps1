# Compiles the server the way a Linux host does (Debian 12, GCC, through the repo's own Dockerfile), so that what
# MSVC lets through and GCC does not is found here and not by whoever builds the server on Linux.
#
#   powershell -ExecutionPolicy Bypass -File Tools\VR\check-linux-build.ps1
#
# Seen's build of 2026-10-01 stopped on "'cSilence' is not captured": a local constexpr used inside a lambda with an
# explicit capture list, which MSVC accepts. Nothing on this machine could have said so.
#
# After a clean build it also runs the unit tests inside the Linux image and compares the protocol id the Linux
# server announces with the Windows build's.
#
# Only the builder stage is built; no image is kept beyond Docker's cache. The first run downloads and compiles every
# dependency and takes a long time; later runs reuse the cache and compile only the server. Heavy while it runs, so
# not during a play session (it refuses if the game is running).

param([switch]$KeepDockerRunning)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$log = Join-Path $root 'build\linux-check.log'

if (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(SkyrimTogetherVR|SkyrimVR)$' }) {
    Write-Host 'The game is running; a Docker build now would cost it frames. Not started.' -ForegroundColor Yellow
    exit 2
}

# Through cmd: Windows PowerShell turns a native program's stderr into an error record, and with 'Stop' that ends the
# script on the very line that is only asking whether Docker is up yet.
function Test-Docker { & cmd.exe /c 'docker info >nul 2>&1'; return $LASTEXITCODE -eq 0 }

$startedDocker = $false
if (-not (Test-Docker)) {
    $desktop = 'C:\Program Files\Docker\Docker\Docker Desktop.exe'
    if (-not (Test-Path $desktop)) { Write-Host 'Docker Desktop is not installed.' -ForegroundColor Red; exit 2 }
    Start-Process $desktop
    $startedDocker = $true
    $deadline = (Get-Date).AddMinutes(4)
    while (-not (Test-Docker)) {
        if ((Get-Date) -gt $deadline) { Write-Host 'Docker did not come up in four minutes.' -ForegroundColor Red; exit 2 }
        Start-Sleep -Seconds 5
    }
}

New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
Push-Location $root
try {
    # Plain progress, so compiler lines reach the log whole.
    $ErrorActionPreference = 'Continue'
    & cmd.exe /c "docker build --progress=plain --target builder -t st-linux-check . > `"$log`" 2>&1"
    $code = $LASTEXITCODE
}
finally { Pop-Location }

$errors = @(Select-String -Path $log -Pattern '\berror:' | Select-Object -First 40)
if ($code -eq 0) {
    Write-Host 'Linux build: ok.' -ForegroundColor Green

    # The unit tests, under GCC, and the protocol id the Linux server announces. That id has to be the one the Windows
    # build has, or a Windows client cannot join a Linux server: it is a digest of Code/encoding, and the two systems
    # keep those files with different line endings.
    $tests = & cmd.exe /c 'docker run --rm st-linux-check bash -lc "cd /src/package/bin && ./TPTests 2>&1 | tail -1" 2>&1'
    Write-Host ('Linux unit tests: ' + ((@($tests | Where-Object { $_ -match 'passed|failed' }) | Select-Object -Last 1) -replace '^\s+', ''))
    if (-not ($tests -match 'All tests passed')) { $code = 3 }

    $start = & cmd.exe /c 'docker run --rm st-linux-check bash -lc "cd /src/package/bin && cp ../lib/libSTServer.so . && (timeout 8 ./SkyrimTogetherServer 2>&1 || true) | grep -a Protocol | head -1" 2>&1'
    $linuxProtocol = if ("$start" -match 'Protocol (\S+):') { $Matches[1] } else { '' }
    $bot = Join-Path $root 'build\windows\x64\release\STBot.exe'
    $winProtocol = ''
    if (Test-Path $bot) { if ("$(& $bot --version 2>&1)" -match 'protocol (\S+)') { $winProtocol = $Matches[1] } }
    if ($linuxProtocol -and $winProtocol -and $linuxProtocol -eq $winProtocol) {
        Write-Host "Protocol: $linuxProtocol on both Linux and Windows." -ForegroundColor Green
    }
    elseif ($linuxProtocol -and $winProtocol) {
        Write-Host "PROTOCOL DIFFERS: Linux $linuxProtocol, Windows $winProtocol. A Windows client would be refused by a Linux server." -ForegroundColor Red
        Write-Host '  (If the Windows build is older than the last change to Code\encoding, run xmake build first.)' -ForegroundColor Yellow
        $code = 4
    }
    else {
        Write-Host "Protocol: Linux '$linuxProtocol', Windows '$winProtocol' -- could not compare." -ForegroundColor Yellow
    }
}
else {
    Write-Host "Linux build FAILED (exit $code). First errors:" -ForegroundColor Red
    $errors | ForEach-Object { Write-Host ('  ' + ($_.Line -replace '^#\d+\s+[\d.]+\s+', '')) }
    Write-Host "Full log: $log"
}

if ($startedDocker -and -not $KeepDockerRunning) {
    # Docker Desktop keeps a virtual machine running; left up it costs memory during the next play session.
    Get-Process 'Docker Desktop', 'com.docker.backend', 'com.docker.build' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    & wsl.exe --shutdown 2>$null
}
exit $code
