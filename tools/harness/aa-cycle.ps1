# Walk the Antialiasing row through every value while the game is playing, one step at a time,
# and say which step killed it. Written for the tester's report of 2026-09-20: "if I turn the
# antialiasing to off the screen flashes black for a second and it crashes, and if I turn it up
# to x8 it flashes black but it doesn't crash".
#
#   tools\harness\aa-cycle.ps1 -Tag aa1
#   tools\harness\aa-cycle.ps1 -Tag aa-launcher -InLauncher
#
# Antialiasing is row 5 counting from the top (window, resolution, downsampling, aspect ratio,
# interface ratio, antialiasing), values Off, 2x, 4x, 8x. The run starts at the value -Start
# names, walks DOWN to Off one press at a time, then back UP to 8x, checking after each press
# that the process is still alive and that a frame is still being drawn. Every change goes
# through the same path a person's does: the settings document, the runtime, then RT64's
# updateMultisampling, which destroys the shader cache and every render target.
#
# -InLauncher makes the same changes before Play is pressed, where no game frame exists yet.
# If it survives there and dies in play, the difference is the game's own workload in flight.
param([string]$Tag = "aa", [int]$Start = 2, [switch]$InLauncher)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'aa'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$settingsFile = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
Set-Setting 'antialiasing' $Start
Set-Setting 'window' 0

$names = @('Off', '2x', '4x', '8x')
$script:proc = $null

function Alive {
    return $null -ne (Get-Process -Id $script:proc.Id -ErrorAction SilentlyContinue)
}

# One press on the row, then a look: is it still running, and did it draw anything after.
function Step {
    param([string]$key, [int]$from, [int]$to, [string]$label)
    Write-Host ""
    Write-Host "--- $label : $($names[$from]) -> $($names[$to])"
    if (-not (Alive)) { Write-Host "    already gone before this step"; return $false }
    Press $key 5 3500
    if (-not (Alive)) {
        Write-Host "    DIED on this step"
        return $false
    }
    Write-Host "    alive"
    Shot (Join-Path $out "$Tag-$label-$($names[$to]).png")
    return $true
}

try {
    if ($InLauncher) {
        $script:proc = Start-Game $Tag @() -NoPlay
        Start-Sleep -Seconds 6
        # The launcher's own list: the ROM row, then the settings rows in the same order.
        Press 'DDOWN' 5 300
    }
    else {
        $script:proc = Start-Game $Tag @()
        Enter-Play
        Start-Sleep -Seconds 4
        Press 'F1' 6 1500
    }
    for ($i = 0; $i -lt 5; $i++) { Press 'DDOWN' 5 250 }
    Shot (Join-Path $out "$Tag-row.png")

    $v = $Start
    while ($v -gt 0) {
        if (-not (Step 'DLEFT' $v ($v - 1) "down")) { break }
        $v--
    }
    while ($v -lt 3 -and (Alive)) {
        if (-not (Step 'DRIGHT' $v ($v + 1) "up")) { break }
        $v++
    }

    Write-Host ""
    Write-Host "=== ended at $($names[$v]), process $(if (Alive) { 'alive' } else { 'GONE' }) ==="
    $err = Join-Path $script:HarnessOut "$Tag.stderr.txt"
    if (Test-Path $err) {
        Write-Host "=== last 40 lines of the trace ==="
        Get-Content $err | Select-Object -Last 40
    }
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
    elseif (Test-Path $settingsFile) { Remove-Item -Force $settingsFile }
}
