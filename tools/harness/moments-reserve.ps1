# moments-reserve.ps1: Mark as reserved and Unreserve on the Moments page (2026-10-13), in the
# launcher, so no game has to boot.
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\moments-reserve.ps1
#
# Turns the dial back to Moments, walks down to the first of your slots that holds a moment, presses
# it (the cursor goes to the buttons), goes up to Mark as reserved and presses it: the trace must
# say MOMENT_MOVED <slot> 101 and the list must grow a Reserved section. The cursor stays on the
# button, now Unreserve, and a second press must move it back (MOMENT_MOVED 101 <slot>). Every step
# is captured under build-cmake\harness\moments\. The build tree's own moments folder is copied
# aside first and put back after, whatever happens. Prints RESERVE_OK or RESERVE_FAIL <why>.
# Refuses to start while the program is running, because Start-Game closes it (it may be the
# user's game).
param([string]$Tag = "reserve")

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "RESERVE_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}
. (Join-Path $PSScriptRoot 'drive.ps1')

$out = Join-Path $script:HarnessOut 'moments'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$moments = Join-Path $script:HarnessRoot 'build-cmake\saves\moments'
$aside = Join-Path $script:HarnessRoot 'build-cmake\saves\moments-aside'
$trace = Join-Path $script:HarnessOut "$Tag.stderr.txt"
function Snap([string]$name) { Shot (Join-Path $out "$Tag-$name.png") }
function Seen([string]$pattern) {
    return @(Get-Content $trace -ErrorAction SilentlyContinue | Where-Object { $_ -match $pattern }).Count -gt 0
}

# The first of your slots holding a moment. The list opens on slot 1, so it is that many rows down,
# less one.
$first = 0
for ($s = 1; $s -le 8; $s++) { if (Test-Path (Join-Path $moments "$s.moment")) { $first = $s; break } }
if ($first -eq 0) { Write-Output "RESERVE_FAIL none of slots 1 to 8 holds a moment in build-cmake\saves\moments"; exit 1 }

Remove-Item -Recurse -Force $aside -ErrorAction SilentlyContinue
Copy-Item -Recurse $moments $aside
$verdict = "RESERVE_OK"
$p = $null
try {
    $p = Start-Game $Tag @() -NoPlay
    Start-Sleep -Seconds 6
    # Back round the dial from Play: About, then Moments.
    for ($i = 0; $i -lt 9 -and -not (Seen '\[ui\] screen moments'); $i++) { Press 'Z' 4 900 }
    if (-not (Seen '\[ui\] screen moments')) { throw "the Moments section did not open" }
    Snap 'list'
    for ($i = 0; $i -lt ($first - 1); $i++) { Press 'DDOWN' 4 250 }
    Press 'START' 6 700; Snap 'buttons'
    Press 'DUP' 4 500; Snap 'reserve-focused'
    Press 'START' 6 1200; Snap 'reserved'
    if (-not (Seen "^MOMENT_MOVED $first 101")) { throw "Mark as reserved did not move slot $first into r1" }
    if (-not (Test-Path (Join-Path $moments 'reserved\r1.moment'))) { throw "r1.moment is not in the reserved folder" }
    Press 'START' 6 1200; Snap 'unreserved'
    if (-not (Seen "^MOMENT_MOVED 101 ")) { throw "Unreserve did not move r1 back" }
}
catch {
    $verdict = "RESERVE_FAIL $($_.Exception.Message)"
}
finally {
    if ($p -and -not $p.HasExited) { Close-Game $p.Id | Out-Null }
    Remove-Item -Recurse -Force $moments -ErrorAction SilentlyContinue
    Rename-Item $aside 'moments'
    Select-String -Path $trace -Pattern 'MOMENT_|\[ui\] screen' -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("  " + $_.Line) }
}
Write-Output $verdict
