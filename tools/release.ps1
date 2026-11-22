# release.ps1: build the share, stage it to R2, and promote it to everyone when you say so.
#
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1              # build + stage
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Promote     # make it live, now
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Status      # what is where
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -NoBuild     # stage what exists
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -SiteData    # stage only the site's data
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -PromoteHere # promote from this machine
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Approve     # let Friday promote what is staged
#
# NOTHING GOES LIVE ON THE SCHEDULE WITHOUT YOUR WORD (2026-10-12: a staged 0.5.2 carried a regression,
# and the user: "this is why i didn't want to promote yet!"). Friday's run promotes only the staged
# version named in next/approved, which -Approve writes and every new staging resets to none.
#
# PROMOTING HAPPENS IN CI (2026-10-12, the user: "build that promotion and get the site hooked up to
# use it"). The site repository's .github/workflows/promote.yml promotes every Friday at 9 a.m. in New
# York, and redeploys the site from the live release straight after. -Promote runs that same workflow
# now (one way to promote, so the site always follows); -PromoteHere is the copy from this machine for
# when GitHub cannot be reached, and leaves the site's redeploy to be run by hand.
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
    [switch]$PromoteHere,
    [switch]$Approve,
    [switch]$Status,
    [switch]$NoBuild,
    [switch]$NoSign,
    [switch]$SiteData,
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

# The approval the Friday promote reads: the staged version, or "none".
function Set-Approval {
    param([string]$Value)
    $dir = Join-Path $root "release\staging"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $file = Join-Path $dir "approved"
    Set-Content -Path $file -Value $Value -Encoding ascii -NoNewline
    $result = Invoke-R2 @("put", "$Slug/next/approved", $file)
    if ($result.Code -ne 0) {
        Write-Output "RELEASE_FAIL writing the approval: $($result.Output)"
        exit 1
    }
}

if ($Approve) {
    $staged = Invoke-R2 @("get", "$Slug/next/latest.yml")
    if ($staged.Code -ne 0) {
        Write-Output "RELEASE_FAIL nothing is staged for $Slug"
        exit 1
    }
    $stagedVersion = ((($staged.Output -split "`n") | Where-Object { $_ -match '^version:' }) -replace '^version:\s*', '').Trim()
    Set-Approval $stagedVersion
    Write-Output "APPROVED ${stagedVersion}; it goes live at the next Friday promote, 9 a.m. New York (or now with -Promote)"
    exit 0
}

if ($Promote) {
    # THE WORKFLOW, NOT A COPY FROM HERE, so a promote on demand does exactly what Friday's does,
    # site and all. Run as the personal GitHub account whatever gh has active (the work account
    # cannot see the site's repository), by handing its token to these calls alone.
    $workflowRepo = "JonB-Dev/oot-recompiled-site"
    $token = (& gh auth token --user JonB-Dev 2>$null)
    if (-not $token) {
        Write-Output "RELEASE_FAIL gh has no JonB-Dev login; log it in, or promote from here with -PromoteHere"
        exit 1
    }
    $previousToken = $env:GH_TOKEN
    $env:GH_TOKEN = $token
    try {
        Write-Output "=== promoting ${Slug} through $workflowRepo's promote.yml ==="
        $before = (& gh run list --workflow promote.yml --repo $workflowRepo --limit 1 --json databaseId --jq '.[0].databaseId' 2>$null)
        & gh workflow run promote.yml --repo $workflowRepo -f promote=true -f deploy=false -f dry_run=false
        if ($LASTEXITCODE -ne 0) {
            Write-Output "RELEASE_FAIL the workflow would not start; promote from here with -PromoteHere"
            exit 1
        }
        $runId = $null
        for ($i = 0; ($i -lt 30) -and (-not $runId); $i++) {
            Start-Sleep -Seconds 2
            $latestRun = (& gh run list --workflow promote.yml --repo $workflowRepo --limit 1 --json databaseId --jq '.[0].databaseId' 2>$null)
            if ($latestRun -and ($latestRun -ne $before)) { $runId = $latestRun }
        }
        if (-not $runId) {
            Write-Output "RELEASE_FAIL the workflow started but its run did not appear; see the repository's Actions tab"
            exit 1
        }
        Write-Output "  run $runId, watching it"
        & gh run watch $runId --repo $workflowRepo --exit-status
        if ($LASTEXITCODE -ne 0) {
            Write-Output "RELEASE_FAIL the promote run failed: gh run view $runId --repo $workflowRepo --log-failed"
            exit 1
        }
        Write-Output "PROMOTE_OK run $runId finished; tools\release.ps1 -Status says what is live"
        exit 0
    }
    finally {
        $env:GH_TOKEN = $previousToken
    }
}

if ($PromoteHere) {
    # ${Slug} rather than $Slug because a colon straight after a variable name reads as a drive
    # reference in PowerShell, which is a parse error rather than a runtime one.
    Write-Output "=== promoting ${Slug} from this machine: the staged files become the live ones ==="
    $staged = Invoke-R2 @("get", "$Slug/next/latest.yml")
    if ($staged.Code -ne 0) {
        Write-Output "RELEASE_FAIL nothing is staged for $Slug (run without -Promote first)"
        exit 1
    }
    $stagedVersion = (($staged.Output -split "`n") | Where-Object { $_ -match '^version:' }) -replace '^version:\s*', ''
    Write-Output "  staged version: $stagedVersion"

    # THE SITE'S DATA AND THE CHANGELOG FIRST, THE POINTER LAST, in the same order as the workflow:
    # until the pointer moves no installed copy is told, and the site leaves out anything newer than
    # it, so a failure part way announces nothing that is not offered. The changelog goes live with
    # the pointer and not before it is staged (2026-09-26): the site's notes are read from it.
    foreach ($file in @("site/roadmap.json", "site/settings.json")) {
        if ((Invoke-R2 @("get", "$Slug/next/$file")).Code -ne 0) {
            Write-Output "  $file was not staged with this release; the site keeps its own"
            continue
        }
        $copySite = Invoke-R2 @("copy", "$Slug/next/$file", "$Slug/$file")
        if ($copySite.Code -ne 0) {
            Write-Output "RELEASE_FAIL promote: $file did not move: $($copySite.Output)"
            exit 1
        }
        Write-Output $copySite.Output
    }
    foreach ($file in @("changelog.json", "latest.yml")) {
        $copy = Invoke-R2 @("copy", "$Slug/next/$file", "$Slug/$file")
        if ($copy.Code -ne 0) {
            Write-Output "RELEASE_FAIL promote: $file did not move: $($copy.Output)"
            exit 1
        }
        Write-Output $copy.Output
    }
    Write-Output "PROMOTE_OK $stagedVersion is now what installed copies will be offered"
    Write-Output "  The site was NOT redeployed: run the site repository's promote.yml with deploy on."
    exit 0
}

# THE SITE'S DATA FOR THIS VERSION, STAGED BESIDE THE POINTER (2026-10-12). The site is rebuilt in
# CI after a promote, with no copy of this repository to generate from, so the roadmap and the
# settings reference are generated here, for the version being staged, and travel with it:
# next/site/ now, live site/ at the promote (scripts/fetch-release-data.mjs in the site reads them).
function Publish-SiteData {
    param([string]$StagingDir)
    $siteDir = Join-Path $StagingDir "site"
    New-Item -ItemType Directory -Force $siteDir | Out-Null
    $roadmap = Join-Path $siteDir "roadmap.json"
    $settings = Join-Path $siteDir "settings.json"
    $made = & python (Join-Path $PSScriptRoot "roadmap_doc.py") --released $version --out $roadmap 2>&1
    if (($LASTEXITCODE -ne 0) -or -not (Test-Path $roadmap)) {
        Write-Output ($made -join "`n")
        Write-Output "RELEASE_FAIL the site's roadmap did not generate"
        exit 1
    }
    $made = & python (Join-Path $PSScriptRoot "settings_doc.py") --out $settings 2>&1
    if (($LASTEXITCODE -ne 0) -or -not (Test-Path $settings)) {
        Write-Output ($made -join "`n")
        Write-Output "RELEASE_FAIL the site's settings reference did not generate"
        exit 1
    }
    foreach ($pair in @(@{ key = "$Slug/next/site/roadmap.json"; file = $roadmap },
                        @{ key = "$Slug/next/site/settings.json"; file = $settings })) {
        $result = Invoke-R2 @("put", $pair.key, $pair.file)
        if ($result.Code -ne 0) {
            Write-Output "RELEASE_FAIL uploading the site's data ($($pair.key)): $($result.Output)"
            exit 1
        }
        Write-Output $result.Output
    }
}

if ($SiteData) {
    Write-Output "=== staging the site's data for $Slug $version ==="
    $staged = Invoke-R2 @("get", "$Slug/next/latest.yml")
    $stagedVersion = (($staged.Output -split "`n") | Where-Object { $_ -match '^version:' }) -replace '^version:\s*', ''
    if (($staged.Code -ne 0) -or ($stagedVersion -ne $version)) {
        Write-Output "RELEASE_FAIL the staged version is '$stagedVersion', not the tree's ${version}; stage the release first"
        exit 1
    }
    Publish-SiteData (Join-Path $root "release\staging")
    Write-Output "SITE_DATA_OK staged for $version; it goes live with the promote"
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
Publish-SiteData $staging
# A NEW BUILD IS NEVER APPROVED BY AN OLD WORD: Friday promotes it only after -Approve.
Set-Approval "none"
Write-Output "  approval reset: Friday will not promote $version until tools\release.ps1 -Approve"

Write-Output ""
Write-Output "STAGE_OK $version"
Write-Output "  staged:   https://releases.jonbarnes.dev/$Slug/next/latest.yml"
Write-Output "  the file: https://releases.jonbarnes.dev/$Slug/$zipName"
Write-Output ""
Write-Output "  Nothing installed is offered this yet. It goes live on Friday at 9 a.m. (New York), or now:"
Write-Output "    powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Promote"
exit 0
