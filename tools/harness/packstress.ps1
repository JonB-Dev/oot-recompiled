# Does a texture pack survive play, and what does it cost while it does?
#
#   tools\harness\packstress.ps1 -Tag on  -Pack 1
#   tools\harness\packstress.ps1 -Tag off -Pack 0
#
# The user, 2026-10-07: "The texture pack loading is actually causing the game to crash." The pack
# STREAMS (its rt64.json sets no default operation), so the cost arrives scene by scene rather than
# at the start. This warps through a list of scenes with the pack on, which is what makes the
# renderer stream a new set of textures each time, and samples the process and the GPU every few
# seconds. A run that ends before the script closes it is a crash, and the log's CRASH block says
# where. The off run is the baseline the on run is read against.
#
# IT RUNS THE INSTALLED COPY, for the reason texturepack.ps1 gives: the pack lives under the
# installed data folder, whose path is short enough for the pack's deep directories.
#
# THE PERSON'S OWN STATE IS NEVER WRITTEN. The installed copy is the one they play, so for the run
# the settings file turns off the three things that would write a moment (autosave, save on quit,
# resume on start), and the file is put back exactly as it was afterward, crash or not. Nothing
# here saves in game.
param(
    [string]$Tag = "packstress",
    [int]$Pack = 1,
    # The Texture detail row: 0 Quarter, 1 Half, 2 Full (lightest first since 2026-10-08). -1 takes
    # the row out of the file for the run, so the program's own default is what loads (Quarter
    # since 2026-10-08, for a machine that has never set it).
    [int]$Detail = 2,
    # Extra keys for this run, "key=value,key=value": -Settings for settings.txt (downsampling,
    # antialiasing, resolution), -Lighting for lighting.txt (raytracing). Both files go back
    # exactly as they were afterward.
    [string]$Settings = "",
    [string]$Lighting = "",
    [string]$Entrances = "0x0CD,0x0EE,0x0000,0x040F,0x11E,0x0B1,0x07A,0x0DB,0x13D,0x004,0x108,0x028,0x053,0x169,0x165,0x010,0x037,0x082",
    [int]$DwellSeconds = 20,
    [int]$SampleSeconds = 3,
    [string]$Exe = "$env:LOCALAPPDATA\OoT Recompiled\bin\OoTRecompiled.exe"
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'packs'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$data = Split-Path -Parent (Split-Path -Parent $Exe)
$settingsFile = Join-Path $data 'settings.txt'
$log = Join-Path $data 'oot-recompiled.log'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
$lightingFile = Join-Path $data 'lighting.txt'
$savedLighting = if (Test-Path $lightingFile) { @(Get-Content $lightingFile) } else { $null }

function Write-Keys {
    param([string]$file, $original, [hashtable]$forced)
    $lines = @()
    if ($null -ne $original) {
        $lines = $original | Where-Object {
            $line = $_
            -not ($forced.Keys | Where-Object { $line -match "^\s*$_\s*=" })
        }
    }
    foreach ($k in $forced.Keys) { $lines += "$k = $($forced[$k])" }
    Set-Content -Path $file -Value $lines -Encoding ASCII
}

function ConvertFrom-Spec {
    param([string]$spec)
    $h = @{}
    foreach ($kv in $spec.Split(',')) {
        $pair = $kv.Split('=')
        if ($pair.Count -eq 2 -and $pair[0].Trim() -ne '') { $h[$pair[0].Trim()] = $pair[1].Trim() }
    }
    return $h
}

$forced = @{ texturepack = "$Pack"; texturedetail = "$Detail"; autosave = '0'; saveonquit = '0'; resumeonstart = '0' }
# The run's copy only: $saved is what goes back afterward, untouched.
$base = $saved
if ($Detail -lt 0) {
    $forced.Remove('texturedetail')
    if ($null -ne $saved) { $base = @($saved | Where-Object { $_ -notmatch '^\s*texturedetail\s*=' }) }
}
$extra = ConvertFrom-Spec $Settings
foreach ($k in $extra.Keys) { $forced[$k] = $extra[$k] }
Write-Keys $settingsFile $base $forced
if ($Lighting -ne "") { Write-Keys $lightingFile $savedLighting (ConvertFrom-Spec $Lighting) }

$count = @($Entrances.Split(',') | Where-Object { $_.Trim() -ne '' }).Count
$dwellFrames = $DwellSeconds * 20
$samples = Join-Path $out "$Tag-samples.csv"
"time,seconds,private_mb,working_set_mb,game_gpu_dedicated_mb,game_gpu_shared_mb,card_used_mb,alive" | Set-Content -Path $samples -Encoding ASCII
# The probe's once a second line (main/probe.h), for the frames actually presented each second:
# the resolution plan of 2026-10-07 is about memory, and what a person feels first is speed.
$probe = Join-Path $out "$Tag-probe.txt"
if (Test-Path $probe) { Remove-Item $probe -Force }

# THE GAME'S OWN VIDEO MEMORY, NOT THE CARD'S (corrected 2026-10-07). The first version recorded
# nvidia-smi's memory.used, which is every process on the desktop together, and reported it as the
# game's: about 5.7 GB of the "9 GB with the pack off" was other programs, which the user rightly
# did not believe. Windows' GPU Process Memory counter is per process; its Dedicated Usage is the
# video memory the game holds. The card total is still written down, under its own name.
function Get-GameGpu {
    param([int]$procId)
    $dedicated = 0; $shared = 0
    try {
        $set = Get-Counter -Counter @("\GPU Process Memory(pid_$($procId)_*)\Dedicated Usage",
                                      "\GPU Process Memory(pid_$($procId)_*)\Shared Usage") -ErrorAction Stop
        foreach ($s in $set.CounterSamples) {
            if ($s.Path -like '*dedicated usage') { $dedicated += $s.CookedValue } else { $shared += $s.CookedValue }
        }
    }
    catch { }
    return @([int]($dedicated / 1MB), [int]($shared / 1MB))
}

$p = $null
$crashed = $false
$peakPrivate = 0
$peakGpu = 0
$peakCard = 0
try {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 700
    $p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) `
            -ArgumentList @('--play', '--warp', $Entrances, '--dwell', "$dwellFrames", '--probe', $probe) -PassThru
    Find-Game $p.Id | Out-Null
    Enter-Play

    # Every scene's dwell, the arrival before the first, and some slack.
    $budget = ($count * $DwellSeconds) + 40
    $start = Get-Date
    while (((Get-Date) - $start).TotalSeconds -lt $budget) {
        $alive = -not $p.HasExited
        $priv = 0; $ws = 0; $game = @(0, 0)
        if ($alive) {
            $q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
            if ($q) { $priv = [int]($q.PrivateMemorySize64 / 1MB); $ws = [int]($q.WorkingSet64 / 1MB) }
            $game = Get-GameGpu $p.Id
        }
        $card = ((nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits) -join '').Trim()
        $secs = [int]((Get-Date) - $start).TotalSeconds
        "{0:HH:mm:ss},{1},{2},{3},{4},{5},{6},{7}" -f (Get-Date), $secs, $priv, $ws, $game[0], $game[1], $card, $alive |
            Add-Content -Path $samples -Encoding ASCII
        if ($priv -gt $peakPrivate) { $peakPrivate = $priv }
        if ($game[0] -gt $peakGpu) { $peakGpu = $game[0] }
        if ([int]$card -gt $peakCard) { $peakCard = [int]$card }
        if (-not $alive) { $crashed = $true; break }
        Start-Sleep -Seconds $SampleSeconds
    }
}
finally {
    if ($null -ne $p -and -not $p.HasExited) { Close-Game $p.Id | Out-Null }
    Start-Sleep -Seconds 2
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
    if ($null -ne $savedLighting) { Set-Content -Path $lightingFile -Value $savedLighting -Encoding ASCII }
    if (Test-Path $log) { Copy-Item $log (Join-Path $out "$Tag.log") -Force }
}

Write-Output ("=== {0}: pack {1}, detail {2}, {3} scenes, {4} ===" -f $Tag, $Pack, $Detail, $count, $(if ($crashed) { 'ENDED ON ITS OWN (crash)' } else { 'survived, closed by the script' }))
Write-Output ("peak: the game's own video memory {0} MB, the whole card {1} MB (every program), the game's private memory {2} MB; samples in {3}" -f $peakGpu, $peakCard, $peakPrivate, $samples)
$copy = Join-Path $out "$Tag.log"
if (Test-Path $copy) {
    Select-String -Path $copy -Pattern '\[packs\]|\[state\] entrance|\[gfx\] drawing' | Select-Object -Last 30 | ForEach-Object { $_.Line }
    $crash = Select-String -Path $copy -Pattern '=+ CRASH =+' -Context 0, 40
    if ($crash) { $crash | ForEach-Object { $_.Line; $_.Context.PostContext } }
}

# Frames presented per second, column 26 of the probe's line, over the seconds the game was in
# play (the first twenty are the boot and the intro's warp, and are left out).
if (Test-Path $probe) {
    $rates = @(Get-Content $probe | Where-Object { $_ -notmatch '^#' } | ForEach-Object { ($_ -split ' ')[25] } |
               Where-Object { $_ -match '^\d+$' } | ForEach-Object { [int]$_ } | Select-Object -Skip 20)
    if ($rates.Count -gt 0) {
        $sorted = $rates | Sort-Object
        Write-Output ("presented per second: median {0}, lowest {1}, over {2} seconds" -f $sorted[[int]($sorted.Count / 2)], $sorted[0], $rates.Count)
    }
}
