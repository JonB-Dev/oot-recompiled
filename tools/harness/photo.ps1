# Photo mode (2026-10-09, src/main/photo.h): freeze, hold, move the camera, put it back, release.
#
#   tools\harness\photo.ps1 -Tag photo
#   tools\harness\photo.ps1 -Tag photo-field -Warp 0x0EE   # somewhere built of geometry
#
# Turns the Photo mode row on in the build tree's settings, enters play, and captures: the game
# moving, frozen, frozen again a few seconds later (the same frame), the camera moved with the
# stick, turned with the C buttons, raised with R, put back with B, and the game moving again after
# release. The probe file's audio columns (samples a second) show the sound stopping while frozen.
param([string]$Tag = "photo", [string]$Warp = "")

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'photo'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$settingsFile = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
Set-Setting 'photomode' '1'

try {
    # A launch warp lapses in the intro, so it is queued twice and taken after the dwell, the way
    # abshots.ps1 does; the extra wait is the dwell and the transition.
    $extra = if ($Warp -ne '') { @('--warp', "$Warp,$Warp", '--dwell', '100') } else { @() }
    $p = Start-Game $Tag $extra
    Enter-Play
    Start-Sleep -Seconds $(if ($Warp -ne '') { 12 } else { 3 })
    Shot (Join-Path $out "$Tag-1-moving.png")
    Press 'F10' 4 1500
    Shot (Join-Path $out "$Tag-2-frozen.png")
    Start-Sleep -Seconds 4
    Shot (Join-Path $out "$Tag-3-frozen-later.png")
    Hold 'SUP' 1500
    Shot (Join-Path $out "$Tag-4-moved.png")
    Hold 'CLEFT' 1200
    Shot (Join-Path $out "$Tag-5-turned.png")
    Hold 'R' 1000
    Shot (Join-Path $out "$Tag-6-raised.png")
    Press 'B' 4 1200
    Shot (Join-Path $out "$Tag-7-back.png")
    # The HUD hidden and shown again (Show or hide the HUD, H by default).
    Press 'H' 4 1200
    Shot (Join-Path $out "$Tag-7b-hud-hidden.png")
    Press 'H' 4 1200
    Shot (Join-Path $out "$Tag-7c-hud-shown.png")
    Press 'F10' 4 2500
    Shot (Join-Path $out "$Tag-8-released.png")
    Write-Host "=== photo and shot lines ==="
    $err = Join-Path $script:HarnessOut "$Tag.stderr.txt"
    if (Test-Path $err) { Select-String -Path $err -Pattern '\[photo\]|\[shot\]|toast' | ForEach-Object { $_.Line } }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
}
