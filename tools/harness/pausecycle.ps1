# The reproduction for PHASE-26, and the regression check for every phase that touches a
# display list or the pause path.
#
# Opening the pause menu makes the game overwrite gameplay_keep in place (Player_InitPauseDrawData
# DMAs a fresh copy 0x3800 bytes further along, for the equipment screen's render of Link) while the
# previous frame's display list, which draws the minimap compass arrow out of gameplay_keep, may
# still be in the renderer. On the console the RSP wins that race by a mile. Here the renderer is a
# host thread that also has to sync with the GPU for the pause backdrop, so it can lose. The fix is
# patches\fixes\pause_object_race.c; this script is how the fix was proved and how it stays proved.
#
# It warps to a heavy outdoor scene, clears the conversation that plays on first arrival, then opens
# and closes the menu over and over. Each cycle is one roll of the dice. A build with the race shows
# the display list guard firing in stderr or dies; a build without it shows neither.
#
#   tools\harness\pausecycle.ps1 -Entrance 0x0211 -Cycles 40 -Tag patched
#
# -CaptureCycles N takes capture pairs a display frame apart through the first N opens and closes
# (six pairs across the half second after each press). That is how a camera smear on the pause
# transition would show: the pause menu has its own camera, and a frame drawn between the play
# camera and the menu's would be a view from nowhere. A still capture half a second later cannot
# see a one frame smear; the pairs during the press can. Those pairs are also how the pause
# backdrop is surveyed in a wide frame (phase 43): -Settings "aspect=1,hud=2" -Width 1280
# -Height 720 runs the cycles at that size.
param(
    [string]$Entrance = "0x0211",
    [int]$Cycles = 40,
    [string]$Tag = "pausecycle",
    [int]$CaptureCycles = 0,
    [string]$Settings = "",
    [int]$Width = 0,
    [int]$Height = 0,
    # In each captured cycle, turn the menu's pages this many times with R while it is open,
    # eight pairs through each turn: the cube of pages rotates over a dozen game frames, and a
    # pair taken between two of them shows an intermediate angle only if the pages are tagged
    # (phase 45).
    [int]$TurnPages = 0,
    # With -TurnBurst N, each turn is captured as a burst of N back to back grabs instead of the
    # eight pairs. The menu runs at thirty game frames a second (the pause setup halves the
    # update rate), so a pair a few milliseconds apart is a weak witness there: it straddles a
    # picture change about half as often as in play whichever rate is on. A burst counted by
    # tools\burst_count.py holds about twenty distinct pictures in twenty four grabs at sixty
    # hertz and about ten when the picture steps at the menu's own rate.
    [int]$TurnBurst = 0,
    # Extra arguments for the game, appended after the warp (phase 53: @('--rt-level','1') runs
    # the race with the ray tracing level above Off, which is one of that phase's gates).
    [string[]]$ExtraArgs = @()
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'pausecycle'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$err = Join-Path $script:HarnessOut "$Tag.stderr.txt"
$pairDir = Join-Path $out $Tag
if ($CaptureCycles -gt 0) {
    if (Test-Path $pairDir) { Remove-Item -Recurse -Force $pairDir }
    New-Item -ItemType Directory -Force -Path $pairDir | Out-Null
}

# Press START and capture through the transition it starts. Six pairs at about sixty
# millisecond spacing cover the opening or closing animation.
function Invoke-StartCaptured {
    param([string]$name)
    Focus-Game
    $vk = [uint16]$VK['START']
    [Drv]::Down($vk)
    Start-Sleep -Milliseconds 200
    [Drv]::Up($vk)
    for ($k = 1; $k -le 6; $k++) {
        $a = Join-Path $pairDir ('{0}-{1}-a.png' -f $name, $k)
        $b = Join-Path $pairDir ('{0}-{1}-b.png' -f $name, $k)
        [void](Shot-Pair $a $b)
        Start-Sleep -Milliseconds 60
    }
    Start-Sleep -Milliseconds 900
}

# Press R and capture through the page turn it starts, eight pairs at about sixty millisecond
# spacing across the half second the cube takes to come round.
function Invoke-TurnCaptured {
    param([string]$name)
    Focus-Game
    $vk = [uint16]$VK['R']
    [Drv]::Down($vk)
    if ($TurnBurst -gt 0) {
        # The turn takes sixteen game frames, about half a second at the menu's rate. R held
        # for two frames and the burst started at once, so its twenty four grabs of about
        # fourteen milliseconds each all fall inside the turn.
        Start-Sleep -Milliseconds 70
        [Drv]::Up($vk)
        $burstDir = Join-Path $pairDir ('{0}-burst\bitblt' -f $name)
        New-Item -ItemType Directory -Force -Path $burstDir | Out-Null
        $ms = [Drv]::ShotBurst($script:hwnd, $burstDir, $TurnBurst, $false)
        Write-Host ("  {0}: burst of {1} grabs in {2:F0} ms" -f $name, $TurnBurst, $ms)
    } else {
        Start-Sleep -Milliseconds 120
        [Drv]::Up($vk)
        for ($k = 1; $k -le 8; $k++) {
            $a = Join-Path $pairDir ('{0}-{1}-a.png' -f $name, $k)
            $b = Join-Path $pairDir ('{0}-{1}-b.png' -f $name, $k)
            [void](Shot-Pair $a $b)
            Start-Sleep -Milliseconds 60
        }
    }
    Start-Sleep -Milliseconds 700
}

$saved = $null
if ($Settings -ne '') { $saved = Use-Settings $Settings }

try {
    # The entrance twice, as abshots.ps1 queues it: a launch warp lapses in the intro since the
    # moments work, and a single one left the race in the file's starting scene with its count
    # at zero (2026-09-24, phase 53).
    $p = Start-Game $Tag (@('--warp', "$Entrance,$Entrance") + $ExtraArgs) -width $Width -height $Height
    Write-Host "pid $($p.Id), warping to $Entrance, then $Cycles pause cycles"

    Enter-Play

    # Let the warp land and the scene settle.
    Start-Sleep -Seconds 8

    # The first arrival in this scene plays a cutscene with a conversation, and the pause menu is
    # refused for as long as it runs. A is what advances a text box, and outside one it is a roll,
    # which is harmless.
    for ($i = 1; $i -le 16; $i++) {
        if ($p.HasExited) { break }
        Press 'A' 6 1200
    }

    $done = 0
    for ($i = 1; $i -le $Cycles; $i++) {
        if ($p.HasExited) { break }
        if ($i -le $CaptureCycles) { Invoke-StartCaptured ('cycle-{0}-open' -f $i) } else { Press 'START' 6 1500 }
        if ($p.HasExited) { break }
        if ($i -le $CaptureCycles) {
            for ($t = 1; $t -le $TurnPages; $t++) {
                if ($p.HasExited) { break }
                Invoke-TurnCaptured ('cycle-{0}-turn-{1}' -f $i, $t)
            }
        }
        if ($p.HasExited) { break }
        if ($i -le $CaptureCycles) { Invoke-StartCaptured ('cycle-{0}-close' -f $i) } else { Press 'START' 6 1500 }
        $done = $i
        # A little movement between cycles so the frames are not identical every time.
        Hold 'SUP' 300
        Hold 'SLEFT' 200
    }

    Start-Sleep -Seconds 2
    $alive = -not $p.HasExited
    if ($alive) { Shot (Join-Path $out "$Tag.final.png") }

    $guard = 0
    $opened = 0
    if (Test-Path $err) {
        $guard = (Select-String -Path $err -Pattern 'IMPLAUSIBLE NESTED LIST' -ErrorAction SilentlyContinue | Measure-Object).Count
        # The menu opening is visible as segment 4 leaving the object bank for the pause buffer.
        # Count the moves after the warp landed; zero means the menu never opened and the run
        # proved nothing, whatever else it says. Substring tests, not -like: square brackets are
        # wildcard classes to -like.
        $lines = @(Get-Content $err)
        $wanted = '0x{0:X4}' -f [int]$Entrance
        $last = -1
        for ($k = 0; $k -lt $lines.Count; $k++) { if ($lines[$k].Contains("[state] entrance $wanted")) { $last = $k } }
        if ($last -ge 0 -and $last -lt ($lines.Count - 1)) {
            $opened = @($lines[($last + 1)..($lines.Count - 1)] | Where-Object { $_.Contains('[seg] segment 4 moved') }).Count
        }
    }
    Write-Host ("=== {0}: {1} of {2} cycles, alive={3}, guard fired {4} times, segment 4 moved {5} times after the warp ===" -f $Tag, $done, $Cycles, $alive, $guard, $opened)
    if (-not $alive) { Write-Host ("exit code {0}" -f $p.ExitCode) }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Restore-Settings $saved
}
