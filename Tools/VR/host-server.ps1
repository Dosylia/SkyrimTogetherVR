# Starts the urSovngarde server and prints the address to give to friends.
# Keep this next to urSovngardeServer.exe (host-server.bat does the rest), or pass -ServerFolder.
param(
    [string]$ServerFolder = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

# urSovngardeServer.exe since the rename of 2026-10-09; a folder not updated since still has SkyrimTogetherServer.exe.
$exe = 'urSovngardeServer.exe', 'SkyrimTogetherServer.exe' | ForEach-Object { Join-Path $ServerFolder $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) {
    Write-Host "urSovngardeServer.exe not found in $ServerFolder"
    exit 1
}

if (Get-Process urSovngardeServer, SkyrimTogetherServer -ErrorAction SilentlyContinue) {
    Write-Host "A server is already running. Close it first: two servers can't share the port."
    exit 1
}

$port = 10578
$ini = Join-Path $ServerFolder 'config\STServer.ini'
if (Test-Path $ini) {
    $match = Select-String -Path $ini -Pattern '^\s*uPort\s*=\s*(\d+)' | Select-Object -First 1
    if ($match) { $port = [int]$match.Matches[0].Groups[1].Value }
}

# The server's working directory decides where config\ and logs\ are.
$process = Start-Process -FilePath $exe -WorkingDirectory $ServerFolder -PassThru
# Below normal priority: the host also plays in VR on this PC, and everyone's sync follows the host's frame rate.
# The server needs little CPU; when both want the same core, the game wins.
try { $process.PriorityClass = 'BelowNormal' } catch { Write-Host "Could not lower the server priority: $_" }

Write-Host "Server started on UDP port $port."
Write-Host ""
Write-Host "Your own connect.txt:  127.0.0.1:$port"
try {
    $public = (Invoke-RestMethod -Uri 'https://api.ipify.org' -TimeoutSec 5).Trim()
    Write-Host "Give your friends:     $public`:$port"
} catch {
    Write-Host "Couldn't look up your public IP; open https://api.ipify.org in a browser."
}
Write-Host ""
Write-Host "Friends from outside your home network need UDP $port forwarded to this PC on the router,"
Write-Host "and allowed in Windows Firewall (see VR_MULTIPLAYER_GUIDE.md)."
