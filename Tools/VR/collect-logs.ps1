# Zips everything needed for a Skyrim Together VR bug report onto the Desktop.
# Run it from the "Skyrim Together VR" tools folder (collect-logs.bat does that), or pass -ClientFolder.
param(
    [string]$ClientFolder = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

$stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm'
$staging = Join-Path $env:TEMP "SkyrimTogetherVR-logs-$stamp"
$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "SkyrimTogetherVR-logs-$stamp.zip"
New-Item -ItemType Directory -Force $staging | Out-Null

function Add-File([string]$Path, [string]$Name = (Split-Path $Path -Leaf)) {
    if (Test-Path $Path) {
        Copy-Item $Path (Join-Path $staging $Name)
        Write-Host "  + $Name"
    }
}

Write-Host "Collecting Skyrim Together VR logs..."

# Client logs (the rotated ones hold earlier sessions of the same day).
Get-ChildItem (Join-Path $ClientFolder 'logs') -Filter 'tp_client*.log' -ErrorAction SilentlyContinue | ForEach-Object { Add-File $_.FullName }
Add-File (Join-Path $ClientFolder 'logs\cef_debug.log')

# Newest SKSE crash log (Crash Logger), if one was written in the last day.
$skse = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim VR\SKSE'
Get-ChildItem $skse -Filter 'crash-*.log' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt (Get-Date).AddDays(-1) } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 |
    ForEach-Object { Add-File $_.FullName }

# Newest crash dump from the last day. They are large, but compress well.
$dumps = @(Get-ChildItem $ClientFolder -Filter 'crash_*.dmp' -ErrorAction SilentlyContinue)
$overwrite = Join-Path (Split-Path (Split-Path $ClientFolder -Parent) -Parent) 'overwrite\Root'
$dumps += @(Get-ChildItem $overwrite -Filter 'crash_*.dmp' -ErrorAction SilentlyContinue)
$dumps | Where-Object { $_.LastWriteTime -gt (Get-Date).AddDays(-1) } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 |
    ForEach-Object { Add-File $_.FullName }

# Build of the client, from the first log line that names it.
$log = Join-Path $ClientFolder 'logs\tp_client.log'
if (Test-Path $log) {
    $build = Select-String -Path $log -Pattern 'Skyrim Together client, build (.+)$' | Select-Object -Last 1
    if ($build) { Set-Content (Join-Path $staging 'build.txt') $build.Matches[0].Groups[1].Value }
}

if (-not (Get-ChildItem $staging)) {
    Write-Host "No logs found in $ClientFolder. Run this from the Skyrim Together VR tools folder."
    Remove-Item $staging -Recurse -Force
    exit 1
}

Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip -Force
Remove-Item $staging -Recurse -Force
Write-Host ""
Write-Host "Done: $zip"
Write-Host "Send that file."
