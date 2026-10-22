# vc_runtime.ps1: where the Visual C++ runtime files are, for everything that packs a copy.
#
#   powershell -ExecutionPolicy Bypass -File tools\vc_runtime.ps1     # one full path per line
#
# WHY THE PROGRAM CARRIES THESE (2026-09-28). The first person outside this machine to open a share
# got "it flashed as if it was loading and then closed and nothing happend". That is Windows
# refusing to load the process before a line of ours runs, so there is no window, no message and NO
# LOG to send back. The executable imports vcruntime140.dll, vcruntime140_1.dll, msvcp140.dll and
# msvcp140_atomic_wait.dll, and MICROSOFT'S OWN dxcompiler.dll and dxil.dll import the first three
# as well, so a statically linked executable would not have saved it either. None of them was in
# the share, and a machine without the Visual C++ redistributable has none of them.
#
# They ship in the folder rather than being asked for. Somebody handed a game should not be sent to
# install a redistributable first, and Windows searches the program's own folder before the system
# one, so this covers the shipped Microsoft DLLs too. The UCRT itself (api-ms-win-crt-*) is part of
# Windows and is not ours to carry.
#
# ONE SCRIPT, because there are two packers and they must never disagree: make_share.ps1 fills the
# portable folder, and CMake's own rule packs the setup file's payload (CMakeLists.txt, OOT_PAYLOAD).
# The first version of this put the search in make_share alone, and ninja then repacked the payload
# without them, so the zip was fixed and the setup file still was not.
#
# From the Visual Studio redist folder, which is the copy licensed to be shipped, never from
# System32. The newest version that holds all four wins.

$ErrorActionPreference = "Stop"

$names = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_atomic_wait.dll")

$roots = @()
if ($env:ProgramFiles) { $roots += (Join-Path $env:ProgramFiles "Microsoft Visual Studio") }
if (${env:ProgramFiles(x86)}) { $roots += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio") }

foreach ($root in $roots) {
    if (-not (Test-Path $root)) { continue }
    $folders = Get-ChildItem -Path $root -Recurse -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\Redist\\" -and $_.FullName -match "\\x64\\" -and $_.FullName -notmatch "onecore" } |
        Sort-Object FullName -Descending
    foreach ($folder in $folders) {
        $paths = @()
        foreach ($n in $names) {
            $p = Join-Path $folder.FullName $n
            if (Test-Path $p) { $paths += $p }
        }
        if ($paths.Count -eq $names.Count) {
            $paths | ForEach-Object { Write-Output $_ }
            exit 0
        }
    }
}

Write-Output ("VCRUNTIME_MISSING no x64 redist folder holds all of: " + ($names -join ", "))
exit 1
