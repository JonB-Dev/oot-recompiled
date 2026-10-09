# Capture pairs through a scene's arrival cutscene, which is where the camera CUTS.
#
# The camera tag (phase 36) interpolates the camera between game frames and must NOT do so across
# a cut, or the world sweeps from the old viewpoint to the new one over a display frame. The
# arrival cutscenes in this game (Kokiri Forest's conversation on the first visit, for one) cut
# between fixed camera angles several times, so pairs taken steadily through one are the evidence:
# every capture should be one viewpoint or the other, never a view from somewhere in between.
#
# The judgment is by eye, on the captures around each cut; nothing here presses A, so the
# cutscene runs to its own end. tools\motion_check.py on the directory still says whether the
# picture moves between display frames during the panning parts.
#
# The same steady pairs from the warp onward are also how the 2D layer is surveyed (phase 43):
# the fade in from black, the letterbox bars, a title card, a text box and a transition all
# happen in the first seconds of an arrival.
#
#   tools\harness\cutscene.ps1 -Entrance 0x0EE -Tag camera-cuts -FrameRate 1 -Pairs 40
#   -Settings "aspect=1,hud=2" -Width 1280 -Height 720 takes the captures in a wide frame;
#   -ExtraArgs passes further flags to the game, for example @('--transition', '0x20').
param(
    [string]$Entrance = "0x0EE",
    [string]$Tag = "cutscene",
    [int]$FrameRate = 1,
    [int]$Pairs = 40,
    [int]$IntervalMs = 250,
    [string]$Settings = "",
    [int]$Width = 0,
    [int]$Height = 0,
    [string[]]$ExtraArgs = @()
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut "cutscene\$Tag"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$spec = "framerate=$FrameRate"
if ($Settings -ne '') { $spec = "$spec,$Settings" }
$saved = Use-Settings $spec

try {
    $p = Start-Game $Tag (@('--warp', $Entrance) + $ExtraArgs) -width $Width -height $Height
    Write-Host "pid $($p.Id), warping to $Entrance, frame rate setting $FrameRate, $Pairs pairs through the cutscene, extra args: $ExtraArgs"

    Enter-Play
    # The warp lands a few seconds after play begins; start capturing as the new scene appears so
    # the cutscene's first cuts are inside the window.
    Start-Sleep -Seconds 3

    $gaps = @()
    for ($i = 1; $i -le $Pairs; $i++) {
        if ($p.HasExited) { break }
        $a = Join-Path $out ('pair-{0:d3}-a.png' -f $i)
        $b = Join-Path $out ('pair-{0:d3}-b.png' -f $i)
        $gaps += ('{0:d3} {1:F1}' -f $i, (Shot-Pair $a $b))
        Start-Sleep -Milliseconds $IntervalMs
    }

    Set-Content -Path (Join-Path $out 'gaps.txt') -Value $gaps -Encoding ASCII
    $alive = -not $p.HasExited
    Write-Host ("=== {0}: {1} pairs, alive={2}, captures in {3} ===" -f $Tag, $gaps.Count, $alive, $out)
    $err = Join-Path $script:HarnessOut "$Tag.stderr.txt"
    if (Test-Path $err) {
        $bad = (Select-String -Path $err -Pattern 'IMPLAUSIBLE|BAD ADDRESS|unknown|unsupported|invalid' | Measure-Object).Count
        Write-Host ("renderer complaints: {0}" -f $bad)
    }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Restore-Settings $saved
}
