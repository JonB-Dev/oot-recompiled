# Tests for bump_version.ps1, each in a throwaway Git repository holding the three files it reads.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\bump_version.test.ps1
#
# Prints one line per case and BUMP_TESTS_OK, or BUMP_TESTS_FAIL with the failed cases; exits 1 on
# a failure.
$ErrorActionPreference = 'Continue'
$script = Join-Path $PSScriptRoot 'bump_version.ps1'

$script:failed = @()
function Case([string]$name, [scriptblock]$body) {
    $root = Join-Path ([IO.Path]::GetTempPath()) ("bump test " + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root | Out-Null
    try { & $body $root; Write-Output "ok    $name" }
    catch { $script:failed += $name; Write-Output "FAIL  $name : $($_.Exception.Message)" }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}
function Expect([bool]$condition, [string]$what) { if (-not $condition) { throw $what } }

function Write-Plain([string]$path, [string]$text) {
    New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false))
}

function Invoke-RepoGit([string]$root) { & git.exe -C $root @args 2>&1 | Out-Null }

# A repository on main at $version, with $noted as the changelog's newest entry, all committed.
function New-Repo([string]$root, [string]$version, [string]$noted, [string]$branch = 'main') {
    Write-Plain (Join-Path $root 'CMakeLists.txt') "cmake_minimum_required(VERSION 3.20)`r`nproject(OoTRecompiled VERSION $version LANGUAGES C CXX)`r`n"
    Write-Plain (Join-Path $root 'README.md') "# Title`n`n## Status`n`nVersion $version, released. More text.`n"
    Write-Plain (Join-Path $root 'assets\changelog.json') "[`n  { `"version`": `"$noted`", `"date`": `"2026-11-20`" },`n  { `"version`": `"$version`", `"date`": `"2026-10-22`" }`n]`n"
    Write-Plain (Join-Path $root 'notes.txt') "other work`n"
    Invoke-RepoGit $root init -q -b $branch
    Invoke-RepoGit $root config user.name 'bump test'
    Invoke-RepoGit $root config user.email 'bump-test@example.invalid'
    Invoke-RepoGit $root config commit.gpgsign false
    Invoke-RepoGit $root add -A
    Invoke-RepoGit $root commit -q -m 'start'
}

function Invoke-Bump([string]$root, [string[]]$more) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Root $root @more
    return [pscustomobject]@{ Text = ($out -join "`n"); Code = $LASTEXITCODE }
}

function Read-Plain([string]$path) { [IO.File]::ReadAllText($path) }
function Subjects([string]$root) { @(& git -C $root log --format=%s) }

Case 'a patch bump moves both files and commits only them' {
    param($root)
    New-Repo $root '0.6.1' '0.6.2'
    Write-Plain (Join-Path $root 'notes.txt') "changed but not part of the release`n"
    $run = Invoke-Bump $root @('-Kind', 'patch')
    Expect ($run.Code -eq 0 -and $run.Text -match 'BUMP_OK 0\.6\.1 -> 0\.6\.2') "the run said: $($run.Text)"
    Expect ((Read-Plain (Join-Path $root 'CMakeLists.txt')) -match 'project\(OoTRecompiled VERSION 0\.6\.2 LANGUAGES') 'CMakeLists.txt was not moved'
    Expect ((Read-Plain (Join-Path $root 'CMakeLists.txt')).Contains("`r`n")) 'the line endings of CMakeLists.txt changed'
    Expect ((Read-Plain (Join-Path $root 'README.md')) -match 'Version 0\.6\.2, released\. More text\.') 'README.md was not moved'
    Expect ((Subjects $root)[0] -eq 'chore(release): 0.6.2') "the newest commit is '$((Subjects $root)[0])'"
    $files = @(& git -C $root show --name-only --format= HEAD | Where-Object { $_ })
    Expect (($files -join ',') -eq 'CMakeLists.txt,README.md') "the commit holds: $($files -join ', ')"
    Expect ((& git -C $root status --porcelain -- notes.txt) -match 'notes\.txt') 'the unrelated change was committed or lost'
}

Case 'a minor bump starts the patch at zero' {
    param($root)
    New-Repo $root '0.6.1' '0.7.0'
    $run = Invoke-Bump $root @('-Kind', 'Minor ')
    Expect ($run.Code -eq 0 -and $run.Text -match 'BUMP_OK 0\.6\.1 -> 0\.7\.0') "the run said: $($run.Text)"
}

Case 'a major bump starts minor and patch at zero' {
    param($root)
    New-Repo $root '0.6.1' '1.0.0'
    $run = Invoke-Bump $root @('-Kind', 'major')
    Expect ($run.Code -eq 0 -and $run.Text -match 'BUMP_OK 0\.6\.1 -> 1\.0\.0') "the run said: $($run.Text)"
}

Case 'a kind that disagrees with the notes changes nothing' {
    param($root)
    New-Repo $root '0.6.1' '0.6.2'
    $run = Invoke-Bump $root @('-Kind', 'minor')
    Expect ($run.Code -eq 1 -and $run.Text -match 'BUMP_FAIL a minor bump of 0\.6\.1 is 0\.7\.0, but the changelog''s newest entry is 0\.6\.2') "the run said: $($run.Text)"
    Expect ((Read-Plain (Join-Path $root 'CMakeLists.txt')) -match 'VERSION 0\.6\.1 ') 'CMakeLists.txt changed'
    Expect ((Subjects $root).Count -eq 1) 'a commit was made'
}

Case 'notes not yet written for the next version stop the run' {
    param($root)
    New-Repo $root '0.6.1' '0.6.1'
    $run = Invoke-Bump $root @('-Kind', 'patch')
    Expect ($run.Code -eq 1 -and $run.Text -match "BUMP_FAIL the changelog's newest entry is 0\.6\.1, the version the tree already carries") "the run said: $($run.Text)"
}

Case 'running it again after a bump bumps nothing' {
    param($root)
    New-Repo $root '0.6.1' '0.6.2'
    $first = Invoke-Bump $root @('-Kind', 'patch')
    Expect ($first.Code -eq 0) "the first run said: $($first.Text)"
    $again = Invoke-Bump $root @('-Kind', 'patch')
    Expect ($again.Code -eq 0 -and $again.Text -match 'BUMP_OK already 0\.6\.2') "the second run said: $($again.Text)"
    Expect ((Subjects $root).Count -eq 2) "there are $((Subjects $root).Count) commits"
    Expect ((Read-Plain (Join-Path $root 'CMakeLists.txt')) -match 'VERSION 0\.6\.2 ') 'CMakeLists.txt moved again'
}

Case 'another branch is refused' {
    param($root)
    New-Repo $root '0.6.1' '0.6.2' 'fix/something'
    $run = Invoke-Bump $root @('-Kind', 'patch')
    Expect ($run.Code -eq 1 -and $run.Text -match "BUMP_FAIL the tree is on 'fix/something', not main") "the run said: $($run.Text)"
}

Case 'notes not committed yet are refused' {
    param($root)
    New-Repo $root '0.6.1' '0.6.1'
    Write-Plain (Join-Path $root 'assets\changelog.json') "[ { `"version`": `"0.6.2`" }, { `"version`": `"0.6.1`" } ]`n"
    $run = Invoke-Bump $root @('-Kind', 'patch')
    Expect ($run.Code -eq 1 -and $run.Text -match 'BUMP_FAIL commit these first') "the run said: $($run.Text)"
    Expect ((Read-Plain (Join-Path $root 'CMakeLists.txt')) -match 'VERSION 0\.6\.1 ') 'CMakeLists.txt changed'
}

Case 'a kind that is not one of the three is refused' {
    param($root)
    New-Repo $root '0.6.1' '0.6.2'
    $run = Invoke-Bump $root @('-Kind', 'hotfix')
    Expect ($run.Code -eq 1 -and $run.Text -match "BUMP_FAIL the kind is 'hotfix'") "the run said: $($run.Text)"
}

Case 'without a commit it writes the files and leaves Git alone' {
    param($root)
    New-Repo $root '0.6.1' '0.6.2'
    $run = Invoke-Bump $root @('-Kind', 'patch', '-NoCommit')
    Expect ($run.Code -eq 0 -and $run.Text -match 'BUMP_OK 0\.6\.1 -> 0\.6\.2') "the run said: $($run.Text)"
    Expect ((Subjects $root).Count -eq 1) 'a commit was made'
    Expect ((& git -C $root status --porcelain -- CMakeLists.txt) -match 'CMakeLists') 'CMakeLists.txt was not written'
}

Case 'a byte order mark is kept' {
    param($root)
    New-Repo $root '0.6.1' '0.6.2'
    $path = Join-Path $root 'README.md'
    [IO.File]::WriteAllText($path, "Version 0.6.1, released.`n", (New-Object Text.UTF8Encoding $true))
    Invoke-RepoGit $root commit -q -am 'readme with a mark'
    $run = Invoke-Bump $root @('-Kind', 'patch')
    Expect ($run.Code -eq 0) "the run said: $($run.Text)"
    $bytes = [IO.File]::ReadAllBytes($path)
    Expect ($bytes[0] -eq 0xEF -and $bytes[3] -eq [byte][char]'V') 'the mark was lost or doubled'
}

if ($script:failed.Count) { Write-Output "BUMP_TESTS_FAIL $($script:failed -join '; ')"; exit 1 }
Write-Output 'BUMP_TESTS_OK'
