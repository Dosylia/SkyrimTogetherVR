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
    [string]$Destination = '',
    [int]$MinimumBytes = 5000000
)

$ErrorActionPreference = 'Stop'

if (-not $Destination) {
    $fusRoot = $env:SKYRIM_FUS_ROOT
    if (-not $fusRoot) {
        $fusRoot = @('C:\FUS', 'E:\FUS') | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'ModOrganizer.exe') } | Select-Object -First 1
    }
    if (-not $fusRoot) { throw 'FUS installation not found. Set SKYRIM_FUS_ROOT or -Destination.' }
    $Destination = Join-Path $fusRoot 'tools\Skyrim Together VR'
}

$release = (Resolve-Path (Join-Path $PSScriptRoot '..\..\build\windows\x64\release')).Path
# urSovngarde.exe since the rename of 2026-10-09; SkyrimTogetherVR.exe before (an older build output may still hold it).
$exe = Join-Path $release 'urSovngarde.exe'
$pdb = Join-Path $release 'urSovngarde.pdb'

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
    # A build exactly on a tag is just "v1.9.0": git describe adds "-<commits>-g<hash>" only past one (the v1.9.0
    # release build was refused here, 2026-10-07).
    # The longest one: a binary can also hold a cut piece of it ("v1.9.0-d" in STServer.dll, 2026-10-08), whose
    # "v1.9.0" alone then looked like another build.
    $all = [regex]::Matches($text, 'v\d+\.\d+\.\d+(-\d+-g[0-9a-f]+)?(-dirty\.[0-9a-f]+)?') | ForEach-Object { $_.Value }
    if ($all) { return ($all | Sort-Object Length -Descending | Select-Object -First 1) }
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

# Exactly one fallback is kept, so the folder never fills with old builds. The old name goes aside too: left live,
# an MO2 executable still pointing at it would run the old build.
Get-ChildItem -Path $Destination -Filter '*.old-*' -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(urSovngarde|SkyrimTogetherVR)\.' } | Remove-Item -Force
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
foreach ($name in @('urSovngarde.exe', 'urSovngarde.pdb', 'SkyrimTogetherVR.exe', 'SkyrimTogetherVR.pdb')) {
    $live = Join-Path $Destination $name
    if (Test-Path $live) { Move-Item $live "$live.old-$stamp" -Force }
}

Copy-Item $exe (Join-Path $Destination 'urSovngarde.exe') -Force
Copy-Item $pdb (Join-Path $Destination 'urSovngarde.pdb') -Force

# The MO2 executable that runs the old name is pointed at the new one, MO2 closed (it writes its own settings back
# when it closes, and the launcher never closes it). Open, a copy under the old name keeps Play on the new build
# until the next deploy with MO2 closed.
$instance = Split-Path (Split-Path $Destination -Parent) -Parent
$mo2Ini = Join-Path $instance 'ModOrganizer.ini'
if (Test-Path $mo2Ini) {
    # MO2 writes "12\binary=E:/FUS/tools/Skyrim Together VR/SkyrimTogetherVR.exe": an entry for this folder's old exe.
    $oldExe = (Join-Path $Destination 'SkyrimTogetherVR.exe').ToLowerInvariant()
    $lines = [System.IO.File]::ReadAllLines($mo2Ini)
    $hits = @(for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\d+\\binary=(.+)$' -and ($Matches[1].Trim() -replace '/', '\').ToLowerInvariant() -eq $oldExe) { $i }
    })
    if ($hits.Count -gt 0) {
        if (Get-Process ModOrganizer -ErrorAction SilentlyContinue) {
            Copy-Item $exe (Join-Path $Destination 'SkyrimTogetherVR.exe') -Force
            Write-Host "MO2 is open: its executable still points at SkyrimTogetherVR.exe, a copy of this build for now. Deploy again with MO2 closed to point it at urSovngarde.exe." -ForegroundColor Yellow
        } else {
            Copy-Item $mo2Ini "$mo2Ini.bak-$stamp"
            foreach ($i in $hits) { $lines[$i] = $lines[$i] -replace 'SkyrimTogetherVR\.exe\s*$', 'urSovngarde.exe' }
            [System.IO.File]::WriteAllLines($mo2Ini, $lines, (New-Object System.Text.UTF8Encoding $false))
            Write-Host "MO2's executable now runs urSovngarde.exe (ModOrganizer.ini copied aside as .bak-$stamp)." -ForegroundColor Green
        }
    }
}

$deployed = Join-Path $Destination 'urSovngarde.exe'
$deployedSize = (Get-Item $deployed).Length
$srcHash = (Get-FileHash $exe -Algorithm SHA256).Hash
$dstHash = (Get-FileHash $deployed -Algorithm SHA256).Hash

if ($deployedSize -ne $size -or $srcHash -ne $dstHash) {
    Write-Host "The copy did not land intact: $size -> $deployedSize bytes." -ForegroundColor Red
    exit 1
}

Write-Host "Deployed $version" -ForegroundColor Green
Write-Host "  $deployedSize bytes, sha256 $($dstHash.Substring(0,16))..., one fallback kept as *.old-$stamp"
