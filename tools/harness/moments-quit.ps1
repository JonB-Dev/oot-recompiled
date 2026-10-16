# moments-quit.ps1: prove Save on quit and Resume on start (phase 78b).
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\moments-quit.ps1
#
# Both rows on for the run. First launch: an open scene through a queued warp, a walk, then the
# window's close button; the trace must show the quit waiting, MOMENT_AUTOSAVED, and the process
# gone. Second launch: the intro into a file; the trace must show the newest moment resumed on
# start and a capture on the far side (--moment-save 8,600) equal to the autosave in entrance,
# scene, room and position. The settings file is put back afterward. Prints MOMENTS_QUIT_OK or
# MOMENTS_QUIT_FAIL <why>. Refuses to start while the program is running.
param(
    [string]$Entrance = "0x00CD"
)
. (Join-Path $PSScriptRoot 'drive.ps1')

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "MOMENTS_QUIT_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}

$momentsDir = Join-Path $script:HarnessRoot "build-cmake\saves\moments"
$autoFile = Join-Path $momentsDir "auto.moment"
$eightFile = Join-Path $momentsDir "8.moment"
foreach ($f in @($autoFile, $eightFile)) { if (Test-Path $f) { Remove-Item $f -Force } }

function Dump-Moment([string]$file, [string]$name) {
    $out = Join-Path $script:HarnessOut "moments-quit.dump-$name.txt"
    $d = Start-Process -FilePath $script:HarnessExe -ArgumentList @('--moment-dump', ('"' + $file + '"')) `
            -RedirectStandardOutput $out -RedirectStandardError (Join-Path $script:HarnessOut "moments-quit.dump.stderr.txt") -Wait -PassThru
    return @{ code = $d.ExitCode; lines = (Get-Content $out) }
}
function Field([string[]]$lines, [string]$name) {
    $l = $lines | Where-Object { $_ -match ('^\s+' + $name + '\s+(.+)$') } | Select-Object -First 1
    if ($l -match ('^\s+' + $name + '\s+(.+)$')) { return $Matches[1].Trim() }
    return ""
}
function Lines([string]$tag) { return (Get-Content (Join-Path $script:HarnessOut "$tag.stderr.txt") -ErrorAction SilentlyContinue) }

$saved = Use-Settings "saveonquit=1,resumeonstart=1"
try {
    # Run one: play, walk, close the window.
    $p = Start-Game "moments-quit-1" @('--warp', "$Entrance,$Entrance", '--dwell', '100')
    Write-Host "pid $($p.Id), run one: close saves"
    Enter-Play
    Start-Sleep -Seconds 10
    Hold 'SUP' 2000
    Start-Sleep -Seconds 1
    $closed = Close-Game $p.Id 20
    (Lines "moments-quit-1") | Where-Object { $_ -match '^\[moments\]|^MOMENT_|quit' } | ForEach-Object { Write-Host "  $_" }
    if (-not $closed) { Stop-Process -Id $p.Id -Force; Write-Output "MOMENTS_QUIT_FAIL the program did not close within twenty seconds"; exit 1 }
    if (-not ((Lines "moments-quit-1") | Where-Object { $_ -match '^MOMENT_AUTOSAVED' })) { Write-Output "MOMENTS_QUIT_FAIL closing did not keep a moment"; exit 1 }
    if (-not (Test-Path $autoFile)) { Write-Output "MOMENTS_QUIT_FAIL auto.moment is not there"; exit 1 }

    # Run two: start, and the newest moment resumes on its own; a capture proves where.
    $p = Start-Game "moments-quit-2" @('--moment-save', '8,600')
    Write-Host "pid $($p.Id), run two: start resumes"
    Enter-Play
    $ok = $false
    for ($i = 0; $i -lt 150; $i++) {
        if ($p.HasExited) { break }
        $l = Lines "moments-quit-2"
        if (($l | Where-Object { $_ -match '^MOMENT_RESUMED' }) -and ($l | Where-Object { $_ -match '^MOMENT_WRITTEN 8 ' })) { $ok = $true; break }
        Start-Sleep -Milliseconds 500
    }
    (Lines "moments-quit-2") | Where-Object { $_ -match '^\[moments\]|^MOMENT_' } | ForEach-Object { Write-Host "  $_" }
    if (-not $p.HasExited) { if (-not (Close-Game $p.Id)) { Stop-Process -Id $p.Id -Force } }
    if (-not $ok) { Write-Output "MOMENTS_QUIT_FAIL the start did not resume, or the capture after it did not land"; exit 1 }

    $a = Dump-Moment $autoFile "auto"
    $b = Dump-Moment $eightFile "eight"
    if ($a.code -ne 0 -or $b.code -ne 0) { Write-Output "MOMENTS_QUIT_FAIL a dump refused a file"; exit 1 }
    foreach ($name in @('entrance', 'scene', 'room')) {
        $x = Field $a.lines $name; $y = Field $b.lines $name
        Write-Host ("  {0,-9} {1,-12} -> {2}" -f $name, $x, $y)
        if ($x -ne $y) { Write-Output "MOMENTS_QUIT_FAIL $name differs: $x then $y"; exit 1 }
    }
    $pa = (Field $a.lines 'position') -split '\s+' | ForEach-Object { [double]$_ }
    $pb = (Field $b.lines 'position') -split '\s+' | ForEach-Object { [double]$_ }
    Write-Host ("  position  {0} -> {1}" -f (Field $a.lines 'position'), (Field $b.lines 'position'))
    for ($k = 0; $k -lt 3; $k++) {
        if ([Math]::Abs($pa[$k] - $pb[$k]) -gt 1.0) { Write-Output "MOMENTS_QUIT_FAIL the player did not land on the spot"; exit 1 }
    }
    Write-Output "MOMENTS_QUIT_OK closing kept a moment, starting resumed it, the spot matched"
    exit 0
}
finally {
    Restore-Settings $saved
}
