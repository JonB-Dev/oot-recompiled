# Open the settings document over the game and capture it. The check for any change to the
# settings surface: does the row exist, does it show the value the file holds, does the file
# round-trip.
#
#   tools\harness\settings-shot.ps1 -Tag rows -Settings "framerate=1"
#   tools\harness\settings-shot.ps1 -Tag p50-controls -Open controls
#
# Writes the settings keys given, launches, captures the document, then reads the file back and
# prints it, so "the row shows the edited value" and "garbage clamps" are both visible in one run.
#
# With -Open controls it goes on from the settings document to the controls document THROUGH the
# settings document's own Controls row (down past every setting, Enter), captures that, moves the
# focus down the list and picks the second slot for a second capture, and closes it with Escape.
param([string]$Tag = "settings", [string]$Settings = "", [string]$Open = "")

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'settings'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$settingsFile = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
if ($Settings -ne "") {
    foreach ($kv in $Settings.Split(',')) {
        $pair = $kv.Split('=')
        if ($pair.Count -eq 2) { Set-Setting $pair[0].Trim() $pair[1].Trim() }
    }
}

try {
    $p = Start-Game $Tag @()
    Enter-Play
    Start-Sleep -Seconds 4
    Press 'F1' 6 1500
    if ($Open -eq 'controls') {
        # Ten settings rows (the render distance row since 2026-09-20), then the Controls row;
        # Enter opens the document.
        for ($i = 0; $i -lt 10; $i++) { Press 'DDOWN' 5 200 }
        Press 'START' 6 1500
        Shot (Join-Path $out "$Tag.png")
        for ($i = 0; $i -lt 6; $i++) { Press 'DDOWN' 5 200 }
        Press 'DRIGHT' 5 400
        Shot (Join-Path $out "$Tag-moved.png")
        Press 'ESC' 6 800
    }
    else {
        Shot (Join-Path $out "$Tag.png")
        # Move down through every row so the capture is not the only evidence the rows respond.
        for ($i = 0; $i -lt 10; $i++) { Press 'DDOWN' 5 250 }
        Press 'F1' 6 800
    }
    Start-Sleep -Seconds 1
    Write-Host "=== settings.txt as the game wrote it back ==="
    if (Test-Path $settingsFile) { Get-Content $settingsFile } else { Write-Host "(no file)" }
    $err = Join-Path $script:HarnessOut "$Tag.stderr.txt"
    Write-Host "=== renderer and interface lines ==="
    if (Test-Path $err) { Select-String -Path $err -Pattern '\[gfx\]|\[ui\]' | ForEach-Object { $_.Line } }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
    elseif (Test-Path $settingsFile) { Remove-Item -Force $settingsFile }
}
