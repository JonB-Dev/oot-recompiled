# Prove the bindings file is read and obeyed: bind Start to a key nothing uses, launch, press
# that key, and watch the pause menu open in the trace.
#
# The default keys stay bound as the second binding of each input (the file only names Start),
# so Enter-Play still works with Return; the proof is the new key, P, which is not Start by
# default and opens the menu only if the file was read and the model consulted. The pause menu
# opening is a `[seg] segment 4` move in the trace (the object bank moves for the equipment
# screen's render of Link), the same signal pausecycle.ps1 counts.
#
#   tools\harness\bindcheck.ps1 -Tag p47-bind
#   tools\harness\bindcheck.ps1 -Tag p50-bind -ThroughDocument
#
# Prints BINDCHECK_OK or BINDCHECK_FAILED, restores controls.txt to what it was (or removes it
# if there was none), and exits accordingly.
param(
    [string]$Entrance = "0x0EE",
    [string]$Tag = "bindcheck",
    # "keyboard" binds Start to P and presses P. "virtual" attaches SDL's virtual joystick with
    # --virtual-controller and a script that presses its Start button (SDL's button 6) twice at
    # fixed times; the profile for it is keyed "virtual" so it can be written before launch,
    # and Start is bound on both the raw button (jbutton:6) and the mapped one (button:6) so
    # the proof holds whichever way SDL sees the virtual pad.
    [ValidateSet('keyboard', 'virtual')]
    [string]$Device = 'keyboard',
    # With -Raw the virtual pad is attached as a joystick SDL has no mapping for (--virtual-raw),
    # the way a custom Bluetooth pad arrives, so only a raw binding (jbutton:6) can reach Start.
    [switch]$Raw,
    # With -Capture nothing is bound beforehand: the script arms a capture for Start on the pad,
    # presses its button 6 (which the capture binds and the file records), then presses it
    # again to open and close the menu. Proves bind by pressing without a hand on a pad.
    [switch]$Capture,
    # With -ThroughDocument (the keyboard) nothing is written beforehand either: the run opens
    # the controls document with F3, moves down to the Start row, presses Enter to listen, and
    # presses P, which the document's capture binds and the game writes to the file; Escape
    # closes the document and P then opens and closes the pause menu. Proves the document end
    # to end: open, navigate, bind, save, close, and the binding in play.
    [switch]$ThroughDocument
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$controls = Join-Path $script:HarnessRoot 'build-cmake\controls.txt'
$err = Join-Path $script:HarnessOut "$Tag.stderr.txt"
# In the temp directory, not the harness output: the project directory's path has spaces, and
# the launch helper hands extra arguments to the game unquoted, so a path under it arrived cut
# at the first space and the layer found no script.
$scriptPath = Join-Path $env:TEMP "oot-$Tag.virtual.txt"

$existed = Test-Path $controls
$saved = if ($existed) { Get-Content $controls } else { $null }
$ok = $false
$script:captureFailed = $false

# The presses happen after Enter-Play, the settle and the A presses: about sixty two seconds
# after launch. The script's times are from the device layer's start, which is at launch.
$pressAt = 66000

try {
    if ($Device -eq 'virtual') {
        if ($Capture) {
            @('profile.virtual.start.0 = none', 'profile.virtual.start.1 = none') | Set-Content -Encoding ascii $controls
            @(
                "$pressAt capture start 0",
                "$($pressAt + 500) button 6 1", "$($pressAt + 800) button 6 0",
                "$($pressAt + 3500) button 6 1", "$($pressAt + 3800) button 6 0",
                "$($pressAt + 6500) button 6 1", "$($pressAt + 6800) button 6 0"
            ) | Set-Content -Encoding ascii $scriptPath
        }
        elseif ($Raw) {
            @('profile.virtual.start.0 = jbutton:6') | Set-Content -Encoding ascii $controls
            @(
                "$pressAt button 6 1", "$($pressAt + 300) button 6 0",
                "$($pressAt + 3000) button 6 1", "$($pressAt + 3300) button 6 0"
            ) | Set-Content -Encoding ascii $scriptPath
        }
        else {
            # SDL maps its virtual game controller, so Start is bound by SDL's name only. Binding
            # the raw index as well doubled every press: the joystick and controller views of one
            # button change on different ticks and the game saw a flicker as two presses.
            @('profile.virtual.start.0 = button:6') | Set-Content -Encoding ascii $controls
            @(
                "$pressAt button 6 1", "$($pressAt + 300) button 6 0",
                "$($pressAt + 3000) button 6 1", "$($pressAt + 3300) button 6 0"
            ) | Set-Content -Encoding ascii $scriptPath
        }
        $extra = @('--warp', $Entrance, '--virtual-controller', $scriptPath)
        if ($Raw) { $extra += '--virtual-raw' }
    }
    elseif ($ThroughDocument) {
        # Nothing written: the defaults, and the document does the binding.
        Remove-Item -Force $controls -ErrorAction SilentlyContinue
        $extra = @('--warp', $Entrance)
    }
    else {
        # SDL scancodes: P is 19, Return is 40. Start on both, P first.
        @('keyboard.start.0 = key:19', 'keyboard.start.1 = key:40') | Set-Content -Encoding ascii $controls
        $extra = @('--warp', $Entrance)
    }

    $launched = Get-Date
    $p = Start-Game $Tag $extra
    Write-Host "pid $($p.Id), warping to $Entrance, device $Device"
    Enter-Play
    Start-Sleep -Seconds 8

    # The first arrival plays a conversation, and the pause menu is refused while it runs.
    for ($i = 1; $i -le 12; $i++) { if ($p.HasExited) { break }; Press 'A' 6 1200 }

    $before = 0
    if (Test-Path $err) { $before = (Select-String -Path $err -Pattern 'segment 4 moved' | Measure-Object).Count }

    if ($Device -eq 'virtual') {
        # Wait for the script's presses to have happened, with a margin.
        $lastAt = if ($Capture) { 6800 } else { 3300 }
        $until = $launched.AddMilliseconds($pressAt + $lastAt + 3000)
        while ((Get-Date) -lt $until -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
        $applied = 0
        if (Test-Path $err) { $applied = (Select-String -Path $err -Pattern 'virtual event at' | Measure-Object).Count }
        Write-Host ("  virtual events applied: {0}" -f $applied)
        if (Test-Path $err) { Select-String -Path $err -Pattern 'controller added|capture' | ForEach-Object { Write-Host ("  " + $_.Line) } }
        if ($Capture) {
            # The file must now hold what the capture bound, written by the game itself.
            $bound = if (Test-Path $controls) { Select-String -Path $controls -Pattern '^profile\.virtual\.start\.0 = (jbutton|button):6' } else { $null }
            if ($bound) { Write-Host ("  file holds: " + $bound.Line) } else { Write-Host "  file does not hold the captured binding"; $script:captureFailed = $true }
        }
    }
    elseif ($ThroughDocument) {
        # F3 opens the controls document on the device row. Four steps down: A, B, Z, Start.
        Press 'F3' 6 1500
        for ($i = 0; $i -lt 4; $i++) { Press 'DDOWN' 5 300 }
        Press 'START' 6 800       # Enter: listen for the first slot
        Press 'P' 6 1500          # bound, and the file written by the game
        Press 'ESC' 6 1500        # the document closes; the game reads again
        if (Test-Path $err) { Select-String -Path $err -Pattern 'capture|\[ui\]' | ForEach-Object { Write-Host ("  " + $_.Line) } }
        $bound = if (Test-Path $controls) { Select-String -Path $controls -Pattern '^keyboard\.start\.0 = key:19' } else { $null }
        if ($bound) { Write-Host ("  file holds: " + $bound.Line) } else { Write-Host "  file does not hold the captured binding"; $script:captureFailed = $true }
        # P opens the menu, then P closes it; each is a move of the bank.
        Press 'P' 6 2500
        Press 'P' 6 2500
    }
    else {
        # P opens the menu, then P closes it; each is a move of the bank.
        Press 'P' 6 2500
        Press 'P' 6 2500
    }

    $after = 0
    if (Test-Path $err) { $after = (Select-String -Path $err -Pattern 'segment 4 moved' | Measure-Object).Count }
    $alive = -not $p.HasExited
    $ok = $alive -and ($after -gt $before) -and (-not $script:captureFailed)
    Write-Host ("=== {0}: segment 4 moves before {1}, after {2}, alive={3} ===" -f $Tag, $before, $after, $alive)
    if ($ok) { Write-Host 'BINDCHECK_OK' } else { Write-Host 'BINDCHECK_FAILED' }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($existed) { $saved | Set-Content -Encoding ascii $controls } else { Remove-Item -Force $controls -ErrorAction SilentlyContinue }
    Remove-Item -Force $scriptPath -ErrorAction SilentlyContinue
}

if ($ok) { exit 0 } else { exit 1 }
