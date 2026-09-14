# Starts the Skyrim Together server and prints the address to give to friends.
# Keep this next to SkyrimTogetherServer.exe (host-server.bat does the rest), or pass -ServerFolder.
param(
    [string]$ServerFolder = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

$exe = Join-Path $ServerFolder 'SkyrimTogetherServer.exe'
if (-not (Test-Path $exe)) {
    Write-Host "SkyrimTogetherServer.exe not found in $ServerFolder"
    exit 1
}

if (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue) {
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
Start-Process -FilePath $exe -WorkingDirectory $ServerFolder

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
