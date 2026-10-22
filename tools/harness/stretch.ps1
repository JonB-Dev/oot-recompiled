# Cover a STRETCH of the game the way phases 26 to 30 describe one, as far as a harness can.
#
# What it does in each scene: clear any conversation (A), open and close the pause menu a few
# times (the race fixed in patches\fixes\pause_object_race.c lived exactly there), and walk about.
# The game warps itself to the next entrance on a dwell long enough for all of that, and the
# harness watches the state trace: a scene that never comes back is killed and the run continues
# with what is left, blamed on the entrance it was sitting on.
#
# What it is NOT: a playthrough. Nothing here fights, solves or opens anything. It is scene
# coverage with the two interactions that have actually found bugs, and the record says so.
#
#   tools\harness\stretch.ps1 -Entrances "0x0EE,0x0000,0x040F,0x11E" -Tag stretch26
#
# Output: build-cmake\harness\stretch\<Tag>-run-<n>.err.txt (the state trace) and one capture per
# entrance reached. The summary at the end is only as good as the trace it summarizes; when they
# disagree, the trace is right.
#   -Settings "aspect=1,hud=2" writes those keys to build-cmake\settings.txt for the run and
#   restores the file afterward; -Width and -Height resize the window's client area once it is
#   up, which is how the wide-frame captures are taken.
param(
    [string]$Entrances = "0x0EE,0x0000,0x040F,0x11E",
    [string]$Tag = "stretch",
    [int]$DwellSeconds = 36,
    [int]$BudgetMinutes = 30,
    [string]$Settings = "",
    [int]$Width = 0,
    [int]$Height = 0
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'stretch'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$savedSettings = $null
if ($Settings -ne "") { $savedSettings = Use-Settings $Settings }

# Normalize to the exact spelling the state trace uses, 0x plus four upper case hex digits. The
# first version passed 0x0EE and looked for it in a trace that printed 0x00EE, matched nothing,
# and reported the scenes it had visited as never attempted.
$todo = New-Object System.Collections.ArrayList
foreach ($e in $Entrances.Split(',')) {
    $t = $e.Trim()
    if ($t -ne '') { [void]$todo.Add(('0x{0:X4}' -f [int]$t)) }
}

$result = @{}
$guardHits = 0
$budget = (Get-Date).AddMinutes($BudgetMinutes)
$run = 0
# The game's logic runs at twenty frames a second, and the dwell counts those, not display frames.
$dwellFrames = $DwellSeconds * 20

function Invoke-SceneExercise {
    # A conversation blocks the pause menu, and A is what advances one. Outside one it is a
    # roll, which is harmless.
    for ($i = 1; $i -le 6; $i++) { if ($p.HasExited) { return }; Press 'A' 6 900 }
    for ($i = 1; $i -le 3; $i++) {
        if ($p.HasExited) { return }
        Press 'START' 6 1400
        if ($p.HasExited) { return }
        Press 'START' 6 1400
    }
    Hold 'SUP' 700
    Hold 'SLEFT' 400
    Press 'A' 5 300
    Hold 'SRIGHT' 400
    Press 'Z' 8 300
}

while ($todo.Count -gt 0 -and (Get-Date) -lt $budget) {
    $run++
    $list = ($todo -join ',')
    $runTag = "$Tag-run-$run"
    $err = Join-Path $script:HarnessOut "$runTag.stderr.txt"

    try { $p = Start-Game $runTag @('--warp', $list, '--dwell', $dwellFrames) -width $Width -height $Height }
    catch { Write-Host "  no window"; continue }
    Write-Host ("run {0}: pid {1}, {2} entrances left: {3}" -f $run, $p.Id, $todo.Count, $list)

    Enter-Play

    $lastSeen = ''
    $lastMove = Get-Date
    $exercised = New-Object System.Collections.ArrayList

    while (-not $p.HasExited -and (Get-Date) -lt $budget) {
        Start-Sleep -Seconds 2

        $seen = @()
        if (Test-Path $err) {
            $seen = Select-String -Path $err -Pattern '\[state\] entrance 0x([0-9A-F]{4})\s+age\s+\d+\s+mode 0' `
                        -ErrorAction SilentlyContinue |
                    ForEach-Object { '0x' + $_.Matches[0].Groups[1].Value }
        }

        $current = if ($seen.Count -gt 0) { $seen[-1] } else { '' }

        # Every listed entrance that has appeared in the trace counts as reached, not only the
        # newest one. The first version read only the newest line, and an entrance Link left
        # within one two-second poll (the first exercise roll at the Market's spawn carries him
        # straight through the loading zone) was never marked, stayed on the list, and the
        # timeout at the end of the run was blamed on whatever scene the game was sitting in.
        foreach ($e in $seen) {
            if ($todo.Contains($e)) {
                $result[$e] = 'reached'
                [void]$todo.Remove($e)
                if ($e -ne $current) { Write-Host ("  reached {0} (left again before the poll saw it), {1} left" -f $e, $todo.Count) }
            }
        }
        if ($current -ne $lastSeen) {
            $lastSeen = $current
            $lastMove = Get-Date
            if ($result[$current] -eq 'reached' -and -not $exercised.Contains($current)) {
                [void]$exercised.Add($current)
                Write-Host ("  reached {0}, {1} left; exercising" -f $current, $todo.Count)
                Start-Sleep -Seconds 3
                Invoke-SceneExercise
                if (-not $p.HasExited) { Shot (Join-Path $out ("$Tag-{0}.png" -f $current)) }
            }
        }
        elseif (((Get-Date) - $lastMove).TotalSeconds -gt ($DwellSeconds + 30)) {
            # Nothing new for a whole dwell and then some. What is blamed is the next entrance
            # still on the list, which is where the warp queue went and which never produced a
            # play frame; the scene the game is sitting in is healthy and is named for context.
            if ($todo.Count -gt 0) {
                $result[$todo[0]] = ('STALLS, no play frame within the dwell (the game sat healthy at {0})' -f $lastSeen)
                Write-Host ("  stalled on {0} (last play frame at {1}), relaunching" -f $todo[0], $lastSeen)
                [void]$todo.RemoveAt(0)
            }
            else {
                Write-Host ("  nothing new after {0}, relaunching" -f $lastSeen)
            }
            break
        }

        # The list is done when every entrance has been reached and its dwell has passed.
        if ($todo.Count -eq 0 -and ((Get-Date) - $lastMove).TotalSeconds -gt $DwellSeconds) { break }
    }

    if ($p.HasExited) {
        $crashed = if ($lastSeen -ne '') { $lastSeen } else { 'during startup' }
        $result[$crashed] = 'CRASHED'
        if ($todo.Contains($crashed)) { [void]$todo.Remove($crashed) }
        Write-Host ("  CRASHED on {0}" -f $crashed)
    }

    if (Test-Path $err) {
        $guardHits += (Select-String -Path $err -Pattern 'IMPLAUSIBLE NESTED LIST' -ErrorAction SilentlyContinue | Measure-Object).Count
    }
}

Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
Restore-Settings $savedSettings

Write-Host ''
Write-Host ("================ STRETCH RESULT: {0} ================" -f $Tag)
foreach ($k in ($result.Keys | Sort-Object)) { Write-Host ("  {0}  {1}" -f $k, $result[$k]) }
Write-Host ("  not attempted: {0}" -f ($todo -join ' '))
Write-Host ("  guard fired {0} times across the run" -f $guardHits)
