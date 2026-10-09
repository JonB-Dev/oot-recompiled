# moments-doc.ps1: prove the Moments surface and the quick keys (phase 77).
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\moments-doc.ps1
#
# In an open scene (a queued warp past the intro): F6 saves slot 1 (MOMENT_WRITTEN 1), F9 resumes
# it (MOMENT_RESUMED 1); then F1 opens the menu, the focus walks down to the Moments row, Enter
# opens the document ([ui] screen moments in the trace), a capture is taken of it, Delete arms
# and deletes the focused slot (MOMENT_DELETED), and Escape closes. Prints MOMENTS_DOC_OK or
# MOMENTS_DOC_FAIL <why>. Refuses to start while the program is running.
param(
    [string]$Entrance = "0x00CD",
    [int]$WaitSeconds = 60
)
. (Join-Path $PSScriptRoot 'drive.ps1')

if (Get-Process OoTRecompiled -ErrorAction SilentlyContinue) {
    Write-Output "MOMENTS_DOC_FAIL the program is already running; close it first (it may be the user's game)"
    exit 1
}

$tag = "moments-doc"
$trace = Join-Path $script:HarnessOut "$tag.stderr.txt"
$out = Join-Path $script:HarnessOut "moments"
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Wait-Line([string]$pattern, [int]$seconds) {
    for ($i = 0; $i -lt ($seconds * 2); $i++) {
        $lines = Get-Content $trace -ErrorAction SilentlyContinue
        if ($lines | Where-Object { $_ -match $pattern }) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

$p = Start-Game $tag @('--warp', "$Entrance,$Entrance", '--dwell', '100')
Write-Host "pid $($p.Id), queued warp to $Entrance"
Enter-Play
Start-Sleep -Seconds 10

# The quick keys.
Press 'F6' 6 1500
if (-not (Wait-Line '^MOMENT_WRITTEN 1 ' 20)) { if (-not $p.HasExited) { Close-Game $p.Id | Out-Null }; Write-Output "MOMENTS_DOC_FAIL F6 did not write slot 1"; exit 1 }
Press 'F9' 6 1500
if (-not (Wait-Line '^MOMENT_RESUMED 1' 30)) { if (-not $p.HasExited) { Close-Game $p.Id | Out-Null }; Write-Output "MOMENTS_DOC_FAIL F9 did not resume slot 1"; exit 1 }
Start-Sleep -Seconds 6

# The document: F1, then UP to the Moments row rather than down to it.
#
# Walking DOWN means counting the settings rows, and that count has grown every time the lighting
# work added a control: the walk was written for thirteen settings and there are twenty three, so
# it stopped on a settings row and Enter did nothing, which this reported as "the Moments screen
# did not open" (2026-09-25, the user: "your harness doesn't scroll down far enough"). Walking UP
# from the top wraps to the END of the list, which in play is Quit, then Debug, then Moments, and
# that tail does not move when a setting is added. Three presses, for ever.
Press 'F1' 6 1200
for ($i = 0; $i -lt 3; $i++) { Press 'DUP' 4 180 }
Press 'START' 6 1500
if (-not (Wait-Line '^\[ui\] screen moments' 5)) { if (-not $p.HasExited) { Close-Game $p.Id | Out-Null }; Write-Output "MOMENTS_DOC_FAIL the Moments screen did not open"; exit 1 }
Shot (Join-Path $out 'moments-open.png')
# Down to slot 1 (the list starts on it; one Up and one Down proves the walk), then Delete twice.
Press 'DUP' 4 300
Press 'DDOWN' 4 300
Press 'BACK' 6 800
Shot (Join-Path $out 'moments-delete-armed.png')
Press 'BACK' 6 800
if (-not (Wait-Line '^MOMENT_DELETED 1' 5)) { if (-not $p.HasExited) { Close-Game $p.Id | Out-Null }; Write-Output "MOMENTS_DOC_FAIL Delete did not remove slot 1"; exit 1 }
Shot (Join-Path $out 'moments-after-delete.png')
Press 'ESC' 6 800

(Get-Content $trace -ErrorAction SilentlyContinue) | Where-Object { $_ -match '^\[moments\]|^MOMENT_|screen moments|thumbnail' } | ForEach-Object { Write-Host "  $_" }
if (-not $p.HasExited) { if (-not (Close-Game $p.Id)) { Stop-Process -Id $p.Id -Force } }
Write-Output "MOMENTS_DOC_OK quick keys and the document; captures in $out"
exit 0
