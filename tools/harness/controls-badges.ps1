# The controls screen's badges (2026-10-09, ui_controls.h, row_badge): with Photo mode On and
# Screenshots and Video recording Off, the Photo mode binding has no badge and Screenshot, Record
# video and Pause video each say which row they need.
#
#   tools\harness\controls-badges.ps1 -Tag controls-badges
param([string]$Tag = "controls-badges")

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'settings'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$settingsFile = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
Set-Setting 'photomode' '1'
Set-Setting 'screenshots' '0'
Set-Setting 'videorecording' '0'
Set-Setting 'menu' '1'

try {
    $p = Start-Game $Tag @()
    Enter-Play
    Start-Sleep -Seconds 3
    Press 'F3' 6 1500
    # Down to the last bindings, which are the program's own.
    for ($i = 0; $i -lt 26; $i++) { Press 'DDOWN' 4 150 }
    Shot (Join-Path $out "$Tag.png")
    Press 'ESC' 6 800
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
}
