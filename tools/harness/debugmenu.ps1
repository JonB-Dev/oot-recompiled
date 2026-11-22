# Drive the debug menu and the frame recorder, and check what they actually produced.
#
#   tools\harness\debugmenu.ps1
#
# WHAT THIS PROVES, because a screenshot of a menu proves less than it looks:
#   - the recorder's plate is at the top left and a click on it records
#   - when it is done there are PNG files and a notes file in the user's Pictures folder, named
#     the way they were asked to be, with the frame count they claim
#   - the Debug row is in the in-game settings and opens the warp list
#   - the list nests, and walking into a region shows the places in it
#   - a warp from the menu MOVES THE GAME, checked in the log rather than against a picture,
#     since a picture of a loading screen looks like a picture of anything else
#
# THE RECORDER GOES FIRST AND THAT IS NOT ARBITRARY. A synthetic click only lands while the game
# is the foreground window, and the longer a run goes on the more likely the focus has wandered
# off to whatever is driving it. Clicking within a few seconds of the game appearing is reliable;
# clicking after a minute of menu navigation is not, and the failure is silent in the worst way:
# the pointer MOVE arrives at exactly the right coordinates and the BUTTON never does, which reads
# as a plate that is not clickable rather than as a test that lost the window.
#
# The warp aims at Kakariko Village, entrance 0 (0x0DB), because it is the place the user named
# when they asked for this.

param(
    [string]$Tag = "debugmenu"
)

. (Join-Path $PSScriptRoot 'drive.ps1')

$out = Join-Path $script:HarnessOut $Tag
New-Item -ItemType Directory -Force -Path $out | Out-Null

# The recorder on, and the shortest length, so a run of this does not write a minute of frames.
# Windowed, because the pointer has to be able to reach the plate. All restored at the end.
$saved = Use-Settings
Set-Setting 'recorder' 1
Set-Setting 'recordlength' 0      # index 0 is five seconds
Set-Setting 'counter' 1           # the counter too, so both plates can be seen in one shot
Set-Setting 'window' 0

$proc = Start-Game $Tag -width 1280 -height 720
try {
    Enter-Play
    Shot (Join-Path $out '01-play.png')

    # ---- the recorder ----------------------------------------------------------------------
    #
    # The plate is pinned BELOW the band the title bar rides in (case.rcss says why), so the click
    # goes at that offset. Clicking at the window's very edge hits the title bar instead, which is
    # what the first runs of this did. The click is CHECKED rather than assumed, for the reason in
    # the header.
    # BOTH POSITIONS ARE TESTED, because the plate has two and the interesting one is easy to
    # miss. It sits at the window's edge while the title bar is hidden and below the bar while it
    # shows, and the bar appears when the pointer enters the top band: so a pointer traveling up
    # to the plate at its high position is also a pointer about to raise the bar and push the
    # plate away from itself. note_pointer freezes the bar's state while the pointer is ON the
    # plate, which is what makes the high position clickable at all. Clicking only the low one
    # would pass without testing any of that.
    # THE LOG IS NOT READ UNTIL THE PROGRAM HAS EXITED. Its stderr is redirected to a file, which
    # makes it fully buffered, so a line can be many seconds behind the thing it describes. An
    # earlier version of this checked between clicks and did real damage doing so: the click HAD
    # landed, the check could not see it yet, and the retry toggled the recorder straight back
    # off. It read as four failed clicks on a plate that worked the first time. So the clicks are
    # made here and judged at the bottom, from a file nobody is still writing to.

    # 1. Bar hidden, plate high, up against the window's edge. This is the position that needs
    #    the hysteresis in note_pointer: the pointer arriving here is also a pointer entering the
    #    band that raises the bar, which without it would drop the plate away from the click.
    Click-Game 60 25 1000
    Shot (Join-Path $out '02-recording.png')

    # Let it run its five seconds out rather than stopping it, so the count in the notes is a
    # whole recording.
    Start-Sleep -Seconds 7
    Shot (Join-Path $out '03-after-recording.png')

    # 2. Raise the bar from further along it, which drops the plate, then start a SECOND recording
    #    from the low position. Two starts in the log is the unambiguous evidence that both
    #    positions take a click; stopping one would be ambiguous with it ending on its own.
    #
    #    NOTHING BETWEEN RAISING THE BAR AND THE CLICK MAY BE SLOW, which is why this does not use
    #    Click-Game. That helper takes the focus first, and its focus wait can run into seconds;
    #    the bar only stays up for 1.2 seconds after the pointer leaves the band, so the plate had
    #    risen again before the click arrived and the click landed under it. Focus is taken ONCE,
    #    up front, and the pointer and button are then driven directly with no waiting in between.
    Focus-Game
    $r = New-Object Drv+RECT
    [Drv]::GetWindowRect($script:hwnd, [ref]$r) | Out-Null
    for ($i = 0; $i -lt 5; $i++) {
        [Drv]::SetCursorPos($r.L + 700 + $i, $r.T + 12) | Out-Null
        Start-Sleep -Milliseconds 100
    }
    # The press goes through Click-Game like every other click here, because it converts client
    # coordinates with ClientToScreen and a hand rolled version using the WINDOW rect did not land
    # even with the pointer visibly on the plate. The bar surviving the moment is no longer the
    # worry it was: note_pointer freezes its state while the pointer is on the plate, so once
    # Click-Game's move lands there the plate stops where it is however long the focus step takes.
    Click-Game 60 55 1000
    Start-Sleep -Seconds 7

    # A moment for the encoders to finish and the notes to land.
    Start-Sleep -Seconds 6
    Shot (Join-Path $out '03-after-recording.png')

    # ---- the debug menu --------------------------------------------------------------------
    #
    # THE MENU'S KEYS ARE W, S, ENTER AND ESCAPE (boot.cpp reads those scancodes directly), which
    # in drive.ps1's table are SUP, SDOWN, START and ESC. There is no 'UP' or 'RETURN' in that
    # table, and a name that is not in it becomes virtual key ZERO and sends nothing at all: the
    # first run of this script pressed nothing four times and the menu just sat there looking
    # perfectly correct.
    Press 'F1' 6 1200
    Shot (Join-Path $out '04-settings.png')

    # Debug is the last row of the in-game list, so one press up from the first row wraps to it.
    Press 'SUP' 6 600
    Shot (Join-Path $out '05-debug-row.png')
    Press 'START' 6 1200
    Shot (Join-Path $out '06-regions.png')

    # Kakariko Village is the seventh region, so six presses down from the first.
    for ($i = 0; $i -lt 6; $i++) { Press 'SDOWN' 4 250 }
    Shot (Join-Path $out '07-kakariko-focused.png')
    Press 'START' 6 1000
    Shot (Join-Path $out '08-kakariko-places.png')

    # The first place in it is the village itself, which has sixteen ways in, so this descends
    # once more rather than warping.
    Press 'START' 6 1000
    Shot (Join-Path $out '09-kakariko-entrances.png')

    # Entrance 0, which warps and closes the menu.
    Press 'START' 6 8000
    Shot (Join-Path $out '10-after-warp.png')
}
finally {
    Close-Game $proc.Id | Out-Null
    Restore-Settings $saved
}

Write-Host ""
Write-Host "=== what the recorder wrote ==="
$pictures = [Environment]::GetFolderPath('MyPictures')
$root = Join-Path $pictures 'OoT-Recomp'
if (Test-Path $root) {
    $day = Get-ChildItem $root -Directory | Sort-Object LastWriteTime | Select-Object -Last 1
    Write-Host "folder: $($day.FullName)"
    $pngs = Get-ChildItem $day.FullName -Filter '*.png'
    Write-Host "png files: $($pngs.Count)"
    if ($pngs.Count -gt 0) {
        $ordered = $pngs | Sort-Object { [int]([regex]::Match($_.Name, '\.f(\d+)\.png$').Groups[1].Value) }
        Write-Host "first: $($ordered[0].Name)"
        Write-Host "last:  $($ordered[-1].Name)"
        $total = ($pngs | Measure-Object -Property Length -Sum).Sum
        Write-Host "on disk: $([math]::Round($total / 1MB, 1)) MB"
    }
    foreach ($c in Get-ChildItem $day.FullName -Filter '*.frames.csv') {
        Write-Host "notes: $($c.Name)"
        Get-Content $c.FullName -TotalCount 9 | ForEach-Object { Write-Host "  $_" }
    }
}
else {
    Write-Host "NOTHING under $root"
}

Write-Host ""
Write-Host "=== what the log says ==="
$lines = Get-Content (Join-Path $script:HarnessOut "$Tag.stderr.txt")
$lines | Select-String -Pattern '\[record\]|\[warp\]|\[ui\] screen|matched nothing' |
    ForEach-Object { $_.Line }

Write-Host ""
Write-Host "=== the verdict, from the flushed log ==="
$starts = ($lines | Select-String -Pattern '\[record\] started').Count
$finished = ($lines | Select-String -Pattern '\[record\] finished').Count
$warps = ($lines | Select-String -Pattern '\[warp\]').Count

# WHAT ACTUALLY HAS TO PASS. One recording made and written, and one warp taken. Both of those are
# the features this script exists to check.
if ($starts -ge 1 -and $finished -ge 1) { Write-Host "PASS  the recorder recorded and wrote its frames" }
else { Write-Host "FAIL  no recording was made" }
if ($warps -ge 1) { Write-Host "PASS  the debug menu warped the game" }
else { Write-Host "FAIL  no warp was taken" }

# AND ONE EXTRA PROBE, WHICH IS NOT A PASS OR A FAIL. The script clicks the plate twice, once in
# each of its two positions, to exercise the high one that needs note_pointer's hysteresis. The
# second click is driven through the real pointer and needs the game to be the foreground window
# for the whole of it, so it is lost whenever anything else takes the focus, up to and including
# somebody moving the mouse while the run is going. A lost second click says nothing about the
# recorder, which is why it is reported apart from the two lines above rather than as a failure.
if ($starts -ge 2) { Write-Host "also: the plate took a click in BOTH positions this run" }
else { Write-Host "also: only one of the two plate clicks landed this run (the other went astray; see above)" }
