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

# A release built from a clean, tagged tree is the standalone package anyone can install; "dirty" in the
# version means uncommitted changes, and such a build must not be handed out.
if ($version -like '*dirty*') { throw "The build is from an uncommitted tree ($version); commit and build again before releasing." }
$name = "SkyrimTogetherVR-$version"
$standaloneName = "SkyrimTogetherVR-standalone-$version"
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
foreach ($readme in 'README-mod-manager.md', 'README-manual.md', 'README-host.md') {
    Copy-Item (Join-Path $PSScriptRoot $readme) $staging
}

$zip = Join-Path $OutputFolder "$standaloneName.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip

# Update pack: what someone who already installed a build needs. Small enough to attach to a message,
# unlike the full zip. The pdb goes with it, or crash logs from the new exe name the wrong functions.
$updateZip = Join-Path $OutputFolder "$name-update.zip"
if (Test-Path $updateZip) { Remove-Item $updateZip -Force }
Compress-Archive -Path (Join-Path $buildFolder 'SkyrimTogetherVR.exe'), (Join-Path $buildFolder 'SkyrimTogetherVR.pdb') -DestinationPath $updateZip

# Server update pack, for whoever hosts: the three files that change, without the 165 MB full zip.
$serverZip = Join-Path $OutputFolder "$name-server-update.zip"
if (Test-Path $serverZip) { Remove-Item $serverZip -Force }
Compress-Archive -Path (Join-Path $server '*') -DestinationPath $serverZip

Write-Host ("Standalone:    {0} ({1:N0} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
Write-Host ("Client update: {0} ({1:N1} MB)" -f $updateZip, ((Get-Item $updateZip).Length / 1MB))
Write-Host ("Server update: {0} ({1:N1} MB)" -f $serverZip, ((Get-Item $serverZip).Length / 1MB))
