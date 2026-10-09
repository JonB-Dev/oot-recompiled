# Record every presented frame across a few pause menu closes, for a flash that lasts one frame.
#
# The user saw a flash of lighting on the ground when the pause menu closes, in some places,
# especially in Kokiri Forest (2026-09-24), and asked for the in-game frame recorder to be the
# tool: a screenshot pair catches a frame in a few hundred; the recorder writes every one. So:
# warp, have the game start the recorder itself once it has run enough updates in play
# (`--record <seconds>,<play ticks>`, since a synthetic click on the plate only lands while the
# game is the foreground window, which it is not while someone is at the keyboard), open and
# close the pause menu a few times inside the recording, and let tools\pause_flash.py find the
# frames whose ground brightened against their neighbors.
#
# The recorder writes to the user's own Pictures folder under OoT-Recomp (places.h); the frames
# are pictures of the game's assets and stay on this machine.
#
#   tools\harness\pauseflash.ps1 -Entrance 0x0EE -Tag flash -ExtraArgs @('--rt-level','3')
param(
    [string]$Entrance = "0x0EE",
    [string]$Tag = "pauseflash",
    [int]$Closes = 3,
    # When the recording starts, in updates run in play, and how long it runs. The defaults
    # reach past the warp and the cleared conversation; 1 starts it at the first update, right
    # before the warp lands, so the transition and an arrival are on film (the user's suggestion,
    # 2026-09-24).
    [int]$RecordAt = 560,
    [int]$RecordSeconds = 10,
    # No warp and no key: the game is left on its title, which is the play state too, so a
    # recording from tick 1 films the title's own scene (the user's streak on the start screen,
    # 2026-09-24). -Closes is ignored.
    [switch]$NoWarp,
    [string[]]$ExtraArgs = @()
)

. (Join-Path $PSScriptRoot 'drive.ps1')

# The recorder writes into a folder per day and names each frame after the recording's start
# time, so a new recording is the newest time prefix among today's files.
$pictures = Join-Path ([Environment]::GetFolderPath('MyPictures')) 'OoT-Recomp'
function Get-RecordingPrefixes {
    if (-not (Test-Path $pictures)) { return @() }
    Get-ChildItem $pictures -Recurse -Filter *.png | ForEach-Object { $_.Name.Substring(0, 8) } | Sort-Object -Unique
}
$before = @(Get-RecordingPrefixes)

# The recording: by default ten seconds from the 560th update in play, which the timeline below
# reaches after the warp has landed and the arrival's conversation has been cleared (about
# 8 + 16 x 1.2 seconds after play begins, at twenty updates a second).
$recordAt = $RecordAt
$recordSeconds = $RecordSeconds

try {
    if ($NoWarp) {
        $p = Start-Game $Tag (@('--record', "$recordSeconds,$recordAt") + $ExtraArgs) -width 1280 -height 720
        Write-Host "pid $($p.Id), no warp, recording $recordSeconds s from play tick $recordAt on the title"
        Start-Sleep -Seconds ($recordSeconds + 20)
        return
    }

    $p = Start-Game $Tag (@('--warp', "$Entrance,$Entrance", '--record', "$recordSeconds,$recordAt") + $ExtraArgs) -width 1280 -height 720
    Write-Host "pid $($p.Id), warping to $Entrance, recording $recordSeconds s from play tick $recordAt, $Closes pause closes"
    Enter-Play
    Start-Sleep -Seconds 8
    # Clear whatever conversation the arrival opened, as pausecycle does.
    for ($i = 1; $i -le 16; $i++) {
        if ($p.HasExited) { break }
        Press 'A' 6 1200
    }

    # Into the recording: a moment for it to start, then the cycles, well inside its length.
    Start-Sleep -Seconds 2
    for ($c = 1; $c -le $Closes; $c++) {
        if ($p.HasExited) { break }
        Focus-Game
        Press 'START' 1 700     # open
        Press 'START' 1 700     # close
    }

    # Let the recording run out and drain.
    Start-Sleep -Seconds 8
}
finally {
    Close-Game
}

$after = @(Get-RecordingPrefixes)
$new = @($after | Where-Object { $before -notcontains $_ })
if ($new.Count -gt 0) {
    $prefix = $new | Select-Object -Last 1
    $frames = Get-ChildItem $pictures -Recurse -Filter "$prefix*.png"
    $dir = ($frames | Select-Object -First 1).DirectoryName
    Write-Host "=== $Tag`: recording $prefix in $dir, $($frames.Count) frames ==="
}
else {
    Write-Host "=== $Tag`: no recording appeared under $pictures ==="
}
