# make_share.ps1: the folder that can be handed to another person, and its zip.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_share.ps1 [-Source build-cmake] [-NoSign]
#
# Copies ONLY what the program is: the executable, our own SDL2.dll, Microsoft's dxcompiler.dll
# and dxil.dll, the four Visual C++ runtime files they all need (toolsc_runtime.ps1 says why),
# assets\, and a README for the person receiving it. Never the ROM, never the
# runtime's stored copy of it, never saves, settings, bindings or logs: the receiver brings their
# own copy of the game, and the launcher asks for it. The result is checked for anything with a
# ROM's extension before it is zipped, and the copy refuses if one is found.
#
# The executable and SDL2.dll are signed in the share folder (tools\sign.ps1) unless -NoSign, and
# the signed executable is copied back over release\OoT-Recompiled-<version>\ so the user's own
# folder carries the same signed file. Prints SHARE_OK <zip> or SHARE_FAIL <why>.
#
# TWO ARTIFACTS SHIP, not one (the user, 2026-09-20: "release the portable version as well as the
# bundled EXE version"). This zip IS the portable one, unchanged: the program runs portable when
# there is no `.installed` marker beside it, keeping its saves and settings in its own folder. With
# it also builds, every time, the single downloadable executable that installs into the user's own
# application data, and the ORDER matters: the payload inside that file has to be packed from the
# SIGNED binaries, or the thing a person installs is unsigned inside a signed wrapper.
param(
    [string]$Source = "",
    [switch]$NoSign,
    # THE SETUP FILE IS BUILT EVERY TIME, and this is a standing instruction rather than a
    # default worth revisiting (the user, 2026-09-23, of the setup file: "that alsdo must be
    # included every build"). It was opt in, which meant a rebuild produced a current zip beside a
    # setup file from hours earlier, and the two artifacts that ship disagreed with each other
    # without anything saying so. Pass -SkipSetup only to check the zip in isolation.
    [switch]$SkipSetup
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Source -eq "") { $Source = Join-Path $root "build-cmake" }

$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(OoTRecompiled VERSION (\d+\.\d+\.\d+)') { Write-Output "SHARE_FAIL no version in CMakeLists.txt"; exit 1 }
$version = $Matches[1]

$release = Join-Path $root "release"
# A BRANCH BUILDS ITS OWN FILES (the user, 2026-09-24: "the branch should build its own installer
# too"). On any branch but main the share folder, the zip and the setup file carry the branch's
# name, so the good build's files on main are never overwritten by work in progress.
$branch = (& git -C $root branch --show-current 2>$null)
$branchTag = ""
if ($branch -and ($branch -ne "main")) { $branchTag = "-" + ($branch -replace '[^A-Za-z0-9._-]', '-') }
$share = Join-Path $release "OoT-Recompiled-$version$branchTag-share"
$zip = Join-Path $release "OoT-Recompiled-$version$branchTag-windows.zip"
$own = Join-Path $release "OoT-Recompiled-$version"

if (Test-Path $share) { Remove-Item $share -Recurse -Force }
New-Item -ItemType Directory -Force $share | Out-Null

# INSIDE vcvars64, and that is not optional for either build below: this build is configured
# with clang-cl from the VS 2022 Build Tools, which finds the MSVC headers and libraries through
# that environment. Run from a bare shell it picks the NEWEST toolset on the machine instead,
# whose standard library refuses this clang outright, and the failure arrives as nothing more
# than a step that could not build. tools\build_native.cmd carries the same wrapper.
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

# BUILD THE PROGRAM BEFORE SIGNING IT, or the signature is put on a file that is about to be
# replaced (2026-09-28). The signing below is in the build tree on purpose, and the setup step
# further down runs ninja, so ANY work left outstanding at that point relinks the executable
# after it was signed and the payload rule repacks the archive from the unsigned result. That is
# the same fault the paragraph below describes from 0.2.0, arriving by a different door.
#
# A VERSION BUMP IS EXACTLY THAT DOOR, which is how this was found: the release script calls
# this one with no native build in front of it, the bump had changed CMakeLists.txt, so the
# setup step reconfigured and relinked, and the check after it refused the release with "the
# program inside the setup payload is not signed". The check was right and the order was wrong.
#
# So everything the share and the payload carry is built here first. It is a no-op on a tree
# that is already current, which is the usual case, and it cannot be forgotten by a caller.
if ($Source -eq (Join-Path $root "build-cmake")) {
    & cmd.exe /c "call `"$vcvars`" >nul && cd /d `"$Source`" && ninja OoTRecompiled OoTUninstall" | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Output "SHARE_FAIL the program could not be built"; exit 1 }
}

# SIGN THE BUILD TREE'S OWN FILES FIRST, IN PLACE, and only then copy them anywhere. This used to
# sign the copies in the share folder and leave the build tree unsigned, and the setup file's
# payload is a build tree output: ninja owns generated\payload.zip as the product of a rule whose
# inputs are the build tree executable, so whenever it judged that rule stale (its log entry
# predating a relink, which is every release build) it REPACKED the payload from the unsigned
# executable, over the signed one this script had just packed, moments before the setup embedded
# it. 0.2.0 and the first 0.2.1 shipped a signed wrapper around an unsigned program that way
# (the user, 2026-09-23, from the launcher's foot: "not signed??"). With the build tree signed,
# there is no unsigned copy left for anything to pack, and the check after the relink below
# proves it on every build rather than trusting the order.
if (-not $NoSign -and $Source -eq (Join-Path $root "build-cmake")) {
    # The executable, the uninstall finisher (it ships in the payload and runs on the person's
    # machine, so it is signed like everything else that does), and EVERY copy of SDL2.dll under
    # the build tree: the one beside the executable, and the one in the fetched package, which is
    # the copy the payload rule packs.
    $inTree = @((Join-Path $Source "OoTRecompiled.exe"), (Join-Path $Source "uninstall.exe"))
    foreach ($dll in (Get-ChildItem -Path $Source -Recurse -Filter "SDL2.dll" -File -ErrorAction SilentlyContinue)) {
        $inTree += $dll.FullName
    }
    & (Join-Path $PSScriptRoot "sign.ps1") -Files $inTree
    if ($LASTEXITCODE -ne 0) { Write-Output "SHARE_FAIL signing the build tree"; exit 1 }
}

foreach ($name in @("OoTRecompiled.exe", "SDL2.dll", "dxcompiler.dll", "dxil.dll")) {
    $from = Join-Path $Source $name
    if (-not (Test-Path $from)) { Write-Output "SHARE_FAIL missing $from"; exit 1 }
    Copy-Item $from (Join-Path $share $name)
}

# THE C RUNTIME SHIPS WITH THE PROGRAM (2026-09-28), because the first person outside this
# machine to open a share got "it flashed as if it was loading and then closed and nothing
# happend": Windows refusing to load the process, with no window and no log, for want of files
# nobody had told him to install. tools\vc_runtime.ps1 holds the whole reasoning and the search,
# in one place because CMake's payload rule packs them too and the two must never disagree.
$crtFiles = @()
foreach ($line in (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "vc_runtime.ps1"))) {
    if ($line -like "VCRUNTIME_MISSING*") { Write-Output "SHARE_FAIL $line"; exit 1 }
    $to = Join-Path $share (Split-Path -Leaf $line)
    Copy-Item $line $to -Force
    $crtFiles += $to
}
if ($crtFiles.Count -eq 0) { Write-Output "SHARE_FAIL the C runtime files were not found"; exit 1 }
Write-Output "runtime: $($crtFiles.Count) C runtime files beside the program"
# THE ASSETS COME FROM THE REPOSITORY, NOT THE BUILD TREE, and that is not a shortcut.
# CMake copies assets\ into the build tree as a POST_BUILD step on the executable, so it only
# runs when the executable RELINKS. Edit nothing but a stylesheet, run ninja, and it reports "no
# work to do": the build tree keeps the old stylesheet, and a share made from it ships a current
# executable against stale assets. That has already cost a day here once, with a symptom that
# pointed nowhere near the cause ("assets/ui/counter.rml is missing"). The repository is the only
# copy that is always right.
# A caller who passes -Source (a release folder, say) is packaging something already assembled,
# so that one keeps its own assets.
$assetsFrom = if ($Source -eq (Join-Path $root "build-cmake")) { Join-Path $root "assets" } else { Join-Path $Source "assets" }
if (-not (Test-Path $assetsFrom)) { Write-Output "SHARE_FAIL missing $assetsFrom"; exit 1 }
Copy-Item $assetsFrom (Join-Path $share "assets") -Recurse

$readme = @"
OoT: Recompiled $version, for testing

What this is
  A native Windows build of the game, produced by static recompilation. It needs your own copy
  of the game and nothing else: no runtime to install, no framework, no launcher. Windows 10 or
  11 on a 64 bit machine with a graphics card that supports DirectX 12, which is any card of the
  last decade.
  Your copy of the game is the NTSC-U 1.0 ROM, as a .z64, .n64 or .v64 file. Nothing of the
  game is in this folder, and the program never uploads, downloads or connects to anything.

How to run it
  1. Unzip this folder anywhere you like.
  2. Either put your ROM file in the same folder as OoTRecompiled.exe (any file name; it is
     recognized by its contents), or start the program and pick the file in the launcher.
  3. Choose your settings in the launcher (the defaults are chosen for your machine), set up a
     controller if you use one, and press Play.

While playing
  F1 opens the menu (settings, controls); a controller's menu button does too once it is set up.
  The window's title bar appears when the pointer is at the top edge.
  Keyboard, unless you change it in Controls: WASD moves, Space is A, Left Shift is B, Q targets,
  E and R are the shoulder buttons, I, J, K and L are the C buttons, Enter is Start and the
  arrow keys are the D-pad.

If something goes wrong
  The file oot-recompiled.log, next to OoTRecompiled.exe, is written fresh every start and holds
  everything the program reported, including a crash report if it crashed. Send that file back,
  and say what you were doing. It contains no data from the game.

Your ROM
  It is never copied. The program reads it where you put it, and remembers the PATH in
  rom.txt so it does not have to ask again. Move or rename the file and it will ask again,
  which is deliberate: your copy is the only copy.

Files
  The program writes only next to itself: the log, settings.txt, controls.txt, rom.txt (the
  path to your ROM, not the ROM), and saves\ for the game's save data.
"@
Set-Content -Path (Join-Path $share "README.txt") -Value $readme -Encoding utf8

# Nothing with a ROM's extension, nothing of the runtime's stored copy, no saves.
$stray = Get-ChildItem $share -Recurse -File | Where-Object { $_.Extension -in @(".z64", ".n64", ".v64", ".sra", ".bin", ".bak") -or $_.Name -like "oot.n64*" }
if ($stray) { Write-Output ("SHARE_FAIL the share holds " + (($stray | ForEach-Object { $_.FullName }) -join ", ")); exit 1 }
if (Test-Path (Join-Path $share "saves")) { Write-Output "SHARE_FAIL the share holds saves"; exit 1 }

if (-not $NoSign) {
    # In process, with the list as one array: a nested powershell -File call splits a list into
    # loose arguments, and the second file once bound itself to the script's -Account.
    # The copies came from a build tree signed above, so this is a verification, unless the
    # source is somewhere else (a caller packaging an assembled folder), where it signs.
    $toSign = @((Join-Path $share "OoTRecompiled.exe"), (Join-Path $share "SDL2.dll"))
    if ($Source -eq (Join-Path $root "build-cmake")) {
        & (Join-Path $PSScriptRoot "sign.ps1") -Files $toSign -VerifyOnly
    }
    else {
        & (Join-Path $PSScriptRoot "sign.ps1") -Files $toSign
    }
    if ($LASTEXITCODE -ne 0) { Write-Output "SHARE_FAIL signing"; exit 1 }
    if (Test-Path $own) {
        Copy-Item (Join-Path $share "OoTRecompiled.exe") (Join-Path $own "OoTRecompiled.exe") -Force
        Copy-Item (Join-Path $share "SDL2.dll") (Join-Path $own "SDL2.dll") -Force

        # AND THE ASSETS, because a folder holding today's executable against last week's
        # documents is not a build, it is two builds. The user's own folder ran exactly that for a
        # day: their log said "assets/ui/counter.rml is missing, so that screen will not open" and
        # "changelog: the file is missing", so the frame rate counter could not open and About had
        # no changelog, while the same executable from the share had both. It was also the first
        # thing suspected when that build showed a black window, which cost real time and was not
        # the cause.
        #
        # Only assets\. The ROM, saves, settings, bindings, mods and logs in that folder are the
        # user's and are never touched; assets\ is ours and is part of the executable.
        $ownAssets = Join-Path $own "assets"
        if (Test-Path $ownAssets) { Remove-Item $ownAssets -Recurse -Force }
        Copy-Item (Join-Path $share "assets") $ownAssets -Recurse
        Write-Output "the signed executable, SDL2.dll and assets copied over $own"
    }
}

# ---------------------------------------------------------------------------------------------
# THE SECOND ARTIFACT: the setup file, one downloadable executable carrying the whole program.
#
# AFTER THE SIGNING, DELIBERATELY. The payload inside it is packed from the files in the share
# folder, which have just been signed, so the game a person ends up running is the signed one.
# Packing from the build tree instead would put an unsigned executable inside a signed wrapper,
# which passes every check a person can see and is exactly the thing signing exists to prevent.
# ---------------------------------------------------------------------------------------------
$setupExe = Join-Path $release "OoT-Recompiled-$version$branchTag-windows.exe"
if (-not $SkipSetup) {
    $payload = Join-Path $Source "generated\payload.zip"
    $finisher = Join-Path $Source "uninstall.exe"
    if (-not (Test-Path $finisher)) { Write-Output "SHARE_FAIL missing $finisher (build OoTUninstall)"; exit 1 }

    & (Join-Path $PSScriptRoot "make_payload.ps1") -Out $payload `
        -Game (Join-Path $share "OoTRecompiled.exe") `
        -Finish $finisher `
        -Sdl2 (Join-Path $share "SDL2.dll") `
        -Dxil (Join-Path $share "dxil.dll") `
        -Dxc (Join-Path $share "dxcompiler.dll") `
        -Runtime $crtFiles
    if ($LASTEXITCODE -ne 0) { Write-Output "SHARE_FAIL the payload could not be packed"; exit 1 }

    # Relink the setup file against the new payload. The resource depends on the archive
    # (CMakeLists.txt sets OBJECT_DEPENDS), so ninja picks this up rather than reporting no work.
    #
    # INSIDE vcvars64, and that is not optional: this build is configured with clang-cl from the
    # VS 2022 Build Tools, which finds the MSVC headers and libraries through that environment.
    # Run from a bare shell it picks the NEWEST toolset on the machine instead, whose standard
    # library refuses this clang outright, and the failure arrives here as nothing more than
    # "SHARE_FAIL the setup file could not be built". toolsuild_native.cmd carries the same
    # wrapper and the same paragraph; this script was quietly missing it, so the setup file could
    # never be built from a normal shell.
    & cmd.exe /c "call `"$vcvars`" >nul && cd /d `"$Source`" && ninja OoTRecompiledSetup" | Out-Null
    $built = $LASTEXITCODE
    if ($built -ne 0) { Write-Output "SHARE_FAIL the setup file could not be built"; exit 1 }

    # THE PROOF: the program INSIDE the payload the setup just embedded is signed. Whatever ninja
    # did to payload.zip above, this is what a person ends up running, and this is the check that
    # would have caught 0.2.0.
    if (-not $NoSign) {
        Add-Type -AssemblyName System.IO.Compression
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [System.IO.Compression.ZipFile]::OpenRead($payload)
        try {
            $probes = @()
            foreach ($inner in @("OoTRecompiled.exe", "SDL2.dll", "uninstall.exe")) {
                $entry = $archive.GetEntry($inner)
                if ($null -eq $entry) { Write-Output "SHARE_FAIL the payload holds no $inner"; exit 1 }
                $target = Join-Path $Source ("generated\payload-probe-" + $inner)
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
                $probes += $target
            }
        }
        finally { $archive.Dispose() }
        & (Join-Path $PSScriptRoot "sign.ps1") -Files $probes -VerifyOnly
        $probeOk = $LASTEXITCODE
        foreach ($t in $probes) { Remove-Item $t -Force }
        if ($probeOk -ne 0) { Write-Output "SHARE_FAIL the program inside the setup payload is not signed"; exit 1 }
        Write-Output "PAYLOAD_SIGNED the program inside the setup file carries a valid signature"
    }

    Copy-Item (Join-Path $Source "OoTRecompiledSetup.exe") $setupExe -Force
    if (-not $NoSign) {
        & (Join-Path $PSScriptRoot "sign.ps1") -Files @($setupExe)
        if ($LASTEXITCODE -ne 0) { Write-Output "SHARE_FAIL signing the setup file"; exit 1 }
    }
    Write-Output ("setup: {0:N0} bytes  {1}" -f (Get-Item $setupExe).Length, $setupExe)
}

if (Test-Path $zip) { Remove-Item $zip -Force }

# THE ZIP WAITS FOR WHATEVER IS STILL READING THE FOLDER. This tree lives inside a synced folder,
# and the sync client opens each file moments after it is written, so zipping a just-signed share
# fails with "the process cannot access the file ... because it is being used by another process"
# on one of the DLLs. It happened twice on 2026-09-27 and both times succeeded on a plain retry,
# which is the shape of a file held for a second rather than a file genuinely in use. So: try,
# wait, try again, and only call it a failure once it has been unavailable for a quarter of a
# minute. Failing a whole signed release over a passing lock is the wrong trade.
$zipped = $false
for ($attempt = 1; $attempt -le 6; $attempt++) {
    try {
        Compress-Archive -Path $share -DestinationPath $zip -CompressionLevel Optimal -ErrorAction Stop
        $zipped = $true
        break
    }
    catch {
        if (Test-Path $zip) { Remove-Item $zip -Force -ErrorAction SilentlyContinue }
        if ($attempt -eq 6) {
            Write-Output "SHARE_FAIL the zip could not be written: $($_.Exception.Message)"
            exit 1
        }
        Write-Output "  a file in the share is still held by something; waiting (attempt $attempt)"
        Start-Sleep -Seconds 5
    }
}
if (-not $zipped) { Write-Output "SHARE_FAIL the zip could not be written"; exit 1 }

Get-ChildItem $share -Recurse -File | ForEach-Object { Write-Output ("  {0,12:N0}  {1}" -f $_.Length, $_.FullName.Substring($share.Length + 1)) }
Write-Output ("zip: {0:N0} bytes" -f (Get-Item $zip).Length)
Write-Output "SHARE_OK $zip"
exit 0
