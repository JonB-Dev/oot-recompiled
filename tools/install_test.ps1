# install_test.ps1: install the build tree's program as the TEST copy, beside the installed one.
#
#   powershell -ExecutionPolicy Bypass -File tools\install_test.ps1 [-Label "stick fix"] [-NoShortcut]
#
# The installed program lives in %LOCALAPPDATA%\OoT Recompiled\bin\ and is never written to: a test
# build goes to bin-test\ beside it, so the known-good copy stays one click away (my note,
# 2026-10-08: "i neevr want to accidentally regress here"). The test copy starts from bin\ (the
# runtime libraries and the `.installed` marker, which makes it use my own saves, settings and
# moments: places.cpp), then takes the build tree's executable, SDL2.dll and assets\ over them.
# install::register_for keeps the shortcuts and the uninstall entry with the program in bin\, and
# the uninstaller is not copied, so the test copy claims neither.
#
# A desktop shortcut, "OoT Recompiled TEST" or "OoT Recompiled TEST (<label>)", starts it; a TEST
# shortcut an earlier install left is replaced, so only one is ever on the desktop.
#
# Refuses while the test copy is running: its files are locked, and a half copied install is worse
# than none. Prints INSTALL_TEST_OK <folder> once the copied executable's hash matches the build's,
# or INSTALL_TEST_FAIL <why>; exits 1 on a failure.
param(
    [string]$Label = "",
    [switch]$NoShortcut,
    # Where things are, these when left out; the tests point them at throwaway folders.
    [string]$Source = "",
    [string]$DataDir = "",
    [string]$Desktop = ""
)
$ErrorActionPreference = 'Continue'
# Here and not in the parameters' defaults: Windows PowerShell leaves $PSScriptRoot empty there.
if (-not $Source) { $Source = Join-Path (Split-Path -Parent $PSScriptRoot) 'build-cmake' }
if (-not $DataDir) { $DataDir = Join-Path $env:LOCALAPPDATA 'OoT Recompiled' }
if (-not $Desktop) { $Desktop = [Environment]::GetFolderPath('Desktop') }

$exeName = 'OoTRecompiled.exe'
$shortcutName = 'OoT Recompiled TEST'
$installed = Join-Path $DataDir 'bin'
$test = Join-Path $DataDir 'bin-test'

function Fail([string]$why) {
    Write-Output "INSTALL_TEST_FAIL $why"
    exit 1
}

# Robocopy's exit codes below 8 are successes (files copied, extra files seen, and so on).
function Copy-Tree([string]$from, [string]$to, [string[]]$more) {
    $output = & robocopy $from $to /E /NFL /NDL /NJH /NJS /NP @more 2>&1
    if ($LASTEXITCODE -ge 8) { Fail "copying $from to $to failed: $(($output -join ' ').Trim())" }
}

$built = Join-Path $Source $exeName
if (-not (Test-Path -LiteralPath $built)) { Fail "no build at $built; build the program first" }
if (-not (Test-Path -LiteralPath (Join-Path $installed $exeName))) { Fail "no installed copy at $installed to sit beside" }
if (-not (Test-Path -LiteralPath (Join-Path $installed '.installed'))) {
    Fail "$installed has no .installed marker, so a copy of it would keep its own saves instead of mine"
}

$running = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($exeName)) -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($test + '\', [StringComparison]::OrdinalIgnoreCase) }
if ($running) { Fail "the test copy is running (process $($running[0].Id)); close it first" }

New-Item -ItemType Directory -Force -Path $test | Out-Null
Copy-Tree $installed $test @('/XF', 'uninstall.exe', $exeName, 'SDL2.dll', '/XD', (Join-Path $installed 'assets'))
foreach ($file in @($exeName, 'SDL2.dll')) {
    Copy-Item -LiteralPath (Join-Path $Source $file) -Destination (Join-Path $test $file) -Force -ErrorAction Stop
}
# The build's assets replace the folder whole, so an asset the build dropped does not linger.
Copy-Tree (Join-Path $Source 'assets') (Join-Path $test 'assets') @('/PURGE')

$want = (Get-FileHash -LiteralPath $built -Algorithm SHA256).Hash
$got = (Get-FileHash -LiteralPath (Join-Path $test $exeName) -Algorithm SHA256).Hash
if ($want -ne $got) { Fail "the copied executable does not match the build" }

if (-not $NoShortcut) {
    Get-ChildItem -LiteralPath $Desktop -Filter "$shortcutName*.lnk" -ErrorAction SilentlyContinue | Remove-Item -Force
    $clean = ($Label -replace '[\\/:*?"<>|]', ' ').Trim()
    $name = if ($clean) { "$shortcutName ($clean)" } else { $shortcutName }
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Join-Path $Desktop "$name.lnk"))
    $link.TargetPath = Join-Path $test $exeName
    $link.WorkingDirectory = $test
    $link.Save()
    Write-Output "  shortcut: $name"
}

Write-Output "INSTALL_TEST_OK $test"
