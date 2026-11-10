#!/usr/bin/env python3
"""Launch a verified local ROM with bounded memory and catch early Metal crashes."""

import argparse
import os
import re
import subprocess
import tempfile
import time
from datetime import datetime, timedelta
from pathlib import Path


def is_visible_png(path: Path) -> bool:
    """Reject nearly black recordings using decoded luma, not PNG file size."""
    try:
        result = subprocess.run(
            ["ffmpeg", "-hide_banner", "-i", str(path),
             "-vf", "signalstats,metadata=print", "-frames:v", "1", "-f", "null", "-"],
            capture_output=True, text=True, check=False,
        )
    except FileNotFoundError:
        return False
    match = re.search(r"lavfi\.signalstats\.YAVG=([\d.]+)", result.stderr)
    return result.returncode == 0 and match is not None and float(match.group(1)) > 20.0


def metal_gpu_errors(pid: int, started: datetime) -> bool:
    """Detect driver faults that can leave the process drawing corrupt frames."""
    predicate = (f'processID == {pid} AND '
                 '(eventMessage CONTAINS[c] "kIOGPUCommandBufferCallbackError" OR '
                 'eventMessage CONTAINS[c] "GPU Address Fault")')
    result = subprocess.run(
        ["/usr/bin/log", "show", "--start", started.strftime("%Y-%m-%d %H:%M:%S"),
         "--style", "compact", "--predicate", predicate],
        capture_output=True, text=True, timeout=15, check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"could not inspect macOS Metal errors: {result.stderr[-300:]}")
    return "OoTRecompiled[" in result.stdout


def shader_validation_environment(base: dict[str, str]) -> dict[str, str]:
    """Enable Apple's fault-reporting instrumentation for guarded diagnostics."""
    environment = base.copy()
    environment.update({
        "MTL_SHADER_VALIDATION": "1",
        "MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING": "1",
        "MTL_SHADER_VALIDATION_REPORT_TO_STDERR": "1",
        "MTL_SHADER_VALIDATION_ABORT_ON_FAULT": "1",
    })
    return environment


def require_unlocked_capture_session() -> None:
    """A locked console session can return black Metal drawables for long runs."""
    code = (
        "import CoreGraphics; import Foundation; import Darwin; "
        "guard let session = CGSessionCopyCurrentDictionary() as NSDictionary? "
        "else { exit(3) }; "
        "if (session[\"CGSSessionScreenIsLocked\"] as? Int ?? 0) != 0 { exit(2) }"
    )
    try:
        probe = subprocess.run(["swift", "-e", code], capture_output=True, text=True,
                               timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError("could not verify whether the Mac desktop is unlocked") from exc
    if probe.returncode == 2:
        raise RuntimeError("the Mac desktop is locked; unlock it before recording gameplay")
    if probe.returncode != 0:
        raise RuntimeError("could not verify whether the Mac desktop is unlocked")


def renderer_launch_mode(raytracing: bool, raster_rt_app: bool,
                         requested_level: int = 1) -> tuple[str, int | None]:
    if raytracing and raster_rt_app:
        raise ValueError("choose only one ray tracing mode")
    if requested_level not in (1, 2, 3):
        raise ValueError("ray tracing level must be 1, 2, or 3")
    if requested_level != 1 and not raytracing:
        raise ValueError("--rt-level requires --raytracing")
    if raytracing or raster_rt_app:
        return "build/rt-compile-probe", requested_level if raytracing else 0
    return "build-cmake", None


def tutorial_input_events(delay_ms: int) -> str:
    if delay_ms < 0:
        raise ValueError("input delay cannot be negative")
    events = (
        (65000, "axis", 4, 30000),
        (66000, "button", 0, 1),
        (66300, "button", 0, 0),
        (67500, "axis", 4, 0),
        (69000, "axis", 0, 18000),
        (74000, "axis", 0, 0),
        (75000, "axis", 1, -18000),
        (80000, "axis", 1, 0),
        (81000, "axis", 0, -18000),
        (86000, "axis", 0, 0),
        (87000, "axis", 1, 18000),
        (92000, "axis", 1, 0),
    )
    return "".join(f"{at + delay_ms} {kind} {index} {value}\n"
                   for at, kind, index, value in events)


def monitor_game(process: subprocess.Popen, seconds: int, max_rss_mib: int,
                 started: datetime, *, interactive: bool = False) -> int:
    """Bound a session by time, RSS, and driver health; return peak RSS in KiB."""
    peak = 0
    deadline = time.monotonic() + seconds
    gpu_since = started
    next_gpu_check = time.monotonic() + 3
    while time.monotonic() < deadline:
        status = process.poll()
        if status is not None:
            if interactive and status == 0:
                break
            raise RuntimeError(f"app exited {status}")
        if time.monotonic() >= next_gpu_check:
            checked_at = datetime.now()
            if metal_gpu_errors(process.pid, gpu_since):
                raise RuntimeError("macOS logged Metal GPU errors")
            # Overlap windows to cover timestamp rounding and delayed log delivery.
            gpu_since = checked_at - timedelta(seconds=10)
            next_gpu_check = time.monotonic() + 3
        result = subprocess.run(["ps", "-o", "rss=", "-p", str(process.pid)],
                                capture_output=True, text=True, check=False)
        if result.returncode != 0 or not result.stdout.strip():
            if process.poll() is not None:
                continue  # Handle a quit racing with ps on the next iteration.
            raise RuntimeError("could not inspect game RSS")
        peak = max(peak, int(result.stdout.strip()))
        if peak > max_rss_mib * 1024:
            raise RuntimeError(f"RSS exceeded {max_rss_mib} MiB")
        time.sleep(0.25)
    status = process.poll()
    if status is not None and not (interactive and status == 0):
        raise RuntimeError(f"app exited {status}")
    return peak


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--seconds", type=int, default=20)
    parser.add_argument("--max-rss-mib", type=int, default=3072)
    parser.add_argument("--probe", type=Path,
                        help="write the app's frame cadence and thread timing probe")
    parser.add_argument("--enter-save", action="store_true",
                        help="drive file 1 with a virtual controller")
    parser.add_argument("--interactive", action="store_true",
                        help="use real controls and accept a clean user quit; keep the watchdog")
    parser.add_argument("--no-warp", action="store_true",
                        help="load file 1 at its saved entrance, without a startup warp")
    parser.add_argument("--data-root", type=Path,
                        help="use an isolated macOS profile instead of the normal save folder")
    parser.add_argument("--exit-house", action="store_true",
                        help="walk toward Link's house door after loading the isolated child save")
    parser.add_argument("--dismiss-tutorial", action="store_true",
                        help="Z-target and press A after the Kokiri Forest tutorial, then move")
    parser.add_argument("--raytracing", action="store_true",
                        help="use the isolated RT-enabled app and explicitly opt into tracing")
    parser.add_argument("--rt-level", type=int, default=1,
                        help="lighting level for --raytracing: 1 shadows, 2 local lights and occlusion, 3 bounce")
    parser.add_argument("--raster-rt-app", action="store_true",
                        help="use the RT-enabled app with tracing at level 0 for a lighting comparison")
    parser.add_argument("--shader-validation", action="store_true",
                        help="enable Apple's Metal shader validation and abort on a detected fault")
    parser.add_argument("--capture-gameplay", action="store_true",
                        help="record 20 seconds after loading file 1 while moving with the virtual stick")
    parser.add_argument("--capture-seconds", type=int, default=20,
                        help="length of the frame recording (default: 20 seconds)")
    parser.add_argument("--record-start-tick", type=int, default=1000,
                        help="gameplay tick when recording begins (default: 1000)")
    parser.add_argument("--entrance", type=lambda value: int(value, 0), default=0x0EE)
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    try:
        app_tree, rt_level = renderer_launch_mode(args.raytracing, args.raster_rt_app,
                                                 args.rt_level)
    except ValueError as exc:
        parser.error(str(exc))
    app = root / app_tree / "OoTRecompiled.app/Contents/MacOS/OoTRecompiled"
    log_path = root / "build/macos-gameplay-smoke.log"
    if not app.is_file() or not args.rom.is_file():
        parser.error("build the app and supply an existing USA 1.0 ROM")
    if args.seconds < 1 or args.max_rss_mib < 1:
        parser.error("seconds and max-rss-mib must be positive")
    if args.enter_save and args.seconds < 55:
        parser.error("--enter-save needs at least 55 seconds")
    if args.interactive and (args.enter_save or args.capture_gameplay):
        parser.error("--interactive uses real controls instead of automated input or capture")
    if args.capture_gameplay and (not args.enter_save or not 1 <= args.capture_seconds <= 30 or
                                  args.seconds < 80 + args.capture_seconds):
        parser.error("--capture-gameplay needs --enter-save, 1-30 capture seconds, and enough run time")
    if args.record_start_tick < 1:
        parser.error("--record-start-tick must be positive")
    if args.exit_house and (not args.enter_save or not args.no_warp):
        parser.error("--exit-house needs --enter-save and --no-warp")
    if args.dismiss_tutorial and (not args.enter_save or not args.capture_gameplay):
        parser.error("--dismiss-tutorial needs --enter-save and --capture-gameplay")
    if args.data_root and not args.data_root.is_dir():
        parser.error("--data-root must already exist")
    if not 0 <= args.entrance <= 0xFFFF:
        parser.error("entrance must be a 16-bit index")
    if args.capture_gameplay:
        try:
            require_unlocked_capture_session()
        except RuntimeError as exc:
            parser.error(str(exc))

    peak = 0
    script = None
    capture_root = (args.data_root.resolve() if args.data_root else
                    Path.home() / "Library/Application Support/OoT Recompiled") / "captures"
    known_recordings = set(capture_root.rglob("*.frames.csv"))
    command = [str(app), "--play", str(args.rom.resolve())]
    if args.probe:
        command[1:1] = ["--probe", str(args.probe.resolve())]
    if args.data_root:
        command[1:1] = ["--data-root", str(args.data_root.resolve())]
    if not args.no_warp:
        command[1:1] = ["--warp", f"0x{args.entrance:03X}"]
    environment = os.environ.copy()
    if args.shader_validation:
        environment = shader_validation_environment(environment)
    if rt_level is not None:
        command[1:1] = ["--rt-level", str(rt_level)]
        environment["OOT_METAL_RAYTRACING"] = "1"
    if args.enter_save:
        # Start from the title, open file 1, and confirm it. File 1 must already
        # contain a save. Delayed presses cross the title and file-select fades.
        script = tempfile.NamedTemporaryFile(mode="w", suffix=".virtual.txt",
                                             prefix="macos-startup-warp-", dir=root / "build",
                                             delete=False)
        script.write("14000 button 6 1\n17000 button 6 0\n"
                     "20000 button 6 1\n23000 button 6 0\n"
                     "26000 button 0 1\n27000 button 0 0\n"
                     "32000 button 0 1\n33000 button 0 0\n"
                     "38000 button 0 1\n39000 button 0 0\n")
        if args.exit_house:
            script.write("50000 axis 1 22000\n62000 axis 1 0\n")
        if args.capture_gameplay:
            if args.dismiss_tutorial:
                # The prompt explicitly requires Z-target before A can speak
                # to the waiting Kokiri; SDL's left trigger is axis 4. The
                # level-0 raster path reaches that prompt later in wall time.
                script.write(tutorial_input_events(15000 if args.raster_rt_app else 0))
            else:
                # Short, half-strength stick holds exercise controller input
                # during capture. The picture still needs visual review.
                script.write("66000 axis 1 -18000\n71000 axis 1 0\n"
                             "74000 axis 0 18000\n79000 axis 0 0\n"
                             "82000 axis 1 -18000\n87000 axis 1 0\n")
        script.close()
        command[1:1] = ["--virtual-controller", script.name]
    if args.capture_gameplay:
        command[1:1] = ["--record", f"{args.capture_seconds},{args.record_start_tick}"]
    with log_path.open("w") as log:
        started = datetime.now()
        process = subprocess.Popen(
            command,
            stdout=log, stderr=subprocess.STDOUT, env=environment,
        )
        try:
            peak = monitor_game(process, args.seconds, args.max_rss_mib, started,
                                interactive=args.interactive)
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as exc:
            print(f"GAMEPLAY_SMOKE_FAILED {exc}; see {log_path}")
            return 1
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if script is not None:
                Path(script.name).unlink(missing_ok=True)

    output = log_path.read_text(errors="replace")
    try:
        if metal_gpu_errors(process.pid, started):
            print(f"GAMEPLAY_SMOKE_FAILED macOS logged Metal GPU errors; see {log_path}")
            return 1
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as exc:
        print(f"GAMEPLAY_SMOKE_FAILED {exc}")
        return 1
    if args.interactive:
        print(f"PLAY_SESSION_ENDED peak_rss_mib={peak / 1024:.1f} log={log_path}")
        return 0
    if "[gfx] drawing" not in output or "[dl]" not in output:
        print(f"GAMEPLAY_SMOKE_FAILED no rendered game frames; see {log_path}")
        return 1
    if args.enter_save and not args.no_warp and f"[warp] startup request applied: 0x{args.entrance:03X}" not in output:
        print(f"GAMEPLAY_SMOKE_FAILED startup warp did not apply; see {log_path}")
        return 1
    if args.raytracing and ("[rt] device raytracing supported" not in output or
                            f"[rt] lighting level {args.rt_level}" not in output):
        print(f"GAMEPLAY_SMOKE_FAILED Metal tracing was not activated; see {log_path}")
        return 1
    if args.capture_gameplay and (f"[record] started: {args.capture_seconds} seconds" not in output or
                                  "[record] finished:" not in output):
        print(f"GAMEPLAY_SMOKE_FAILED frame recording did not finish; see {log_path}")
        return 1
    if args.capture_gameplay:
        # The room-pieces hook does not report every playable interior, so
        # decoded, visible frames are the capture gate. Review the picture
        # manually for free movement and the intended location.
        finished = re.search(r"\[record\] finished: (\d+) frames, \d+ stalls, in (.+)", output)
        recordings = (set(Path(finished.group(2)).glob("*.frames.csv")) - known_recordings
                      if finished else set())
        if len(recordings) != 1:
            print(f"GAMEPLAY_SMOKE_FAILED could not identify the new recording; see {log_path}")
            return 1
        csv = recordings.pop()
        frame_count = int(finished.group(1))
        stem = csv.name.removesuffix(".frames.csv")
        samples = (1, (frame_count + 1) // 2, frame_count)
        visible = sum(is_visible_png(csv.with_name(f"{stem}.f{number}.png")) for number in samples)
        if visible < 2:
            print(f"GAMEPLAY_SMOKE_FAILED black capture ({visible}/3 visible samples); see {csv}")
            return 1
    print(f"GAMEPLAY_SMOKE_OK peak_rss_mib={peak / 1024:.1f} log={log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
