# Does a prerendered room hold the console's 4:3, and does an ordinary room get the wide view
# back? The check for patches/prerender_aspect.c.
#
#   tools\harness\prerender.ps1 -Tag pr1
#
# The user, 2026-09-20: "during the fixed width scenes where it kind of draws the background,
# such as right as soon as you enter High Rules Castle area or things like inside of Link's
# house, the larger aspect ratio is stretching it out ... it actually needs to force these to
# maintain a four x three ratio."
#
# THE RUN. The aspect row is set to expand and the interface row to follow, which is the
# configuration the bug appears in, and then the game is warped to a prerendered room and to an
# ordinary one in the same run. Two things are read, and both are needed:
#
#   the trace    `[gfx] prerendered room entered` and `... left`, which say the game decided
#                correctly, and the `[gfx] refresh rate = ..., aspect = ...` line that follows
#                each, which says the decision reached the renderer
#   the captures the picture itself, which is the only thing that can show the backdrop no
#                longer stretched and the pillarbox at the sides
#
# THE SWITCH BACK MATTERS AS MUCH AS THE SWITCH. A fix that forces 4:3 and never releases it
# would pass a test that only looked at the first room, and would be a worse bug than the one it
# replaced.
param(
    [string]$Tag = "prerender",
    # Link's house, which is where this save starts, and Kokiri Forest, which is ordinary
    # geometry. Both are reached from the save without fighting anything.
    [string]$Prerendered = "0x0BB",
    # Empty visits only the prerendered room. THE DWELL IS COUNTED IN GAME FRAMES AND IT RUNS
    # DURING THE MENUS TOO, where the game updates sixty times a second rather than twenty, so a
    # dwell meant to hold a scene for forty seconds of play expires before play begins and the
    # second warp fires the moment Link appears. The first run of this script lost its prerendered
    # capture that way: the trace showed the room entered and left within a few frames and the
    # picture was captured after the view had already gone back. So the switch ON is proven in a
    # run with ONE warp, and the switch OFF in a run with both.
    [string]$Ordinary = "",
    [int]$Dwell = 3000,
    [int]$Width = 1280,
    [int]$Height = 720
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'prerender'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$settingsFile = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
Set-Setting 'window' 0
Set-Setting 'aspect' 1
Set-Setting 'hud' 2
Set-Setting 'framerate' 1
Set-Setting 'counter' 1

$errFile = Join-Path $script:HarnessOut "$Tag.stderr.txt"

function Read-Live {
    param([string]$path)
    if (-not (Test-Path $path)) { return @() }
    $tmp = Join-Path $env:TEMP ("live-" + [System.IO.Path]::GetFileName($path))
    try {
        Copy-Item -Path $path -Destination $tmp -Force -ErrorAction Stop
        return @(Get-Content $tmp -ErrorAction Stop)
    }
    catch { return @() }
}

function Wait-ForEntrance {
    param([string]$entrance, [int]$timeoutSec = 90)
    $want = "0x{0:X4}" -f [Convert]::ToInt32($entrance, 16)
    for ($i = 0; $i -lt ($timeoutSec * 2); $i++) {
        $lines = Read-Live $errFile
        for ($j = $lines.Count - 1; $j -ge 0 -and $j -gt $lines.Count - 300; $j--) {
            if ($lines[$j].Contains('[state] entrance') -and $lines[$j].Contains('mode 0') -and $lines[$j].Contains($want)) {
                Write-Host "reached: $($lines[$j])"
                return $true
            }
        }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "NEVER REACHED $want"
    return $false
}

try {
    # Both warps queued at once, with a long dwell, so the run visits the prerendered room first
    # and then the ordinary one without needing Link to walk anywhere.
    $warps = if ($Ordinary -ne "") { "$Prerendered,$Ordinary" } else { $Prerendered }
    Start-Game $Tag @('--warp', $warps, '--dwell', "$Dwell") -width $Width -height $Height | Out-Null
    Enter-Play

    Wait-ForEntrance $Prerendered | Out-Null
    Start-Sleep -Seconds 5
    Shot (Join-Path $out "$Tag-prerendered.png")

    if ($Ordinary -ne "") {
        Wait-ForEntrance $Ordinary | Out-Null
        Start-Sleep -Seconds 5
        Shot (Join-Path $out "$Tag-ordinary.png")
    }

    Start-Sleep -Seconds 1
    $trace = Read-Live $errFile
    Write-Host ""
    Write-Host "=== what the game decided, and what reached the renderer ==="
    $trace | Where-Object { $_.Contains('prerendered room') -or $_.Contains('[gfx] refresh rate') } |
        Select-Object -Last 12
    Write-Host ""
    Write-Host "=== guard and crash lines, which should be none ==="
    $hits = $trace | Where-Object { $_.Contains('IMPLAUSIBLE') -or $_.Contains('CRASH') }
    if ($hits) { $hits | Select-Object -First 8 } else { Write-Host "none" }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
    elseif (Test-Path $settingsFile) { Remove-Item -Force $settingsFile }
}
