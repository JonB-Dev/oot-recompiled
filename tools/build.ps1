# Build everything, in one command.
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -NoShare   # just the exe
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -NoSign    # skip signing
#
# WHY THIS EXISTS (the user, 2026-09-23: "Are you building it in two separate commands and why are
# you having to do it via PowerShell? You should just build a script that does this all
# automatically"). It was two commands because `build_native.cmd` and `make_share.ps1` were
# written in different phases, and both have to run every time: the user tests from `release\`, so
# a native build that is not followed by a share build leaves them testing the previous one.
#
# Nothing here is new work. It runs the three scripts that already exist (the patches and their
# recompile, the native build, the share), in the order that is already required, and reports one
# result. Each still works on its own.
#
# THE TRAP IT ALSO CLOSES. The native build must run inside the MSVC environment, which is why it
# is a .cmd that calls vcvars64 (its own header explains what goes wrong otherwise). Invoking that
# .cmd through some shells returns a success code having run nothing at all, printing only the
# command prompt banner, which reads exactly like a build that had nothing to do. This script
# checks the executable's timestamp actually moved rather than trusting an exit code.

param(
    [switch]$NoShare,
    [switch]$NoSign
)

# NOT "Stop". Redirecting a native command's stderr in Windows PowerShell wraps every line in
# an ErrorRecord, so one harmless warning from the toolchain (vswhere not being on PATH, which the
# build handles) would abort the whole script. Failures here are detected by looking at what the
# tools actually produced, which is the honest test anyway.
$ErrorActionPreference = "Continue" 
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build-cmake\OoTRecompiled.exe"

function Fail($why) {
    Write-Output ""
    Write-Output "BUILD FAILED: $why"
    exit 1
}

Write-Output ""
Write-Output "1/3  the patches, and their recompile"

# The patches are part of the build, not a separate step to remember: a native build that follows
# a changed patch WITHOUT this relinks the previous patch output and reports success, which is a
# build that tests the wrong thing while looking right. The step is incremental (make) and then a
# recompile of the patch ELF, which refuses when a patch reaches a symbol the game does not have.
$patches = & cmd /c "`"$(Join-Path $PSScriptRoot 'build_patches.cmd')`""
$patches | Select-String -Pattern "error:|warning:|PATCHES_OK|RECOMPILED_OK|FAILED|Undefined" |
    ForEach-Object { Write-Output "     $_" }
if ($patches -match "PATCH BUILD FAILED") { Fail "a patch did not compile" }
if ($patches -match "PATCH RECOMPILE FAILED") { Fail "the patch recompile was refused" }
if (-not ($patches -match "RECOMPILED_OK")) { Fail "the patch step did not finish" }

Write-Output ""
Write-Output "2/3  the program"

# No ternary: this is Windows PowerShell 5.1, where `?:` is a parser error rather than an
# operator. Same reason there are no `&&` chains below.
$before = [datetime]::MinValue
if (Test-Path $exe) { $before = (Get-Item $exe).LastWriteTime }
$native = & cmd /c "`"$(Join-Path $PSScriptRoot 'build_native.cmd')`""
$native | Select-String -Pattern "error:|NATIVE_OK|NATIVE BUILD FAILED" |
    ForEach-Object { Write-Output "     $_" }
if ($native -match "NATIVE BUILD FAILED") { Fail "the program did not compile" }
# A SHADER ERROR SAYS NEITHER (2026-10-08): the renderer's shaders failed to compile, the step
# printed the errors and neither NATIVE_OK nor its failure line, and this script went on to say
# BUILD OK over an executable that had not been rebuilt. Success is now the line itself.
if (-not ($native -match "NATIVE_OK")) { Fail "the program step did not finish (no NATIVE_OK)" }

if (-not (Test-Path $exe)) { Fail "the executable is not there" }
$after = (Get-Item $exe).LastWriteTime
if ($after -eq $before) {
    # Not a failure: a build with nothing to do legitimately leaves it alone. Said out loud
    # because the other way to get here is the .cmd returning success having run nothing, which
    # looks identical and has wasted time before.
    Write-Output ("     unchanged  {0:HH:mm:ss}   (nothing to rebuild, or nothing ran)" -f $after)
} else {
    Write-Output ("     built      {0:HH:mm:ss}" -f $after)
}

if ($NoShare) {
    Write-Output ""
    Write-Output "BUILD OK  (share skipped)"
    exit 0
}

Write-Output ""
Write-Output "3/3  the signed share, both artifacts"

$shareArgs = @()
if ($NoSign) { $shareArgs += "-NoSign" }
$out = & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "make_share.ps1") @shareArgs
$out | Select-String -Pattern "SHARE_OK|SHARE_FAIL|setup:|zip:|signed" |
    ForEach-Object { Write-Output "     $_" }
if ($out -match "SHARE_FAIL") { Fail "the share could not be built" }

$release = Join-Path $root "release"
Write-Output ""
Write-Output "BUILD OK"
Get-ChildItem (Join-Path $release "*windows.*") |
    Sort-Object Name |
    ForEach-Object {
        # With the date: a clock time alone let an older version's files, built at a later hour
        # on an earlier day, read as the newest thing in the folder.
        Write-Output ("  {0,-40} {1,12:N0} bytes   {2:yyyy-MM-dd HH:mm:ss}" -f $_.Name, $_.Length, $_.LastWriteTime)
    }
Write-Output ""
Write-Output "  Test from release\. Nothing was sent anywhere and the version did not move."
