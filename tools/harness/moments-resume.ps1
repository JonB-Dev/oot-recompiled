# moments-resume.ps1: prove a saved moment is resumed (phase 76).
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\moments-resume.ps1 [-From 3] [-Into 4]
#
# Needs a moment in slot -From (moments.ps1 writes one). Starts the game with --moment-load From
# and --moment-save Into, walks the intro into a file, and waits for MOMENT_RESUMED then
# MOMENT_WRITTEN: the resume runs first on a frame, its transition makes the capture wait, and the
# capture lands on the far side, which is the resumed spot. Then both slots are dumped and must
# agree on entrance, scene, room and position (within a unit: the player settles onto the floor).
# Then two damaged copies of the moment are dumped and must be refused. Prints MOMENTS_RESUME_OK or
# MOMENTS_RESUME_FAIL <why>. Refuses to start while the program is running.
param(
    [int]$From = 3,
    [int]$Into = 4,
    [int]$WaitSeconds = 75
)
. (Join-Path $PSScriptRoot 'drive.ps1')

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "MOMENTS_RESUME_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}

$tag = "moments-resume"
$trace = Join-Path $script:HarnessOut "$tag.stderr.txt"
$momentsDir = Join-Path $script:HarnessRoot "build-cmake\saves\moments"
$fromFile = Join-Path $momentsDir ("{0}.moment" -f $From)
$intoFile = Join-Path $momentsDir ("{0}.moment" -f $Into)
if (-not (Test-Path $fromFile)) { Write-Output "MOMENTS_RESUME_FAIL no moment in slot $From; run moments.ps1 -Slot $From first"; exit 1 }
if (Test-Path $intoFile) { Remove-Item $intoFile -Force }

function Dump-Moment([string]$file) {
    $out = Join-Path $script:HarnessOut ("$tag.dump-" + [IO.Path]::GetFileNameWithoutExtension($file) + ".txt")
    $d = Start-Process -FilePath $script:HarnessExe -ArgumentList @('--moment-dump', ('"' + $file + '"')) `
            -RedirectStandardOutput $out -RedirectStandardError (Join-Path $script:HarnessOut "$tag.dump.stderr.txt") -Wait -PassThru
    return @{ code = $d.ExitCode; lines = (Get-Content $out) }
}
function Field([string[]]$lines, [string]$name) {
    $l = $lines | Where-Object { $_ -match ('^\s+' + $name + '\s+(.+)$') } | Select-Object -First 1
    if ($l -match ('^\s+' + $name + '\s+(.+)$')) { return $Matches[1].Trim() }
    return ""
}

$p = Start-Game $tag @('--moment-load', "$From", '--moment-save', "$Into")
Write-Host "pid $($p.Id), resuming slot $From, capturing the result into slot $Into"
Enter-Play

$resumed = $false; $written = $false; $refused = ""
for ($i = 0; $i -lt ($WaitSeconds * 2); $i++) {
    if ($p.HasExited) { $refused = "the game exited"; break }
    $lines = Get-Content $trace -ErrorAction SilentlyContinue
    if ($lines | Where-Object { $_ -match '^MOMENT_RESUMED' }) { $resumed = $true }
    if ($lines | Where-Object { $_ -match "^MOMENT_WRITTEN $Into " }) { $written = $true }
    $r = $lines | Where-Object { $_ -match '^MOMENT_REFUSED' } | Select-Object -Last 1
    if ($r) { $refused = $r; break }
    if ($resumed -and $written) { break }
    Start-Sleep -Milliseconds 500
}
(Get-Content $trace -ErrorAction SilentlyContinue) | Where-Object { $_ -match '^\[moments\]|^MOMENT_' } | ForEach-Object { Write-Host "  $_" }
if (-not $p.HasExited) { if (-not (Close-Game $p.Id)) { Stop-Process -Id $p.Id -Force } }

if ($refused -ne "") { Write-Output "MOMENTS_RESUME_FAIL $refused"; exit 1 }
if (-not $resumed) { Write-Output "MOMENTS_RESUME_FAIL no MOMENT_RESUMED in $WaitSeconds s"; exit 1 }
if (-not $written) { Write-Output "MOMENTS_RESUME_FAIL the capture after the resume did not land"; exit 1 }

$a = Dump-Moment $fromFile
$b = Dump-Moment $intoFile
if ($a.code -ne 0 -or $b.code -ne 0) { Write-Output "MOMENTS_RESUME_FAIL a dump refused a file"; exit 1 }
foreach ($name in @('entrance', 'scene', 'room')) {
    $x = Field $a.lines $name; $y = Field $b.lines $name
    Write-Host ("  {0,-9} {1,-12} -> {2}" -f $name, $x, $y)
    if ($x -ne $y) { Write-Output "MOMENTS_RESUME_FAIL $name differs: $x then $y"; exit 1 }
}
$pa = (Field $a.lines 'position') -split '\s+' | ForEach-Object { [double]$_ }
$pb = (Field $b.lines 'position') -split '\s+' | ForEach-Object { [double]$_ }
Write-Host ("  position  {0} -> {1}" -f (Field $a.lines 'position'), (Field $b.lines 'position'))
for ($k = 0; $k -lt 3; $k++) {
    if ([Math]::Abs($pa[$k] - $pb[$k]) -gt 1.0) { Write-Output "MOMENTS_RESUME_FAIL the player did not land on the spot"; exit 1 }
}

# Damaged copies are refused, and say why.
$bytes = [IO.File]::ReadAllBytes($fromFile)
$short = Join-Path $momentsDir "damaged-short.moment"
[IO.File]::WriteAllBytes($short, $bytes[0..($bytes.Length - 100)])
$flipped = Join-Path $momentsDir "damaged-flipped.moment"
$copy = [byte[]]$bytes.Clone(); $copy[0x200] = $copy[0x200] -bxor 0xFF
[IO.File]::WriteAllBytes($flipped, $copy)
foreach ($f in @($short, $flipped)) {
    $d = Dump-Moment $f
    $line = $d.lines | Where-Object { $_ -match '^MOMENT_REFUSED' } | Select-Object -First 1
    Write-Host "  $([IO.Path]::GetFileName($f)): $line"
    Remove-Item $f -Force
    if ($d.code -eq 0 -or -not $line) { Write-Output "MOMENTS_RESUME_FAIL a damaged file was accepted"; exit 1 }
}
Write-Output "MOMENTS_RESUME_OK slot $From resumed, matched by slot $Into, damaged files refused"
exit 0
