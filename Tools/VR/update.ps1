# Applies a Skyrim Together VR update zip without closing Mod Organizer 2 or the server window's folder.
# Keep this next to SkyrimTogetherVR.exe (update.bat does the rest). Drag the update zip onto update.bat,
# or run:  update.bat "path\to\SkyrimTogetherVR-...-update.zip"
#
# Client files (SkyrimTogetherVR.exe, SkyrimTogetherVR.pdb) go into this folder. Server files
# (SkyrimTogetherServer.exe, its .manifest, STServer.dll) go into the folder that holds SkyrimTogetherServer.exe:
# this one, a "Server" folder next to it, or -ServerFolder.
#
# Each file is replaced by renaming the old one aside first: a file that is open (MO2 keeps handles on the
# tools folder) can still be renamed, while overwriting it fails. The old copies are removed at the end.
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [string]$ClientFolder = $PSScriptRoot,
    [string]$ServerFolder = ''
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not (Test-Path $Zip)) {
    Write-Host "Zip not found: $Zip"
    exit 1
}
if (Get-Process SkyrimTogetherVR, SkyrimVR -ErrorAction SilentlyContinue) {
    Write-Host "The game is running. Close it, then run the update again."
    exit 1
}

if ($ServerFolder -eq '') {
    foreach ($candidate in $ClientFolder, (Join-Path $ClientFolder 'Server'), (Join-Path (Split-Path $ClientFolder -Parent) 'Server')) {
        if (Test-Path (Join-Path $candidate 'SkyrimTogetherServer.exe')) { $ServerFolder = $candidate; break }
    }
}

$clientFiles = 'SkyrimTogetherVR.exe', 'SkyrimTogetherVR.pdb'
$serverFiles = 'SkyrimTogetherServer.exe', 'SkyrimTogetherServer.exe.manifest', 'STServer.dll'

$temp = Join-Path $env:TEMP ("st-update-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
[System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $temp)

function Replace-File([string]$source, [string]$folder) {
    $name = Split-Path $source -Leaf
    $target = Join-Path $folder $name
    $old = "$target.old"
    if (Test-Path $old) { Remove-Item $old -Force }
    if (Test-Path $target) {
        try { Rename-Item $target $old -ErrorAction Stop }
        catch {
            Write-Host "  $name is in use (a running game or server holds it). Close it and run the update again."
            throw "update stopped at $name"
        }
    }
    Copy-Item $source $target
    if (Test-Path $old) { Remove-Item $old -Force -ErrorAction SilentlyContinue }
    Write-Host "  replaced $name in $folder"
}

$found = Get-ChildItem $temp -Recurse -File
$done = 0
foreach ($file in $found) {
    if ($clientFiles -contains $file.Name) {
        Replace-File $file.FullName $ClientFolder
        $done++
    } elseif ($serverFiles -contains $file.Name) {
        if ($ServerFolder -eq '') {
            Write-Host "  $($file.Name) is in the zip but no server folder was found; skipped (pass -ServerFolder if you host)."
            continue
        }
        if (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue) {
            Write-Host "  the server is running; close its window and run the update again for the server files."
            break
        }
        Replace-File $file.FullName $ServerFolder
        $done++
    }
}
Remove-Item $temp -Recurse -Force
if ($done -eq 0) {
    Write-Host "Nothing in that zip matched an update file."
    exit 1
}
Write-Host "Update done: $done file(s)."
