# Capture the startup hint (phase 50): the toast over the game that names the key that opens
# the menu, shown when the application starts, gone on the first press or after five seconds.
#
#   tools\harness\hint-shot.ps1 -Tag p50-hint
#   tools\harness\hint-shot.ps1 -Tag p50-hint-pad -Pad
#
# Two launches. The first captures the window two seconds in (the hint should be there), presses
# a key, and captures again a second later (it should be gone). The second presses nothing and
# captures at two seconds and at seven (gone on its own). With -Pad the virtual pad is attached
# with an empty script, so the hint's second line names the pad's menu button as well. The trace's
# `[ui] hint:` line says what the hint said, and `[ui] hint gone` when it went.
param([string]$Tag = "hint", [switch]$Pad)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'hint'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# In the temp directory: the launch helper hands extra arguments to the game unquoted, and the
# project's path has spaces.
$scriptPath = Join-Path $env:TEMP "oot-$Tag.virtual.txt"
$extra = @()
if ($Pad) {
    @('# no events: the pad only has to be present for the hint to name its menu button') | Set-Content -Encoding ascii $scriptPath
    $extra = @('--virtual-controller', $scriptPath)
}

function Report([string]$run) {
    $err = Join-Path $script:HarnessOut "$run.stderr.txt"
    Write-Host "=== $run ==="
    if (Test-Path $err) { Select-String -Path $err -Pattern '\[ui\]|controller added' | ForEach-Object { Write-Host ("  " + $_.Line) } }
}

try {
    $run = "$Tag-press"
    $p = Start-Game $run $extra
    Start-Sleep -Milliseconds 2000
    Shot (Join-Path $out "$run-before.png")
    Press 'DDOWN' 6 1000
    Shot (Join-Path $out "$run-after.png")
    Start-Sleep -Milliseconds 500
    Report $run

    $run = "$Tag-wait"
    $p = Start-Game $run $extra
    Start-Sleep -Milliseconds 2000
    Shot (Join-Path $out "$run-before.png")
    Start-Sleep -Milliseconds 5500
    Shot (Join-Path $out "$run-after.png")
    Start-Sleep -Milliseconds 500
    Report $run
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item -Force $scriptPath -ErrorAction SilentlyContinue
}
