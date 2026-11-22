# moments-autosave.ps1: prove the autosave (phase 78).
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\moments-autosave.ps1
#
# Two runs in an open scene (a queued warp past the intro). The first with the interval forced
# short for the test (--autosave-ticks 300, fifteen seconds; the row's own choices are minutes):
# MOMENT_AUTOSAVED must appear, into the autosave slot, and never during a transition. The second
# with the row Off and no override, for the same time: no MOMENT_AUTOSAVED at all. Prints
# MOMENTS_AUTOSAVE_OK or MOMENTS_AUTOSAVE_FAIL <why>. Refuses to start while the program is running.
param(
    [string]$Entrance = "0x00CD",
    [int]$WaitSeconds = 60
)
. (Join-Path $PSScriptRoot 'drive.ps1')

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "MOMENTS_AUTOSAVE_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}

$momentsDir = Join-Path $script:HarnessRoot "build-cmake\saves\moments"
$autoFile = Join-Path $momentsDir "auto.moment"
if (Test-Path $autoFile) { Remove-Item $autoFile -Force }

function Run-Once([string]$tag, [string[]]$extra, [string]$pattern, [int]$seconds) {
    $trace = Join-Path $script:HarnessOut "$tag.stderr.txt"
    $p = Start-Game $tag (@('--warp', "$Entrance,$Entrance", '--dwell', '100') + $extra)
    Write-Host "pid $($p.Id), $tag"
    Enter-Play
    $seen = $false
    for ($i = 0; $i -lt ($seconds * 2); $i++) {
        if ($p.HasExited) { break }
        $lines = Get-Content $trace -ErrorAction SilentlyContinue
        if ($lines | Where-Object { $_ -match $pattern }) { $seen = $true; break }
        Start-Sleep -Milliseconds 500
    }
    (Get-Content $trace -ErrorAction SilentlyContinue) | Where-Object { $_ -match '^\[moments\]|^MOMENT_' } | ForEach-Object { Write-Host "  $_" }
    if (-not $p.HasExited) { if (-not (Close-Game $p.Id)) { Stop-Process -Id $p.Id -Force } }
    return $seen
}

# On, short interval: an autosave lands.
$on = Run-Once "moments-autosave-on" @('--autosave-ticks', '300') '^MOMENT_AUTOSAVED' $WaitSeconds
if (-not $on) { Write-Output "MOMENTS_AUTOSAVE_FAIL no autosave in $WaitSeconds s with the interval forced to fifteen seconds"; exit 1 }
if (-not (Test-Path $autoFile)) { Write-Output "MOMENTS_AUTOSAVE_FAIL the game said autosaved but auto.moment is not there"; exit 1 }
Write-Host "  auto.moment $((Get-Item $autoFile).Length) bytes"

# Off: nothing lands.
Remove-Item $autoFile -Force
$off = Run-Once "moments-autosave-off" @() '^MOMENT_AUTOSAVED' 45
if ($off -or (Test-Path $autoFile)) { Write-Output "MOMENTS_AUTOSAVE_FAIL an autosave landed with the row Off"; exit 1 }
Write-Output "MOMENTS_AUTOSAVE_OK lands on the interval, nothing with the row Off"
exit 0
