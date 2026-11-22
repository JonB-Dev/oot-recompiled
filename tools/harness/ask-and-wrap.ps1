# Three of the menus plan's baseline defects, DRIVEN (baseline.md, 1, 2 and 7): on a question, Left and
# Right move between the answers and only Enter answers; and Enter on a value at its last option
# wraps it to the first. Captured under build-cmake\harness\menus\, the trace's lines printed.
#
#   tools\harness\ask-and-wrap.ps1 -Tag fix
param([string]$Tag = "askwrap", [string]$Settings = "menu=2,antialiasing=3")

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
    # The Debugging question: up from the top of Settings wraps to Quit, then Debugging.
    Press 'F1' 6 1200
    Press 'DUP' 4 300; Press 'DUP' 4 300
    Press 'START' 6 1000; Snap 'ask-open'
    Press 'DRIGHT' 4 600; Snap 'ask-yes-focused'
    Press 'DLEFT' 4 600; Snap 'ask-no-focused'
    Press 'DRIGHT' 4 400; Press 'START' 6 1000; Snap 'ask-answered'
    Press 'ESC' 6 900; Press 'ESC' 6 900
    # Defect 2: a click on a Debugging row's body acts (it opens the Debugging section first).
    Press 'F1' 6 1200
    Press 'DUP' 4 300; Press 'DUP' 4 300
    Press 'START' 6 1000; Press 'DRIGHT' 4 400; Press 'START' 6 1200; Snap 'debug-before'
    Click-Game 150 437 900; Snap 'debug-clicked'
    Press 'ESC' 6 900; Press 'ESC' 6 900; Press 'ESC' 6 900
    # Antialiasing starts at its last option (8x): Enter on it wraps to Off.
    Press 'F1' 6 1200
    for ($i = 0; $i -lt 4; $i++) { Press 'DDOWN' 4 300 }
    Snap 'wrap-before'
    Press 'START' 6 800; Snap 'wrap-after'
    $err = Join-Path $script:HarnessOut "$run.stderr.txt"
    Write-Host "=== $run ==="
    if (Test-Path $err) {
        Select-String -Path $err -Pattern '\[ui\] (screen|press)' | ForEach-Object { Write-Host ("  " + $_.Line) }
    }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Restore-Settings $saved
}
