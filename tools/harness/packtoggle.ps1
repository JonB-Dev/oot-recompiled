# Does changing the Texture pack row while the game runs take the program down?
#
#   tools\harness\packtoggle.ps1 -Tag toggle -Cycles 6
#
# The second half of the user's crash report of 2026-10-07. packstress.ps1 showed that STREAMING a
# pack through eighteen scenes does not crash. What it never does is change the row while the
# renderer is up, and that is the one path where the program called the pack loader from a thread
# other than the graphics thread, which RT64 says it must not ("assumed to be called from the only
# thread that is capable of submitting new textures"). This drives exactly that: the in-game menu
# opened over play, Up seven times (the list wraps through the Quit button and the five link rows
# to the Texture pack row), then the row flipped Off and back to the pack several times with the
# game running behind it. Each flip that reaches the renderer leaves a `[packs]` line in the log, so
# the count of those lines is the proof the row really moved.
#
# A capture is taken after the first Up so the focused row can be checked by eye: a run that flips
# some other row proves nothing, and the script cannot tell the two apart on its own.
#
# THE PERSON'S OWN STATE IS NEVER WRITTEN, exactly as packstress.ps1: the installed copy runs with
# autosave, save on quit and resume on start off, and the settings file is put back afterward.
param(
    [string]$Tag = "toggle",
    [int]$Cycles = 6,
    [int]$SettleSeconds = 8,
    # Eight since 2026-10-07: the Texture detail row shows under the pack row while a pack is on,
    # which it is at the start of the run. Seven was right before that row existed.
    [int]$Ups = 8,
    [string]$Exe = "$env:LOCALAPPDATA\OoT Recompiled\bin\OoTRecompiled.exe"
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'packs'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$data = Split-Path -Parent (Split-Path -Parent $Exe)
$settingsFile = Join-Path $data 'settings.txt'
$log = Join-Path $data 'oot-recompiled.log'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }

$forced = @{ texturepack = '1'; autosave = '0'; saveonquit = '0'; resumeonstart = '0' }
$lines = @()
if ($null -ne $saved) {
    $lines = $saved | Where-Object {
        $line = $_
        -not ($forced.Keys | Where-Object { $line -match "^\s*$_\s*=" })
    }
}
foreach ($k in $forced.Keys) { $lines += "$k = $($forced[$k])" }
Set-Content -Path $settingsFile -Value $lines -Encoding ASCII

$p = $null
$crashedAt = ''
try {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 700
    $p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -ArgumentList @('--play') -PassThru
    Find-Game $p.Id | Out-Null
    Enter-Play
    Start-Sleep -Seconds 4

    # EIGHT Ups from the top, by default. In play the list ends Texture pack, Texture detail (shown
    # while a pack is on), then five link rows (Lighting, Controls, Your files, Moments,
    # Debugging), then the Quit button, and Up wraps from the top to Quit first. Turning the pack
    # Off takes the detail row away BELOW the focus, so the focus stays on the pack row. The first version pressed once and sat on Quit, the second pressed
    # twice and sat on Debugging; both flipped Left and Right on something inert for six cycles
    # and reported a pass that proved nothing. The capture caught both, which is why it stays.
    Press 'F1' 6 1500
    for ($u = 0; $u -lt $Ups; $u++) { Press 'DUP' 5 400 }
    Shot (Join-Path $out "$Tag-focus.png")

    for ($c = 1; $c -le $Cycles; $c++) {
        foreach ($dir in @('DLEFT', 'DRIGHT')) {
            if ($p.HasExited) { $crashedAt = "before cycle $c $dir"; break }
            Press $dir 5 300
            Write-Host ("cycle {0} {1} at {2:HH:mm:ss}" -f $c, $dir, (Get-Date))
            for ($s = 0; $s -lt $SettleSeconds; $s++) {
                if ($p.HasExited) { $crashedAt = "during cycle $c after $dir"; break }
                Start-Sleep -Seconds 1
            }
            if ($crashedAt -ne '') { break }
        }
        if ($crashedAt -ne '') { break }
    }
    if (-not $p.HasExited) {
        Press 'ESC' 6 800
        Shot (Join-Path $out "$Tag-after.png")
    }
}
finally {
    if ($null -ne $p -and -not $p.HasExited) { Close-Game $p.Id | Out-Null }
    Start-Sleep -Seconds 2
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
    if (Test-Path $log) { Copy-Item $log (Join-Path $out "$Tag.log") -Force }
}

Write-Output ("=== {0}: {1} ===" -f $Tag, $(if ($crashedAt -ne '') { "ENDED ON ITS OWN $crashedAt" } else { "survived $Cycles cycles, closed by the script" }))
$copy = Join-Path $out "$Tag.log"
if (Test-Path $copy) {
    Select-String -Path $copy -Pattern '\[packs\]' | ForEach-Object { $_.Line }
    $crash = Select-String -Path $copy -Pattern '=+ CRASH =+' -Context 0, 40
    if ($crash) { $crash | ForEach-Object { $_.Line; $_.Context.PostContext } }
}
