# bump_version.ps1: move the program's version to the next patch, minor or major everywhere it is
# written, and commit it.
#
#   powershell -ExecutionPolicy Bypass -File tools\bump_version.ps1 -Kind patch
#   powershell -ExecutionPolicy Bypass -File tools\bump_version.ps1 -Kind minor -NoCommit
#
# The version has one source, the project line in CMakeLists.txt: the build writes it into the
# program (src/ui/build_info.h.in) and the setup file, and tools\release.ps1 stages under it.
# README.md's Status line repeats it for a reader. Those two are written here, and nothing else
# needs to be.
#
# THE NOTES COME FIRST. assets\changelog.json is not written here: its newest entry must already
# name the new version, committed, because a release without its notes is not shippable and the
# notes are written for each release. So the changelog decides the version and -Kind confirms it:
# a patch asked for when the notes name a minor version stops the run before anything is built.
#
# SAFE TO RUN AGAIN. A deploy that fails after this step runs it again on the next try. When the
# tree already carries the changelog's newest version and this script's commit for it is in the
# history, nothing is bumped twice; when the tree carries it WITHOUT that commit, the notes for the
# next version were never written, and that stops the run instead of staging an old version again.
#
# On main only, with the two files and the changelog unchanged since the last commit. The commit
# holds the two files alone, whatever else the working tree has. Prints BUMP_OK <old> -> <new>,
# BUMP_OK already <version>, or BUMP_FAIL <why>; exits 1 on a failure.
param(
    [Parameter(Mandatory)][string]$Kind,
    # The repository to work in, this one when left out; the tests point it at a throwaway one.
    [string]$Root = "",
    [switch]$NoCommit
)
$ErrorActionPreference = 'Continue'
# Here and not in the parameter's default: Windows PowerShell leaves $PSScriptRoot empty there.
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }

$versionFiles = @('CMakeLists.txt', 'README.md')
$changelogFile = 'assets/changelog.json'
$cmakePattern = 'project\(OoTRecompiled VERSION (\d+\.\d+\.\d+)'
$readmePattern = 'Version \d+\.\d+\.\d+'

function Fail([string]$why) {
    Write-Output "BUMP_FAIL $why"
    exit 1
}

function Step-Version([string]$version, [string]$kind) {
    $major, $minor, $patch = $version.Split('.') | ForEach-Object { [int]$_ }
    switch ($kind) {
        'major' { return "$($major + 1).0.0" }
        'minor' { return "$major.$($minor + 1).0" }
        'patch' { return "$major.$minor.$($patch + 1)" }
    }
}

function Invoke-Git {
    $output = & git -C $Root @args 2>&1
    return [pscustomobject]@{ Output = ($output -join "`n").Trim(); Code = $LASTEXITCODE }
}

# The file's text and whether it began with a byte order mark, so it is written back the same way.
function Read-Text([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $bom = ($bytes.Length -ge 3) -and ($bytes[0] -eq 0xEF) -and ($bytes[1] -eq 0xBB) -and ($bytes[2] -eq 0xBF)
    $encoding = New-Object Text.UTF8Encoding $false
    $start = if ($bom) { 3 } else { 0 }
    $text = $encoding.GetString($bytes, $start, $bytes.Length - $start)
    return [pscustomobject]@{ Text = $text; Encoding = $encoding; Bom = $bom }
}

function Write-Text([string]$path, $file, [string]$text) {
    $body = $file.Encoding.GetBytes($text)
    $bytes = if ($file.Bom) { [byte[]](0xEF, 0xBB, 0xBF) + $body } else { $body }
    [IO.File]::WriteAllBytes($path, [byte[]]$bytes)
}

$kindName = $Kind.Trim().ToLowerInvariant()
if ($kindName -notin @('patch', 'minor', 'major')) { Fail "the kind is '$Kind'; it must be patch, minor or major" }

foreach ($name in $versionFiles + $changelogFile) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $name))) { Fail "no $name in $Root" }
}

$cmakePath = Join-Path $Root 'CMakeLists.txt'
$readmePath = Join-Path $Root 'README.md'
$cmake = Read-Text $cmakePath
if ($cmake.Text -notmatch $cmakePattern) { Fail 'no version on the project line of CMakeLists.txt' }
$current = $Matches[1]
$readme = Read-Text $readmePath
if ($readme.Text -notmatch $readmePattern) { Fail "README.md has no 'Version x.y.z' line to update" }

$changelog = Read-Text (Join-Path $Root $changelogFile)
$noted = [string](ConvertFrom-Json $changelog.Text)[0].version
if ($noted -notmatch '^\d+\.\d+\.\d+$') { Fail "the changelog's newest entry has no version" }

if (-not $NoCommit) {
    if ((Invoke-Git rev-parse --is-inside-work-tree).Output -ne 'true') { Fail "$Root is not a Git repository" }
    $branch = (Invoke-Git branch --show-current).Output
    if ($branch -ne 'main') { Fail "the tree is on '$branch', not main; a release is cut from main" }
    $changed = (Invoke-Git status --porcelain -- @($versionFiles + $changelogFile)).Output
    if ($changed) { Fail "commit these first, the changelog entry in its own commit: $($changed -replace '\s+', ' ')" }
}

if ($current -eq $noted) {
    $bumped = (Invoke-Git log --format=%s --fixed-strings "--grep=chore(release): $current").Output
    if ($bumped -and -not $NoCommit) {
        Write-Output "BUMP_OK already $current"
        exit 0
    }
    Fail "the changelog's newest entry is $noted, the version the tree already carries; write the next version's notes first"
}

$next = Step-Version $current $kindName
if ($next -ne $noted) { Fail "a $kindName bump of $current is $next, but the changelog's newest entry is $noted" }

Write-Text $cmakePath $cmake ([regex]::new($cmakePattern).Replace($cmake.Text, "project(OoTRecompiled VERSION $next", 1))
Write-Text $readmePath $readme ([regex]::new($readmePattern).Replace($readme.Text, "Version $next", 1))

if (-not $NoCommit) {
    $add = Invoke-Git add -- @versionFiles
    if ($add.Code -ne 0) { Fail "git add: $($add.Output)" }
    $commit = Invoke-Git commit -m "chore(release): $next" -- @versionFiles
    if ($commit.Code -ne 0) { Fail "git commit: $($commit.Output)" }
}

Write-Output "BUMP_OK $current -> $next"
