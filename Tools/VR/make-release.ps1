# Packages a Skyrim Together VR release zip: client tools folder, server, game files and the guide.
#   powershell -ExecutionPolicy Bypass -File Tools\VR\make-release.ps1
param(
    [string]$ClientFolder = 'E:\FUS\tools\Skyrim Together VR',
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$OutputFolder = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build\release')
)

$ErrorActionPreference = 'Stop'

$buildFolder = Join-Path $RepoRoot 'build\windows\x64\release'
# The build writes its version string to build\BuildVersion.txt (root xmake.lua, before_build). BuildInfo.h only
# carries fallbacks since the version became a compile define.
$versionFile = Join-Path $RepoRoot 'build\BuildVersion.txt'
$version = if (Test-Path $versionFile) { (Get-Content $versionFile -Raw).Trim() } else { 'unknown' }
if (-not $version -or $version -like 'unknown*') { throw "No build version found in $versionFile; build first (xmake -y)." }

$name = "SkyrimTogetherVR-$version"
$staging = Join-Path $OutputFolder $name
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Force $staging | Out-Null

# Client: the tools folder as it runs, minus anything local to one PC. The exe comes from the build, so a
# release never ships a stale client.
$client = Join-Path $staging 'Skyrim Together VR'
New-Item -ItemType Directory -Force $client | Out-Null
$skip = @('logs', 'cache')
Get-ChildItem $ClientFolder | Where-Object {
    $skip -notcontains $_.Name -and
    $_.Name -notmatch '\.old|\.running|^crash_.*\.dmp$|\.threaddiag\.'
} | ForEach-Object { Copy-Item $_.FullName (Join-Path $client $_.Name) -Recurse }
Copy-Item (Join-Path $buildFolder 'SkyrimTogetherVR.exe') $client -Force
Copy-Item (Join-Path $buildFolder 'TPProcess.exe') $client -Force
Copy-Item (Join-Path $PSScriptRoot 'collect-logs.*') $client
Copy-Item (Join-Path $PSScriptRoot 'setup-connect.*') $client

# Server, with default settings: never the host's password.
$server = Join-Path $staging 'Server'
New-Item -ItemType Directory -Force (Join-Path $server 'config') | Out-Null
foreach ($file in 'SkyrimTogetherServer.exe', 'SkyrimTogetherServer.exe.manifest', 'STServer.dll') {
    Copy-Item (Join-Path $buildFolder $file) $server
}
(Get-Content (Join-Path $buildFolder 'config\STServer.ini')) -replace '^(sPassword|sAdminPassword)=.*$', '$1=' |
    Set-Content (Join-Path $server 'config\STServer.ini')
Copy-Item (Join-Path $PSScriptRoot 'host-server.*') $server

# Game files, installed as an MO2 mod.
Copy-Item (Join-Path $RepoRoot 'GameFiles\Skyrim') (Join-Path $staging 'Skyrim Together mod') -Recurse

# Never VR_MULTIPLAYER_GUIDE.md: it carries the host's public IP, PC name and router settings.
Copy-Item (Join-Path $PSScriptRoot 'README-release.md') (Join-Path $staging 'README.md')

$zip = Join-Path $OutputFolder "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip

# Update pack: what someone who already installed a build needs. Small enough to attach to a message,
# unlike the full zip. The pdb goes with it, or crash logs from the new exe name the wrong functions.
$updateZip = Join-Path $OutputFolder "$name-update.zip"
if (Test-Path $updateZip) { Remove-Item $updateZip -Force }
Compress-Archive -Path (Join-Path $buildFolder 'SkyrimTogetherVR.exe'), (Join-Path $buildFolder 'SkyrimTogetherVR.pdb') -DestinationPath $updateZip

Write-Host ("Full install: {0} ({1:N0} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
Write-Host ("Update only:  {0} ({1:N1} MB)" -f $updateZip, ((Get-Item $updateZip).Length / 1MB))
