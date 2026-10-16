# make_payload.ps1: pack the DLLs and assets\ into the zip that the shipped single file carries
# inside itself and unpacks on first run (src/main/places.cpp).
#
#   tools\make_payload.ps1 -Out build-cmake\generated\payload.zip
#   tools\make_payload.ps1 -Out ... -Empty        # a valid, empty archive
#
# CMake calls this, not a person. -Empty is what an ordinary build gets, because embedding twenty
# four megabytes would put a re-zip, a resource compile and a relink between editing a stylesheet
# and seeing it. See src/main/app.rc.in.
#
# WHAT GOES IN, and nothing else:
#   OoTRecompiled.exe                    the game itself; the setup file carries it
#   uninstall.exe                        finishes a removal from outside the folder
#   SDL2.dll, dxcompiler.dll, dxil.dll   the three it cannot start without
#   vcruntime140*.dll, msvcp140*.dll     the C runtime, which ships rather than being asked for
#   assets\**                            our own documents, stylesheets, fonts and icon
#
# WHAT NEVER GOES IN: the ROM, a save, a setting, a binding, a log, or anything the user made.
# Those are THEIRS and live beside bin\ rather than in it, which is the entire point of the split.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Game = "",
    [string]$Finish = "",
    [string]$Sdl2 = "",
    [string]$Dxil = "",
    [string]$Dxc = "",
    # The Visual C++ runtime files, which ship WITH the program rather than being asked for.
    # See the reasoning in make_share.ps1: without them Windows refuses to load the process and
    # nothing of ours runs, so there is no window and no log. An installed copy needs them as
    # much as the portable one does.
    [string[]]$Runtime = @(),
    [switch]$Empty
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
if (Test-Path $Out) { Remove-Item $Out -Force }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# System.IO.Compression rather than Compress-Archive, for one reason that matters: Compress-Archive
# writes entry names with backslashes, and miniz (which reads this on the other side) treats a
# backslash as part of the file name rather than as a separator, so every asset would unpack into
# bin\ as one file called "ui\case.rcss". Entry names here are written with forward slashes, which
# is what the zip format actually specifies.
$zip = [System.IO.Compression.ZipFile]::Open($Out, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    if (-not $Empty) {
        # THE C RUNTIME, found here when the caller did not name it, because CMake's own payload
        # rule (CMakeLists.txt, OOT_PAYLOAD) calls this script and knows nothing about it. That
        # rule repacking the archive is what made the first attempt at this fix ship a fixed zip
        # beside an unfixed setup file. See tools\vc_runtime.ps1 for why they ship at all.
        if ($Runtime.Count -eq 0) {
            foreach ($line in (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "vc_runtime.ps1"))) {
                if ($line -like "VCRUNTIME_MISSING*") { Write-Output "PAYLOAD_FAIL $line"; exit 1 }
                $Runtime += $line
            }
        }

        foreach ($dll in (@($Game, $Finish, $Sdl2, $Dxil, $Dxc) + $Runtime)) {
            if ($dll -eq "") { continue }
            if (-not (Test-Path $dll)) { Write-Output "PAYLOAD_FAIL missing $dll"; exit 1 }
            $name = Split-Path -Leaf $dll
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $dll, $name,
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }

        $assets = Join-Path $root "assets"
        if (-not (Test-Path $assets)) { Write-Output "PAYLOAD_FAIL missing $assets"; exit 1 }
        $prefix = (Resolve-Path $assets).Path
        foreach ($file in Get-ChildItem -Path $assets -Recurse -File) {
            $relative = $file.FullName.Substring($prefix.Length).TrimStart('\', '/').Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName,
                "assets/$relative", [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
}
finally {
    $zip.Dispose()
}

$size = (Get-Item $Out).Length
Write-Output ("PAYLOAD_OK {0} bytes -> {1}" -f $size, $Out)
