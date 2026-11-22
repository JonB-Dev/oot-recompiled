# The video recorder's whole window (2026-10-09, src/main/video.h, Video shows).
#
#   tools\harness\video-window.ps1 -Tag video-window
#
# Records with Video shows on the whole window, at 1080p from the harness's 960 by 720 window (so
# the encoder scales), opens the settings menu partway through and closes it, and prints the
# recorder's lines. The video lands in the person's Videos folder like any other; the caller
# inspects it and removes it.
param([string]$Tag = "video-window")

. (Join-Path $PSScriptRoot 'drive.ps1')
$settingsFile = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
Set-Setting 'videoshows' '1'
Set-Setting 'videosize' '2'

try {
    # Start once in play, stop twenty seconds later, in game updates.
    $p = Start-Game $Tag @('--video', '40,440')
    Enter-Play
    Start-Sleep -Seconds 2
    Press 'F1' 6 3000
    Press 'F1' 6 2000
    Start-Sleep -Seconds 14
    Write-Host "=== video lines ==="
    $err = Join-Path $script:HarnessOut "$Tag.stderr.txt"
    if (Test-Path $err) { Select-String -Path $err -Pattern '\[video\]' | ForEach-Object { $_.Line } }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
}
