# Runs the real game with nobody in the headset, for tests a bot alone cannot do.
#
#   powershell -ExecutionPolicy Bypass -File Tools\VR\headless.ps1 up        # SteamVR (null headset), server, game, last save, connected
#   powershell -ExecutionPolicy Bypass -File Tools\VR\headless.ps1 up -NoConnect   # the same without a server: the game alone
#   powershell -ExecutionPolicy Bypass -File Tools\VR\headless.ps1 status
#   powershell -ExecutionPolicy Bypass -File Tools\VR\headless.ps1 down      # quit the game, stop what 'up' started, headset setting off
#
# First done by hand on 2026-10-01; this is those steps, in the order that worked. What it relies on:
#   - SteamVR's 'null' driver with two virtual controllers (the SkyrimVR Devkit's source, built and copied in by
#     Emma). Valve's stock null driver has no controllers, and skyrimvrtools.dll crashes six seconds after a load
#     without them. The driver is only active while a 'driver_null' block is in steamvr.vrsettings, which 'up' adds
#     and 'down' removes -- SteamVR must be closed for either, because it rewrites that file when it exits.
#   - DevBench (an SKSE plugin, http://127.0.0.1:8921) to load the save and to quit.
#   - The client connecting by itself five seconds after a load (%LOCALAPPDATA%\SkyrimTogetherVR\connect.txt).
#
# 'down' closes SteamVR by force: it ignores a polite close, and Emma allowed it for these runs (2026-10-01). It
# never touches MO2.

param(
    [Parameter(Position = 0)][ValidateSet('up', 'down', 'status')][string]$Action = 'status',
    [string]$Save = '',                 # a save name; empty loads the most recent one
    [switch]$NoConnect,                 # no server and no waiting for a connection: the game alone, mod loaded
    [int]$LoadTimeout = 180
)

$ErrorActionPreference = 'Stop'

$settingsPath = 'C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
$mo2 = 'E:\FUS\ModOrganizer.exe'
$release = (Resolve-Path (Join-Path $PSScriptRoot '..\..\build\windows\x64\release')).Path
$clientLog = 'E:\FUS\tools\Skyrim Together VR\logs\tp_client.log'
$stateFile = Join-Path $env:TEMP 'st-headless-state.json'
$devbench = 'http://127.0.0.1:8921'

function Test-Running([string]$pattern) {
    return [bool](Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match $pattern })
}

function Invoke-Tool([string]$tool, [hashtable]$body, [int]$timeout = 15) {
    $json = $body | ConvertTo-Json -Compress -Depth 6
    return Invoke-RestMethod -Uri "$devbench/api/tool/$tool" -Method Post -ContentType 'application/json' -Body $json -TimeoutSec $timeout
}

function Get-Health {
    try { return Invoke-RestMethod -Uri "$devbench/api/health" -TimeoutSec 3 } catch { return $null }
}

# The settings file is JSON that SteamVR writes itself; it is read and written whole, and only one key is touched.
function Set-NullDriver([bool]$on) {
    if (Test-Running '^(vrserver|vrmonitor|vrcompositor)$') { throw 'SteamVR is running; its settings cannot be changed until it is closed.' }
    $text = [System.IO.File]::ReadAllText($settingsPath)
    $settings = $text | ConvertFrom-Json
    $has = [bool]($settings.PSObject.Properties.Name -contains 'driver_null')
    if ($on) {
        $block = [pscustomobject]@{ enable = $true; windowWidth = 1280; windowHeight = 720; renderWidth = 1024; renderHeight = 1024; displayFrequency = 90.0 }
        if ($has) { $settings.driver_null = $block } else { $settings | Add-Member -NotePropertyName 'driver_null' -NotePropertyValue $block }
    }
    elseif ($has) { $settings.PSObject.Properties.Remove('driver_null') }
    else { return }
    $out = $settings | ConvertTo-Json -Depth 20
    [System.IO.File]::WriteAllText($settingsPath, $out + "`n", (New-Object System.Text.UTF8Encoding($false)))
}

# A test that throws (a request timing out while the game is busy loading) counts as "not yet". With -NeedsGame the
# wait ends at once if the game has gone, instead of sitting out its timeout on a process that crashed.
function Wait-Until([scriptblock]$test, [int]$seconds, [string]$what, [switch]$NeedsGame) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if ($NeedsGame -and -not (Test-Running '^SkyrimTogetherVR$')) {
            throw "The game closed while waiting for $what; see the newest crash log in Documents\My Games\Skyrim VR\SKSE."
        }
        $ok = $false
        try { $ok = [bool](& $test) } catch { $ok = $false }
        if ($ok) { return }
        Start-Sleep -Milliseconds 1500
    }
    throw "Timed out after $seconds s waiting for $what."
}

function Show-Status {
    $health = Get-Health
    $game = Test-Running '^SkyrimTogetherVR$'
    $connected = 'unknown'
    if ($game -and (Test-Path $clientLog)) {
        $probe = Get-Content $clientLog -Tail 400 | Select-String -Pattern 'Probe: connected (yes|no)' | Select-Object -Last 1
        if ($probe) { $connected = $probe.Matches[0].Groups[1].Value }
    }
    Write-Host ("game {0} | devbench {1} | connected {2} | SteamVR {3} | server {4}" -f `
        $(if ($game) { 'running' } else { 'not running' }), `
        $(if ($health) { "frame $($health.frame), $($health.lastLifecycle)" } else { 'no answer' }), `
        $connected, `
        $(if (Test-Running '^vrserver$') { 'running' } else { 'closed' }), `
        $(if (Test-Running '^SkyrimTogetherServer$') { 'running' } else { 'stopped' }))
}

function Stop-Everything {
    $state = $null
    if (Test-Path $stateFile) { $state = Get-Content $stateFile -Raw | ConvertFrom-Json }

    if (Test-Running '^SkyrimTogetherVR$') {
        # The game's own quit. A kill would leave a crash report that reads like a real crash later.
        try { Invoke-Tool 'console' @{ action = 'exec'; command = 'qqq' } 8 | Out-Null } catch { }
        try { Wait-Until { -not (Test-Running '^SkyrimTogetherVR$') } 40 'the game to quit' }
        catch {
            Write-Host 'The game did not quit on its own; stopping it.' -ForegroundColor Yellow
            Get-Process SkyrimTogetherVR -ErrorAction SilentlyContinue | Stop-Process -Force
        }
    }

    if ($state -and $state.startedServer) { Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue | Stop-Process -Force }

    # More than once if need be. SteamVR's parts restart one another: on 2026-10-02 a single pass left all five
    # running, the setting could not be removed, and the fake headset stayed switched on until somebody noticed.
    $closed = $false
    foreach ($attempt in 1..4) {
        foreach ($name in 'vrmonitor', 'vrstartup', 'vrdashboard', 'vrwebhelper', 'vrcompositor', 'vrserver', 'steamvr_room_setup') {
            Get-Process $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Seconds 3
        if (-not (Test-Running '^(vrserver|vrmonitor|vrcompositor|vrdashboard|vrwebhelper|vrstartup)$')) {
            # Still gone a moment later, or it was only between two restarts.
            Start-Sleep -Seconds 3
            if (-not (Test-Running '^(vrserver|vrmonitor|vrcompositor|vrdashboard|vrwebhelper|vrstartup)$')) { $closed = $true; break }
        }
    }
    if (-not $closed) { Write-Host 'SteamVR is still running after four attempts to close it.' -ForegroundColor Red }

    $settingOff = $false
    try { Set-NullDriver $false; $settingOff = $true; Write-Host 'Headset setting off: the Quest starts normally again.' }
    catch { Write-Host "COULD NOT REMOVE the driver_null setting: $_" -ForegroundColor Red }

    Remove-Item $stateFile -ErrorAction SilentlyContinue
    if (-not $settingOff) {
        Write-Host 'THE FAKE HEADSET IS STILL SWITCHED ON. Close SteamVR and run "headless.ps1 down" again before playing.' -ForegroundColor Red
        $script:downFailed = $true
    }
}

if ($Action -eq 'status') { Show-Status; return }
if ($Action -eq 'down') { Stop-Everything; Show-Status; if ($script:downFailed) { exit 3 }; return }

# ---- up ----
if (Test-Running '^(SkyrimTogetherVR|SkyrimVR)$') { throw 'The game is already running. If that is a real session, leave it alone; otherwise run "down" first.' }
if (Test-Running '^(vrserver|vrmonitor)$') { throw 'SteamVR is already running. If the headset is in use, leave it alone; otherwise close it first.' }

$startedServer = $false
try {
    Set-NullDriver $true
    @{ startedServer = $false } | ConvertTo-Json | Set-Content $stateFile

    Start-Process 'steam://rungameid/250820'
    Wait-Until { Test-Running '^vrserver$' } 90 'SteamVR to start'
    Start-Sleep -Seconds 12

    $vrLog = 'C:\Program Files (x86)\Steam\logs\vrserver.txt'
    $loaded = Select-String -Path $vrLog -Pattern "Driver 'null' finished adding tracked device with serial number 'CTRL2Serial'" -Quiet
    if (-not $loaded) { throw "SteamVR is up, but the null driver did not add its controllers (see $vrLog). Is the built driver_null.dll still in place?" }

    if (-not $NoConnect -and -not (Test-Running '^SkyrimTogetherServer$')) {
        Start-Process -FilePath (Join-Path $release 'SkyrimTogetherServer.exe') -WorkingDirectory $release -WindowStyle Minimized
        $startedServer = $true
        @{ startedServer = $true } | ConvertTo-Json | Set-Content $stateFile
        Start-Sleep -Seconds 4
    }

    Start-Process -FilePath $mo2 -ArgumentList '"moshortcut://:PLAY Skyrim Together VR"'
    Wait-Until { Get-Health } 180 'DevBench to answer (the game to start)'
    Wait-Until { (Invoke-Tool 'menu' @{ action = 'list' }).openMenus -contains 'Main Menu' } 180 'the main menu' -NeedsGame

    if ($Save) { Invoke-Tool 'game' @{ action = 'load'; name = $Save } | Out-Null }
    else { Invoke-Tool 'game' @{ action = 'loadLast' } | Out-Null }
    Wait-Until { $h = Get-Health; $h -and $h.lastLifecycle -eq 'postLoadGame' } $LoadTimeout 'the save to load' -NeedsGame

    if ($NoConnect) {
        Write-Host 'Up, not connected (no server was started).' -ForegroundColor Green
        Show-Status
        return
    }

    # Connected, by the client's own five-second probe line. The line count is taken after the load, so an earlier
    # session's "connected yes" cannot satisfy it.
    $before = @(Get-Content $clientLog).Count
    Wait-Until {
        $new = Get-Content $clientLog | Select-Object -Skip $before
        [bool]($new | Select-String -Pattern 'Probe: connected yes' -Quiet)
    } 90 'the client to connect' -NeedsGame

    Write-Host 'Up.' -ForegroundColor Green
    Show-Status
}
catch {
    Write-Host "Failed: $_" -ForegroundColor Red
    Write-Host 'Cleaning up.' -ForegroundColor Yellow
    Stop-Everything
    exit 1
}
