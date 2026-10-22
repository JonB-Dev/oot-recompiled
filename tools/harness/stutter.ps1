# Measure the stutter, phase by phase, against what the two reports describe.
#
#   tools\harness\stutter.ps1 -Tag st1
#   tools\harness\stutter.ps1 -Tag st-field -Entrance 0x0051
#
# The user, 2026-09-20: "Some hitching during traversal", "Audio popping/skipping ... along side
# slight hitching or stuttering in framerate", "Z targeting camera seemingly adjusts at 20fps".
# A tester the same morning: "when I roll and am panning the camera I can get a pretty consistent
# frame drop of like 1-2 frames", "just when I'm running or rolling at a brisk pace, it just
# stutters and I lose a frame or two and the audio skips a bit".
#
# So the run is divided into phases that isolate those motions, and each phase is measured on its
# own. The measurements come from two places and both are read LIVE, by line count, so a phase
# owns exactly the lines written while it ran:
#
#   the probe file   one line a second: audio queue depth, underruns, and (since 2026-09-20) the
#                    graphics thread's spans, so `dl_over` is display list walks over 12 ms,
#                    `submit_over` is game frames that arrived more than 40 ms apart
#   the trace        `[cam] CUT` per camera cut, `[hitch]` per span over its threshold
#
# A CUT IS NOT A HITCH AND THE DIFFERENCE IS THE POINT. A hitch is work that took too long. A cut
# is the interpolation switching itself off for a frame, which drops the PICTURE to the game's own
# twenty updates a second while every span stays fast. They look identical to a person and have
# nothing in common underneath, so the summary prints them side by side.
param(
    [string]$Tag = "stutter",
    [string]$Entrance = "",
    [int]$Width = 1280,
    [int]$Height = 720,
    # counter=1 turns the frame rate counter on for the whole run (the user's ask of 09:16), so
    # every capture carries the rate it was taken at and the number a person reads is in the
    # record beside the numbers the probe writes.
    [string]$Settings = "framerate=1,aspect=1,hud=2,counter=1",
    [int]$BurstGrabs = 14,
    # Run the whole phase set this many times without restarting the game. A single pass is about
    # a minute of play and the reports are from people playing for much longer, so a hitch that
    # happens every few minutes cannot show up in one pass. With -Repeat the phases are suffixed
    # with the pass number and each is still measured on its own.
    [int]$Repeat = 1,
    # Pass --present-early, which puts RT64 in the presentation mode the reference project turns
    # on unconditionally ("Enable the present early presentation mode for minimal latency") and
    # which this program leaves off by default in favor of RT64's own skip buffering. The frame
    # a game produces reaches the present queue sooner, which leaves more of the game frame to
    # render the interpolated frames in, so it is a candidate for the dropped frames.
    [switch]$PresentEarly
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'stutter'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$settingsFile = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
foreach ($kv in $Settings.Split(',')) {
    $pair = $kv.Split('=')
    if ($pair.Count -eq 2) { Set-Setting $pair[0].Trim() $pair[1].Trim() }
}

$probeFile = Join-Path $script:HarnessRoot "build-cmake\$Tag-probe.txt"
$errFile   = Join-Path $script:HarnessOut  "$Tag.stderr.txt"

# Read a file that the game still has open. Copy first: the game holds the probe file with
# _SH_DENYWR and the redirected trace is held by the shell, and a straight Get-Content on either
# can fail with a sharing violation part way through a run.
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

# WAIT FOR ORDINARY PLAY, never a fixed sleep. The first run of this script measured its first
# two phases against the title screen's attract demo: the queued warp fired before the file was
# loaded, the title sequence took the game back, and the script had already started counting.
# The trace says plainly when the game is really playing, so read it instead of guessing:
# `[state] entrance 0x00CD  age 1  mode 0`, where mode 0 is ordinary play.
function Wait-ForPlay {
    param([string]$entrance = "", [int]$timeoutSec = 90)
    $want = ""
    if ($entrance -ne "") { $want = ("0x{0:X4}" -f [Convert]::ToInt32($entrance, 16)) }
    for ($i = 0; $i -lt ($timeoutSec * 2); $i++) {
        $lines = Read-Live $errFile
        for ($j = $lines.Count - 1; $j -ge 0 -and $j -gt $lines.Count - 200; $j--) {
            $l = $lines[$j]
            if (-not $l.Contains('[state] entrance')) { continue }
            if (-not $l.Contains('mode 0')) { continue }
            if ($want -ne "" -and -not $l.Contains($want)) { continue }
            Write-Host "playing: $l"
            return $true
        }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "NEVER REACHED ORDINARY PLAY in $timeoutSec s; the numbers below are not gameplay"
    return $false
}

$script:phases = @()

# WHAT THE SCREEN ACTUALLY SHOWS, which none of the probe's spans can say.
#
# The spans are all taken inside the program and every one of them can look healthy while the
# picture stutters. This is the outside view: a burst of back to back screen copies, counted
# afterward by ..\burst_count.py for how many CONSECUTIVE STEPS MOVED. At the display's rate
# with interpolation working, nearly every step moves, because a new in between picture reached
# the screen; at the game's own twenty a second, about one step in three moves and the other two
# are the same picture again. So the moved fraction is the picture's real rate, measured from
# outside the program, and it is the number that decides whether a phase stutters.
#
# Taken while the motion of the phase is still going, which is why each phase takes its own.
function Burst {
    param([string]$name, [int]$grabs)
    $dir = Join-Path $out "$Tag-$name"
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'bitblt') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'printwindow') | Out-Null
    for ($i = 0; $i -lt $grabs; $i++) {
        # burst_count.py globs `burst-*.png`, so the name is not free.
        [Drv]::Shot($script:hwnd, (Join-Path $dir ("bitblt\burst-{0:D2}.png" -f $i)))
    }
}

$script:rep = 0

function Start-Phase {
    param([string]$name)
    if ($Repeat -gt 1) { $name = "$name-$($script:rep)" }
    $p = [PSCustomObject]@{
        Name       = $name
        ProbeStart = (Read-Live $probeFile).Count
        TraceStart = (Read-Live $errFile).Count
        ProbeEnd   = 0
        TraceEnd   = 0
    }
    $script:phases += $p
    Write-Host ""
    Write-Host "--- phase: $name"
    return $p
}

function End-Phase {
    param($p)
    $p.ProbeEnd = (Read-Live $probeFile).Count
    $p.TraceEnd = (Read-Live $errFile).Count
}

# Hold two keys at once: running while turning, which is how the camera is made to swing.
function Hold-Two {
    param([string]$a, [string]$b, [int]$ms)
    Focus-Game
    [Drv]::Down([uint16]$VK[$a])
    [Drv]::Down([uint16]$VK[$b])
    Start-Sleep -Milliseconds $ms
    [Drv]::Up([uint16]$VK[$b])
    [Drv]::Up([uint16]$VK[$a])
    Start-Sleep -Milliseconds 150
}

# A roll is the A button pressed while moving. Held forward throughout, tapping A, which is the
# tester's "running or rolling at a brisk pace".
function Roll-Repeatedly {
    param([int]$count, [int]$gapMs = 900)
    Focus-Game
    [Drv]::Down([uint16]$VK['SUP'])
    for ($i = 0; $i -lt $count; $i++) {
        Start-Sleep -Milliseconds 350
        [Drv]::Down([uint16]$VK['A'])
        Start-Sleep -Milliseconds 120
        [Drv]::Up([uint16]$VK['A'])
        Start-Sleep -Milliseconds $gapMs
    }
    [Drv]::Up([uint16]$VK['SUP'])
    Start-Sleep -Milliseconds 150
}

try {
    $extra = @()
    if ($Entrance -ne "") { $extra = @('--warp', $Entrance) }
    if ($PresentEarly) { $extra += '--present-early' }
    $p = Start-Game $Tag $extra -width $Width -height $Height
    Enter-Play
    Wait-ForPlay $Entrance | Out-Null
    # A moment past the arrival so the scene's first frames (its own loads, its first sight of
    # every texture) are not counted as the stutter being hunted.
    Start-Sleep -Seconds 6
    Shot (Join-Path $out "$Tag-start.png")

    for ($script:rep = 0; $script:rep -lt $Repeat; $script:rep++) {

    # Standing still, the control. Every number here is the floor the others are read against.
    # Its burst SHOULD show little movement: nothing is moving but the grass and the sky, so a
    # low moved count here is right and means the measurement is not counting noise.
    $ph = Start-Phase 'still'
    Start-Sleep -Seconds 5
    Burst 'still' $BurstGrabs
    Start-Sleep -Seconds 4
    End-Phase $ph

    # Running in a straight line. The camera follows from behind and barely turns.
    $ph = Start-Phase 'run-straight'
    for ($i = 0; $i -lt 2; $i++) { Hold 'SUP' 2200 }
    Focus-Game
    [Drv]::Down([uint16]$VK['SUP'])
    Start-Sleep -Milliseconds 600
    Burst 'run-straight' $BurstGrabs
    [Drv]::Up([uint16]$VK['SUP'])
    for ($i = 0; $i -lt 2; $i++) { Hold 'SUP' 2200 }
    End-Phase $ph

    # Running in an arc, which swings the camera: the tester's "panning the camera".
    $ph = Start-Phase 'run-arc'
    for ($i = 0; $i -lt 2; $i++) {
        Hold-Two 'SUP' 'SLEFT' 1800
        Hold-Two 'SUP' 'SRIGHT' 1800
    }
    Focus-Game
    [Drv]::Down([uint16]$VK['SUP'])
    [Drv]::Down([uint16]$VK['SLEFT'])
    Start-Sleep -Milliseconds 600
    Burst 'run-arc' $BurstGrabs
    [Drv]::Up([uint16]$VK['SLEFT'])
    [Drv]::Up([uint16]$VK['SUP'])
    End-Phase $ph

    # Rolling forward, repeatedly.
    $ph = Start-Phase 'roll'
    Roll-Repeatedly 4
    Focus-Game
    [Drv]::Down([uint16]$VK['SUP'])
    Start-Sleep -Milliseconds 300
    [Drv]::Down([uint16]$VK['A'])
    Start-Sleep -Milliseconds 120
    [Drv]::Up([uint16]$VK['A'])
    # The burst lands inside the roll itself, which is the motion being complained about.
    Burst 'roll' $BurstGrabs
    [Drv]::Up([uint16]$VK['SUP'])
    Roll-Repeatedly 2
    End-Phase $ph

    # Running sideways while slashing, which is the tester's own case: "if I just kinda run left
    # or right and slash my sword it kinda causes the same hitching to happen". B is the sword.
    $ph = Start-Phase 'run-and-slash'
    Focus-Game
    for ($i = 0; $i -lt 5; $i++) {
        $side = if (($i % 2) -eq 0) { 'SLEFT' } else { 'SRIGHT' }
        [Drv]::Down([uint16]$VK[$side])
        Start-Sleep -Milliseconds 400
        [Drv]::Down([uint16]$VK['B'])
        Start-Sleep -Milliseconds 120
        [Drv]::Up([uint16]$VK['B'])
        if ($i -eq 2) { Burst 'run-and-slash' $BurstGrabs }
        Start-Sleep -Milliseconds 700
        [Drv]::Up([uint16]$VK[$side])
    }
    End-Phase $ph

    # Rolling while arcing, which is the tester's exact combination.
    $ph = Start-Phase 'roll-and-turn'
    Focus-Game
    [Drv]::Down([uint16]$VK['SUP'])
    for ($i = 0; $i -lt 6; $i++) {
        $side = if (($i % 2) -eq 0) { 'SLEFT' } else { 'SRIGHT' }
        [Drv]::Down([uint16]$VK[$side])
        Start-Sleep -Milliseconds 500
        [Drv]::Down([uint16]$VK['A'])
        Start-Sleep -Milliseconds 120
        [Drv]::Up([uint16]$VK['A'])
        Start-Sleep -Milliseconds 700
        [Drv]::Up([uint16]$VK[$side])
    }
    [Drv]::Up([uint16]$VK['SUP'])
    End-Phase $ph

    # Z targeting: held down while moving, which is the user's "Z targeting camera seemingly
    # adjusts at 20fps rather than set framerate".
    $ph = Start-Phase 'ztarget'
    Focus-Game
    [Drv]::Down([uint16]$VK['Z'])
    for ($i = 0; $i -lt 3; $i++) {
        $side = if (($i % 2) -eq 0) { 'SLEFT' } else { 'SRIGHT' }
        Hold-Two 'SUP' $side 1500
    }
    [Drv]::Down([uint16]$VK['SUP'])
    [Drv]::Down([uint16]$VK['SRIGHT'])
    Start-Sleep -Milliseconds 500
    Burst 'ztarget' $BurstGrabs
    [Drv]::Up([uint16]$VK['SRIGHT'])
    [Drv]::Up([uint16]$VK['SUP'])
    [Drv]::Up([uint16]$VK['Z'])
    Start-Sleep -Milliseconds 300
    End-Phase $ph
    Shot (Join-Path $out "$Tag-end-$($script:rep).png")

    } # each pass of -Repeat

    Start-Sleep -Seconds 2
    $probe = Read-Live $probeFile
    $trace = Read-Live $errFile

    Write-Host ""
    Write-Host "=== per phase ==="
    # fps is the presented frames a second: the picture's real rate, counted in the render hook,
    # interpolated frames included. pres_max is the longest a single frame stayed on screen, and
    # pres_over counts the frames that stayed longer than one and a half refreshes. Those three
    # are the ones the reports are about; the rest say whose fault it was.
    # LOST is the number that matters and the one an average hides. Each probe line counts the
    # frames actually presented in that second; the display runs at a fixed rate, so any second
    # holding fewer than that lost the difference. A phase can average 60 and still drop a frame
    # every few seconds, which is exactly what a person calls a stutter and exactly what the
    # first version of this table could not see, because it printed the average.
    $rate = 60
    Write-Host ("{0,-14} {1,4} {2,5} {3,5} {4,8} {5,8} {6,7} {7,6} {8,5}" -f `
        'phase', 'secs', 'fps', 'lost', 'pres_max', 'pres_over', 'dl_max', 'under', 'cuts')
    foreach ($ph in $script:phases) {
        $lines = @()
        for ($i = $ph.ProbeStart; $i -lt $ph.ProbeEnd -and $i -lt $probe.Count; $i++) {
            if ($probe[$i] -notmatch '^#') { $lines += $probe[$i] }
        }
        $dlMax = 0; $dlOver = 0; $under = 0; $presMax = 0; $presOver = 0; $presents = 0; $lost = 0
        foreach ($l in $lines) {
            $f = $l -split '\s+'
            if ($f.Count -lt 26) { continue }
            $under += [int]$f[10]
            if ([int64]$f[14] -gt $dlMax) { $dlMax = [int64]$f[14] }
            $dlOver += [int]$f[15]
            if ([int64]$f[23] -gt $presMax) { $presMax = [int64]$f[23] }
            $presOver += [int]$f[24]
            $presents += [int]$f[25]
        }
        # NET over the phase, not the sum of per-second shortfalls. A second that reads 59
        # followed by one that reads 61 lost nothing: the frame fell the other side of a sampling
        # boundary. Counting each second on its own turns that into a loss and roughly triples
        # the figure, which is how a 0.3 percent shortfall gets reported as a 20 percent problem.
        $lost = ($rate * $lines.Count) - $presents
        if ($lost -lt 0) { $lost = 0 }
        $fps = if ($lines.Count -gt 0) { [int]($presents / $lines.Count) } else { 0 }
        $cuts = 0
        for ($i = $ph.TraceStart; $i -lt $ph.TraceEnd -and $i -lt $trace.Count; $i++) {
            if ($trace[$i].Contains('[cam] CUT')) { $cuts++ }
        }
        Write-Host ("{0,-14} {1,4} {2,5} {3,5} {4,8} {5,8} {6,7} {7,6} {8,5}" -f `
            $ph.Name, $lines.Count, $fps, $lost, $presMax, $presOver, $dlMax, $under, $cuts)
    }

    Write-Host ""
    Write-Host "=== what the screen showed, per phase (steps that MOVED out of the burst) ==="
    Write-Host "at the display's rate nearly every step moves; at the game's own rate about one in three"
    foreach ($ph in $script:phases) {
        $dir = Join-Path $out "$Tag-$($ph.Name)"
        if (Test-Path (Join-Path $dir 'bitblt')) {
            Write-Host ""
            Write-Host "--- $($ph.Name)"
            & python (Join-Path $script:HarnessRoot 'tools\burst_count.py') $dir 2>&1 | ForEach-Object { Write-Host "    $_" }
        }
    }

    Write-Host ""
    Write-Host "=== every hitch line ==="
    $trace | Where-Object { $_.Contains('[hitch]') } | Select-Object -First 40
    Write-Host ""
    Write-Host "=== a sample of the cuts ==="
    $trace | Where-Object { $_.Contains('[cam] CUT') } | Select-Object -Last 25
    Write-Host ""
    Write-Host "=== the last probe lines ==="
    $probe | Select-Object -First 1
    $probe | Select-Object -Last 8
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
    elseif (Test-Path $settingsFile) { Remove-Item -Force $settingsFile }
}
