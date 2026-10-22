# The one menu (the menus plan, phases 96 to 119), DRIVEN rather than checked: the launcher's
# shell turned through every section by the keys and by clicks on the dial, a page opened and left
# by Back, Play; then in the game the shell opened by F1 in the placement the settings name, turned
# through its sections, and closed. Every step is captured under build-cmake\harness\menus\, and
# the trace's [ui] lines (which screen, which press, anything RmlUi refused) are printed after each
# run, so a step that did nothing shows as a missing line, not only as a picture.
#
#   tools\harness\menus.ps1 -Tag p96
#   tools\harness\menus.ps1 -Tag p96-right -GameOnly -Settings "menuside=1"
#   tools\harness\menus.ps1 -Tag p96-center -GameOnly -Settings "menuside=0"
#
# The keys are the UI layer's (src/ui/input/ui_input.cpp): Q and E turn the dial (the table names
# them Z and L, the game's buttons they also are), Enter and X accept, Escape and C go back.
param([string]$Tag = "menus", [string]$Settings = "", [switch]$GameOnly, [switch]$LauncherOnly,
      [int]$Turns = 8)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'menus'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$saved = Use-Settings $Settings

function Report([string]$run) {
    $err = Join-Path $script:HarnessOut "$run.stderr.txt"
    Write-Host "=== $run ==="
    if (Test-Path $err) {
        Select-String -Path $err -Pattern '\[ui\]|\[launch\]|RmlUi|rml|focus lost' |
            Where-Object { $_.Line -notmatch 'hint' } | ForEach-Object { Write-Host ("  " + $_.Line) }
    }
}

function Snap([string]$name) { Shot (Join-Path $out "$Tag-$name.png") }

try {
    if (-not $GameOnly) {
        $run = "$Tag-launcher"
        $p = Start-Game $run @() -NoPlay
        Start-Sleep -Seconds 6
        Snap 'l-0-play'
        # The ROM browser, a page of Play: up to the ROM row, Enter opens it; Back goes up a folder
        # and then back to Play (phase 99).
        Press 'DUP' 4 400; Press 'START' 6 1200; Snap 'l-browse'
        Press 'DDOWN' 4 300; Press 'DDOWN' 4 300; Snap 'l-browse-moved'
        Press 'ESC' 4 900; Snap 'l-browse-up'
        Press 'ESC' 4 900; Press 'ESC' 4 900; Snap 'l-browse-back'
        # Round the dial forward with E, one section at a time, and back once with Q.
        for ($i = 1; $i -le $Turns; $i++) { Press 'L' 4 800; Snap "l-$i-next" }
        Press 'Z' 4 800; Snap 'l-prev'
        # Down the page and back up, so a capture shows the page answering the keys too.
        for ($i = 0; $i -lt 3; $i++) { Press 'DDOWN' 4 300 }
        Snap 'l-page-moved'
        # A click on the medallion left of the current one (the launcher's dial: 34% across,
        # below the 112 dp head), then on the one right of it.
        Click-Game 326 210 900; Snap 'l-click-left'
        Click-Game 634 210 900; Snap 'l-click-right'
        # Back on a section's first page in the launcher does nothing: the launcher stays.
        Press 'ESC' 4 800; Snap 'l-back'
        Report $run
        Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 1
    }
    if (-not $LauncherOnly) {
        $run = "$Tag-game"
        $p = Start-Game $run @()
        Enter-Play
        Start-Sleep -Seconds 3
        # THE HOLD (phase 97): the picture beside the panel, two seconds apart, running and then
        # held. A held game draws the same frame again; a running one moves on (Link's idle, the
        # water, the flames). ..\motion_check.py's thresholds: a pixel counts above forty levels.
        Snap 'g-free-a'; Start-Sleep -Seconds 2; Snap 'g-free-b'
        Snap 'g-0-playing'
        Press 'F1' 6 1500; Snap 'g-1-settings'
        Start-Sleep -Seconds 1; Snap 'g-held-a'; Start-Sleep -Seconds 2; Snap 'g-held-b'
        for ($i = 1; $i -le $Turns; $i++) { Press 'L' 4 900; Snap "g-turn-$i" }
        Press 'Z' 4 900; Snap 'g-turn-back'
        Press 'F2' 6 1200; Snap 'g-2-about'
        Press 'F3' 6 1200; Snap 'g-3-controls'
        Press 'ESC' 6 1200; Snap 'g-4-closed'
        Press 'F1' 6 1200
        Press 'ESC' 6 1200; Snap 'g-5-closed-again'
        Report $run
    }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Restore-Settings $saved
}
