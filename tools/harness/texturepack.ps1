# Does a texture pack in the person's own folder actually reach the renderer, and does the picture
# change when it does?
#
#   tools\harness\texturepack.ps1 -Tag on  -Pack 1
#   tools\harness\texturepack.ps1 -Tag off -Pack 0
#
# The verdict is the `[packs]` line in the log, which comes from the renderer itself: `loaded`,
# `REFUSED <why>`, or `none`. The capture is for the half no script can judge, which is whether the
# replaced textures look right; take one with the pack on and one with it off and compare them.
#
# IT RUNS THE INSTALLED COPY, not the build tree, and that is deliberate. The pack lives under the
# installed data folder, whose path is short; the build tree sits under a long project path, and
# the pack's own directories are deep enough that the two together pass MAX_PATH, at which point
# the renderer's directory walk throws. The program survives that now (it refuses the pack and
# says so) but it is not what a person's machine does, so the check runs where they run.
param(
    [string]$Tag = "packs",
    [int]$Pack = 1,
    [int]$PlaySeconds = 25,
    [string]$Exe = "$env:LOCALAPPDATA\OoT Recompiled\bin\OoTRecompiled.exe"
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'packs'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$data = Split-Path -Parent (Split-Path -Parent $Exe)
$settingsFile = Join-Path $data 'settings.txt'
$log = Join-Path $data 'oot-recompiled.log'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }

# The row, written straight into the installed copy's own settings file.
$lines = @()
if ($null -ne $saved) { $lines = $saved | Where-Object { $_ -notmatch '^\s*texturepack\s*=' } }
$lines += "texturepack = $Pack"
Set-Content -Path $settingsFile -Value $lines -Encoding ASCII

if (Test-Path $log) { Remove-Item $log -Force -ErrorAction SilentlyContinue }

$p = $null
try {
    $p = Start-Process -FilePath $Exe -ArgumentList '--play' -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
    Find-Game $p.Id | Out-Null
    Enter-Play
    Start-Sleep -Seconds $PlaySeconds
    Shot (Join-Path $out "$Tag.png")
}
finally {
    if ($null -ne $p) { Close-Game $p.Id | Out-Null }
    Start-Sleep -Seconds 2
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
}

Write-Output "=== what the renderer said about the pack ==="
if (Test-Path $log) {
    Select-String -Path $log -Pattern '\[packs\]' | ForEach-Object { $_.Line }
}
else {
    Write-Output "no log at $log"
}
