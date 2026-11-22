# Motion in the menus (the menus plan, phase 114), DRIVEN: captures taken during the menu opening
# over the game, a turn of the dial, a walk down the Settings list that scrolls it, and the menu
# closing, each as soon after the press as the harness can take one, so a fade or a scroll caught
# part way shows as a frame between the two states rather than a jump.
#
#   tools\harness\menus-motion.ps1 -Tag p114 -Settings "menu=2"
param([string]$Tag = "motion", [string]$Settings = "menu=2")

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
    # Each press followed at once by a burst of back to back grabs (drive.ps1's ShotBurst), so the
    # frames of a fade or a scroll are caught between the two states.
    function Burst([string]$name) {
        $dir = Join-Path $out "$Tag-$name"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $ms = [Drv]::ShotBurst($script:hwnd, $dir, 10, $false)
        Write-Host ("burst {0}: 10 grabs in {1:F0} ms" -f $name, $ms)
    }
    Focus-Game
    Press 'F1' 2 0; Burst 'open'; Start-Sleep -Milliseconds 600; Snap 'open-c'
    Press 'L' 2 0; Burst 'turn'; Start-Sleep -Milliseconds 600; Snap 'turn-b'
    Press 'Z' 3 700
    for ($i = 0; $i -lt 5; $i++) { Press 'DDOWN' 3 120 }
    Press 'DDOWN' 2 0; Burst 'scroll'; Start-Sleep -Milliseconds 600; Snap 'scroll-b'
    Press 'ESC' 2 0; Burst 'close'; Start-Sleep -Milliseconds 600; Snap 'close-b'
    $err = Join-Path $script:HarnessOut "$run.stderr.txt"
    Write-Host "=== $run ==="
    if (Test-Path $err) {
        Select-String -Path $err -Pattern '\[ui\] (screen|interface)' | ForEach-Object { Write-Host ("  " + $_.Line) }
    }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Restore-Settings $saved
}
