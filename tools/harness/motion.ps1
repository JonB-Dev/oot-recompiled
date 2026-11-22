# Is anything drawn BETWEEN game frames? The objective half of the interpolation verification.
#
# The game runs its logic at twenty frames a second. With the frame rate setting at Original the
# renderer presents each game frame once, so two captures taken a few milliseconds apart are
# identical unless they happen to straddle a game frame. With the setting at Match the display and
# transforms TAGGED, the renderer presents interpolated frames in between, so the two captures
# differ almost every time. Untagged transforms are not interpolated, which is why phase 34 expects
# this script to report the same thing at both settings and phase 36 onward expect the difference.
#
# The script warps to an entrance, clears the conversation, then walks Link sideways (the camera
# follows, so the whole picture moves) while taking capture pairs. tools\motion_check.py turns the
# pairs into numbers.
#
#   tools\harness\motion.ps1 -Entrance 0x0EE -Tag camera -FrameRate 1
#   python tools\motion_check.py build-cmake\harness\motion\camera
param(
    [string]$Entrance = "0x0EE",
    [string]$Tag = "motion",
    [int]$FrameRate = 1,
    [int]$Pairs = 30,
    # Further flags for the game, for example --present-early while a renderer setting is being
    # brought up. Passed through untouched.
    [string[]]$ExtraArgs = @(),
    # "walk": Link walks sideways and the camera follows him, which moves the camera very little
    # per game frame; a sub pixel drift is invisible to the check. "look": first person view
    # (C up) and the stick pans the camera itself, tens of pixels per game frame, with Link out
    # of the picture. The camera phases use "look"; "walk" is what the baseline was taken with.
    # "still": no input at all, so only the actors that animate on their own move; the actor
    # tagging phases measure with this.
    # "roll": Link walks and rolls (A pressed every so often), which kicks up dust: the
    # particle phase's test, since a soft sprite particle is what a roll leaves behind.
    [string]$Mode = "walk",
    # Walk Link forward for this long before the pairs start, whatever the mode, so a capture
    # can begin where something worth measuring is in view (a scene's entrance rarely is).
    [int]$LeadSeconds = 0,
    # Press A this many times after the lead and before the pairs (two seconds after the lead,
    # so a greeting that takes a moment to appear is there to be dismissed), then tap the stick
    # left once with -Nudge: in a shop that dismisses the keeper's greeting and moves the cursor
    # on to an item, which is when the item spins.
    [int]$PressA = 0,
    [switch]$Nudge
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut "motion\$Tag"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null

# The frame rate is a setting the game reads at launch. Written before the launch, restored after,
# so a measurement never changes what the player sees next time.
$saved = Use-Settings "framerate=$FrameRate"

try {
    $p = Start-Game $Tag (@('--warp', $Entrance) + $ExtraArgs)
    Write-Host "pid $($p.Id), warping to $Entrance, frame rate setting $FrameRate, $Pairs pairs, extra args: $ExtraArgs"

    Enter-Play
    Start-Sleep -Seconds 8
    for ($i = 1; $i -le 12; $i++) { if ($p.HasExited) { break }; Press 'A' 6 900 }

    if ($LeadSeconds -gt 0) {
        Focus-Game
        [Drv]::Down([uint16]$VK['SUP'])
        Start-Sleep -Seconds $LeadSeconds
        [Drv]::Up([uint16]$VK['SUP'])
        Start-Sleep -Milliseconds 800
    }

    if ($PressA -gt 0) { Start-Sleep -Seconds 2 }
    for ($i = 1; $i -le $PressA; $i++) { if ($p.HasExited) { break }; Press 'A' 6 1200 }
    if ($Nudge) { Hold 'SLEFT' 150; Start-Sleep -Milliseconds 1200 }

    if ($Mode -eq 'look') {
        # First person view, then the stick pans the camera. The press is held long enough for the
        # game to see it and the view takes a moment to settle before the pan starts.
        Press 'CUP' 8 1500
    }

    # "forward": Link walks away from the camera and it follows him, eye and focus moving
    # together, which is the motion the cut heuristic is surest about; the world flows past at
    # several pixels a game frame. The stick is held for the whole run with no turn.
    # "still": nothing is pressed. The camera does not move, so the only things that change
    # between the two captures of a pair are the actors that animate on their own (Link's idle,
    # the fairy beside him, whoever else lives in the scene). That is the test for the actor
    # tagging phases, because a walk's captures are dominated by the background gliding behind
    # Link once the camera is tagged, and a tagged Link is invisible under it.
    $stick = if ($Mode -eq 'forward') { 'SUP' } else { 'SLEFT' }

    # Push the stick for the whole measurement. Down, measure, up: Hold would block.
    Focus-Game
    if ($Mode -ne 'still') { [Drv]::Down([uint16]$VK[$stick]) }
    Start-Sleep -Milliseconds 600
    $gaps = @()
    for ($i = 1; $i -le $Pairs; $i++) {
        if ($p.HasExited) { break }
        $a = Join-Path $out ('pair-{0:d3}-a.png' -f $i)
        $b = Join-Path $out ('pair-{0:d3}-b.png' -f $i)
        $gaps += ('{0:d3} {1:F1}' -f $i, (Shot-Pair $a $b))
        Start-Sleep -Milliseconds 300
        # A roll every third pair: A is tapped while the stick is held, and the capture that
        # follows lands on the dust it leaves.
        if ($Mode -eq 'roll' -and ($i % 3) -eq 1) {
            [Drv]::Down([uint16]$VK['A']); Start-Sleep -Milliseconds 80; [Drv]::Up([uint16]$VK['A'])
        }
        # Turn round halfway so Link does not walk into a wall and stop the camera.
        if ($Mode -ne 'forward' -and $Mode -ne 'still' -and $i -eq [int]($Pairs / 2)) {
            [Drv]::Up([uint16]$VK['SLEFT']); Start-Sleep -Milliseconds 100
            [Drv]::Down([uint16]$VK['SRIGHT'])
        }
    }
    [Drv]::Up([uint16]$VK['SLEFT']); [Drv]::Up([uint16]$VK['SRIGHT']); [Drv]::Up([uint16]$VK['SUP'])

    Set-Content -Path (Join-Path $out 'gaps.txt') -Value $gaps -Encoding ASCII
    $alive = -not $p.HasExited
    Write-Host ("=== {0}: {1} pairs, alive={2}, gaps in {3} ===" -f $Tag, $gaps.Count, $alive, (Join-Path $out 'gaps.txt'))

    # The probe's last health line carries presents per VI, which is the other half of the check.
    $probe = Join-Path $script:HarnessRoot "build-cmake\$Tag-probe.txt"
    if (Test-Path $probe) { Get-Content $probe | Select-Object -Last 2 }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Restore-Settings $saved
}
