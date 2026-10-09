# Measure the CAPTURE, not the game: how many distinct pictures does a burst of back to back grabs
# hold while the camera pans?
#
# Built when the renderer's own counters said it presents three frames per game frame and the
# screen copies said the picture changes once per game frame. One of them is wrong about what is
# on the screen, and a screen copy taken by BitBlt is not guaranteed to see a swap chain that the
# compositor scans out on its own plane. So: a burst of grabs by both methods, and Python counts
# the distinct frames in each. Sixty hertz on screen gives about ten distinct in twelve grabs,
# twenty hertz about four.
#
#   tools\harness\burst.ps1 -Tag capture -Grabs 12
#   python tools\burst_count.py build-cmake\harness\burst\capture
param(
    [string]$Entrance = "0x0EE",
    [string]$Tag = "burst",
    [int]$FrameRate = 1,
    [int]$Grabs = 12,
    [string[]]$ExtraArgs = @()
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut "burst\$Tag"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path (Join-Path $out 'bitblt') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $out 'printwindow') | Out-Null

$saved = Use-Settings "framerate=$FrameRate"

try {
    $p = Start-Game $Tag (@('--warp', $Entrance) + $ExtraArgs)
    Write-Host "pid $($p.Id), warping to $Entrance, frame rate setting $FrameRate"

    Enter-Play
    Start-Sleep -Seconds 8
    for ($i = 1; $i -le 12; $i++) { if ($p.HasExited) { break }; Press 'A' 6 900 }

    # First person view, then pan: the camera itself moves, tens of pixels a game frame.
    Press 'CUP' 8 1500
    Focus-Game
    [Drv]::Down([uint16]$VK['SLEFT'])
    Start-Sleep -Milliseconds 800

    $ms1 = [Drv]::ShotBurst($script:hwnd, (Join-Path $out 'bitblt'), $Grabs, $false)
    Start-Sleep -Milliseconds 300
    $ms2 = [Drv]::ShotBurst($script:hwnd, (Join-Path $out 'printwindow'), $Grabs, $true)
    [Drv]::Up([uint16]$VK['SLEFT'])

    Write-Host ("=== {0}: {1} grabs, BitBlt burst {2:F0} ms, PrintWindow burst {3:F0} ms, alive={4} ===" -f $Tag, $Grabs, $ms1, $ms2, (-not $p.HasExited))
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Restore-Settings $saved
}
