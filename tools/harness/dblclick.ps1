# dblclick.ps1: the one start the other scripts never do.
#
# Every script here starts the game with its standard output and error redirected into trace
# files, which is how a double click on 0.1.0 died unseen: with NO standard handles the log
# redirect left a stream closed, and the first print through it ended the process before its
# window. So this script starts the program the way a double click does (no handles, no
# arguments, its own folder as the working directory) and expects a window and a log carrying
# both streams; then it starts it five more times with --crash-test <kind>, one ending each,
# and expects a report with a stack in the log every time, and for the invalid parameter a
# program that goes on and exits 0.
#
#   powershell -ExecutionPolicy Bypass -File tools\harness\dblclick.ps1 [-Tag t] [-Folder f]
#       [-WaitSeconds 25] [-NoCrashTests]
#
# Prints DBLCLICK_OK, or one DBLCLICK_FAIL line per failed expectation. Each run's log is kept
# as build-cmake\harness\<tag>-<run>.log. The window that appears is the launcher; the script
# closes it itself. Nothing is pressed, so a run does not mind the user typing elsewhere.
param(
    [string]$Tag = "dblclick",
    [string]$Folder = "",
    [int]$WaitSeconds = 25,
    [switch]$NoCrashTests
)

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($Folder -eq "") { $Folder = Join-Path $root "build-cmake" }
$exe = Join-Path $Folder "OoTRecompiled.exe"
$log = Join-Path $Folder "oot-recompiled.log"
$out = Join-Path $root "build-cmake\harness"
New-Item -ItemType Directory -Force $out | Out-Null
$failures = @()

function Start-Bare {
    param([string[]]$arguments, [int]$wait, [bool]$expectWindow)
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    if (Test-Path $log) { Remove-Item $log -Force }
    # No redirection: a program of the Windows subsystem started this way gets no standard
    # handles, exactly as from a double click.
    if ($arguments.Count -gt 0) {
        $p = Start-Process -FilePath $exe -WorkingDirectory $Folder -ArgumentList $arguments -PassThru
    } else {
        $p = Start-Process -FilePath $exe -WorkingDirectory $Folder -PassThru
    }
    $deadline = (Get-Date).AddSeconds($wait)
    $window = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if ($p.HasExited) { break }
        if ($p.MainWindowHandle -ne 0) {
            $window = $true
            if ($expectWindow) { Start-Sleep -Seconds 3; break }
        }
    }
    $p.Refresh()
    $exited = $p.HasExited
    $code = $null
    if ($exited) {
        $code = $p.ExitCode
    } else {
        Stop-Process -Id $p.Id -Force
        Start-Sleep -Milliseconds 500
    }
    $text = ""
    if (Test-Path $log) { $text = [IO.File]::ReadAllText($log) }
    return @{ window = $window; exited = $exited; code = $code; log = $text }
}

function Format-Code {
    param($code)
    if ($null -eq $code) { return "alive" }
    return ("0x{0:X8}" -f [int64]($code -band 0xFFFFFFFF))
}

# 1. A bare start: a window, and a log whose first line is the build line (stdout) and which
#    carries a [launch] line (stderr), so both streams reach the same file.
$r = Start-Bare @() $WaitSeconds $true
Copy-Item $log (Join-Path $out "$Tag-bare.log") -ErrorAction SilentlyContinue
Write-Output ("bare start: window {0}, exited {1}, code {2}, log {3} bytes" -f $r.window, $r.exited, (Format-Code $r.code), $r.log.Length)
if (-not $r.window) { $failures += ("no window from a bare start (exited {0}, code {1})" -f $r.exited, (Format-Code $r.code)) }
if ($r.log -notmatch "^OoT: Recompiled ") { $failures += "the log's first line is not the build line" }
if (-not $r.log.Contains("[launch]")) { $failures += "no stderr line ([launch]) in the log" }
if ($r.log.Contains("CRASH")) { $failures += "a crash report in a bare start" }

# 2. Each ending made to happen at the start of main, before the window.
if (-not $NoCrashTests) {
    $kinds = @(
        @{ kind = "invalid-parameter"; marker = "invalid parameter"; goesOn = $true },
        @{ kind = "terminate";         marker = "uncaught C++ exception"; goesOn = $false },
        @{ kind = "abort";             marker = "abort called"; goesOn = $false },
        @{ kind = "purecall";          marker = "pure virtual"; goesOn = $false },
        @{ kind = "access-violation";  marker = "access violation"; goesOn = $false }
    )
    foreach ($k in $kinds) {
        $r = Start-Bare @('--crash-test', $k.kind) 20 $false
        Copy-Item $log (Join-Path $out ("$Tag-" + $k.kind + ".log")) -ErrorAction SilentlyContinue
        $reported = $r.log.Contains($k.marker)
        Write-Output ("crash test {0}: exited {1}, code {2}, report {3}, stack {4}" -f $k.kind, $r.exited, (Format-Code $r.code), $reported, $r.log.Contains("stack:"))
        if (-not $r.exited) { $failures += ($k.kind + ": still running") }
        if (-not $reported) { $failures += ($k.kind + ": no report in the log") }
        if (-not $r.log.Contains("stack:")) { $failures += ($k.kind + ": no stack in the report") }
        if ($k.goesOn) {
            if ($r.code -ne 0 -or -not $r.log.Contains("the program goes on")) {
                $failures += ($k.kind + ": the program did not go on (code " + (Format-Code $r.code) + ")")
            }
        } elseif ($r.code -eq 0) {
            $failures += ($k.kind + ": exit code 0 for an ending")
        }
    }
}

if ($failures.Count -eq 0) {
    Write-Output "DBLCLICK_OK"
    exit 0
}
foreach ($f in $failures) { Write-Output ("DBLCLICK_FAIL " + $f) }
exit 1
