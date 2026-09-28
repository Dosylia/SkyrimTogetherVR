# Copy the freshly built VR client into the folder the game actually runs from.
#
#   powershell -ExecutionPolicy Bypass -File Tools\VR\deploy-client.ps1
#
# Written 2026-09-26 after a deploy that verified nothing. The build of 14:29 produced an exe of exactly
# 2,097,152 bytes -- a truncated link, three binaries hit at once -- and the deploy "passed" because it compared
# the destination's hash against the source's, and a truncated file copies perfectly and matches itself. MO2 then
# refused to start the game with ERROR_FILE_CORRUPT and it looked like a mod problem.
#
# A hash proves the copy worked. It says nothing about whether the thing copied is a program. So the size is
# checked against what a real build of this target looks like, and the version string has to be readable inside
# the binary -- a truncated exe has neither.

param(
    [string]$Destination = 'E:\FUS\tools\Skyrim Together VR',
    [int]$MinimumBytes = 5000000
)

$ErrorActionPreference = 'Stop'

$release = (Resolve-Path (Join-Path $PSScriptRoot '..\..\build\windows\x64\release')).Path
$exe = Join-Path $release 'SkyrimTogetherVR.exe'
$pdb = Join-Path $release 'SkyrimTogetherVR.pdb'

foreach ($f in @($exe, $pdb)) {
    if (-not (Test-Path $f)) { Write-Host "Missing $f -- build first" -ForegroundColor Red; exit 2 }
}

$size = (Get-Item $exe).Length
if ($size -lt $MinimumBytes) {
    Write-Host "REFUSING: $exe is $size bytes. A real build of this target is about 6.4 MB." -ForegroundColor Red
    Write-Host "  That is a truncated link, not a small build. Delete it, build again, and check the size before deploying." -ForegroundColor Yellow
    Write-Host "  Windows Defender real-time scanning of the build folder is the leading suspect for this." -ForegroundColor Yellow
    exit 1
}

# The version is a hash of the uncommitted diff, so it is also the check that everything was built together.
# Read as raw bytes: Select-String cannot scan a binary in Windows PowerShell 5.1.
function Get-BuildVersion($path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    $m = [regex]::Match($text, 'v\d+\.\d+\.\d+-\d+-g[0-9a-f]+(-dirty\.[0-9a-f]+)?')
    if ($m.Success) { return $m.Value }
    return $null
}

$version = Get-BuildVersion $exe
if (-not $version) {
    Write-Host "REFUSING: no version string found inside $exe. It is not a complete binary." -ForegroundColor Red
    exit 1
}

foreach ($other in @('STServer.dll', 'STBot.exe')) {
    $p = Join-Path $release $other
    if (-not (Test-Path $p)) { continue }
    $v = Get-BuildVersion $p
    if ($v -and $v -ne $version) {
        Write-Host "REFUSING: $other is $v but the client is $version. Build them in one go." -ForegroundColor Red
        exit 1
    }
}

if (-not (Test-Path $Destination)) { Write-Host "No such folder: $Destination" -ForegroundColor Red; exit 2 }

# Exactly one fallback is kept, so the folder never fills with old builds.
Get-ChildItem -Path $Destination -Filter 'SkyrimTogetherVR.*.old-*' -ErrorAction SilentlyContinue | Remove-Item -Force
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
foreach ($name in @('SkyrimTogetherVR.exe', 'SkyrimTogetherVR.pdb')) {
    $live = Join-Path $Destination $name
    if (Test-Path $live) { Move-Item $live "$live.old-$stamp" -Force }
}

Copy-Item $exe (Join-Path $Destination 'SkyrimTogetherVR.exe') -Force
Copy-Item $pdb (Join-Path $Destination 'SkyrimTogetherVR.pdb') -Force

$deployed = Join-Path $Destination 'SkyrimTogetherVR.exe'
$deployedSize = (Get-Item $deployed).Length
$srcHash = (Get-FileHash $exe -Algorithm SHA256).Hash
$dstHash = (Get-FileHash $deployed -Algorithm SHA256).Hash

if ($deployedSize -ne $size -or $srcHash -ne $dstHash) {
    Write-Host "The copy did not land intact: $size -> $deployedSize bytes." -ForegroundColor Red
    exit 1
}

Write-Host "Deployed $version" -ForegroundColor Green
Write-Host "  $deployedSize bytes, sha256 $($dstHash.Substring(0,16))..., one fallback kept as *.old-$stamp"
