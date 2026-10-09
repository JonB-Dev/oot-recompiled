#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// THE VIDEO RECORDER (the user, 2026-10-09): gameplay recorded to one MP4, H.264 picture and AAC
// sound, "so that the files are readily available to upload to places like YouTube or Vimeo",
// with "the ability to control quality via a setting" and "choose the output resolution", a
// setting "to enable or disable sound so that way it doesn't have to waste the time recording
// the sound if it doesn't need it", and "a pause and resume button be an assignable key and a
// start and stop button be a separate assignable key", so a montage is made in one file without
// editing. Not the frame recorder (main/recorder.h), which writes one PNG per frame to count them.
//
// THE PICTURE is the one the window shows, cropped, gamma corrected and fitted the same way, with
// nothing of this program's own interface in it: the renderer draws its last pass a second time
// into a target of the recording's size (renderer patch 0053). That size is the Video size row's
// height at the window's shape, so it can be LARGER than the window ("use 4K for the output of the
// recording while still allowing me to play it at my resolution"); the detail in it is what the
// Resolution row renders, so a sharp 4K video wants Resolution at 4K too. Fixed for the length of
// one recording, from the window's shape when it starts.
//
// OR THE WHOLE WINDOW (the Video shows row; the user, 2026-10-09: "allow recording gameplay only,
// or whether it also allows recording the menu ... recording the window as a whole"): the swap
// chain once this program's interface is drawn over the game, menus, toasts and plate included, so
// photo mode or the settings can be recorded in use. That picture is the window's size, and the
// encoder scales it to the recording's (the Video size row still decides it).
//
// SIXTY FRAMES A SECOND, CONSTANT, whatever the game or the display runs at: each presented frame
// lands on the sixty a second grid by its time, and a slot with no new frame (the game drawing
// twenty a second, or the encoder behind) repeats the one before, so the file plays and edits
// like any other video. Time spent paused is in neither the picture nor the sound.
//
// THE SOUND is the game's, before the Volume row, resampled to 48 kHz, kept in step with the
// picture by the same clock.
//
// A RECORDING THAT NEVER FINISHES STILL PLAYS (the user, 2026-10-09: "it would be really unfortunate
// for some users to go through a recording and then lose all of it if it crashes"). It is written
// as a fragmented MP4, complete every couple of seconds, so a crash or a forced close leaves a file
// that plays up to its last moments. The stop rewrites it into an ordinary MP4, the samples copied
// as they are, which every editor reads; if that fails the fragmented file stays. One file per
// recording either way, pauses inside it. That is the Video mode row's Reliable, the default; its
// Compatible (the user, the same day: "a compatibility mode versus a reliability mode") writes the
// ordinary MP4 from the start, as the recorder first did, and loses the video to a crash.
//
// THE FILES go to the person's own Videos folder (places::videos), named by the time they started.
// A frame of this game is Restricted the way the ROM is (.scaffold/security/pre-app.md): never
// attach one to anything that leaves this machine from inside the repository.
namespace oot::video {

    enum class State {
        Idle,
        Starting,   // asked for; begins at the next presented frame, which is when its size is known
        Recording,
        Paused,
        Saving,     // stopped; the encoder is finishing the file
    };

    struct Status {
        State state = State::Idle;
        double seconds = 0;   // of video so far, pauses excluded
        bool failed = false;  // the last recording could not be written; the log says why
    };

    // Safe from any thread.
    Status status();

    // The Record video binding: start, or stop and save. Safe from any thread.
    void toggle_record();
    // The Pause video binding: pause or resume; nothing when no recording is running.
    void toggle_pause();

    // Whether presented frames are wanted (Starting or Recording). Present thread, every frame.
    bool wants_pictures();

    // Present thread: whether THIS presented frame is to be recorded, at what size, and at which
    // place on the sixty a second grid. The first call of a recording fixes its size from the
    // window's and starts its clock.
    bool wants_picture(uint32_t window_width, uint32_t window_height, uint32_t& width, uint32_t& height,
                       uint64_t& index);

    // Where this recording's pictures come from (the Video shows row, fixed when it starts): the
    // whole window, the swap chain after the program's interface is drawn over the game, or the
    // game's picture alone, drawn again at the recording's size (renderer patch 0053).
    bool records_window();

    // A recorded picture: B, G, R, A, `pitch` bytes a row, `width` by `height` (the recording's
    // size, or the window's to be scaled to it), in memory that stays readable until `done` is
    // called. `done` is called exactly once, from the encoder's thread or from here, whether or not
    // the picture was used.
    void submit_picture(const uint8_t* bgra, uint32_t pitch, uint32_t width, uint32_t height, uint64_t index,
                        std::function<void()> done);

    // The game's sound as it leaves for the speakers: interleaved stereo, `frames` frames at
    // `rate`. Kept only while a recording with sound is running; cheap otherwise.
    bool wants_audio();
    void submit_audio(const int16_t* stereo, std::size_t frames, uint32_t rate);

    // The microphone's sound (main/microphone.h), from SDL's capture thread: 48 kHz stereo, mixed
    // with the game's. Kept only while a recording with the Microphone row On is running.
    void submit_mic(const int16_t* stereo, std::size_t frames);

    // A recording asked for on the command line (`--video <start>,<stop>` or
    // `--video <start>,<pause>,<resume>,<stop>`, in game updates in play): the harness's way to
    // record, pause and stop without a key. `tick` is called once a frame by the renderer's
    // context with the play tick count; it does nothing when nothing was asked.
    void request_at_play_ticks(const uint32_t* ticks, int count);
    void tick(uint32_t play_ticks);

    // The renderer is going away: a recording is stopped and saved.
    void renderer_gone();

    // Stop, finish the file, and let the thread go. At shutdown; safe when nothing ever recorded.
    void shutdown();

} // namespace oot::video
