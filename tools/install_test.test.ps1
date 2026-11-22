# Tests for install_test.ps1, each with a throwaway installed copy, build tree and desktop: the real
# data folder, build and desktop are never touched.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_test.test.ps1
#
# Prints one line per case and INSTALL_TEST_TESTS_OK, or INSTALL_TEST_TESTS_FAIL with the failed
# cases; exits 1 on a failure. Not covered: the refusal while the test copy runs, which needs a
# running process at that path.
$ErrorActionPreference = 'Continue'
$script = Join-Path $PSScriptRoot 'install_test.ps1'

$script:failed = @()
function Case([string]$name, [scriptblock]$body) {
    $root = Join-Path ([IO.Path]::GetTempPath()) ("install test " + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root | Out-Null
    try { & $body $root; Write-Output "ok    $name" }
    catch { $script:failed += $name; Write-Output "FAIL  $name : $($_.Exception.Message)" }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}
function Expect([bool]$condition, [string]$what) { if (-not $condition) { throw $what } }

function Put([string]$path, [string]$text) {
    New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
    Set-Content -LiteralPath $path -Value $text -NoNewline
}
function Text([string]$path) { Get-Content -LiteralPath $path -Raw }

# An installed copy (bin\) and a build tree, each file holding a word that says where it came from.
function New-Setup([string]$root, [switch]$NoMarker, [switch]$NoBuild) {
    $bin = Join-Path $root 'data\bin'
    foreach ($file in @('OoTRecompiled.exe', 'SDL2.dll', 'dxil.dll', 'vcruntime140.dll', 'uninstall.exe', 'assets\old.txt', 'assets\both.txt')) {
        Put (Join-Path $bin $file) "installed $file"
    }
    if (-not $NoMarker) { Put (Join-Path $bin '.installed') 'yes' }
    if (-not $NoBuild) {
        foreach ($file in @('OoTRecompiled.exe', 'SDL2.dll', 'assets\new.txt', 'assets\both.txt', 'CMakeCache.txt')) {
            Put (Join-Path $root "build\$file") "built $file"
        }
    }
    New-Item -ItemType Directory -Path (Join-Path $root 'desktop') -Force | Out-Null
}

function Invoke-Install([string]$root, [string[]]$more) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Source (Join-Path $root 'build') -DataDir (Join-Path $root 'data') -Desktop (Join-Path $root 'desktop') @more
    return [pscustomobject]@{ Text = ($out -join "`n"); Code = $LASTEXITCODE }
}

Case 'the build goes to bin-test with the installed copy''s libraries and marker' {
    param($root)
    New-Setup $root
    $run = Invoke-Install $root @()
    Expect ($run.Code -eq 0 -and $run.Text -match 'INSTALL_TEST_OK') "the run said: $($run.Text)"
    $test = Join-Path $root 'data\bin-test'
    Expect ((Text "$test\OoTRecompiled.exe") -eq 'built OoTRecompiled.exe') 'the executable is not the build''s'
    Expect ((Text "$test\SDL2.dll") -eq 'built SDL2.dll') 'SDL2.dll is not the build''s'
    Expect ((Text "$test\dxil.dll") -eq 'installed dxil.dll') 'the runtime libraries were not copied'
    Expect (Test-Path "$test\.installed") 'the marker was not copied, so the test copy would keep saves of its own'
    Expect (-not (Test-Path "$test\uninstall.exe")) 'the uninstaller was copied'
    Expect (-not (Test-Path "$test\CMakeCache.txt")) 'build output beyond the program was copied'
    Expect ((Text "$test\assets\both.txt") -eq 'built assets\both.txt') 'the assets are not the build''s'
    Expect (Test-Path "$test\assets\new.txt") 'a new asset is missing'
    Expect (-not (Test-Path "$test\assets\old.txt")) 'an asset the build does not have was kept'
}

Case 'the installed copy is left exactly as it was' {
    param($root)
    New-Setup $root
    $bin = Join-Path $root 'data\bin'
    $before = Get-ChildItem -LiteralPath $bin -Recurse -File | ForEach-Object { "$($_.FullName)=$((Get-FileHash $_.FullName).Hash)" }
    $run = Invoke-Install $root @()
    Expect ($run.Code -eq 0) "the run said: $($run.Text)"
    $after = Get-ChildItem -LiteralPath $bin -Recurse -File | ForEach-Object { "$($_.FullName)=$((Get-FileHash $_.FullName).Hash)" }
    Expect ((@($before) -join '|') -eq (@($after) -join '|')) 'bin\ changed'
}

Case 'the shortcut carries the label and replaces an earlier one' {
    param($root)
    New-Setup $root
    $desktop = Join-Path $root 'desktop'
    Put (Join-Path $desktop 'OoT Recompiled TEST (older fix).lnk') 'old'
    Put (Join-Path $desktop 'OoT Recompiled.lnk') 'the real one'
    $run = Invoke-Install $root @('-Label', 'stick: deadzone fix')
    Expect ($run.Code -eq 0) "the run said: $($run.Text)"
    $tests = @(Get-ChildItem -LiteralPath $desktop -Filter 'OoT Recompiled TEST*.lnk' | ForEach-Object Name)
    Expect (($tests -join ',') -eq 'OoT Recompiled TEST (stick  deadzone fix).lnk') "the TEST shortcuts are: $($tests -join ', ')"
    Expect (Test-Path (Join-Path $desktop 'OoT Recompiled.lnk')) 'the installed copy''s shortcut was removed'
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $desktop $tests[0]))
    Expect ($link.TargetPath -eq (Join-Path $root 'data\bin-test\OoTRecompiled.exe')) "the shortcut points at $($link.TargetPath)"
}

Case 'without a shortcut the desktop is not touched' {
    param($root)
    New-Setup $root
    $run = Invoke-Install $root @('-NoShortcut')
    Expect ($run.Code -eq 0) "the run said: $($run.Text)"
    Expect (@(Get-ChildItem -LiteralPath (Join-Path $root 'desktop')).Count -eq 0) 'a shortcut was made'
}

Case 'a second install replaces the first' {
    param($root)
    New-Setup $root
    $first = Invoke-Install $root @()
    Expect ($first.Code -eq 0) "the first run said: $($first.Text)"
    Put (Join-Path $root 'build\OoTRecompiled.exe') 'built again'
    $second = Invoke-Install $root @()
    Expect ($second.Code -eq 0) "the second run said: $($second.Text)"
    Expect ((Text (Join-Path $root 'data\bin-test\OoTRecompiled.exe')) -eq 'built again') 'the executable was not replaced'
}

Case 'no build is refused before anything is copied' {
    param($root)
    New-Setup $root -NoBuild
    $run = Invoke-Install $root @()
    Expect ($run.Code -eq 1 -and $run.Text -match 'INSTALL_TEST_FAIL no build at') "the run said: $($run.Text)"
    Expect (-not (Test-Path (Join-Path $root 'data\bin-test'))) 'bin-test was made'
}

Case 'an installed copy without its marker is refused' {
    param($root)
    New-Setup $root -NoMarker
    $run = Invoke-Install $root @()
    Expect ($run.Code -eq 1 -and $run.Text -match 'has no \.installed marker') "the run said: $($run.Text)"
    Expect (-not (Test-Path (Join-Path $root 'data\bin-test'))) 'bin-test was made'
}

Case 'no installed copy is refused' {
    param($root)
    New-Setup $root
    Remove-Item -LiteralPath (Join-Path $root 'data\bin') -Recurse -Force
    $run = Invoke-Install $root @()
    Expect ($run.Code -eq 1 -and $run.Text -match 'INSTALL_TEST_FAIL no installed copy at') "the run said: $($run.Text)"
}

if ($script:failed.Count) { Write-Output "INSTALL_TEST_TESTS_FAIL $($script:failed -join '; ')"; exit 1 }
Write-Output 'INSTALL_TEST_TESTS_OK'
