# The launcher (play milestone): the program started with no --play shows the launch document
# over the runtime's empty frames, with the ROM's state, every settings row, Controls, Play and
# Quit. This captures it, walks down to Play, presses Enter, and captures the game starting; a
# second launch opens the ROM browser from the ROM row and captures that.
#
#   tools\harness\launcher-shot.ps1 -Tag p-launch
#   tools\harness\launcher-shot.ps1 -Tag p-launch-norom -NoRom
#
# With -NoRom the ROM files beside the executable (the user's and the runtime's stored copy) are
# moved into a folder beside them for the run and moved back afterward, so the launcher is seen
# asking for one. The trace's `[launch]` lines say what was found and what Play did, and the
# `[ui] screen` lines say which document was up.
param([string]$Tag = "launcher", [switch]$NoRom)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'launcher'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$bin = Join-Path $script:HarnessRoot 'build-cmake'
$aside = Join-Path $bin 'rom-aside'
$moved = @()

function Report([string]$run) {
    $err = Join-Path $script:HarnessOut "$run.stderr.txt"
    Write-Host "=== $run ==="
    if (Test-Path $err) { Select-String -Path $err -Pattern '\[launch\]|\[ui\] screen|\[ui\] hint|\[ui\] machine|focus lost' | ForEach-Object { Write-Host ("  " + $_.Line) } }
}

try {
    if ($NoRom) {
        New-Item -ItemType Directory -Force -Path $aside | Out-Null
        Get-ChildItem -Path $bin -File | Where-Object { $_.Extension -in '.z64', '.n64', '.v64' } | ForEach-Object {
            Move-Item -Path $_.FullName -Destination (Join-Path $aside $_.Name)
            $moved += $_.Name
        }
        Write-Host ("moved aside: " + ($moved -join ', '))
    }

    # Run 1: the launcher, then Play.
    $run = "$Tag-play"
    $p = Start-Game $run @() -NoPlay
    Start-Sleep -Seconds 4
    Shot (Join-Path $out "$run-launcher.png")
    # About from the launcher (F2, the signature line's target by key): the signature block and
    # the changelog, then three steps down its scrolling body, then back.
    Press 'F2' 6 1200
    Shot (Join-Path $out "$run-about.png")
    for ($i = 0; $i -lt 3; $i++) { Press 'DDOWN' 5 200 }
    Shot (Join-Path $out "$run-about-scrolled.png")
    Press 'ESC' 6 800
    # The ROM row, nine settings rows (the counter's since 2026-09-19), Controls, then Play.
    for ($i = 0; $i -lt 11; $i++) { Press 'DDOWN' 5 200 }
    Shot (Join-Path $out "$run-on-play.png")
    Press 'START' 6 3000
    Shot (Join-Path $out "$run-after-play.png")
    # The game window's frame: the header shows while the pointer is in the top band (and for
    # a few seconds after the window appears), the brass edge whenever the window is a window.
    Start-Sleep -Seconds 3
    $top = New-Object Drv+POINT; $top.X = 300; $top.Y = 20
    [Drv]::ClientToScreen($script:hwnd, [ref]$top) | Out-Null
    [Drv]::SetCursorPos($top.X, $top.Y) | Out-Null
    Start-Sleep -Milliseconds 800
    Shot (Join-Path $out "$run-frame-header.png")
    $mid = New-Object Drv+POINT; $mid.X = 480; $mid.Y = 400
    [Drv]::ClientToScreen($script:hwnd, [ref]$mid) | Out-Null
    [Drv]::SetCursorPos($mid.X, $mid.Y) | Out-Null
    Start-Sleep -Milliseconds 2500
    Shot (Join-Path $out "$run-frame-plain.png")
    Report $run

    # Run 2: the ROM browser from the ROM row, down two, back up to the drives, back to the launcher.
    $run = "$Tag-browse"
    $p = Start-Game $run @() -NoPlay
    Start-Sleep -Seconds 4
    Press 'START' 6 1200
    Shot (Join-Path $out "$run-browser.png")
    Press 'DDOWN' 5 200
    Press 'DDOWN' 5 400
    Shot (Join-Path $out "$run-browser-moved.png")
    Press 'ESC' 6 800
    Shot (Join-Path $out "$run-browser-up.png")
    Press 'ESC' 6 800
    Press 'ESC' 6 800
    Shot (Join-Path $out "$run-back.png")
    Report $run

    # Run 3: the mouse. A click on the Controls row opens the controls document (Escape closes
    # it); the lid's minimize, maximize and close buttons do what they say. The row and button
    # positions are the launcher's at its own size (660 by 580): rows 34 pixels apart from 88,
    # Controls the eleventh (the ROM row, nine settings rows), the three buttons 33 apart ending
    # 33 pixels in from the right edge, at the lid's height.
    $run = "$Tag-mouse"
    $p = Start-Game $run @() -NoPlay
    Start-Sleep -Seconds 4
    Click-Game 300 428 1500
    Shot (Join-Path $out "$run-clicked-controls.png")
    Press 'ESC' 6 1000
    Click-Game 561 34 1200
    $iconic = [Drv]::IsIconic($script:hwnd)
    Write-Host ("  after minimize click: iconic={0}" -f $iconic)
    [Drv]::ShowWindow($script:hwnd, 9) | Out-Null   # SW_RESTORE
    Start-Sleep -Milliseconds 1200
    Click-Game 594 34 1200
    $zoomed = [Drv]::IsZoomed($script:hwnd)
    Write-Host ("  after maximize click: zoomed={0}" -f $zoomed)
    Shot (Join-Path $out "$run-maximized.png")
    if ($zoomed) { [Drv]::ShowWindow($script:hwnd, 9) | Out-Null; Start-Sleep -Milliseconds 1200 }
    Click-Game 627 34 2500
    $gone = $p.HasExited
    Write-Host ("  after close click: exited={0}" -f $gone)
    Report $run
    if ($iconic -and $zoomed -and $gone) { Write-Host "MOUSE_OK" } else { Write-Host "MOUSE_FAILED" }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    foreach ($name in $moved) {
        Move-Item -Path (Join-Path $aside $name) -Destination (Join-Path $bin $name) -Force
    }
    if (Test-Path $aside) { Remove-Item -Force -Recurse $aside -ErrorAction SilentlyContinue }
    if ($moved.Count -gt 0) { Write-Host ("moved back: " + ($moved -join ', ')) }
}
Write-Host "LAUNCHER_DONE"
