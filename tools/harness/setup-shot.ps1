# The setup window (the menus plan, phase 115), captured: its first screen as it opens, and the
# Install button under the pointer. Nothing is installed: the window is closed before any press.
#
#   tools\harness\setup-shot.ps1 -Tag p115
param([string]$Tag = "setup")

. (Join-Path $PSScriptRoot 'drive.ps1')
$out = Join-Path $script:HarnessOut 'setup'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $script:HarnessRoot 'build-cmake\OoTRecompiledSetup.exe'

$p = Start-Process -FilePath $exe -PassThru
try {
    # The setup window is not SDL's, so it is the process's own main window (PowerShell turns a
    # null window title into an empty string, so FindWindow by class alone cannot be called here).
    for ($i = 0; $i -lt 60 -and $script:hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250
        $p.Refresh()
        $script:hwnd = $p.MainWindowHandle
    }
    if ($script:hwnd -eq [IntPtr]::Zero) { throw "the setup window never appeared" }
    Start-Sleep -Milliseconds 1200
    Shot (Join-Path $out "$Tag-open.png")
    # The pointer over the Install button (the right half of the action bar), without a click.
    $r = New-Object Drv+POINT
    $r.X = 420; $r.Y = 370
    [Drv]::ClientToScreen($script:hwnd, [ref]$r) | Out-Null
    [Drv]::SetCursorPos($r.X, $r.Y) | Out-Null
    Start-Sleep -Milliseconds 500
    Shot (Join-Path $out "$Tag-hover.png")
}
finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
