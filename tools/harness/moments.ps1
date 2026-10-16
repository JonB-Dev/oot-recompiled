# moments.ps1: prove a saved moment is captured (phase 75) and, from phase 76, resumed.
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\moments.ps1 [-Entrance 0x00CD] [-Slot 1]
#
# Starts the game with a warp and a capture request for the slot, walks the intro into the file
# (Enter-Play), waits for the game to say MOMENT_WRITTEN (or MOMENT_REFUSED) in its trace, closes
# the game the way a person does, and then reads the file back with --moment-dump. Prints
# MOMENTS_OK or MOMENTS_FAIL <why>. The capture waits on the far side of the warp's transition on
# its own (the request outlives a transition), so nothing here times the door.
#
# REFUSES TO START if the program is already running: that copy is somebody's game, not ours.
param(
    [string]$Entrance = "0x00CD",
    [int]$Slot = 1,
    [int]$WaitSeconds = 60,
    # Hold the stick forward this long before the capture is requested, so the moment is taken
    # somewhere other than the spot the file starts on; the resume test then has a position to
    # prove rather than the spawn point the game would have used anyway.
    [int]$WalkMs = 0
)
. (Join-Path $PSScriptRoot 'drive.ps1')

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "MOMENTS_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}

$tag = "moments"
$trace = Join-Path $script:HarnessOut "$tag.stderr.txt"
$momentsDir = Join-Path $script:HarnessRoot "build-cmake\saves\moments"
$file = Join-Path $momentsDir ("{0}.moment" -f $Slot)
$png = Join-Path $momentsDir ("{0}.png" -f $Slot)
if (Test-Path $file) { Remove-Item $file -Force }
if (Test-Path $png) { Remove-Item $png -Force }

# THE WARP IS QUEUED, NOT ONLY SET: a warp set at launch lapses during the intro (game_state.cpp,
# PENDING_LIFETIME), so the entrance is given twice and the second is taken from the queue after
# the dwell, once play has begun, the way the stretch harness reaches its scenes. A walk needs an
# open scene: the file starts with the player asleep in a cutscene at the first entrance, where
# no key moves him. With a walk, the capture is delayed 600 game updates (thirty seconds) after
# play is first seen, so the queued warp, its transition and the walk all happen before the frame
# that is kept. Without one it lands on the first safe frame.
$spec = if ($WalkMs -gt 0) { "$Slot,600" } else { "$Slot" }
$launch = @('--warp', "$Entrance,$Entrance", '--dwell', '100', '--moment-save', $spec)
$p = Start-Game $tag $launch
Write-Host "pid $($p.Id), warping to $Entrance (queued, dwell 100), capture into slot $Slot, walk $WalkMs ms"
Enter-Play
if ($WalkMs -gt 0) {
    Start-Sleep -Seconds 8
    Hold 'SUP' $WalkMs
}

$verdict = ""
for ($i = 0; $i -lt ($WaitSeconds * 2); $i++) {
    if ($p.HasExited) { $verdict = "the game exited"; break }
    $lines = Get-Content $trace -ErrorAction SilentlyContinue
    $hit = $lines | Where-Object { $_ -match '^MOMENT_(WRITTEN|REFUSED)' } | Select-Object -Last 1
    if ($hit) { $verdict = $hit; break }
    Start-Sleep -Milliseconds 500
}
$waits = (Get-Content $trace -ErrorAction SilentlyContinue) | Where-Object { $_ -match '^\[moments\]' }
foreach ($w in $waits) { Write-Host "  $w" }

if (-not $p.HasExited) {
    if (-not (Close-Game $p.Id)) { Stop-Process -Id $p.Id -Force }
}

if ($verdict -eq "") { Write-Output "MOMENTS_FAIL nothing was written or refused in $WaitSeconds s"; exit 1 }
if ($verdict -notmatch '^MOMENT_WRITTEN') { Write-Output "MOMENTS_FAIL $verdict"; exit 1 }
if (-not (Test-Path $file)) { Write-Output "MOMENTS_FAIL the game said written but $file is not there"; exit 1 }
Write-Host "  $verdict; file $((Get-Item $file).Length) bytes; thumbnail $(if (Test-Path $png) { (Get-Item $png).Length.ToString() + ' bytes' } else { 'missing' })"

# The program is a Windows subsystem executable: its printf reaches nobody unless the handles are
# given, so the dump runs as the game does, with its output redirected to a file.
$dumpOut = Join-Path $script:HarnessOut "$tag.dump.txt"
$d = Start-Process -FilePath $script:HarnessExe -ArgumentList @('--moment-dump', ('"' + $file + '"')) `
        -RedirectStandardOutput $dumpOut -RedirectStandardError (Join-Path $script:HarnessOut "$tag.dump.stderr.txt") -Wait -PassThru
$dump = Get-Content $dumpOut
$dump | Where-Object { $_ -match '^\s' -or $_ -match '^moment ' } | ForEach-Object { Write-Host "  $_" }
if ($d.ExitCode -ne 0) { Write-Output "MOMENTS_FAIL the dump refused the file"; exit 1 }

# THE MOMENT MUST SAY WHERE THE GAME WAS, which is the trace's last entrance before the write,
# not the warp target: a warp asked for at launch can lapse while the intro runs (a harness quirk
# older than this script), and a moment captured wherever the game stands is still correct.
$before = ($lines = Get-Content $trace) | Select-String -Pattern '^\[state\] entrance 0x([0-9A-F]+)' | Select-Object -Last 1
$want = if ($before) { [Convert]::ToInt32($before.Matches[0].Groups[1].Value, 16) } else { -1 }
$got = ($dump | Where-Object { $_ -match 'entrance\s+0x([0-9A-F]+)' } | ForEach-Object { [Convert]::ToInt32($Matches[1], 16) } | Select-Object -First 1)
if ($got -ne $want) { Write-Output ("MOMENTS_FAIL the moment says entrance 0x{0:X4}, the game was at 0x{1:X4}" -f $got, $want); exit 1 }
Write-Output ("MOMENTS_OK slot {0} at entrance 0x{1:X4}" -f $Slot, $got)
exit 0
