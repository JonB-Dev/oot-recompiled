# Capture the window chrome of BOTH surfaces so they can be compared as pixels rather than from
# memory: the launcher's title bar, and the game's own bar that rides over the picture.
#
#   tools\harness\chrome-shot.ps1 -Tag chrome1
#
# The user, 2026-09-20: "this close button in the game window bar doesn't look correct like the
# launcher". Both bars are drawn by this program from the same markup (launch.rml and frame.rml
# carry identical chrome), so any difference between them comes from the stylesheet, and the only
# honest way to see it is side by side at the same magnification.
#
# THE GAME'S BAR ONLY APPEARS WHILE THE POINTER IS AT THE TOP OF THE WINDOW, and it hides again
# on its own. Moving the pointer with a delta of ZERO does not count as motion to Windows, so the
# bar is never shown and the capture looks like a bug in the bar rather than in the test; that
# cost a run once already, and is why the pointer is nudged by two pixels before each grab.
param(
    [string]$Tag = "chrome",
    [int]$Width = 1280,
    [int]$Height = 720
)

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'chrome'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$settingsFile = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
$saved = if (Test-Path $settingsFile) { @(Get-Content $settingsFile) } else { $null }
Set-Setting 'window' 0
Set-Setting 'counter' 0

# Put the pointer inside the window's top band, move it, and capture WHILE it is still there.
#
# The bar shows while the pointer is in the top 30 pixels and for 1.2 seconds after it leaves
# (ui_shell.cpp, HEADER_LINGER_SECONDS), so the whole nudge-and-grab has to fit inside that. One
# nudge followed by a capture is not reliable: it worked on one run and missed on the next, and a
# missed bar looks exactly like a bar that is broken. So the pointer is nudged repeatedly and
# several grabs are taken, and the caller keeps the one that actually has the bar in it.
function Grab-WithBar {
    param([string]$path, [int]$attempts = 4)
    # The struct's fields are L, T, R, B in drive.ps1, not Left/Top/Right/Bottom.
    $r = New-Object Drv+RECT
    [Drv]::GetWindowRect($script:hwnd, [ref]$r) | Out-Null
    $x = $r.L + [int](($r.R - $r.L) / 2)
    $y = $r.T + 8

    for ($i = 0; $i -lt $attempts; $i++) {
        Focus-Game
        [Drv]::SetCursorPos($x, $y) | Out-Null
        Start-Sleep -Milliseconds 100
        [Drv]::SetCursorPos($x + 2 + $i, $y + 1) | Out-Null
        Start-Sleep -Milliseconds 250
        # ChangeExtension with $null leaves a trailing dot, so the attempts came out named
        # "ch3-launcher.-try0.png". Built from the directory and the base name instead.
        $try = Join-Path ([System.IO.Path]::GetDirectoryName($path)) `
                         ([System.IO.Path]::GetFileNameWithoutExtension($path) + "-try$i.png")
        [Drv]::Shot($script:hwnd, $try) | Out-Null
        Write-Host "  attempt $i -> $try"
    }
    # EVERY ATTEMPT IS KEPT AND NONE IS JUDGED AUTOMATICALLY. Deciding "is the bar in this grab"
    # from pixels was tried and got it exactly backward: the test was whether the top row is
    # uniform, and the bar is NOT uniform (it carries the mark, the title and three glyphs) while
    # a dark scene behind a hidden bar IS. A detector that confidently reports the opposite of the
    # truth is worse than no detector, and this is a check on how something LOOKS, which a person
    # has to see anyway. So the run leaves the attempts side by side and the last one is copied to
    # the plain name for convenience.
    $last = Join-Path ([System.IO.Path]::GetDirectoryName($path)) `
                      ([System.IO.Path]::GetFileNameWithoutExtension($path) + "-try$($attempts - 1).png")
    Copy-Item $last $path -Force
    return $true
}

try {
    # The launcher first, with no --play.
    $p = Start-Game "$Tag-launch" @() -NoPlay
    Start-Sleep -Seconds 7
    Grab-WithBar (Join-Path $out "$Tag-launcher.png") | Out-Null
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1

    # Then the game, where the same chrome rides over the picture.
    $p = Start-Game "$Tag-game" @('--warp', '0x0EE') -width $Width -height $Height
    Enter-Play
    Start-Sleep -Seconds 6
    Grab-WithBar (Join-Path $out "$Tag-game.png") | Out-Null

    Write-Host "captured $out\$Tag-launcher.png and $out\$Tag-game.png"
}
finally {
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($null -ne $saved) { Set-Content -Path $settingsFile -Value $saved -Encoding ASCII }
    elseif (Test-Path $settingsFile) { Remove-Item -Force $settingsFile }
}
