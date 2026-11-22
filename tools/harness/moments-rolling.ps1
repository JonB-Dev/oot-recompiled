# moments-rolling.ps1: prove the autosave keeps the most recent FIVE and drops the oldest.
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\moments-rolling.ps1
#
# The user asked for five rolling autosaves above a divider (2026-09-25). One run in an open
# scene with the interval forced very short, long enough for more than five autosaves to land.
# Then the store must hold exactly five, named "auto" through "auto5", each a whole moment, and
# they must be in age order: "auto" the newest, "auto5" the oldest, by the captured_at each one
# carries. A sixth would mean nothing is dropped; fewer than five after enough time would mean
# the rotation is eating its own.
#
# Prints MOMENTS_ROLLING_OK or MOMENTS_ROLLING_FAIL <why>. Refuses to start while the program is
# running, as every harness here does.
param(
    [string]$Entrance = '0x00CD',
    [int]$Seconds = 70
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'drive.ps1')

$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$store = Join-Path $root 'build-cmake\saves\moments'

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output 'MOMENTS_ROLLING_FAIL the program is already running'
    exit 1
}

# A clean store, so the count at the end means what it says. The old ones are moved aside rather
# than deleted: a harness has no business destroying a person's saves.
if (Test-Path $store) {
    $aside = Join-Path $env:TEMP ('moments-rolling-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $aside | Out-Null
    Get-ChildItem $store -File | ForEach-Object { Move-Item $_.FullName (Join-Path $aside $_.Name) }
}

# Six autosaves' worth of time at three seconds apart, which is well past the five that are kept.
$p = Start-Game 'moments-rolling' @('--warp', "$Entrance,$Entrance", '--dwell', '100', '--autosave-ticks', '60')
Enter-Play | Out-Null
Start-Sleep -Seconds $Seconds
if (-not $p.HasExited) { Close-Game $p.Id | Out-Null }

$moments = @(Get-ChildItem $store -Filter 'auto*.moment' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($moments.Count -ne 5) {
    Write-Output "MOMENTS_ROLLING_FAIL the store holds $($moments.Count) autosaves, not five: $(($moments | ForEach-Object { $_.Name }) -join ', ')"
    exit 1
}

$expected = @('auto.moment', 'auto2.moment', 'auto3.moment', 'auto4.moment', 'auto5.moment')
foreach ($name in $expected) {
    if (-not ($moments | Where-Object { $_.Name -eq $name })) {
        Write-Output "MOMENTS_ROLLING_FAIL $name is missing"
        exit 1
    }
}

# Age order: every moment carries the second it was captured at, at a fixed offset in its header
# (moments.h: captured_at at 0x20, eight bytes little endian). "auto" must be the newest and each
# one after it older than the last.
function Captured-At([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    return [System.BitConverter]::ToUInt64($bytes, 0x20)
}

$previous = [uint64]::MaxValue
foreach ($name in $expected) {
    $when = Captured-At (Join-Path $store $name)
    if ($when -gt $previous) {
        Write-Output "MOMENTS_ROLLING_FAIL $name is newer than the one before it, so the rotation is out of order"
        exit 1
    }
    $previous = $when
}

Write-Output "MOMENTS_ROLLING_OK five autosaves, newest first, oldest dropped"
