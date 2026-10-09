# publish_public.ps1: build the PUBLIC copy of this repository and push it.
#
#   powershell -ExecutionPolicy Bypass -File tools\publish_public.ps1            # build and show
#   powershell -ExecutionPolicy Bypass -File tools\publish_public.ps1 -Push      # and push it
#
# WHY A SCRIPT AND NOT A ONE-OFF. The public repository is a DERIVED thing, the way a release is:
# this repository stays the source of truth and keeps its full history, and the public one carries
# the code with a fresh history and none of the working records. Doing that by hand once means the
# next update is done by hand differently, and the difference is what leaks something.
#
# WHAT IS LEFT OUT, and why each one (the user chose this shape on 2026-09-26, asked directly
# because a public history cannot be unpublished):
#
#   .tasks.md      8000 lines of working notes quoting the user verbatim dozens of times
#   .worklog.md    the same, for finished work
#   .scaffold/     4 MB of internal planning, including the security files' own reasoning
#   CLAUDE.md      the project's working rules; quotes the user, and points at .scaffold paths
#                  that do not exist in the public copy, so it would be revealing AND broken
#   PLAN.md        a superseded narrative plan the file itself calls not authoritative
#
# THE HISTORY IS FRESH, ONE COMMIT, and that is the point rather than a shortcut: the records are
# in every commit of this repository's history, so publishing that history would publish them
# however the latest commit looked.
#
# WHAT IS NOT EXCLUDED AND MUST NEVER NEED TO BE: anything derived from the ROM. That is handled
# a layer down, by the gitignore and the pre-commit hook, and was verified across every branch and
# every object before the first push (.scaffold/security/pre-app.md section 7b). This script trusts
# that and checks it again below anyway, because a check that only runs once is not a check.
param(
    [switch]$Push,
    [string]$Repo = "JonB-Dev/oot-recompiled",
    [string]$Account = "JonB-Dev"
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $env:TEMP "oot-recompiled-public"

$EXCLUDE = @(".tasks.md", ".worklog.md", "CLAUDE.md", "PLAN.md", ".scaffold")

Write-Output "=== building the public copy ==="

# git archive gives exactly the TRACKED files at HEAD: nothing ignored, nothing untracked, no
# build output, no ROM, no recompiler output. Starting from the index rather than the working
# directory is what makes that guarantee.
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$tar = Join-Path $env:TEMP "oot-public.tar"
& git -C $root archive --format=tar -o $tar HEAD
if ($LASTEXITCODE -ne 0) { Write-Output "PUBLISH_FAIL could not archive HEAD"; exit 1 }
& tar -x -f $tar -C $out
if ($LASTEXITCODE -ne 0) { Write-Output "PUBLISH_FAIL could not extract the archive"; exit 1 }
Remove-Item $tar -Force

foreach ($name in $EXCLUDE) {
    $path = Join-Path $out $name
    if (Test-Path $path) {
        Remove-Item $path -Recurse -Force
        Write-Output "  left out: $name"
    }
}

# A last look for anything derived from the ROM, by extension and by directory name. The gitignore
# and the hook already prevent these; this is the check that runs every time rather than once.
$forbidden = Get-ChildItem $out -Recurse -File -ErrorAction SilentlyContinue | Where-Object {
    $_.Extension -in @(".z64", ".n64", ".v64", ".rom") -or
    $_.FullName -match "\\RecompiledFuncs\\" -or
    $_.FullName -match "\\RecompiledPatches\\"
}
if ($forbidden) {
    Write-Output "PUBLISH_FAIL something derived from the ROM is in the public copy:"
    $forbidden | ForEach-Object { Write-Output "  $($_.FullName)" }
    exit 1
}
Write-Output "  checked: nothing derived from the ROM"

# ---- the fresh history --------------------------------------------------------------------
Push-Location $out
& git init -b main --quiet
& git add -A

# THE SUBMODULES HAVE TO BE RE-POINTED BY HAND. `git archive` writes .gitmodules but leaves the
# submodule directories empty, and `git add` skips an empty directory, so a naive fresh repo ends
# up with a .gitmodules naming five submodules and no gitlinks for any of them: `git submodule
# update` then does nothing and the build fails on missing headers. Writing the gitlink entries
# directly reproduces exactly what this repository has, pinned to the same commits.
$subs = & git -C $root submodule status
foreach ($line in $subs) {
    if ($line -match '^[\s+-]*([0-9a-f]{40})\s+(\S+)') {
        $sha = $Matches[1]
        $path = $Matches[2]
        & git update-index --add --cacheinfo "160000,$sha,$path"
        Write-Output "  submodule pinned: $path @ $($sha.Substring(0,10))"
    }
}

$version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'project\(OoTRecompiled VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$message = @"
OoT: Recompiled $version

A static recompilation of the game into a native Windows executable, with ray
traced lighting, frame interpolation, widescreen and a rewritten interface.

This is the public copy of the project. It carries the source, the recompiler
configuration, the patches and the tooling. It carries nothing derived from a
ROM, and it never will: the person building or running it supplies their own
copy of the game, and everything the recompiler produces from it is excluded
from version control by the gitignore and refused by a pre-commit hook.

The history starts here on purpose. The working repository keeps its own, which
is full of notes that were written to be read by one person.
"@
& git commit --quiet -m $message
$count = (& git rev-list --count HEAD)
$files = (& git ls-files | Measure-Object).Count
Write-Output "  one commit, $files files"
Pop-Location

if (-not $Push) {
    Write-Output ""
    Write-Output "PUBLISH_READY $out"
    Write-Output "  Look it over, then run this again with -Push."
    exit 0
}

# ---- pushing ------------------------------------------------------------------------------
# The personal account, explicitly. Several accounts are logged in and the active one is whichever
# was used last; pushing a personal project from the work account is the kind of mistake that is
# only noticed by somebody else.
Write-Output "=== pushing ==="
& gh auth switch --user $Account 2>&1 | Out-Null
$who = (& gh api user --jq .login) 2>$null
if ($who -ne $Account) {
    Write-Output "PUBLISH_FAIL the active GitHub account is '$who', expected '$Account'"
    exit 1
}
Write-Output "  account: $who"

Push-Location $out
$exists = (& gh repo view $Repo --json name 2>$null)
if (-not $exists) {
    & gh repo create $Repo --public --source=. --remote=origin --description "A static recompilation of Ocarina of Time into a native Windows executable, with ray traced lighting." 2>&1 | Out-Null
    Write-Output "  created $Repo"
} else {
    & git remote remove origin 2>$null
    & git remote add origin "https://github.com/$Repo.git"
    Write-Output "  using the existing $Repo"
}
& git push --force origin main
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Output "PUBLISH_FAIL the push failed"; exit 1 }
Pop-Location

Write-Output "PUBLISH_OK https://github.com/$Repo"
