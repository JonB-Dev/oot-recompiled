# Does the traced lighting stop where a texture pack's drawn edge stops?
#
#   tools\harness\packedge.ps1 -Tag edge
#
# The user, 2026-10-07, two frames of a Hyrule Field cliff at noon with the pack on: the rock is
# drawn well above the line where the Shadows view says the surface ends, and that line is jagged.
# Patch 0024 makes the traced alpha test read the replacement at the coordinate the raster reads;
# the Experiment row's 34 puts the old raw coordinate back. This takes the ordinary picture and the
# Shadows view at Experiment Off and at 34, from the same frozen frame of the same arrival, so the
# edge can be compared in place rather than argued about.
#
# THE ARRIVAL IS abshots.ps1's, for the same reasons: the entrance is given twice (a launch warp
# lapses in the intro and the queued copy is taken after the dwell), the game's random numbers are
# seeded, the clock is set to noon on arrival, and the game freezes itself a fixed number of updates
# after arriving (the update stops, the draw runs on), so every capture is the same frame.
# `--rt-debug shadow` is the Shadows view. EXPERIMENT HAS NO FLAG and is reset at every launch on
# purpose (ui_lighting.cpp: a session must not open on a leftover rung), so for 34 the Debugging
# menu is driven by keyboard over the frozen frame, and a capture of the menu proves the row moved.
#
# Runs the INSTALLED copy (the pack's path is short there; see texturepack.ps1) with autosave, save
# on quit and resume on start off, and puts the settings and lighting files back exactly as they
# were, crash or not.
# Its streams are redirected to files here, which the program keeps, so the trace can be read live.
param(
    [string]$Tag = "edge",
    [string]$Entrance = "0x00CD",
    [uint32]$FreezeTicks = 100,
    [string]$Clock = "0x8000",
    # Which pack, as the row counts them (1 is the first in the folder's sorted list), and the
    # detail row's step (0 Quarter, 1 Half, 2 Full since 2026-10-08). -PictureOnly takes just the ordinary picture,
    # for comparing the same frozen frame across pack formats (2026-10-07: PNG folder, DDS folder,
    # .rtz).
    [int]$Pack = 1,
    [int]$Detail = 2,
    [switch]$PictureOnly,
    [string]$Exe = "$env:LOCALAPPDATA\OoT Recompiled\bin\OoTRecompiled.exe"
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'packs'
New-Item -ItemType Directory -Force -Path $out | Out-Null

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "PACKEDGE_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}

$data = Split-Path -Parent (Split-Path -Parent $Exe)
$settingsFile = Join-Path $data 'settings.txt'
$savedSettings = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
# The lighting file too: setting a rung through the Debugging menu SAVES it (the menu says "Saved
# as you change it"), and the first version of this script left the person's file on rung 34.
$lightingFile = Join-Path $data 'lighting.txt'
$savedLighting = if (Test-Path $lightingFile) { @(Get-Content $lightingFile) } else { $null }

function Take {
    param([string]$name, [string[]]$extra, [int]$experiment)
    $err = Join-Path $out "$Tag-$name.stderr.txt"
    $std = Join-Path $out "$Tag-$name.stdout.txt"
    $launch = @('--play', '--warp', "$Entrance,$Entrance", '--dwell', '100', '--seed', '1',
                '--freeze', "$Entrance,$FreezeTicks", '--freeze-clock', $Clock) + $extra
    $p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -ArgumentList $launch `
            -RedirectStandardOutput $std -RedirectStandardError $err -PassThru
    try {
        Find-Game $p.Id | Out-Null
        Enter-Play
        $frozen = $false
        for ($i = 0; $i -lt 120; $i++) {
            if ($p.HasExited) { break }
            $trace = Get-Content $err -ErrorAction SilentlyContinue
            if ($trace | Where-Object { $_ -match '^\[shot\] frozen' }) { $frozen = $true; break }
            Start-Sleep -Seconds 1
        }
        if (-not $frozen) { Write-Output "$name : never froze, capture not taken"; return }
        Start-Sleep -Seconds 1
        if ($experiment -gt 0) {
            # In play the settings list ends Debugging then Quit, so two Ups from the top; Enter
            # asks the warning question and Enter again answers Yes (debug-menu.ps1 does the same;
            # the first run of this script skipped it, its Downs went to the question and its first
            # Right answered it, so the rest stepped nothing); Experiment is the sixth row (Warps,
            # Time of day, Frame recorder, Recording length, Inspect one pass, Experiment); Right
            # steps it, one press per rung from Off.
            Press 'F1' 6 1500
            Press 'DUP' 5 400
            Press 'DUP' 5 400
            Press 'START' 6 1200
            Press 'START' 6 1200
            for ($d = 0; $d -lt 5; $d++) { Press 'DDOWN' 5 300 }
            for ($r = 0; $r -lt $experiment; $r++) { Press 'DRIGHT' 4 120 }
            Start-Sleep -Milliseconds 600
            Shot (Join-Path $out "$Tag-$name-menu.png")
            # Escape goes back from Debugging to Settings, and Escape closes Settings. The capture
            # after shows whether the menu really went.
            Press 'ESC' 6 800
            Press 'ESC' 6 800
            Start-Sleep -Seconds 2
        }
        Shot (Join-Path $out "$Tag-$name.png")
    }
    finally {
        if (-not $p.HasExited) { Close-Game $p.Id | Out-Null }
        Start-Sleep -Seconds 2
    }
}

try {
    $lines = @()
    $forced = @{ texturepack = "$Pack"; texturedetail = "$Detail"; autosave = '0'; saveonquit = '0'; resumeonstart = '0' }
    if ($null -ne $savedSettings) {
        $lines = $savedSettings | Where-Object {
            $line = $_
            -not ($forced.Keys | Where-Object { $line -match "^\s*$_\s*=" })
        }
    }
    foreach ($k in $forced.Keys) { $lines += "$k = $($forced[$k])" }
    Set-Content -Path $settingsFile -Value $lines -Encoding ASCII

    Take 'picture' @() 0
    if (-not $PictureOnly) {
        Take 'shadows-fixed' @('--rt-debug', 'shadow') 0
        Take 'shadows-old' @('--rt-debug', 'shadow') 34
    }
}
finally {
    if ($null -ne $savedSettings) { Set-Content -Path $settingsFile -Value $savedSettings -Encoding ASCII }
    if ($null -ne $savedLighting) { Set-Content -Path $lightingFile -Value $savedLighting -Encoding ASCII }
}
Write-Output "=== $Tag : captures under $out ($Tag-picture, $Tag-shadows-fixed, $Tag-shadows-old, and the menu) ==="
