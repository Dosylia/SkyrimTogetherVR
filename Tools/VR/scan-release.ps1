# Scans a release on VirusTotal before it goes to Nexus or GitHub, and writes what to publish with it: the files'
# SHA-256 checksums and their VirusTotal links. Run it after make-release.ps1.
#   powershell -ExecutionPolicy Bypass -File Tools\VR\scan-release.ps1
#   ... -Path a.zip,b.exe   scans those files instead (the launcher's installer, a friend's build)
#   ... -LookupOnly         uploads nothing; reports only what VirusTotal already knows of each file
#
# Why (2026-10-09): Nexus quarantined the first upload, and VirusTotal showed a few guessing engines (Rising,
# Trapmine, MaxSecure) flagging it. A flag found here, before publishing, can be reported as a false positive while
# nobody is waiting on the file; the report this writes lists where to send each one.
#
# The key: a free VirusTotal account's API key (virustotal.com, profile menu, API key), saved as the only text of
# %USERPROFILE%\.str-vt-key. It is read from there and never printed; it never goes in the repo. A free key allows
# 4 calls a minute, so a whole release (15 files) takes 15 to 25 minutes.
#
# Uploading shares a file with VirusTotal's partners and its paying users: only for files that will be public anyway.
param(
    [string[]]$Path,
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$ReleaseFolder = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build\release'),
    [string]$KeyFile = (Join-Path $env:USERPROFILE '.str-vt-key'),
    [string]$ApiBase = 'https://www.virustotal.com/api/v3',
    [double]$SecondsBetweenCalls = 15.5,
    [int]$WaitMinutes = 30,
    [switch]$LookupOnly
)

$ErrorActionPreference = 'Stop'

# What to scan: the three zips make-release.ps1 wrote, our four programs from inside them, because a vendor's
# false-positive form wants the very file it flagged, and the helper scripts one by one: a zip's result does not say
# which file inside it was flagged, and the .bat files start PowerShell with "-ExecutionPolicy Bypass", a pattern some
# engines distrust (2026-10-09: no scan had looked at them alone yet).
$label = $null
# Through powershell -File, "-Path a.zip,b.exe" arrives as one string.
$Path = @($Path | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Path) {
    $versionFile = Join-Path $RepoRoot 'build\BuildVersion.txt'
    if (-not (Test-Path $versionFile)) { throw "No build version in $versionFile; build and run make-release.ps1 first." }
    $label = "urSovngarde-$((Get-Content $versionFile -Raw).Trim())"
    $staging = Join-Path $ReleaseFolder $label
    $Path = @(
        (Join-Path $ReleaseFolder "$label.zip"),
        (Join-Path $ReleaseFolder "$label-update.zip"),
        (Join-Path $ReleaseFolder "$label-server.zip"),
        (Join-Path $staging 'urSovngarde\urSovngarde.exe'),
        (Join-Path $staging 'urSovngarde\TPProcess.exe'),
        (Join-Path $staging 'Server\urSovngardeServer.exe'),
        (Join-Path $staging 'Server\STServer.dll'))
    foreach ($helper in 'collect-logs', 'setup-connect', 'update') {
        $Path += Join-Path $staging "urSovngarde\$helper.bat"
        $Path += Join-Path $staging "urSovngarde\$helper.ps1"
    }
    $Path += Join-Path $staging 'Server\host-server.bat'
    $Path += Join-Path $staging 'Server\host-server.ps1'
    $missing = @($Path | Where-Object { -not (Test-Path $_) })
    if ($missing) { throw ("Not found (run make-release.ps1 for this build first):`n  " + ($missing -join "`n  ")) }
}
$files = @($Path | ForEach-Object { Get-Item $_ })
if (-not $label) { $label = 'files-' + (Get-Date -Format 'yyyy-MM-dd_HH-mm') }

if (-not (Test-Path $KeyFile)) {
    throw "No VirusTotal key. Make a free account at virustotal.com, copy the API key from your profile menu, and save it as the only text of $KeyFile."
}
$key = (Get-Content $KeyFile -Raw).Trim()

Add-Type -AssemblyName System.Net.Http
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromMinutes(30)
$client.DefaultRequestHeaders.Add('x-apikey', $key)
$client.DefaultRequestHeaders.ExpectContinue = $false

$script:lastCall = [datetime]::MinValue

# One call to VirusTotal, spaced to the key's quota. Returns the answer as an object, or $null for "not known".
function Send-Vt([string]$Method, [string]$Url, [string]$Upload) {
    for ($try = 1; $try -le 5; $try++) {
        $wait = $SecondsBetweenCalls - ((Get-Date) - $script:lastCall).TotalSeconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }

        $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($Method), $Url)
        $stream = $null
        if ($Upload) {
            $stream = [IO.File]::OpenRead($Upload)
            $part = [System.Net.Http.StreamContent]::new($stream)
            $part.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
            $part.Headers.ContentDisposition = [System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data')
            $part.Headers.ContentDisposition.Name = '"file"'
            $part.Headers.ContentDisposition.FileName = '"' + [IO.Path]::GetFileName($Upload) + '"'
            $form = [System.Net.Http.MultipartFormDataContent]::new()
            $form.Add($part)
            $request.Content = $form
        }
        try {
            $response = $client.SendAsync($request).GetAwaiter().GetResult()
            $text = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        } finally {
            $script:lastCall = Get-Date
            if ($stream) { $stream.Dispose() }
            $request.Dispose()
        }

        $code = [int]$response.StatusCode
        if ($code -eq 429) {
            Write-Host '  VirusTotal asks to slow down; waiting a minute.'
            Start-Sleep -Seconds 60
            continue
        }
        if ($code -eq 404) { return $null }
        if ($code -eq 401 -or $code -eq 403) { throw "VirusTotal refused the key in $KeyFile (HTTP $code)." }
        if ($code -ge 400) { throw "VirusTotal answered HTTP $code to $Method $Url`: $text" }
        return ($text | ConvertFrom-Json)
    }
    throw 'VirusTotal kept asking to slow down: the daily quota may be spent. Try again tomorrow.'
}

# A finished scan as counts and the engines that flagged it.
function Read-Verdict($Stats, $Results) {
    $flags = @()
    if ($Results) {
        $flags = @($Results.PSObject.Properties | ForEach-Object { $_.Value } |
            Where-Object { $_.category -eq 'malicious' -or $_.category -eq 'suspicious' } |
            ForEach-Object { '{0} ({1}: {2})' -f $_.engine_name, $_.category, $_.result })
    }
    [pscustomobject]@{
        Flagged = [int]$Stats.malicious + [int]$Stats.suspicious
        Engines = [int]$Stats.malicious + [int]$Stats.suspicious + [int]$Stats.undetected + [int]$Stats.harmless
        Flags   = $flags
    }
}

$items = foreach ($f in $files) {
    [pscustomobject]@{
        Name = $f.Name; FullName = $f.FullName; Size = $f.Length
        Sha256 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        Analysis = $null; Verdict = $null; State = ''
    }
}

Write-Host ("Looking up {0} files on VirusTotal ({1:N0} seconds between calls)..." -f $items.Count, $SecondsBetweenCalls)
foreach ($item in $items) {
    $known = Send-Vt GET "$ApiBase/files/$($item.Sha256)"
    if ($known -and $known.data.attributes.last_analysis_stats -and $known.data.attributes.last_analysis_date) {
        $item.Verdict = Read-Verdict $known.data.attributes.last_analysis_stats $known.data.attributes.last_analysis_results
        $item.State = 'known'
        Write-Host ("  {0}: already scanned, {1} of {2} engines flag it" -f $item.Name, $item.Verdict.Flagged, $item.Verdict.Engines)
    } elseif ($LookupOnly) {
        $item.State = 'unknown'
        Write-Host ("  {0}: not known to VirusTotal (not uploaded: -LookupOnly)" -f $item.Name)
    } else {
        # Up to 32 MB goes straight to /files; bigger files to a one-time address asked for first (650 MB at most).
        $target = "$ApiBase/files"
        if ($item.Size -gt 32MB) { $target = (Send-Vt GET "$ApiBase/files/upload_url").data }
        Write-Host ("  {0}: uploading {1:N1} MB..." -f $item.Name, ($item.Size / 1MB))
        $item.Analysis = (Send-Vt POST $target $item.FullName).data.id
        $item.State = 'uploaded'
    }
}

# Each upload is scanned by every engine in a few minutes; ask until all are done, or until -WaitMinutes passes.
$deadline = (Get-Date).AddMinutes($WaitMinutes)
$pending = @($items | Where-Object { $_.Analysis })
if ($pending) { Write-Host 'Waiting for the scans to finish...' }
while ($pending -and (Get-Date) -lt $deadline) {
    foreach ($item in $pending) {
        $analysis = Send-Vt GET "$ApiBase/analyses/$($item.Analysis)"
        if ($analysis -and $analysis.data.attributes.status -eq 'completed') {
            $item.Verdict = Read-Verdict $analysis.data.attributes.stats $analysis.data.attributes.results
            $item.State = 'scanned'
            Write-Host ("  {0}: {1} of {2} engines flag it" -f $item.Name, $item.Verdict.Flagged, $item.Verdict.Engines)
        }
    }
    $pending = @($pending | Where-Object { -not $_.Verdict })
}
foreach ($item in $pending) { $item.State = 'still scanning' }

# What to publish with the release, and what to do about a flag.
$report = New-Object System.Collections.Generic.List[string]
$report.Add("$label, scanned on VirusTotal on $(Get-Date -Format 'yyyy-MM-dd HH:mm')")
$report.Add('')
$report.Add('SHA-256 checksums (for the Nexus description and the GitHub release):')
foreach ($item in $items) { $report.Add("$($item.Sha256)  $($item.Name)") }
$report.Add('')
$report.Add('VirusTotal:')
foreach ($item in $items) {
    $result = if ($item.Verdict) { '{0} of {1} engines flag it' -f $item.Verdict.Flagged, $item.Verdict.Engines } else { $item.State }
    $report.Add(('{0}: {1}, https://www.virustotal.com/gui/file/{2}' -f $item.Name, $result, $item.Sha256))
}
$flagged = @($items | Where-Object { $_.Verdict -and $_.Verdict.Flagged -gt 0 })
if ($flagged) {
    $report.Add('')
    $report.Add('Flagged by:')
    foreach ($item in $flagged) { $report.Add("$($item.Name): $($item.Verdict.Flags -join ', ')") }
    $report.Add('')
    $report.Add('Reporting a false positive (give the VirusTotal link of the flagged file each time):')
    $report.Add('  Microsoft Defender: https://www.microsoft.com/en-us/wdsi/filesubmission, as a software developer, "incorrectly detected".')
    $report.Add('  Any other engine: its vendor''s contact in https://docs.virustotal.com/docs/false-positive-contacts')
    $report.Add('  Then wait for the answers before publishing, or publish and tell Nexus which engines were told.')
}

$reportFile = Join-Path $ReleaseFolder "$label-scan.txt"
New-Item -ItemType Directory -Force $ReleaseFolder | Out-Null
$report | Set-Content -Encoding UTF8 $reportFile
Write-Host ''
$report | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host "Written to $reportFile"

$client.Dispose()
if ($flagged) { exit 1 }
if (@($items | Where-Object { -not $_.Verdict }).Count) { exit 2 }
