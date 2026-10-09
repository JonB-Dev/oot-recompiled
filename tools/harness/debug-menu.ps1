# debug-menu.ps1: the Debugging menu, its warning, and the recorder's second warning.
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\debug-menu.ps1
#
# The user's ask of 2026-09-26: everything meant for development under one menu item, a
# confirmation on the way in saying these are for debugging and development only, and a SECOND
# confirmation when the frame recorder is switched on, naming the space it eats.
#
# Walks there the way a person does, with keys, and shoots each surface:
#   1. the settings menu, to show the Debugging row is there and the recorder rows are NOT
#   2. the confirmation raised by pressing Enter on it
#   3. the Debugging menu itself, with the warps, the recorder, its length, the two lighting
#      inspection rows and the sweep
#   4. the recorder's own confirmation, with this machine's resolution and frame rate in it
#
# Shots go to build-cmake\harness\debug-menu-*.png. Refuses to start while the program is running.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'drive.ps1')

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output 'DEBUG_MENU_FAIL the program is already running'
    exit 1
}

$out = Join-Path $script:HarnessOut 'debug-menu'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$p = Start-Game 'debug-menu' @('--warp', '0x00CD,0x00CD')
Enter-Play | Out-Null

# The menu, then UP to reach the rows at the bottom of the list: Debugging sits just above Quit,
# so two Ups from the top is fewer presses than walking the whole list down.
Press 'F1' 6 1500
Shot (Join-Path $out 'debug-menu-1-settings.png')

Press 'DUP' 4 300
Press 'DUP' 4 500
Shot (Join-Path $out 'debug-menu-2-row-focused.png')

Press 'START' 6 1200
Shot (Join-Path $out 'debug-menu-3-warning.png')

# Right is Yes on the question, the same as Enter.
Press 'START' 6 1200
Shot (Join-Path $out 'debug-menu-4-menu.png')

# Down to the Frame recorder row (Warps is first), then Right to switch it on, which must raise
# the second warning rather than switching anything on.
Press 'DDOWN' 4 400
Shot (Join-Path $out 'debug-menu-5-recorder-row.png')
Press 'DRIGHT' 4 1200
Shot (Join-Path $out 'debug-menu-6-recorder-warning.png')

# Decline it, and the row must still read Off.
Press 'DLEFT' 4 1000
Shot (Join-Path $out 'debug-menu-7-declined.png')

if (-not $p.HasExited) { Close-Game $p.Id | Out-Null }
Start-Sleep -Seconds 2

# The recorder must NOT have been switched on by a declined warning.
$settings = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
if (Test-Path $settings) {
    $line = (Get-Content $settings | Where-Object { $_ -match '^recorder\s*=' })
    Write-Output "settings file says: $line"
    if ($line -notmatch '=\s*0\s*$') {
        Write-Output 'DEBUG_MENU_FAIL the recorder was switched on by a warning that was declined'
        exit 1
    }
}

Write-Output "DEBUG_MENU_OK shots in $out"
