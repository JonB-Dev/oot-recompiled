# Does the game draw more of the world when the frame is wider? The check for the widescreen
# culling patch (patches/widescreen_culling.c).
#
#   tools\harness\cullcheck.ps1 -Tag cull43 -Aspect 0
#   tools\harness\cullcheck.ps1 -Tag cull169 -Aspect 1
#
# THE MEASUREMENT IS ACTORS, NOT PIXELS, and that is the point. A capture of a wide frame can be
# compared by eye, and two people will disagree about whether a tree at the edge was there before.
# The renderer counts every actor transform group it is handed, and the trace prints the running
# total beside the number of display lists it came in: groups per list is how much of the world
# the game decided to draw, per frame, as a number.
#
# The game's culling test is done against its own 4:3 projection, so BEFORE the patch this number
# is identical at both aspect settings (the game does not know the frame got wider, which is the
# bug: it culls the band the player can now see). AFTER the patch the wide run draws more. Run it
# at both settings and compare; the difference IS the objects that were disappearing.
#
# Link stands still throughout. Nothing here moves him, because the comparison only means
# something if both runs look at the same place.
param(
    [string]$Tag = "cull",
    [int]$Aspect = 1,
    # The render distance row, by its index: 0 Original, 1 2x, 2 3x, 3 4x, 4 6x, 5 8x.
    # -1 leaves whatever the settings file holds.
    [int]$RenderDistance = -1,
    [string]$Entrance = "0x00CD",
    [int]$Width = 1280,
    [int]$Height = 720,
    [int]$Seconds = 25
)

. (Join-Path $PSScriptRoot 'drive.ps1')

$settingsFile = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
Set-Setting 'aspect' $Aspect
if ($RenderDistance -ge 0) { Set-Setting 'renderdistance' $RenderDistance }
Set-Setting 'hud' 2
Set-Setting 'framerate' 1
Set-Setting 'window' 0

$out = Join-Path $script:HarnessOut 'cull'
New-Item -ItemType Directory -Force -Path $out | Out-Null
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

try {
    $p = Start-Game $Tag @('--warp', $Entrance) -width $Width -height $Height
    Enter-Play
    for ($i = 0; $i -lt 180; $i++) {
        $lines = Read-Live $errFile
        $hit = $false
        for ($j = $lines.Count - 1; $j -ge 0 -and $j -gt $lines.Count - 200; $j--) {
            if ($lines[$j].Contains('[state] entrance') -and $lines[$j].Contains('mode 0')) {
                Write-Host "playing: $($lines[$j])"
                $hit = $true
                break
            }
        }
        if ($hit) { break }
        Start-Sleep -Milliseconds 500
    }
    # Past the arrival, so the scene's own first frames are not in the sample.
    Start-Sleep -Seconds 5
    Shot (Join-Path $out "$Tag.png")

    $before = Read-Live $errFile
    Start-Sleep -Seconds $Seconds
    $after = Read-Live $errFile

    function Last-Numbers {
        param($lines, [string]$needle, [string]$pattern)
        for ($i = $lines.Count - 1; $i -ge 0; $i--) {
            if ($lines[$i].Contains($needle)) {
                if ($lines[$i] -match $pattern) { return $matches }
            }
        }
        return $null
    }

    $gBefore = Last-Numbers $before 'actor groups seen' 'actor groups seen (\d+)'
    $gAfter  = Last-Numbers $after  'actor groups seen' 'actor groups seen (\d+)'
    # The needle is ' lists,' and not '] lists,': the line reads `[dl] 1080 lists, ...`, so the
    # bracket is four characters and a number away from the word. The first version looked for
    # the bracket next to it, matched nothing, and reported that the run never reached play.
    $lBefore = Last-Numbers $before ' lists,' '\[dl\] (\d+) lists'
    $lAfter  = Last-Numbers $after   ' lists,' '\[dl\] (\d+) lists'

    Write-Host ""
    Write-Host "=== render distance row $RenderDistance, aspect $Aspect ($(if ($Aspect -eq 1) { 'expand to the window' } else { 'original 4:3' })), $Width by $Height ==="
    if ($null -ne $gBefore -and $null -ne $gAfter -and $null -ne $lBefore -and $null -ne $lAfter) {
        $groups = [int64]$gAfter[1] - [int64]$gBefore[1]
        $lists  = [int64]$lAfter[1] - [int64]$lBefore[1]
        Write-Host ("actor groups {0} over {1} display lists" -f $groups, $lists)
        if ($lists -gt 0) {
            Write-Host ("ACTOR GROUPS PER FRAME: {0:N1}" -f ($groups / $lists))
        }
    }
    else {
        Write-Host "could not read the counters; the run may not have reached play"
    }
    $after | Where-Object { $_.Contains('[gfx]') } | Select-Object -Last 2
    $after | Where-Object { $_.Contains('] lists,') } | Select-Object -Last 1
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
    elseif (Test-Path $settingsFile) { Remove-Item -Force $settingsFile }
}
