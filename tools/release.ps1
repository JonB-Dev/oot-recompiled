# release.ps1: build the share, stage it to R2, and promote it to everyone when you say so.
#
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1              # build + stage
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Promote     # make it live
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Status      # what is where
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -NoBuild     # stage what exists
#
# THE TWO CHANNELS, which is the whole point of the scheme and is the same one the other apps
# here use (the global rules' "Staged Release Channel"):
#
#   <slug>/next/latest.yml   STAGED. Testers can point at this. Nothing else reads it.
#   <slug>/latest.yml        LIVE. This is what an installed copy checks. Whoever moves this
#                            file decides WHEN people are told, which is deliberately separate
#                            from when the build was made.
#
# So staging is safe: the zip goes up and is downloadable by direct link, but no installed copy
# learns about it until -Promote copies the pointer across. Build as often as you like; release
# when you mean to.
#
# THE ARTIFACT IS A ZIP, NOT AN INSTALLER, and the hub copes: its download button follows the
# `path:` field in latest.yml verbatim. The `.exe` in its platform table is only used when it has
# to guess from a bucket listing, which it does not have to do here because the manifest exists.

param(
    [switch]$Promote,
    [switch]$Status,
    [switch]$NoBuild,
    [switch]$NoSign,
    # The R2 prefix and the hub's slug for this program. Kept in one place because it appears in
    # every key below and in the hub's apps.json.
    [string]$Slug = "oot-recompiled"
)

# NOT "Stop" (changed 2026-09-23, when this script died on its first line of child output).
# Redirecting a native command's stderr in Windows PowerShell wraps every line in an ErrorRecord,
# so one harmless warning from the toolchain (vswhere not being on PATH, which the signing step
# handles itself) aborted the whole release before the share was built. Every step below already
# checks what the tool actually produced, which is the honest test anyway. Same reasoning, same
# line, as tools/build.ps1.
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$r2 = Join-Path $PSScriptRoot "release\r2.mjs"

function Invoke-R2 {
    param([string[]]$Arguments)
    $output = & node $r2 @Arguments 2>&1
    $code = $LASTEXITCODE
    return [pscustomobject]@{ Output = ($output -join "`n"); Code = $code }
}

# The version is the build's, from CMakeLists, so a release can never claim a version the binary
# does not carry.
$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(OoTRecompiled VERSION (\d+\.\d+\.\d+)') {
    Write-Output "RELEASE_FAIL no version in CMakeLists.txt"
    exit 1
}
$version = $Matches[1]

if ($Status) {
    Write-Output "=== $Slug, version in the tree: $version ==="
    foreach ($pair in @(@{ name = "LIVE  (what installed copies see)"; key = "$Slug/latest.yml" },
                        @{ name = "NEXT  (staged, testers only)";      key = "$Slug/next/latest.yml" })) {
        $result = Invoke-R2 @("get", $pair.key)
        Write-Output ""
        Write-Output $pair.name
        if ($result.Code -ne 0) {
            if ($result.Output -match "NOTFOUND") { Write-Output "  nothing published there yet" }
            else { Write-Output "  could not be read: $($result.Output)" }
        }
        else {
            ($result.Output -split "`n") | Where-Object { $_ -match '^(version|path|releaseDate):' } |
                ForEach-Object { Write-Output "  $_" }
        }
    }
    Write-Output ""
    Write-Output "=== everything under $Slug/ ==="
    Write-Output (Invoke-R2 @("list", "$Slug/")).Output
    exit 0
}

if ($Promote) {
    # ${Slug} rather than $Slug because a colon straight after a variable name reads as a drive
    # reference in PowerShell, which is a parse error rather than a runtime one.
    Write-Output "=== promoting ${Slug}: the staged pointer becomes the live one ==="
    $staged = Invoke-R2 @("get", "$Slug/next/latest.yml")
    if ($staged.Code -ne 0) {
        Write-Output "RELEASE_FAIL nothing is staged for $Slug (run without -Promote first)"
        exit 1
    }
    $stagedVersion = (($staged.Output -split "`n") | Where-Object { $_ -match '^version:' }) -replace '^version:\s*', ''
    Write-Output "  staged version: $stagedVersion"
    $copy = Invoke-R2 @("copy", "$Slug/next/latest.yml", "$Slug/latest.yml")
    if ($copy.Code -ne 0) {
        Write-Output "RELEASE_FAIL promote: $($copy.Output)"
        exit 1
    }
    Write-Output $copy.Output

    # THE CHANGELOG GOES LIVE WITH THE POINTER, and not before it (2026-09-26). The hub's releases
    # page is built from this file, so it is as much a decision about what people are told as
    # latest.yml is, and staging leaves it in next/ for exactly that reason.
    $copyLog = Invoke-R2 @("copy", "$Slug/next/changelog.json", "$Slug/changelog.json")
    if ($copyLog.Code -ne 0) {
        Write-Output "RELEASE_FAIL promote: the pointer moved but the changelog did not: $($copyLog.Output)"
        exit 1
    }
    Write-Output $copyLog.Output
    Write-Output "PROMOTE_OK $stagedVersion is now what installed copies will be offered"
    exit 0
}

# --- build and stage ---------------------------------------------------------------------

if (-not $NoBuild) {
    Write-Output "=== building the share ==="
    # make_share.ps1 builds the setup file every time now (its -Setup switch became -SkipSetup on
    # 2026-09-23, so that a share is never built without it by forgetting a flag); nothing to add.
    $shareArgs = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "make_share.ps1"))
    if ($NoSign) { $shareArgs += "-NoSign" }
    $shareOutput = & powershell -NoProfile @shareArgs 2>&1
    $shareText = ($shareOutput -join "`n")
    if ($shareText -notmatch "SHARE_OK") {
        Write-Output $shareText
        Write-Output "RELEASE_FAIL the share did not build"
        exit 1
    }
    Write-Output "  share built"
}

$zip = Join-Path $root "release\OoT-Recompiled-$version-windows.zip"
if (-not (Test-Path $zip)) {
    Write-Output "RELEASE_FAIL no zip at $zip"
    exit 1
}

$setup = Join-Path $root "release\OoT-Recompiled-$version-windows.exe"
if (-not (Test-Path $setup)) {
    Write-Output "RELEASE_FAIL no setup file at $setup"
    exit 1
}

$zipName = [System.IO.Path]::GetFileName($zip)
$zipInfo = Get-Item $zip
$setupName = [System.IO.Path]::GetFileName($setup)
$setupInfo = Get-Item $setup

# electron-updater's manifest shape, because that is what the hub already knows how to read and
# what every other app here publishes. The hash is base64 of the sha512 digest, which is the
# convention that format uses; it is not hex.
function Get-Sha512Base64 {
    param([string]$Path)
    $sha = [System.Security.Cryptography.SHA512]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try { $digest = $sha.ComputeHash($stream) } finally { $stream.Dispose(); $sha.Dispose() }
    return [System.Convert]::ToBase64String($digest)
}
$sha512 = Get-Sha512Base64 $setup
$zipSha512 = Get-Sha512Base64 $zip

$releaseDate = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ss.fffZ")

# THE POINTER NAMES THE SETUP FILE, AND ONLY IT. Two artifacts ship, but `path:` is what an
# installed copy fetches and installs, and that has to be the single executable: this program's
# updater downloads one file and runs it, and a portable folder is not something it should be
# rewriting under somebody. The portable zip is listed beside it so the hub and anyone reading the
# manifest can see both, and it is downloaded by hand rather than by the updater.
$latest = @"
version: $version
files:
  - url: $setupName
    sha512: $sha512
    size: $($setupInfo.Length)
  - url: $zipName
    sha512: $zipSha512
    size: $($zipInfo.Length)
path: $setupName
sha512: $sha512
releaseDate: '$releaseDate'
"@

$staging = Join-Path $root "release\staging"
New-Item -ItemType Directory -Force $staging | Out-Null
$latestFile = Join-Path $staging "latest.yml"
Set-Content -Path $latestFile -Value $latest -Encoding ascii -NoNewline

# What the hub prints under the download button. Truthful rather than aspirational: this build is
# Windows only and needs a Direct3D 12 device, which is not every machine that runs Windows 10.
$compatibility = '{' + "`n" + '  "windows": "Windows 10, 11 (64-bit, Direct3D 12)"' + "`n" + '}'
$compatibilityFile = Join-Path $staging "compatibility.json"
Set-Content -Path $compatibilityFile -Value $compatibility -Encoding ascii

Write-Output ""
Write-Output "=== staging $Slug $version ==="

# The zip goes to the LIVE prefix and is inert there: downloadable by direct link, referenced by
# no pointer any installed copy reads. Only the pointer decides who is told.
$steps = @(
    @{ args = @("put", "$Slug/$setupName", $setup); what = "the setup file" },
    @{ args = @("put", "$Slug/$zipName", $zip); what = "the portable build" },
    @{ args = @("put", "$Slug/next/latest.yml", $latestFile); what = "the staged pointer" },
    @{ args = @("put", "$Slug/compatibility.json", $compatibilityFile); what = "the compatibility note" },
    # THE CHANGELOG RIDES ALONG (the user, 2026-09-23: the site had no changelog per version, and
    # "it must be updated each release"). The site reads it same origin from the prefix and shows
    # every version's Added, Changed and Fixed under its row, so the notes are current the moment
    # a build is staged and nobody edits the site for a release. The file is the program's own
    # assets\changelog.json, the one About shows, so the two can never disagree.
    #
    # IT GOES TO next/, NOT LIVE (2026-09-26). The hub's releases page reads this file at runtime
    # to list every build that has shipped, so writing it live put a STAGED version on the page as
    # a downloadable row while latest.yml correctly still named the old one: the release was not
    # promoted and the hub announced it anyway (the user: "Looks like even non promoted releases
    # show here?"). Only latest.yml was ever meant to decide what a person is told about, and this
    # file decides as much as that one does. Promote copies both across together.
    @{ args = @("put", "$Slug/next/changelog.json", (Join-Path $root "assets\changelog.json")); what = "the changelog, staged" }
)
foreach ($step in $steps) {
    $result = Invoke-R2 $step.args
    if ($result.Code -ne 0) {
        Write-Output "RELEASE_FAIL uploading $($step.what): $($result.Output)"
        exit 1
    }
    Write-Output $result.Output
}

Write-Output ""
Write-Output "STAGE_OK $version"
Write-Output "  staged:   https://releases.jonbarnes.dev/$Slug/next/latest.yml"
Write-Output "  the file: https://releases.jonbarnes.dev/$Slug/$zipName"
Write-Output ""
Write-Output "  Nothing installed is offered this yet. To release it to everyone:"
Write-Output "    powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Promote"
exit 0
