# abshots.ps1: one capture per scene at a fixed spot, for A against B (phase 51 onward).
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\abshots.ps1 -Tag p46-baseline
#   powershell -ExecutionPolicy Bypass -File tools\harness\abshots.ps1 -Tag p54-shadows -Settings "raytracing=1"
#   powershell -ExecutionPolicy Bypass -File tools\harness\abshots.ps1 -Tag p53-distance -ExtraArgs @('--rt-debug','distance')
#
# For each scene: the game starts with the entrance queued (a launch warp lapses in the intro, so it
# is given twice and taken from the queue after the dwell, the way the stretch harness does), the
# intro is walked into a file, the game FREEZES itself -FreezeTicks updates after arriving (the
# --freeze flag: the update stops, the draw runs on), the script waits for the "[shot] frozen" line
# in the trace, one capture is taken, the game is closed the way a person does. Captures land in
# build-cmake\harness\abshots\<Tag>\<scene>.png, and `python tools\texture_compare.py <folder-a>
# <folder-b>` says whether two tags match.
#
# THE CAPTURE IS DETERMINISTIC, and that is the whole point. The spot is the entrance's own (nothing
# walks); the game's random numbers are seeded with -Seed at every scene load (--seed) instead of
# the clock; the frame is the one after exactly -FreezeTicks updates, not whatever the wall clock
# reached. Two runs of one build give byte identical captures, so a capture that differs from its
# baseline differs because the picture changed. Before this, two runs differed by up to five
# percent of the pixels and no comparison meant anything. The clock is whatever the file's is.
# -Settings is applied to build-cmake\settings.txt for the run and put back afterward.
# Refuses to start while the program is running: that copy is somebody's game.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Settings = "",
    [string[]]$ExtraArgs = @(),
    [int]$SettleSeconds = 30,
    [uint32]$FreezeTicks = 100,
    [uint32]$Seed = 1,
    # The game's clock on arrival, 16 bit, 0x8000 noon: the same sky and light in every run.
    [string]$Clock = '0x8000',
    [int]$Width = 0,
    [int]$Height = 0,
    # scene name = entrance index, in the order the design lists them (lighting-design.md §9).
    [hashtable]$Scenes = [ordered]@{
        'hyrule-field'   = '0x00CD'
        'kokiri-forest'  = '0x0EE'
        'kakariko'       = '0x00DB'
        'lake-hylia'     = '0x0102'
        'links-house'    = '0x00BB'
        'bazaar'         = '0x00B7'
    }
)
. (Join-Path $PSScriptRoot 'drive.ps1')

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "ABSHOTS_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}

$out = Join-Path $script:HarnessOut "abshots\$Tag"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$saved = $null
if ($Settings -ne '') { $saved = Use-Settings $Settings }
$taken = 0
try {
    foreach ($name in $Scenes.Keys) {
        $entrance = $Scenes[$name]
        $runTag = "abshots-$Tag-$name"
        $p = Start-Game $runTag (@('--warp', "$entrance,$entrance", '--dwell', '100', '--seed', "$Seed", '--freeze', "$entrance,$FreezeTicks", '--freeze-clock', "$Clock") + $ExtraArgs) -width $Width -height $Height
        Write-Host "pid $($p.Id), $name at $entrance"
        Enter-Play
        # Wait for the game to freeze itself, which is the trace saying so, rather than for a
        # clock. -SettleSeconds is the most it may take after the intro.
        $frozen = $false
        $tracePath = Join-Path $script:HarnessOut "$runTag.stderr.txt"
        for ($i = 0; $i -lt ($SettleSeconds * 2); $i++) {
            if ($p.HasExited) { break }
            $trace = Get-Content $tracePath -ErrorAction SilentlyContinue
            if ($trace | Where-Object { $_ -match '^\[shot\] frozen' }) { $frozen = $true; break }
            Start-Sleep -Milliseconds 500
        }
        if ($p.HasExited) { Write-Host "  the game exited before the capture"; continue }
        if (-not $frozen) {
            Write-Host "  never froze at $entrance within $SettleSeconds seconds; no capture"
        }
        else {
            # One more second so the frozen frame has been presented and the window is settled.
            Start-Sleep -Seconds 1
            Shot (Join-Path $out "$name.png")
            $taken++
        }
        if (-not $p.HasExited) { if (-not (Close-Game $p.Id)) { Stop-Process -Id $p.Id -Force } }
    }
}
finally {
    Restore-Settings $saved
}
Write-Output "ABSHOTS_OK $taken of $($Scenes.Count) captures in $out"
exit 0
