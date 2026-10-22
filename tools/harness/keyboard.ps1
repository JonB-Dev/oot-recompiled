# The on-screen keyboard (the menus plan, phase 110), DRIVEN in the launcher's ROM browser: opened
# with R (the pad's Y), moved over by the layout engine, a key typed, Backspace, the caret moved by
# the shoulders, Back closing it without a change; then opened again and Done pressed, which loads
# the folder typed. Every step is captured under build-cmake\harness\menus\ and the trace's [ui]
# lines printed.
#
#   tools\harness\keyboard.ps1 -Tag p110
param([string]$Tag = "keyboard")

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'menus'
New-Item -ItemType Directory -Force -Path $out | Out-Null
function Snap([string]$name) { Shot (Join-Path $out "$Tag-$name.png") }

try {
    $run = "$Tag-launcher"
    $p = Start-Game $run @() -NoPlay
    Start-Sleep -Seconds 6
    # The ROM browser: up to the ROM row, Enter opens it.
    Press 'DUP' 4 400; Press 'START' 6 1200; Snap 'browse'
    Press 'R' 4 900; Snap 'open'
    Press 'DRIGHT' 4 300; Press 'DRIGHT' 4 300; Press 'DDOWN' 4 300; Snap 'moved'
    Press 'START' 4 500; Snap 'typed'
    Press 'Z' 4 300; Press 'Z' 4 300; Snap 'caret-left'
    Press 'L' 4 300; Press 'L' 4 300; Press 'BACK' 4 500; Snap 'backspace'
    Press 'ESC' 4 900; Snap 'closed'
    # Again, straight down to Done: the folder shown is loaded as it was.
    Press 'R' 4 900
    for ($i = 0; $i -lt 5; $i++) { Press 'DDOWN' 4 250 }
    Press 'DRIGHT' 4 300; Snap 'on-done'
    Press 'START' 4 1200; Snap 'done'
    $err = Join-Path $script:HarnessOut "$run.stderr.txt"
    Write-Host "=== $run ==="
    if (Test-Path $err) {
        Select-String -Path $err -Pattern '\[ui\] (screen|page|keyboard)' | ForEach-Object { Write-Host ("  " + $_.Line) }
    }
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1

    # The same on a pad (a virtual one; SDL's buttons: A 0, B 1, Y 3, D-pad up 11, right 14): Y
    # opens the keyboard, the footer in the pad's terms, A types the key, B closes it.
    $run = "$Tag-pad"
    $scriptPath = Join-Path $env:TEMP "oot-$Tag.virtual.txt"
    @('8000 button 11 1', '8150 button 11 0', '9000 button 0 1', '9150 button 0 0',
      '10500 button 3 1', '10650 button 3 0', '12000 button 14 1', '12150 button 14 0',
      '12600 button 0 1', '12750 button 0 0', '14500 button 1 1', '14650 button 1 0') |
        Set-Content -Encoding ascii $scriptPath
    $p = Start-Game $run @('--virtual-controller', $scriptPath) -NoPlay
    Start-Sleep -Milliseconds 11500; Snap 'pad-open'
    Start-Sleep -Milliseconds 2000; Snap 'pad-typed'
    Start-Sleep -Milliseconds 2000; Snap 'pad-closed'
    $err = Join-Path $script:HarnessOut "$run.stderr.txt"
    Write-Host "=== $run ==="
    if (Test-Path $err) {
        Select-String -Path $err -Pattern '\[ui\] (screen|page|keyboard)|controller added' | ForEach-Object { Write-Host ("  " + $_.Line) }
    }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
}
