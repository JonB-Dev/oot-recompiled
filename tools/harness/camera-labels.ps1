# camera-labels.ps1: the Camera distance row says what it means.
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\camera-labels.ps1
#
# The user, 2026-09-26: "one of the camera settings marks 'original' in menu but isn't actually
# what's in the original game so this is a lie."
#
# It was true. The row read { Near, Original, Medium, Far } and the console's own distance was the
# step called Near (patches/free_camera.c, sDistanceScale[0] = 1.0). It now reads
# { Original, A little further, Further, Furthest } with the scales untouched.
#
# This walks to the row and shoots it at its first option, which is the one whose name was the lie,
# and then steps once to prove the second option is the one that used to be called Original.
# The scales are NOT checked here: they are unchanged, and the defect was never in the numbers.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'drive.ps1')

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output 'CAMERA_LABELS_FAIL the program is already running'
    exit 1
}

$out = Join-Path $script:HarnessOut 'camera-labels'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$p = Start-Game 'camera-labels' @('--warp', '0x00CD,0x00CD')
Enter-Play | Out-Null

# The menu, then up to reach the rows near the bottom of the list. Camera distance sits a few above
# Menu placement; walking up from the top is fewer presses than walking down the whole list.
Press 'F1' 6 1500
for ($i = 0; $i -lt 7; $i++) { Press 'DUP' 4 200 }
Shot (Join-Path $out 'camera-labels-1-row.png')

# One step right, which moves off the first option.
Press 'DRIGHT' 4 700
Shot (Join-Path $out 'camera-labels-2-stepped.png')

# And back, so nothing is left changed for the person whose machine this is.
Press 'DLEFT' 4 700
Shot (Join-Path $out 'camera-labels-3-back.png')

Press 'ESC' 6 800
if (-not $p.HasExited) { Close-Game $p.Id | Out-Null }
Start-Sleep -Seconds 2

# The settings file is the durable record of what the row is set to; it must be back at 0.
$settings = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
if (Test-Path $settings) {
    $line = (Get-Content $settings | Where-Object { $_ -match '^cameradistance\s*=' })
    Write-Output "settings file says: $line"
    if ($line -notmatch '=\s*0\s*$') {
        Write-Output 'CAMERA_LABELS_FAIL the walk left the camera distance changed'
        exit 1
    }
}

Write-Output "CAMERA_LABELS_OK shots in $out"
