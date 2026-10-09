# Applies an urSovngarde update zip without closing Mod Organizer 2 or the server window's folder.
# Keep this next to urSovngarde.exe (update.bat does the rest). Drag the update zip onto update.bat,
# or run:  update.bat "path\to\urSovngarde-...-update.zip"
#
# Client files (urSovngarde.exe, urSovngarde.pdb) go into this folder. Server files (urSovngardeServer.exe, its
# .manifest, STServer.dll) go into the folder that holds the server: this one, a "Server" or "urSovngarde Server"
# folder next to it, or -ServerFolder.
#
# The programs were renamed on 2026-10-09: SkyrimTogetherVR.exe and SkyrimTogetherServer.exe before. The update zip
# of that release also carries the new build under the old names, for an older update.bat and for an MO2 executable
# that still points at an old name. Here a file under an old name only replaces one that is already there; it is
# never added to a folder that does not have it.
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
if (Get-Process urSovngarde, SkyrimTogetherVR, SkyrimVR -ErrorAction SilentlyContinue) {
    Write-Host "The game is running. Close it, then run the update again."
    exit 1
}

$serverExes = 'urSovngardeServer.exe', 'SkyrimTogetherServer.exe'
if ($ServerFolder -eq '') {
    $parent = Split-Path $ClientFolder -Parent
    foreach ($candidate in $ClientFolder, (Join-Path $ClientFolder 'Server'), (Join-Path $parent 'Server'), (Join-Path $parent 'urSovngarde Server')) {
        if ($serverExes | Where-Object { Test-Path (Join-Path $candidate $_) }) { $ServerFolder = $candidate; break }
    }
}

$clientFiles = 'urSovngarde.exe', 'urSovngarde.pdb'
$legacyClientFiles = 'SkyrimTogetherVR.exe', 'SkyrimTogetherVR.pdb'
$serverFiles = 'urSovngardeServer.exe', 'urSovngardeServer.exe.manifest', 'STServer.dll'
$legacyServerFiles = 'SkyrimTogetherServer.exe', 'SkyrimTogetherServer.exe.manifest'

$temp = Join-Path $env:TEMP ("urs-update-" + [guid]::NewGuid().ToString('N'))
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
    $legacy = ($legacyClientFiles + $legacyServerFiles) -contains $file.Name
    if ($clientFiles -contains $file.Name -or $legacyClientFiles -contains $file.Name) {
        if ($legacy -and -not (Test-Path (Join-Path $ClientFolder $file.Name))) { continue }
        Replace-File $file.FullName $ClientFolder
        $done++
    } elseif ($serverFiles -contains $file.Name -or $legacyServerFiles -contains $file.Name) {
        if ($ServerFolder -eq '') {
            if (-not $legacy) { Write-Host "  $($file.Name) is in the zip but no server folder was found; skipped (pass -ServerFolder if you host)." }
            continue
        }
        if ($legacy -and -not (Test-Path (Join-Path $ServerFolder $file.Name))) { continue }
        if (Get-Process urSovngardeServer, SkyrimTogetherServer -ErrorAction SilentlyContinue) {
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
if (Test-Path (Join-Path $ClientFolder 'SkyrimTogetherVR.exe')) {
    Write-Host "Note: this folder still has SkyrimTogetherVR.exe, the program's old name, now holding the new build. Point your shortcut or MO2 executable at urSovngarde.exe when convenient."
}
Write-Host "Update done: $done file(s)."
