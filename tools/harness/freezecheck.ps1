# freezecheck.ps1: is a frozen run stable with ITSELF? (phase 52)
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\freezecheck.ps1 -Entrance 0x0102 -Name lake-hylia
#
# Starts one run the way abshots.ps1 does (queued warp, seed, freeze), waits for the "[shot] frozen"
# line, takes a capture, waits -GapSeconds, takes another, closes the game. Prints FREEZECHECK_OK
# when the two are byte identical and FREEZECHECK_DRIFT <pixels> when they are not: drift means
# something in the draw still moves with the wall clock or the renderer's own frame count, which
# the freeze has to pin before A against B can mean anything in that scene. Refuses to start while
# the program is running.
param(
    [Parameter(Mandatory = $true)][string]$Entrance,
    [Parameter(Mandatory = $true)][string]$Name,
    [int]$GapSeconds = 3,
    [uint32]$FreezeTicks = 100,
    [uint32]$Seed = 1,
    [int]$SettleSeconds = 30
)
. (Join-Path $PSScriptRoot 'drive.ps1')

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "FREEZECHECK_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}

$out = Join-Path $script:HarnessOut "freezecheck\$Name"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$runTag = "freezecheck-$Name"
$p = Start-Game $runTag @('--warp', "$Entrance,$Entrance", '--dwell', '100', '--seed', "$Seed", '--freeze', "$Entrance,$FreezeTicks")
Write-Host "pid $($p.Id), $Name at $Entrance"
Enter-Play
$frozen = $false
$tracePath = Join-Path $script:HarnessOut "$runTag.stderr.txt"
for ($i = 0; $i -lt ($SettleSeconds * 2); $i++) {
    if ($p.HasExited) { break }
    $trace = Get-Content $tracePath -ErrorAction SilentlyContinue
    if ($trace | Where-Object { $_ -match '^\[shot\] frozen' }) { $frozen = $true; break }
    Start-Sleep -Milliseconds 500
}
if ($p.HasExited -or -not $frozen) {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Write-Output "FREEZECHECK_FAIL never froze"
    exit 1
}
Start-Sleep -Seconds 1
Shot (Join-Path $out "first.png")
Start-Sleep -Seconds $GapSeconds
Shot (Join-Path $out "second.png")
if (-not (Close-Game $p.Id)) { Stop-Process -Id $p.Id -Force }

$a = [IO.File]::ReadAllBytes((Join-Path $out "first.png"))
$b = [IO.File]::ReadAllBytes((Join-Path $out "second.png"))
$same = ($a.Length -eq $b.Length)
if ($same) { for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { $same = $false; break } } }
if ($same) { Write-Output "FREEZECHECK_OK $Name is stable with itself over $GapSeconds seconds"; exit 0 }
Write-Output "FREEZECHECK_DRIFT $Name changed over $GapSeconds seconds while frozen (see $out)"
exit 2
