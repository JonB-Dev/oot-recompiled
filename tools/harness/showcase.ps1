# showcase.ps1: stills of the ray traced lighting, for showing people.
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\showcase.ps1
#   powershell -ExecutionPolicy Bypass -File tools\harness\showcase.ps1 -Only windmill
#
# NOT A TEST. Every other script in this folder exists to answer a question; this one exists to
# take a good photograph. It is here rather than improvised because the useful part is the SHOT
# LIST: which places, at which hour, show what the renderer is actually doing. That list took
# knowing the game, and it should not have to be worked out again the next time somebody wants
# pictures.
#
# WHAT MAKES A SHOT SHOW RAY TRACING. Not a bright field at noon, where the picture looks much as
# it always did. It is:
#   * a MOVING light source with something to cast a shadow from, which in this game means Navi,
#     hovering beside Link in the dark;
#   * a small number of strong point lights in an enclosed space (torches, lava, a fairy);
#   * a shaft of daylight through an opening, which is the windmill's whole architecture;
#   * water, for what reflects in it.
# The list below is chosen on those grounds rather than on how pretty each place is.
#
# It reuses the capture machinery the A/B shots use: the game is started with the entrance queued,
# the clock frozen at a chosen hour, and the update frozen a fixed number of ticks after arrival so
# the picture holds still while the draw runs on. Captures land in
# build-cmake\harness\showcase\<name>.png.
#
# Refuses to start while the program is running: that copy is somebody's game.
#
# IT RUNS ON A LOCKED MACHINE, and that is the constraint everything below is shaped by
# (2026-09-27: "my pc is locked. I'm not home"). A locked session has no foreground window, so
# SetForegroundWindow cannot succeed and every simulated key press is lost. Therefore:
#
#   * NOTHING IS PRESSED. Resume-on-start puts the game into play by itself, and the warp is
#     queued at launch, so the run needs no input at all.
#   * THE CAPTURE ASKS THE WINDOW TO PAINT ITSELF (PrintWindow with PW_RENDERFULLCONTENT) rather
#     than copying the screen, because there is no screen to copy from.
#   * IT IS SILENT. Nobody is there to hear it and a machine that starts making noise while its
#     owner is out is its own kind of alarming.
param(
    [int]$Width = 1920,
    [int]$Height = 1080,
    # Long enough for Navi to settle beside Link rather than still flying in from the transition.
    [uint32]$FreezeTicks = 160,
    [int]$SettleSeconds = 30,
    [uint32]$Seed = 1,
    # Take only the shots whose name contains this, for trying one without redoing them all.
    [string]$Only = "",
    # HOLD A DIRECTION FOR THE WHOLE RUN, so the photograph catches Link MOVING (2026-09-27: "Run
    # through kakariko village at morning", "Starting at one of the house exits"). Every shot up to
    # now has been an arrival: Link standing at a door where the warp dropped him, which is why
    # they all have the same shape. A held stick makes the difference, and it is held rather than
    # tapped because there is nothing to synchronize a tap against.
    #
    # IT GOES THROUGH THE VIRTUAL PAD, which is the only input that works here. The machine is
    # locked, so there is no foreground window and every simulated key press is thrown away; the
    # virtual joystick is attached inside the program and read by the same code that reads a real
    # one, so it does not care whether anybody is logged in. 'forward' is the stick away from the
    # camera, which after stepping out of a door is into the street.
    [ValidateSet('', 'forward', 'back', 'left', 'right')]
    [string]$Hold = "",
    # HOW FAR THE STICK IS PUSHED, 0 to 1. It is here because of a collision between two fixed
    # timings: the game holds the place-name card over the screen for about the first two hundred
    # updates after an arrival, and a full run carries Link past the good part of the village well
    # before then. Pushing the stick halfway makes him walk, which covers the same ground in
    # roughly twice the updates, so the card has gone by the time he reaches the place worth
    # photographing. The stick is analog and the game reads it as such, so this is the game's
    # own speed control rather than anything invented here.
    [double]$HoldAmount = 1.0,
    # PHOTOGRAPH A LIST OF ENTRANCES ONCE EACH, to find out what is actually behind a spawn number.
    # The entrance table says Kakariko has sixteen spawns and which scene each belongs to; it does
    # NOT say which door each one is, because that lives in the scene's exit list, which is in the
    # ROM rather than in the decompilation. So the only honest way to find the house exit that
    # faces into the village is to look at each of them.
    [string[]]$Probe = @(),
    # The hour a probe is taken at. Written as the raw clock rather than a name because the names
    # are defined below the parameters; 0x5555 is 08:00.
    [string]$ProbeClock = '0x5555',
    # SWEEP ONE PLACE ACROSS SEVERAL MOMENTS (2026-09-27, looking for Navi). The freeze stops the
    # update, so a shot holds whatever Link, the camera and Navi were doing at that tick; she
    # orbits him constantly, so the same spot at several counts gives several compositions and one
    # of them has her where she is wanted. This is why it is a sweep rather than a button press:
    # C-up only brings her out when she has something to say and Z needs something to target, so
    # in an empty field at night neither is guaranteed, while she is always somewhere.
    [int[]]$Sweep = @()
)

. (Join-Path $PSScriptRoot 'drive.ps1')

# The game's 16 bit clock. The names are the ones the warp menu shows.
$T = @{
    midnight = '0x0000'
    dawn     = '0x4000'   # 06:00, still counted as night
    morning  = '0x5555'   # 08:00
    noon     = '0x8000'
    teatime  = '0xA000'   # 15:00
    sunset   = '0xC000'   # 18:00, the boundary the game counts as day
    dusk     = '0xC555'   # 18:30, night has just begun
    night    = '0xE000'   # 21:00
}

# THE LIGHTING IS NOT SET HERE, and the first attempt at this got it wrong in a way worth
# recording. The lighting lives in its OWN file, lighting.txt, beside the program's data; settings
# .txt holds everything else. Writing lighting keys into settings.txt therefore changed nothing at
# all, and the run produced a set of perfectly sharp pictures of the game with its ray tracing off,
# which is the one thing they were meant to show.
#
# THE VALUES COME FROM THE INSTALLED COPY (2026-09-27: "Ray tracing needs to be enabled and at all
# my recommended settings"). That file IS the recommended configuration: it is the same file the
# website reads to publish the Ray traced column. Copying it is therefore the only way to be sure
# these pictures show what the site tells people to set, rather than what somebody writing a
# harness thought looked nice.
$InstalledLighting = Join-Path $env:LOCALAPPDATA 'OoT Recompiled\lighting.txt'
$RunLighting = Join-Path $script:HarnessRoot 'build-cmake\lighting.txt'

# Everything that is NOT lighting: the picture size, the silence, and getting into the game with
# no button pressed.
$Look = @(
    'resolution=4', 'aspect=2', 'antialiasing=2', 'renderdistance=3',
    'volume=0', 'resumeonstart=1'
) -join ','

# FIVE PLACES, EACH PHOTOGRAPHED TWICE (2026-09-27: "5 photos with and without. Each as a set.
# So we have full comparison"). The pair is the argument: one picture of ray traced lighting proves
# nothing to somebody who never saw the same frame without it, and these are the same frame, same
# clock, same seed, same position, with one file changed between them.
#
# Chosen for what a PAIR shows rather than for prettiness:
$Shots = [ordered]@{
    'navi-field-night'  = @{ e = '0x00CD'; t = $T.night;   why = 'Navi lighting Link in an empty field at night' }
    # REPLACED (2026-09-27). Kakariko at night froze while the arrival was still fading in and
    # came out almost black, and the Temple of Time showed no difference at all because its light
    # is largely baked into the scene rather than cast. Both are gone; these two are places where
    # the light has somewhere to fall and something to fall from.
    # WHAT ACTUALLY PHOTOGRAPHS WELL, learned by getting it wrong four times (2026-09-27). The
    # Temple of Time showed nothing because its light is baked in; Kakariko at night, the Lost
    # Woods and the crater all came out near black, because the game's own interiors and night
    # scenes are already dim and a still cannot show what the tracing adds to a dark room.
    #
    # The ones that worked are OUTDOORS, WITH THE SUN UP, and have geometry standing on open
    # ground: something to cast a shadow, and somewhere for it to land. That is the rule now.
    'kokiri-forest'     = @{ e = '0x00EE'; t = $T.morning; why = 'huts and trees on open ground, low sun' }
    'zoras-domain'      = @{ e = '0x0108'; t = $T.noon;    why = 'water, and what it does with the light' }
    'graveyard-night'   = @{ e = '0x00E4'; t = $T.night;   why = 'almost no light but Navi' }
    'goron-city'        = @{ e = '0x014D'; t = $T.noon;    why = 'torches in a dark carved cavern' }
}

# THE SAME DOOR, THE SAME RUN, SEVERAL HOURS (2026-09-27: "Run through kakariko village at
# morning", "Starting at one of the house exits", "Do this same test at same door and run but at
# various times of day"). Everything is held still except the clock, so the series is about the
# light and nothing else.
#
# HOW THIS ONE WAS COMPOSED, since the numbers look arbitrary and are not:
#   * Entrance 0x0349 is Kakariko spawn 6, found by photographing all eight of the village's
#     spawns: a house door on the west side facing the square, with the stairs and the far houses
#     opening out ahead of it. Which spawn is which door is not in the decompilation, because the
#     scene exit lists live in the ROM, so it was looked at rather than looked up.
#   * The stick is held at 0.55 rather than 1.0, so Link WALKS. At a run he is past the good part
#     of the village before the game has taken its place-name card off the screen; at a walk the
#     card has gone by the time he gets there. Two fixed timings that would not otherwise fit.
#   * 215 updates is where he is out in the open with the village ahead and the card gone.
$Runs = [ordered]@{
    'kakariko-run-dawn'    = @{ e = '0x0349'; t = $T.dawn;    hold = 'forward'; amount = 0.55; why = 'first light, shadows at their longest' }
    'kakariko-run-morning' = @{ e = '0x0349'; t = $T.morning; hold = 'forward'; amount = 0.55; why = 'the sun up, the house throwing across the path' }
    'kakariko-run-noon'    = @{ e = '0x0349'; t = $T.noon;    hold = 'forward'; amount = 0.55; why = 'overhead, and almost nothing to cast' }
    'kakariko-run-sunset'  = @{ e = '0x0349'; t = $T.sunset;  hold = 'forward'; amount = 0.55; why = 'low and along the street' }
    'kakariko-run-night'   = @{ e = '0x0349'; t = $T.night;   hold = 'forward'; amount = 0.55; why = 'lanterns, and whatever they reach' }
}

# And the times of day asked for separately, taken with the lighting on only.
$TimeSeries = [ordered]@{
    'kakariko-morning'  = @{ e = '0x00DB'; t = $T.morning; why = 'the village early' }
    'kakariko-noon'     = @{ e = '0x00DB'; t = $T.noon;    why = 'the village at noon' }
    'kakariko-sunset'   = @{ e = '0x00DB'; t = $T.sunset;  why = 'long shadows down the hill' }
    'windmill-noon'     = @{ e = '0x044F'; t = $T.noon;    why = 'inside the windmill, overhead light' }
    'lake-hylia-sunset' = @{ e = '0x0102'; t = $T.sunset;  why = 'the sun low across open water' }
}

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "SHOWCASE_FAIL the program is already running; close it first (it may be your own game)"
    exit 1
}
if (-not (Test-Path $InstalledLighting)) {
    Write-Output "SHOWCASE_FAIL no lighting.txt in the installed copy, so the recommended settings are unknown"
    exit 1
}

$out = Join-Path $script:HarnessOut "showcase"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$saved = Use-Settings $Look

# The run's own lighting file, put back exactly as it was afterward.
$hadLighting = Test-Path $RunLighting
$lightingBackup = if ($hadLighting) { Get-Content $RunLighting -Raw } else { $null }

$recommended = Get-Content $InstalledLighting
# The same file with the one line that matters turned off. Everything else is left alone on
# purpose: an "off" shot that also drops the resolution or the smoothing is not a comparison of
# ray tracing, it is a comparison of two different programs.
$plain = $recommended | ForEach-Object { if ($_ -match '^\s*raytracing\s*=') { 'raytracing = 0' } else { $_ } }

$taken = 0
$failed = @()

# THE LIST ARRIVES AS ONE STRING, and this is the second time this project has been caught by it.
# Running a script with -File hands every argument through as text, so `-Probe 0x0191,0x0195`
# reaches the parameter as the single item "401,405": PowerShell read the two hex literals as
# numbers on the way past and then joined them. Splitting here accepts both shapes, and quoting
# the value at the call site ("0x0191,0x0195") is what stops the hex being eaten in the first
# place.
$wanted = @($Probe | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })

# NOT ALWAYS THE SAME PICTURE (2026-09-27: "not always facing way from camera and not always with
# or without navi"). The freeze stops the UPDATE and keeps drawing, so whatever Link and the camera
# and Navi were doing at that tick is what the photograph holds. Different tick, different moment:
# Navi circles, so she is beside him at one count and out of frame at another, and the camera is
# still settling behind him early on. So each place is taken at its own count rather than all of
# them at one, and the counts are spread rather than tidy.
$Ticks = @{
    'navi-field-night'  = 150
    'graveyard-night'   = 210
    'goron-city'        = 132
    'kokiri-forest'     = 240
    'zoras-domain'      = 240
    'kakariko-run-dawn'    = 215
    'kakariko-run-morning' = 215
    'kakariko-run-noon'    = 215
    'kakariko-run-sunset'  = 215
    'kakariko-run-night'   = 215
    'kakariko-morning'  = 168
    'kakariko-noon'     = 112
    'kakariko-sunset'   = 190
    'windmill-noon'     = 140
    'lake-hylia-sunset' = 124
}

# The stick, as the virtual pad's script understands it. SDL's left stick is axis 0 across and
# axis 1 down, and DOWN IS POSITIVE, which is why forward is a negative number and why writing it
# the other way round produces a shot of Link walking backward into the door he came out of.
#
# Full deflection rather than something gentler: past about half travel the game runs instead of
# walking, and a run is what was asked for.
$Directions = @{
    forward = @(1, -32000)
    back    = @(1,  32000)
    left    = @(0, -32000)
    right   = @(0,  32000)
}

# The script is written once and held for the whole run: one event at time zero, and the virtual
# axis keeps whatever it was last set to, so there is nothing to keep feeding. It lives in TEMP
# because the launch helper passes extra arguments to the game unquoted and this project's path
# has spaces in it.
function Write-Hold([string]$direction, [string]$tag, [double]$push) {
    if ($direction -eq "") { return $null }
    $axis, $value = $Directions[$direction]
    $amount = [Math]::Min(1.0, [Math]::Max(0.0, $push))
    $value = [int][Math]::Round($value * $amount)
    $path = Join-Path $env:TEMP "oot-$tag.virtual.txt"
    @(
        "# showcase: hold $direction at $amount for the whole run, so the freeze catches Link moving",
        "0 axis $axis $value"
    ) | Set-Content -Encoding ascii $path
    return $path
}

function Take-Shot {
    # $wantOn is PASSED, not worked out from the name. It used to be inferred from the suffix
    # being '-on', which quietly broke the moment a suffix was anything else: a sweep named its
    # shots '-t40' and so on, every one of them was judged to have the lighting on when it should
    # have been off, and all four were skipped. The run then printed nothing at all, because it
    # returned from inside the try and never reached its own summary.
    param([string]$name, $shot, [string]$suffix, [string[]]$lighting, [bool]$wantOn)

    Set-Content -Path $RunLighting -Value $lighting -Encoding ASCII
    $runTag = "showcase-$name$suffix"
    $ticks = if ($Ticks.ContainsKey($name)) { $Ticks[$name] } else { $FreezeTicks }

    # The shot's own hold wins over the run's, so a moving shot stays moving in a run that takes
    # the standing ones as well.
    $hold = if ($shot.hold) { $shot.hold } else { $Hold }
    $amount = if ($shot.amount) { $shot.amount } else { $HoldAmount }
    $holdPath = Write-Hold $hold $runTag $amount

    $launch = @(
        '--warp', "$($shot.e),$($shot.e)",
        '--dwell', '100',
        '--seed', "$Seed",
        '--freeze', "$($shot.e),$ticks",
        '--freeze-clock', "$($shot.t)"
    )
    if ($holdPath) { $launch += @('--virtual-controller', $holdPath) }

    $p = Start-Game $runTag $launch -width $Width -height $Height

    try {
        # Nothing is pressed: a locked session has no foreground window. The trace says when the
        # picture has stopped moving.
        $trace = Join-Path $script:HarnessOut "$runTag.stderr.txt"
        $frozen = $false
        for ($i = 0; $i -lt ($SettleSeconds * 4); $i++) {
            if ((Test-Path $trace) -and (Select-String -Path $trace -Pattern '\[shot\] frozen' -Quiet)) {
                $frozen = $true
                break
            }
            Start-Sleep -Milliseconds 250
        }
        if (-not $frozen) {
            $script:failed += "$name$suffix (never settled)"
            return
        }
        # Confirm the lighting really is what this shot claims, from the program's own trace, so a
        # pair can never be two pictures of the same thing wearing different names.
        $level = Select-String -Path $trace -Pattern '\[rt\] lighting level (\d)' | Select-Object -Last 1
        $onNow = $false
        if ($level -and $level.Matches[0].Groups[1].Value -ne '0') { $onNow = $true }
        if ($onNow -ne $wantOn) {
            $script:failed += "$name$suffix (the lighting was $(if ($onNow) { 'on' } else { 'off' }) when it should have been $(if ($wantOn) { 'on' } else { 'off' }))"
            return
        }

        Start-Sleep -Milliseconds 400
        $tmp = Join-Path $out "_$name$suffix"
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        [Drv]::ShotBurst($script:hwnd, $tmp, 1, $true) | Out-Null
        $got = Get-ChildItem $tmp -Filter *.png | Select-Object -First 1
        if ($got) {
            Move-Item $got.FullName (Join-Path $out "$name$suffix.png") -Force
            $script:taken++
        } else {
            $script:failed += "$name$suffix (nothing was captured)"
        }
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    } finally {
        Close-Game $p.Id | Out-Null
        if ($holdPath) { Remove-Item -Force $holdPath -ErrorAction SilentlyContinue }
    }
}

try {
    if ($wanted.Count -gt 0) {
        # One picture per entrance, lighting on, so a spawn number can be identified by looking at
        # it. The clock is the one the run was given, and the hold applies here too: a door is
        # easier to recognize from a few paces out than from standing in it.
        foreach ($e in $wanted) {
            Write-Host "probe $e"
            # The tick count is in the name so the same entrance can be probed at several moments
            # without each one writing over the last, which is exactly what is wanted once a hold
            # is involved: the question stops being "where is this door" and becomes "how far has
            # he got".
            $name = "probe-$($e -replace '^0x', '')-t$FreezeTicks"
            $Ticks[$name] = $FreezeTicks
            Take-Shot $name @{ e = $e; t = $ProbeClock } '' $recommended $true
        }
    }
    elseif ($Sweep.Count -gt 0) {
        # One place, several moments, lighting on. -Only names which place.
        $all = @{}
        foreach ($k in $Shots.Keys) { $all[$k] = $Shots[$k] }
        foreach ($k in $Runs.Keys) { $all[$k] = $Runs[$k] }
        foreach ($k in $TimeSeries.Keys) { $all[$k] = $TimeSeries[$k] }
        foreach ($name in $all.Keys) {
            if ($Only -ne "" -and $name -notlike "*$Only*") { continue }
            foreach ($tick in $Sweep) {
                Write-Host "$name at $tick ticks"
                $Ticks[$name] = $tick
                Take-Shot $name $all[$name] "-t$tick" $recommended $true
            }
        }
    }
    else {

    foreach ($name in $Shots.Keys) {
        if ($Only -ne "" -and $name -notlike "*$Only*") { continue }
        Write-Host "$name  ($($Shots[$name].why))"
        Take-Shot $name $Shots[$name] '-on'  $recommended $true
        Take-Shot $name $Shots[$name] '-off' $plain $false
    }
    foreach ($name in $Runs.Keys) {
        if ($Only -ne "" -and $name -notlike "*$Only*") { continue }
        Write-Host "$name  ($($Runs[$name].why))"
        Take-Shot $name $Runs[$name] '-on'  $recommended $true
        Take-Shot $name $Runs[$name] '-off' $plain       $false
    }
    # THESE ARE PAIRS TOO (2026-09-27: "I need a kakariko morning pair one with ray trace and one
    # without"). They were taken with the lighting on only, on the reasoning that the series was
    # about the hour rather than the renderer. That was the wrong call: a picture of the lighting
    # is worth nothing to somebody who cannot see the same frame without it, whatever else the
    # picture is also showing.
    foreach ($name in $TimeSeries.Keys) {
        if ($Only -ne "" -and $name -notlike "*$Only*") { continue }
        Write-Host "$name  ($($TimeSeries[$name].why))"
        Take-Shot $name $TimeSeries[$name] '-on'  $recommended $true
        Take-Shot $name $TimeSeries[$name] '-off' $plain       $false
    }

    }
} finally {
    Restore-Settings $saved
    if ($hadLighting) { Set-Content -Path $RunLighting -Value $lightingBackup -NoNewline -Encoding ASCII }
    else { Remove-Item $RunLighting -Force -ErrorAction SilentlyContinue }
}

Write-Output ""
Write-Output "took $taken shot(s) into $out"
if ($failed.Count -gt 0) { Write-Output "did not work: $($failed -join ', ')" }
