# moments-older.ps1: an OLDER autosave loads (2026-10-13).
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\moments-older.ps1
#
# The four older autosaves are slots -1 to -4, and the staged load used -1 for "nothing staged",
# so loading one of them was accepted and then never happened. This copies the newest autosave
# (auto.*) into the place two back (auto3.*, slot -2), starts the game with --moment-load -2, walks
# the intro into a file, and waits for MOMENT_RESUMED -2, with the autosave off for the run. The
# moments folder is copied aside first and put back after, whatever happens. Prints
# MOMENTS_OLDER_OK or MOMENTS_OLDER_FAIL <why>. Refuses to start while the program is running,
# because Start-Game closes it (it may be the user's game).
param([int]$WaitSeconds = 75)

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "MOMENTS_OLDER_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}
. (Join-Path $PSScriptRoot 'drive.ps1')

$tag = "moments-older"
$trace = Join-Path $script:HarnessOut "$tag.stderr.txt"
$dir = Join-Path $script:HarnessRoot "build-cmake\saves\moments"
if (-not (Test-Path (Join-Path $dir 'auto.moment'))) { Write-Output "MOMENTS_OLDER_FAIL no autosave (auto.moment) to copy"; exit 1 }
if (Test-Path (Join-Path $dir 'auto3.moment')) { Write-Output "MOMENTS_OLDER_FAIL auto3.moment already exists; this run would replace it"; exit 1 }

$aside = Join-Path $script:HarnessRoot "build-cmake\saves\moments-aside"
Remove-Item -Recurse -Force $aside -ErrorAction SilentlyContinue
Copy-Item -Recurse $dir $aside
$saved = Use-Settings "autosave=0"
foreach ($ext in '.moment', '.png', '.rgba') {
    $from = Join-Path $dir "auto$ext"
    if (Test-Path $from) { Copy-Item $from (Join-Path $dir "auto3$ext") }
}
$verdict = "MOMENTS_OLDER_OK"
$p = $null
try {
    $p = Start-Game $tag @('--moment-load', '-2')
    Enter-Play
    $resumed = $false
    for ($i = 0; $i -lt ($WaitSeconds * 2); $i++) {
        if (@(Get-Content $trace -ErrorAction SilentlyContinue | Where-Object { $_ -match '^MOMENT_RESUMED -2' }).Count -gt 0) { $resumed = $true; break }
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Seconds 3
    Shot (Join-Path $script:HarnessOut "$tag.png")
    if (-not $resumed) { throw "MOMENT_RESUMED -2 never came" }
}
catch {
    $verdict = "MOMENTS_OLDER_FAIL $($_.Exception.Message)"
}
finally {
    if ($p -and -not $p.HasExited) { Close-Game $p.Id | Out-Null }
    Restore-Settings $saved
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    Rename-Item $aside 'moments'
    Select-String -Path $trace -Pattern 'MOMENT_|\[moments\]' -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("  " + $_.Line) }
}
Write-Output $verdict
