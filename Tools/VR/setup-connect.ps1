# Writes %LOCALAPPDATA%\SkyrimTogetherVR\connect.txt, the server the game connects to on its own.
$ErrorActionPreference = 'Stop'

$folder = Join-Path $env:LOCALAPPDATA 'SkyrimTogetherVR'
$file = Join-Path $folder 'connect.txt'
New-Item -ItemType Directory -Force $folder | Out-Null

if (Test-Path $file) {
    Write-Host "Current connect.txt:"
    Get-Content $file | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
}

Write-Host "Server address. The host playing on the same PC uses 127.0.0.1:10578,"
Write-Host "everyone else uses the address the host gave (for example 86.248.47.218:10578)."
$address = (Read-Host "Address").Trim()
if ($address -notmatch ':\d+$') {
    $address = "$address`:10578"
    Write-Host "No port given, using $address"
}
$address = $address -replace '^[a-z]+://', ''

$password = (Read-Host "Server password (leave empty if there is none)").Trim()

# No byte order mark: the game reads the first line as the address.
$lines = if ($password) { "$address`r`n$password`r`n" } else { "$address`r`n" }
[System.IO.File]::WriteAllText($file, $lines, (New-Object System.Text.UTF8Encoding $false))

Write-Host ""
Write-Host "Saved $file"
Write-Host "The game connects on its own a few seconds after a save is loaded."
