#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// The frame recorder: every presented frame written as its own PNG, for as long as was asked for.
//
// WHY IT EXISTS, IN THE USER'S OWN WORDS (2026-09-22): "this gives you the ability to see exact
// issues rather than me just describing, and also have the ability to see how many frames the
// issue is affecting play." Both halves matter, and the second is the one that shapes the design.
// A recording that is merely watchable would be a video; a recording you can COUNT has to be one
// file per frame, in order, with nothing missing.
//
// SO NOTHING IS EVER DROPPED. That is the single rule this module is built around. If the encoders
// fall behind, the present thread WAITS rather than discarding a frame, and every wait is counted
// and reported in the recording's own notes file. A dropped frame would silently turn "the hitch
// lasted nine frames" into "the hitch lasted seven", which is worse than no recording at all
// because it looks like an answer. The stall is visible; a drop would not be.
//
// WHAT IT COSTS, stated plainly because it is not free and the person using it should know:
//   - one memory copy of the frame on the present thread, about a millisecond at 1080p
//   - a pool of encoder threads, one fewer than the machine has, running flat out while recording
//   - a lot of disk. A minute at sixty frames a second is 3600 files.
// Recording therefore perturbs what it is recording, a little. The notes file says by how much.
//
// THE FILES ARE RESTRICTED. A frame of this game is a picture of its assets and carries the same
// status as the ROM (.gitignore's captures section, .scaffold/security/pre-app.md). They are
// written to the user's own Pictures folder, which is outside this repository and cannot be
// committed from it by accident. Never attach one to anything that leaves the machine.
namespace oot::recorder {

    // The settings row's index turned into seconds: five seconds to a minute, in fives, which is
    // what was asked for. Index 0 is five seconds, index 11 a minute.
    //
    // THE LABELS ARE NOT HERE. They live in the settings row table (ui_settings.cpp), which is the
    // one place every row's options are written down, and duplicating them would be two lists that
    // have to agree. This is the arithmetic only.
    int length_seconds(int index);

    enum class State {
        Idle,
        Recording,  // frames are being taken
        Draining,   // the time is up and the encoders are finishing what is queued
    };

    struct Status {
        State state = State::Idle;
        int seconds_total = 0;      // what was asked for
        double seconds_elapsed = 0; // how far in
        int frames_taken = 0;       // handed to the encoders
        int frames_written = 0;     // actually on disk
        int queue_depth = 0;        // waiting to be encoded
        int stalls = 0;             // times the present thread had to wait for room
        bool failed = false;        // a write failed; the reason is in the log
    };

    // Safe from any thread. The overlay reads this every frame.
    Status status();

    // The overlay's click. Starts a recording of `seconds`, or ends one early if it is running.
    // Ending early keeps everything already captured.
    void toggle(int seconds);

    // A recording asked for on the command line (`--record <seconds>,<play ticks>`, phase 57):
    // starts once the game has run that many updates in play, which is how the harness records
    // a moment it cannot click for (a pause menu closing) without a pointer landing on the plate.
    // `tick` is called once a frame by the renderer's context with the play tick count and
    // starts the recording when the count is reached; it does nothing when nothing was asked.
    void request_at_play_ticks(int seconds, uint32_t ticks);
    void tick(uint32_t play_ticks);

    bool recording();

    // ------------------------------------------------------------------------------------------
    // The capture path. Render thread only, called from RT64's draw hook.
    // ------------------------------------------------------------------------------------------

    // Is a frame wanted right now? Cheap enough to call every frame; this is what decides whether
    // the hook pays for a readback at all.
    bool wants_frame();

    // Has the asked-for length run out? Separate from submit on purpose, and the separation is
    // what makes the last frame of a recording arrive.
    //
    // THE ORDER MATTERS AND IT IS NOT OBVIOUS. A readback copy is one frame behind the frame that
    // recorded it, so at any moment there is a captured frame in flight. If the recorder decided
    // to stop inside submit, the in-flight frame would arrive after the decision and be thrown
    // away, and the recording would quietly be one frame shorter than it says. So the hook hands
    // over the frame in flight FIRST, and only then asks whether the time is up.
    bool time_is_up();

    // End the recording; whatever is queued is still written. What the overlay's second click
    // calls, and what the hook calls when time_is_up says so.
    void stop();

    // One presented frame, as it came out of the swap chain: B, G, R, A per pixel, `pitch` bytes
    // per row (the copy alignment makes that wider than width * 4). Copied out here and queued;
    // the caller's memory is free the moment this returns.
    //
    // BLOCKS when the queue is full, which is the deliberate choice above. `presented_us` is the
    // time this frame reached the hook, so the notes file can report the real cadence rather than
    // the cadence the encoders happened to write at.
    void submit(const uint8_t* bgra, int width, int height, std::size_t pitch, uint64_t presented_us);

    // ONE FRAME, ON ITS OWN, as a PNG scaled to `width` pixels across: the thumbnail beside a
    // saved moment (phase 75). The next presented frame is taken whether or not a recording is
    // running, encoded on the render thread (a 320 pixel picture costs nothing worth a queue) and
    // written to `png`. Returns false if a snapshot is already waiting.
    //
    // THE GAME ALONE (2026-10-09, the Screenshots show row): with `game_alone`, the frame is the
    // game's picture without this program's interface, drawn again at the window's size through
    // the renderer's picture hook (ui_render.cpp) rather than read from the swap chain, so a shot
    // taken with the settings open is the scene alone.
    bool snapshot(const std::filesystem::path& png, int width, bool game_alone = false);
    // Whether the snapshot waiting wants the game alone (ui_render.cpp takes it, then hands it
    // to submit_snapshot, which writes it and clears the request).
    bool snapshot_wants_game_alone();
    void submit_snapshot(const uint8_t* bgra, int width, int height, std::size_t pitch);
    // Whether the snapshot asked for has not been taken yet (the sweep waits on it).
    bool snapshot_pending();

    // Stop, finish writing whatever is queued, and let the threads go. Called at shutdown; safe
    // when nothing has ever been recorded.
    void shutdown();

} // namespace oot::recorder
