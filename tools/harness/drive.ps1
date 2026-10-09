# Drive the game with synthetic input and capture what it drew.
#
# Dot-source this from the other scripts in this directory. Nothing here starts the game; it finds
# a window by process id, presses keys the way a person does, and copies pixels off the screen.
#
# The game reads the keyboard through SDL, which reads WM_KEYDOWN from the foreground window's
# message queue, so the input has to be real: SendInput, not a posted message. That also means the
# window must actually be in the foreground, which is why Focus-Game runs before every press.
#
# Capture is a BitBlt of the screen region the window occupies rather than PrintWindow, because the
# window is a D3D12 swapchain and PrintWindow gives back a blank rectangle for one of those.
#
# The window is found by PROCESS ID, not by title. FindWindow by title failed against a window that
# was demonstrably there with exactly that title, which cost a while to believe; enumerating and
# matching the owning process needs no string to be right and cannot be confused by a second copy.
#
# WHERE THINGS GO. Every capture is a picture of the game and is never committed. Scripts write
# under build-cmake\harness\, which .gitignore already excludes with the rest of the build output.

Add-Type @'
using System;
using System.Text;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;

public class Drv {
    public delegate bool EnumCb(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb c, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassName(IntPtr h, [Out] StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static bool PostClose(IntPtr h) { return PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); }
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint n, INPUT[] inputs, int size);

    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk; public ushort wScan; public uint dwFlags;
        public uint time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Explicit, Size=40)]
    public struct INPUT {
        [FieldOffset(0)] public uint type;
        [FieldOffset(8)] public KEYBDINPUT ki;
    }

    const uint KEYEVENTF_KEYUP       = 0x0002;
    const uint KEYEVENTF_EXTENDEDKEY = 0x0001;

    static bool IsExtended(ushort vk) {
        // The arrow keys live on the extended part of the keyboard. Without the flag they arrive
        // as their numpad twins, which is a silent wrong answer rather than an error.
        return vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28;
    }

    static void Send(ushort vk, bool up) {
        INPUT[] a = new INPUT[1];
        a[0].type = 1;
        a[0].ki.wVk = vk;
        a[0].ki.wScan = (ushort)MapVirtualKey(vk, 0);
        a[0].ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (IsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, a, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void Down(ushort vk) { Send(vk, false); }
    public static void Up(ushort vk)   { Send(vk, true); }

    // The main window of a process is the visible top level one of SDL's own window class. Taking
    // the first visible window would sometimes pick up a tool window and screenshot nothing.
    public static IntPtr MainWindow(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid || !IsWindowVisible(h)) { return true; }
            StringBuilder cls = new StringBuilder(256);
            GetClassName(h, cls, 256);
            if (cls.ToString() == "SDL_app") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // Resize so the CLIENT area is w by ht: the frame's thickness is measured from the window
    // rather than assumed, so this holds whatever the theme draws around it.
    public static void ResizeClient(IntPtr h, int w, int ht) {
        RECT win; GetWindowRect(h, out win);
        RECT cli; GetClientRect(h, out cli);
        int frameW = (win.R - win.L) - (cli.R - cli.L);
        int frameH = (win.B - win.T) - (cli.B - cli.T);
        MoveWindow(h, win.L, win.T, w + frameW, ht + frameH, true);
    }

    static Bitmap Grab(IntPtr h) {
        RECT r; GetClientRect(h, out r);
        POINT tl; tl.X = r.L; tl.Y = r.T; ClientToScreen(h, ref tl);
        int w = r.R - r.L, ht = r.B - r.T;
        if (w < 1 || ht < 1) { return null; }
        Bitmap bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp)) {
            g.CopyFromScreen(tl.X, tl.Y, 0, 0, new Size(w, ht), CopyPixelOperation.SourceCopy);
        }
        return bmp;
    }

    public static void Shot(IntPtr h, string path) {
        using (Bitmap bmp = Grab(h)) { if (bmp != null) { bmp.Save(path, ImageFormat.Png); } }
    }

    // The other way to read a window: ask it to paint itself into our bitmap. PW_RENDERFULLCONTENT
    // (2) includes content drawn by a swap chain. Kept beside the screen copy because the two can
    // disagree about how often the picture changes, and when they do that is a fact about the
    // capture, not the game.
    static Bitmap GrabPrintWindow(IntPtr h) {
        RECT r; GetClientRect(h, out r);
        int w = r.R - r.L, ht = r.B - r.T;
        if (w < 1 || ht < 1) { return null; }
        Bitmap bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp)) {
            IntPtr hdc = g.GetHdc();
            PrintWindow(h, hdc, 2);
            g.ReleaseHdc(hdc);
        }
        return bmp;
    }

    // A burst: as many grabs as asked, back to back, held in memory and saved only afterward so
    // the cadence is one grab's own cost and nothing else. Returns the milliseconds the burst
    // took. Counting how many DISTINCT pictures come out of a burst is how the capture's own
    // rate is measured: at a sixty hertz picture a burst of twelve fourteen millisecond grabs
    // holds about ten different frames, at twenty hertz about four.
    public static double ShotBurst(IntPtr h, string dir, int count, bool printWindow) {
        Bitmap[] frames = new Bitmap[count];
        Stopwatch sw = Stopwatch.StartNew();
        for (int i = 0; i < count; i++) {
            frames[i] = printWindow ? GrabPrintWindow(h) : Grab(h);
        }
        double ms = sw.Elapsed.TotalMilliseconds;
        for (int i = 0; i < count; i++) {
            if (frames[i] != null) {
                frames[i].Save(System.IO.Path.Combine(dir, string.Format("burst-{0:D2}.png", i)), ImageFormat.Png);
                frames[i].Dispose();
            }
        }
        return ms;
    }

    // Two captures as close together as the copy allows, saved only after both are taken, so the
    // gap between them is one screen copy and not a PNG encode. Returns the gap in milliseconds.
    // This is what the motion check needs: at a display rate of 60 the gap spans about two
    // presented frames, and whether the two pictures differ says whether anything was drawn in
    // between.
    public static double ShotPair(IntPtr h, string pathA, string pathB) {
        Stopwatch sw = Stopwatch.StartNew();
        Bitmap a = Grab(h);
        double t0 = sw.Elapsed.TotalMilliseconds;
        Bitmap b = Grab(h);
        double t1 = sw.Elapsed.TotalMilliseconds;
        if (a != null) { a.Save(pathA, ImageFormat.Png); a.Dispose(); }
        if (b != null) { b.Save(pathB, ImageFormat.Png); b.Dispose(); }
        return t1 - t0;
    }
}
'@ -ReferencedAssemblies System.Drawing

# The keyboard map the game uses (src/main/boot.cpp): X=A, C=B, Z=Z, Enter=Start, arrows=D-pad,
# The keyboard's defaults since 2026-09-19 (input_bindings.cpp, keyboard_defaults): Space=A,
# Left Shift=B, Q=Z, Enter=Start, arrows=D-pad, E/R=shoulders, IJKL=C buttons, WASD=analog stick.
$VK = @{
  'A'=0x20; 'B'=0xA0; 'Z'=0x51; 'START'=0x0D;
  'DUP'=0x26; 'DDOWN'=0x28; 'DLEFT'=0x25; 'DRIGHT'=0x27;
  'L'=0x45; 'R'=0x52;
  'F6'=0x75; 'F9'=0x78; 'BACK'=0x08;
  'CUP'=0x49; 'CDOWN'=0x4B; 'CLEFT'=0x4A; 'CRIGHT'=0x4C;
  'SUP'=0x57; 'SDOWN'=0x53; 'SLEFT'=0x41; 'SRIGHT'=0x44;
  'F1'=0x70; 'F2'=0x71;
  # Not bound to anything by default: what bindcheck.ps1 binds Start to, to prove the file is read.
  'P'=0x50;
  # The controls document (phase 50): F3 opens it, Escape closes a document, Delete clears a binding.
  'F3'=0x72; 'ESC'=0x1B; 'DEL'=0x2E;
  # Photo mode (2026-10-09): F10 freezes and releases, F12 is the Screenshot binding.
  'F10'=0x79; 'F12'=0x7B; 'H'=0x48;
}

$script:hwnd = [IntPtr]::Zero

# Paths every script shares. The project root is two directories up from this file.
$script:HarnessRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:HarnessExe  = Join-Path $script:HarnessRoot 'build-cmake\OoTRecompiled.exe'
$script:HarnessOut  = Join-Path $script:HarnessRoot 'build-cmake\harness'
New-Item -ItemType Directory -Force -Path $script:HarnessOut | Out-Null

function Find-Game {
    param([int]$procId)
    for ($i = 0; $i -lt 90; $i++) {
        $h = [Drv]::MainWindow([uint32]$procId)
        if ($h -ne [IntPtr]::Zero) { $script:hwnd = $h; return $h }
        Start-Sleep -Milliseconds 500
    }
    throw 'game window never appeared'
}

function Focus-Game {
    [Drv]::ShowWindow($script:hwnd, 5) | Out-Null   # SW_SHOW
    [Drv]::SetForegroundWindow($script:hwnd) | Out-Null
    Start-Sleep -Milliseconds 120
    # The presses go to the FOREGROUND window, whichever that is. A person typing in another
    # window while a run is up takes the focus with every keystroke, and the run's presses land
    # in their editor: the game then looks broken when nothing about it is (a bindcheck failed
    # exactly that way on 2026-09-19). So wait for the focus to come back, up to ten seconds,
    # and say so in the run's output, which is the first line to read when a check fails for no
    # reason the trace can show.
    $waited = 0
    while ([Drv]::GetForegroundWindow() -ne $script:hwnd -and $waited -lt 10000) {
        Start-Sleep -Milliseconds 250
        $waited += 250
        [Drv]::SetForegroundWindow($script:hwnd) | Out-Null
    }
    if ($waited -gt 0) {
        $state = if ([Drv]::GetForegroundWindow() -eq $script:hwnd) { "back after $waited ms" }
                 else { "still elsewhere after $waited ms, so this press is lost" }
        Write-Host "  focus lost: another window had the keyboard, $state"
        $script:focusLost = $true
    }
}

function Resize-Game {
    param([int]$width, [int]$height)
    [Drv]::ResizeClient($script:hwnd, $width, $height)
    Start-Sleep -Milliseconds 400
}

# Hold a button for a number of frames rather than milliseconds: the game samples the controller
# once per frame, and a press shorter than a frame is simply not there.
# A left click at a point in the game window's client area (play milestone: the launcher takes
# the mouse). The cursor is moved there for real, so the hover state shows too.
function Click-Game {
    param([int]$x, [int]$y, [int]$after = 500)
    Focus-Game
    $p = New-Object Drv+POINT
    $p.X = $x; $p.Y = $y
    [Drv]::ClientToScreen($script:hwnd, [ref]$p) | Out-Null
    [Drv]::SetCursorPos($p.X, $p.Y) | Out-Null
    Start-Sleep -Milliseconds 150
    [Drv]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # left down
    Start-Sleep -Milliseconds 60
    [Drv]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # left up
    Start-Sleep -Milliseconds $after
}

function Press {
    param([string]$name, [int]$frames = 6, [int]$after = 500)
    Focus-Game
    # A NAME THAT IS NOT IN THE TABLE STOPS THE RUN (2026-09-26). It used to index to $null, cast
    # to 0, and press a key that does not exist: the run carried on, the shots showed the screen
    # the walk never left, and the failure read as the program ignoring the key. One typo can
    # quietly invalidate every assertion after it, so it is fatal here.
    if (-not $VK.ContainsKey($name)) {
        throw "Press: there is no key named '$name' in the table. Have: $(($VK.Keys | Sort-Object) -join ', ')"
    }
    $vk = [uint16]$VK[$name]
    [Drv]::Down($vk)
    Start-Sleep -Milliseconds ([int]($frames * 34))
    [Drv]::Up($vk)
    Start-Sleep -Milliseconds $after
}

function Hold {
    param([string]$name, [int]$ms = 1000)
    Focus-Game
    if (-not $VK.ContainsKey($name)) {
        throw "Hold: there is no key named '$name' in the table. Have: $(($VK.Keys | Sort-Object) -join ', ')"
    }
    $vk = [uint16]$VK[$name]
    [Drv]::Down($vk)
    Start-Sleep -Milliseconds $ms
    [Drv]::Up($vk)
    Start-Sleep -Milliseconds 200
}

function Shot {
    param([string]$path)
    Focus-Game
    Start-Sleep -Milliseconds 250
    [Drv]::Shot($script:hwnd, $path)
    Write-Host "shot -> $path"
}

function Shot-Pair {
    param([string]$pathA, [string]$pathB)
    return [Drv]::ShotPair($script:hwnd, $pathA, $pathB)
}

# Read or write one key in build-cmake\settings.txt, the file the game reads on launch. The game
# clamps whatever it finds, so a value out of range costs nothing but the setting.
function Set-Setting {
    param([string]$key, [string]$value)
    $file = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
    $lines = @()
    if (Test-Path $file) { $lines = @(Get-Content $file) }
    $done = $false
    $out = foreach ($l in $lines) {
        if ($l -match "^\s*$key\s*=") { $done = $true; "$key = $value" } else { $l }
    }
    if (-not $done) { $out = @($out) + "$key = $value" }
    Set-Content -Path $file -Value $out -Encoding ASCII
}

# Apply "key=value,key=value" to the settings file for one run and remember the file as it was.
# Every script that takes -Settings goes through this pair (the frame rate is a setting like
# any other), so the restore is one piece of code: a measurement must never change what the
# player sees next time. Returns what Restore-Settings needs; pass it back even on failure.
function Use-Settings {
    param([string]$spec)
    $file = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
    $saved = @{ existed = (Test-Path $file); lines = @() }
    if ($saved.existed) { $saved.lines = @(Get-Content $file) }
    foreach ($kv in $spec.Split(',')) {
        $pair = $kv.Split('=')
        if ($pair.Count -eq 2 -and $pair[0].Trim() -ne '') { Set-Setting $pair[0].Trim() $pair[1].Trim() }
    }
    return $saved
}

function Restore-Settings {
    param($saved)
    if ($null -eq $saved) { return }
    $file = Join-Path $script:HarnessRoot 'build-cmake\settings.txt'
    if ($saved.existed) {
        if ($saved.lines.Count -gt 0) { Set-Content -Path $file -Value $saved.lines -Encoding ASCII }
        else { Set-Content -Path $file -Value '' -Encoding ASCII }
    }
    elseif (Test-Path $file) { Remove-Item -Force $file }
}

# Start the game with the harness flags, redirecting both streams to files under the output
# directory, and wait for its window. Returns the process. A width and height resize the
# window's client area once it is up, which is how the wide-frame captures are taken.
function Start-Game {
    param([string]$tag, [string[]]$extraArgs = @(), [int]$width = 0, [int]$height = 0, [switch]$NoPlay)
    Get-Process OoTRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 700
    $probe = Join-Path $script:HarnessRoot "build-cmake\$tag-probe.txt"
    # Not $args: that is PowerShell's own automatic variable, and assigning it is a trap.
    # --play goes straight into the game past the launcher (play milestone); every script here
    # expects the game to boot on its own. launcher-shot.ps1 is the one that asks for -NoPlay.
    $launch = @('--probe', "`"$probe`"") + $extraArgs
    if (-not $NoPlay) { $launch = @('--play') + $launch }
    $p = Start-Process -FilePath $script:HarnessExe -WorkingDirectory (Join-Path $script:HarnessRoot 'build-cmake') `
            -ArgumentList $launch `
            -RedirectStandardOutput (Join-Path $script:HarnessOut "$tag.stdout.txt") `
            -RedirectStandardError  (Join-Path $script:HarnessOut "$tag.stderr.txt") -PassThru
    Find-Game $p.Id | Out-Null
    if ($width -gt 0 -and $height -gt 0) { Resize-Game $width $height }
    return $p
}

# From the boot logo to ordinary play with File 1 loaded. Timings are generous on purpose.
function Enter-Play {
    Start-Sleep -Seconds 14
    Press 'START' 8 3000
    Press 'START' 8 3000
    Press 'A'     8 2500
    Press 'A'     8 6000
    Press 'A'     8 8000
}

# Close the game the way a person does: the window's close button, which SDL turns into
# SDL_WINDOWEVENT_CLOSE and then SDL_QUIT. That exercises the real shutdown path rather than
# killing the process, so a shutdown that hangs or crashes shows up here instead of in the field.
function Close-Game {
    param([int]$procId, [int]$timeoutSec = 30)
    [Drv]::PostClose($script:hwnd) | Out-Null
    for ($i = 0; $i -lt ($timeoutSec * 4); $i++) {
        if (-not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}
