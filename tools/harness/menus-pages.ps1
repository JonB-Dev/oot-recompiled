# The one menu's pages that menus.ps1 does not reach (the menus plan, phases 104 to 107), DRIVEN in
# the game: Moments (the slots, then the buttons under them), the Debugging confirmation and the
# section it opens with the Warps page inside it, About scrolled, and Your files from the Settings
# list. Every step is captured under build-cmake\harness\menus\ and the trace's [ui] lines printed.
#
#   tools\harness\menus-pages.ps1 -Tag p104 -Settings "menu=2"
param([string]$Tag = "pages", [string]$Settings = "menu=2")

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'menus'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$saved = Use-Settings $Settings
function Snap([string]$name) { Shot (Join-Path $out "$Tag-$name.png") }

try {
    $run = "$Tag-game"
    $p = Start-Game $run @()
    Enter-Play
    Start-Sleep -Seconds 2

    # Moments: four turns of the dial from Settings (Lighting, HUD, Controls, Moments).
    Press 'F1' 6 1200
    for ($i = 0; $i -lt 4; $i++) { Press 'L' 4 700 }
    Snap 'moments'
    Press 'DDOWN' 4 300; Press 'DDOWN' 4 300; Snap 'moments-moved'
    Press 'START' 6 700; Snap 'moments-buttons'
    Press 'ESC' 6 600; Press 'ESC' 6 900

    # Debugging: up from the top of Settings wraps to Quit, then Debugging; Enter asks first.
    Press 'F1' 6 1200
    Press 'DUP' 4 300; Press 'DUP' 4 300
    Press 'START' 6 1000; Snap 'debug-ask'
    Press 'DRIGHT' 4 600; Snap 'debug-section'
    Press 'START' 6 1000; Snap 'warps'
    Press 'DDOWN' 4 300; Press 'START' 6 800; Snap 'warps-deeper'
    Press 'ESC' 6 700; Press 'ESC' 6 900; Snap 'warps-back'
    Press 'ESC' 6 900

    # About, scrolled.
    Press 'F2' 6 1200; Snap 'about'
    for ($i = 0; $i -lt 4; $i++) { Press 'DDOWN' 4 250 }
    Snap 'about-scrolled'
    Press 'ESC' 6 900

    # Your files: up three from the top of Settings (Quit, Debugging, Your files).
    Press 'F1' 6 1200
    Press 'DUP' 4 300; Press 'DUP' 4 300; Press 'DUP' 4 300
    Press 'START' 6 1000; Snap 'files'
    Press 'ESC' 6 900; Snap 'files-back'
    Press 'ESC' 6 900

    $err = Join-Path $script:HarnessOut "$run.stderr.txt"
    Write-Host "=== $run ==="
    if (Test-Path $err) {
        Select-String -Path $err -Pattern '\[ui\] (screen|press|page)|\[shot\]|\[warp' | ForEach-Object { Write-Host ("  " + $_.Line) }
    }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Restore-Settings $saved
}
