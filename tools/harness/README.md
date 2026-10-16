# Harness

Scripts that drive the game with synthetic input, capture what it drew, and read its state trace.
They found every bug the stability phases found; they live here so the next session does not have
to rebuild them from a temp directory. Windows PowerShell 5.1, nothing to install.

Every script starts the game itself with `--probe` (the health line and the display list
reporting) and writes under `build-cmake\harness\`, which is build output and never committed.
**Captures are pictures of the game and are never committed either.** The records describe what a
capture shows; they do not contain it.

| Script | What it proves | What it does not prove |
|---|---|---|
| `drive.ps1` | Nothing on its own. Dot-sourced by the others: window lookup by process id, SendInput presses, BitBlt captures, capture pairs, the settings file (`Use-Settings` and `Restore-Settings`, the one place a run changes it and puts it back), the window size (`Start-Game -width -height`), the intro sequence | |
| `stretch.ps1` | A list of entrances can each be reached, with a conversation cleared, the pause menu cycled and some movement, without a crash or a guard hit. The watchdog kills a run whose next entrance never produces a play frame and relaunches with the rest. Every listed entrance that appears in the trace counts as reached, even one Link left within a poll (the first exercise roll at the Market's spawn carries him through the loading zone), and a timeout is blamed on the entrance still unreached, with the healthy scene the game sat in named for context | That the game can be PLAYED through those scenes. Nothing here fights, solves or opens anything |
| `pausecycle.ps1` | The PHASE-26 race stays fixed: forty pause cycles in a heavy scene with no guard hit. Also the regression check after any change to a display list or the pause path. With `-CaptureCycles N`, capture pairs through the first N opens and closes, which is where a camera smear on the pause transition would show; `-TurnPages M` turns the pages M times with R in each captured cycle, eight pairs through each turn, which is where a tagged cube of pages shows an intermediate angle (phase 45); `-TurnBurst N` takes a burst of N grabs through each turn instead, counted by `..\burst_count.py`, because the menu runs at thirty game frames a second and a pair is a weak witness there | Anything about scenes it does not visit. Nor which frame rate it ran at: `settings.txt` has no frame rate line unless a run or the settings screen wrote one, so pass `-Settings "framerate=1"` and read the trace's `[gfx] refresh rate` line, which says what the renderer was told |
| `bindcheck.ps1` | The bindings file (`controls.txt` beside the settings file) is read and obeyed: Start is bound to P, which nothing uses by default, and pressing P after the arrival opens the pause menu (a `[seg] segment 4` move in the trace). Return stays bound too so `Enter-Play` still works. With `-Device virtual` the same proof goes through the device layer: SDL's virtual joystick is attached with `--virtual-controller <script>`, the script presses its Start button twice at fixed times from inside the process, and the profile keyed `virtual` binds that button to Start. `-Raw` attaches the virtual pad as a joystick SDL has no mapping for (`--virtual-raw`), the way a custom Bluetooth pad arrives, and binds Start by raw index only. `-Capture` binds nothing beforehand: the script arms a capture for Start on the pad, presses its button, and the game itself writes the captured binding into the file before the next presses open the menu, which proves bind by pressing. `-ThroughDocument` binds nothing beforehand either and does it all through the controls document: F3 opens it, four steps down reach the Start row, Enter listens, P is bound and written by the game, Escape closes the document, and P then opens the pause menu, which proves the document end to end. The file and the script are put back or removed afterward | That a real pad feels right, or which of its buttons SDL calls what; the trace's `[input] controller added` line names the pad, its GUID and where its mapping came from (built in, database, virtual, raw), which is the first thing to read when a pad misbehaves |
| `launcher-shot.ps1` | The launcher (play milestone): started with `-NoPlay` (no `--play`), the program shows the launch document over the runtime's empty frames with the ROM's state, every settings row, Controls, Play and Quit; the script captures it, opens About with F2 and captures its signature block and changelog (then scrolled three steps), walks to Play, presses Enter and captures the game starting; a second launch opens the ROM browser from the ROM row and captures it, moved and one level up. `-NoRom` moves the ROM files beside the executable into a folder for the run (and back), so the launcher is seen asking for one. The trace's `[launch]` lines say what was found and what Play did; `[ui] screen` lines say which document was up | That the browser reaches every drive, or that a dropped file arrives (SDL's drop event needs a real drag); the trace's `[launch] rom accepted` line is the proof when one does |
| `dblclick.ps1` | The one start every other script skips: the program started with NO standard handles, as a double click starts it, in its own folder with no arguments. Expects a window and a log beside the executable whose first line is the build line (stdout) and which carries a `[launch]` line (stderr), so both streams reach one file. Then five starts with `--crash-test <kind>` (invalid-parameter, terminate, abort, purecall, access-violation), each expected to leave a report with a stack in the log, and the invalid parameter one to go on and exit 0. Exists because 0.1.0's double click died before its window with an empty log, a failure the redirected starts could never see | Anything after the launcher appears; the other scripts take it from there |
| `hint-shot.ps1` | The startup hint (phase 50) shows over the game when the application starts, names the key that opens the menu (and the first pad's menu button with `-Pad`, which attaches the virtual pad), and goes on the first press or, in a second launch that presses nothing, on its own by seven seconds. Four captures under `harness\hint\`; the trace's `[ui] hint:` line is the text and `[ui] hint gone` the moment it went | What the hint looks like; that is the capture's to show and a person's to judge |
| `cutscene.ps1` | Capture pairs through a scene's arrival cutscene, where the camera cuts between fixed angles: every capture should be one viewpoint or the other, never a view from between them. The same steady pairs from the warp onward are the wide-frame survey of the 2D layer (the fade, the letterbox bars, a title card, a text box, a transition all happen in an arrival's first seconds): `-Settings "aspect=1,hud=2" -Width 1280 -Height 720`, and `-ExtraArgs @('--transition', '0x20')` makes the warp use a given transition | Judged by eye; nothing here can tell a sweep from a cut on its own |
| `motion.ps1` with `..\motion_check.py` | Whether anything is drawn BETWEEN game frames: capture pairs a few milliseconds apart differ when transforms interpolate and are identical when they do not. `-Mode walk` (the camera follows Link), `look` (a first person pan, the camera phases), `forward` (Link walks away from the camera), `still` (no input: only actors that animate on their own can change a pair, for the actor phases), `roll`; `-LeadSeconds` walks Link forward first so the capture starts where something is in view; `-PressA N` and `-Nudge` dismiss a greeting and move a shop's cursor on to an item before the pairs | Whether the interpolation looks RIGHT. That is a person's judgment, and the plan pauses for it. Nor can a pair tell a TAGGED actor from one the renderer matched on its own, which it does for most static scenes; the trace's `[dl] actor groups` and `[registry] lookups` lines say whether the tags are there |
| `settings-shot.ps1` | The settings document opens over the game, shows its rows, and the file round-trips (it prints the file as the game wrote it back and any `[gfx]` or `[ui]` line the game logged). With `-Open controls` it goes on through the settings document's own Controls row into the controls document (phase 50), captures it, moves the focus down the list and to the second slot, captures again, and closes it with Escape | That a setting has the intended effect on the picture; the other scripts do that |
| `..\wide_survey.py` over a capture directory | What the 2D layer does at the sides of a wide frame, as numbers: the mean luminance of the left margin, the center and the right margin of each capture, whole height and the top and bottom bands. A fade that covers the frame darkens all three together; one that stops at the 4:3 edge leaves the margins where they were; letterbox bars are a band black across all three. Phase 43's record is made of these | Anything about what a capture shows inside the frame |
| `..\hud_survey.py` over captures | Where the interface's elements sit, as numbers: the bounding box of the hearts, the B, A and C buttons and the minimap, found by color in their own quarter of the frame. At the console's ratio in a 1280 by 720 frame the hearts start near column 240 and the buttons end near 1050; anchored to the edges, near 80 and 1200. A missing box is an element that is not there, which is phase 44's count | The rupee counter, whose green is the grass's, and anything a color cannot tell apart |
| `burst.ps1` with `..\burst_count.py` | What rate the CAPTURE sees: a burst of back to back screen copies is counted for distinct pictures, about ten of twelve at a sixty hertz picture and about four at twenty. Run it before trusting a motion result, because a capture that only sees the game's rate cannot see interpolation, and its verdict would look exactly like a broken tag | Anything about what the pictures show |

## Reading a result

The summary each script prints is only as good as the trace it summarizes. When the two disagree,
the trace (`build-cmake\harness\<tag>.stderr.txt`) is right; two of the harness's own defects were
caught exactly that way. The state lines look like:

```
[state] entrance 0x00EE  age 1  mode 0  saved_scene 52
[seg] segment 4 moved from 0x279740 to 0x27CF40
[dl] IMPLAUSIBLE NESTED LIST: ...
```

`mode 0` is ordinary play. A `[seg] segment 4` move after a warp is the pause menu opening. A
`[dl] IMPLAUSIBLE` line is the guard catching a command that would have crashed the renderer, and
in a healthy build there are none. `[cam]` lines carry the camera verdict with the eye and the
sun's offset from it (about 3000 units; (0, 0, 0) means the constants a patch reads are not in
memory, which happened once); the sun offset is nonsense on the pause menu's own view. `[env]`
lines appear only when the sun's depth test coordinates jump or leave the screen by far, and a
`WILD` one is a crash about to happen in the environment's graph callback.

## Flags the game accepts for these scripts

`--play` (straight into the game, past the launcher; `Start-Game` passes it always, so every
script here sees the game boot on its own, and `launcher-shot.ps1` is the one that leaves it
out), `--probe <path>`, `--warp <comma separated entrance indices>`, `--dwell <game frames
between queued warps>`, `--transition <type>` (the transition the warps use, as the game numbers
them: 0 the wipe, 1 the triangles, 2 the fade to black, 0x20 the circle), `--crash-test <kind>`
(one ending made to happen at the start of main, for `dblclick.ps1`; the kinds are listed
there). All absent from a normal run, which starts at the launcher.

## Things learned the hard way

- PowerShell variable names are case insensitive: a parameter `$Exe` and a local `$exe` are one
  variable. A run once launched the wrong build because of it.
- `-like` treats `[state]` as a character class. Use `.Contains()` for trace lines.
- Entrance indices in the trace are four hex digits (`0x00EE`); normalize before comparing.
- A conversation blocks the pause menu. Press A first.
- The game samples input once per frame; a key held for less than a frame was never pressed.
- The link fails with `permission denied` if the game is still running. Every script kills it on
  exit, and `Start-Game` kills any leftover first.
- The native build runs through `tools\build_native.cmd`, never a bare `cmake --build`. From a
  shell without the Build Tools environment, clang-cl picks the newest MSVC toolset on the
  machine, whose standard library refuses it (`STL1000: expected Clang 20 or newer`). A build
  that only relinks succeeds from anywhere, so the mistake shows up one C++ change later.
- At the display's rate the renderer re-dithers every presented frame: two captures a display
  frame apart differ by a few levels in most pixels even when nothing moved. `motion_check.py`
  counts a pixel only above forty levels and a pair only above half a percent for that reason;
  lower the thresholds and every pair "changes".
- The presses go to whichever window has the keyboard. A person typing in another window while
  a run is up takes the focus with every keystroke and the presses land in their editor, so the
  game looks broken when nothing about it is: a bindcheck bound its key, wrote the file, and
  then "never opened the menu", because the user was typing at that moment. `Focus-Game` now
  waits up to ten seconds for the focus to come back and prints `focus lost` when it had to,
  which is the first line to look for when a run fails for no reason the trace can show. A
  run that fails while somebody was typing is rerun before it is diagnosed.
- Every script restores `settings.txt` in a `finally` block, and a script that is KILLED never
  reaches it. After stopping a run from outside, check the file: the last stopped chain left
  `framerate = 1` in it.
- A warp's own transition is over before a capture window that opens after `Enter-Play` can
  see it. To capture a transition, warp twice (`-Entrance "0x0C1,0x0CD"` with `--dwell`) so the
  second one falls inside the window, and start from a scene without an arrival cutscene: a
  cutscene runs the game's logic at sixty frames a second, so a dwell counted in game frames
  passes three times faster there than in play.
