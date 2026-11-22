# Tests for publish_public.ps1, run without -Push from a copy inside a throwaway repository, with
# TEMP pointed into the throwaway folder so the public copy it builds lands there too. Nothing is
# pushed and this repository is not read.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\publish_public.test.ps1
#
# Prints one line per case and PUBLISH_TESTS_OK, or PUBLISH_TESTS_FAIL with the failed cases; exits
# 1 on a failure.
$ErrorActionPreference = 'Continue'

$script:failed = @()
function Case([string]$name, [scriptblock]$body) {
    $root = Join-Path ([IO.Path]::GetTempPath()) ('publish-test-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root | Out-Null
    try { & $body $root; Write-Output "ok    $name" }
    catch { $script:failed += $name; Write-Output "FAIL  $name : $($_.Exception.Message)" }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}
function Expect([bool]$condition, [string]$what) { if (-not $condition) { throw $what } }
# Its own identity and no signing, so the throwaway commits never depend on the machine's Git setup.
function Invoke-Git {
    & git.exe -c user.name='publish test' -c user.email='publish-test@example.invalid' -c commit.gpgsign=false @args 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed" }
}
function Commit([string]$repo, [string]$file, [string]$text) {
    Set-Content (Join-Path $repo $file) $text
    Invoke-Git -C $repo add $file
    Invoke-Git -C $repo commit -q -m $text
    return (& git -C $repo rev-parse HEAD)
}

# A repository with one submodule whose HEAD gitlink is $pinned while its checkout sits at $moved,
# the "+" case of git submodule status.
function Initialize-Repository([string]$root) {
    $sub = Join-Path $root 'sub'
    Invoke-Git init -q -b main $sub
    $pinned = Commit -repo $sub -file 'a.txt' -text 'pinned'
    $moved = Commit -repo $sub -file 'b.txt' -text 'moved'
    $repo = Join-Path $root 'repo'
    Invoke-Git init -q -b main $repo
    New-Item -ItemType Directory -Path (Join-Path $repo 'tools'), (Join-Path $repo 'lib') | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'publish_public.ps1') (Join-Path $repo 'tools\publish_public.ps1')
    Set-Content (Join-Path $repo 'CMakeLists.txt') 'project(OoTRecompiled VERSION 9.9.9)'
    Set-Content (Join-Path $repo '.gitmodules') "[submodule `"lib/sub`"]`n`tpath = lib/sub`n`turl = ../sub"
    Invoke-Git clone -q $sub (Join-Path $repo 'lib\sub')
    Invoke-Git -C (Join-Path $repo 'lib\sub') checkout -q $pinned
    Invoke-Git -C $repo add .gitmodules CMakeLists.txt tools
    Invoke-Git -C $repo update-index --add --cacheinfo "160000,$pinned,lib/sub"
    Invoke-Git -C $repo commit -q -m 'the tree'
    Invoke-Git -C $repo submodule init
    Invoke-Git -C (Join-Path $repo 'lib\sub') checkout -q $moved
    return [pscustomobject]@{ Repo = $repo; Pinned = $pinned; Moved = $moved }
}

function Invoke-Publish([string]$root, [string]$repo) {
    $temp = Join-Path $root 'temp'
    New-Item -ItemType Directory -Path $temp | Out-Null
    $saved = $env:TEMP, $env:PATH
    $env:TEMP = $temp
    # Windows' own tar first, as in a PowerShell window: a shell like Git Bash puts its GNU tar ahead.
    $env:PATH = (Join-Path $env:SystemRoot 'System32') + ';' + $env:PATH
    try { $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\publish_public.ps1') 2>&1 }
    finally { $env:TEMP, $env:PATH = $saved }
    return [pscustomobject]@{ Text = ($out -join "`n"); Copy = (Join-Path $temp 'oot-recompiled-public') }
}

Case 'a submodule is pinned at the commit HEAD names, not the one checked out' {
    param($root)
    $tree = Initialize-Repository $root
    Expect ((& git -C $tree.Repo submodule status) -match '^\+') 'the stand-in checkout is not ahead of HEAD'
    $run = Invoke-Publish $root $tree.Repo
    Expect ($run.Text -match 'PUBLISH_READY') "no PUBLISH_READY: $($run.Text)"
    $entry = & git -C $run.Copy ls-files -s lib/sub
    Expect ($entry -match '^160000 ([0-9a-f]{40}) ') "the public copy has no gitlink for lib/sub: $entry"
    Expect ($Matches[1] -eq $tree.Pinned) ("pinned at {0}, the checked out commit; HEAD names {1}" -f $Matches[1], $tree.Pinned)
}

Case 'the records, the planning and the app''s own records stay out of the public copy' {
    param($root)
    $tree = Initialize-Repository $root
    $private = @('.tasks.md', '.worklog.md', 'CLAUDE.md', 'PLAN.md', '.scaffold\SCAFFOLD_PLAN.md',
        '.cleanup\report.md', '.developer-studio\feedback.json')
    foreach ($file in $private + 'README.md') {
        $path = Join-Path $tree.Repo $file
        New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
        Set-Content $path "private: $file"
    }
    Invoke-Git -C $tree.Repo add -A -- . ':!lib/sub'
    Invoke-Git -C $tree.Repo commit -q -m 'records'
    $run = Invoke-Publish $root $tree.Repo
    Expect ($run.Text -match 'PUBLISH_READY') "no PUBLISH_READY: $($run.Text)"
    foreach ($file in $private) {
        $top = ($file -split '\\')[0]
        Expect (-not (Test-Path (Join-Path $run.Copy $top))) "$top is in the public copy"
    }
    Expect (Test-Path (Join-Path $run.Copy 'README.md')) 'the public copy lost an ordinary file'
}

if ($script:failed.Count) { Write-Output "PUBLISH_TESTS_FAIL $($script:failed -join '; ')"; exit 1 }
Write-Output 'PUBLISH_TESTS_OK'
