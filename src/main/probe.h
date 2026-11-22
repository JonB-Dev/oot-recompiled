#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// A machine readable health line, written once a second to a file, so the program can be driven
// from outside and then CHECKED rather than watched.
//
// Why this exists. Phase 22 asks for "music plays, correct pitch and tempo, no crackling, no
// dropouts, no desync over several minutes". Those are ear judgments, and a session with no ears
// that ticks them off is lying. Every one of them has a measurable shadow, and this records the
// shadows: the device rate against the rate the game asked for, the queue depth over time, the
// number of times the queue ran dry, and a roughness figure over the samples themselves.
//
// It records STATISTICS, NEVER CONTENTS. Counts, depths, rates, and two summed magnitudes. Nothing
// it writes is a copy of anything out of the ROM, and nothing that would be. Keep it that way:
// dumping a buffer here would put game audio on disk, which is the thing security/pre-app.md is
// about, and it would do it in a file somebody would happily attach to a bug report.
//
// Off unless `--probe <path>` is passed. It costs nothing when off and one small write a second
// when on.
namespace oot::probe {

    // Returns false and says why on stderr if the file cannot be opened.
    bool enable(const std::string& path);
    bool enabled();

    // The rate the audio device was actually opened at. SDL is asked for an exact match, so a
    // disagreement here is a pitch error of exactly this ratio and nothing subtler.
    void note_device_rate(uint32_t requested_hz, uint32_t opened_hz);

    // Every buffer the game hands us, before it goes to the device.
    void note_audio_buffer(const int16_t* samples, size_t sample_count);

    // Every time the runtime asks how much is still queued.
    //
    // A SINGLE zero here is not a dropout, and reading it as one was this file's first mistake.
    // SDL's queue count drops frames as soon as its audio thread pulls a chunk into the device
    // buffer, so a poll that lands just after a pull reads zero while the device is perfectly
    // fed. What separates starvation from pull timing is a RUN of zeros: the device buffer is
    // one chunk long, so several consecutive empty polls means nothing arrived in time to
    // replace it. The run length is what the probe records.
    void note_queue_depth(size_t frames_remaining);

    // ------------------------------------------------------------------------------------------
    // Where a frame's time went, added 2026-09-20 for the stutter reports.
    // ------------------------------------------------------------------------------------------
    //
    // WHY A SPAN AND NOT A FRAME RATE. A frame rate counter says a frame was lost; it cannot say
    // which of three threads lost it, and the three have completely different fixes. So the
    // graphics thread times its own two pieces of work and the gap between them:
    //
    //   Span::DisplayList  the renderer walking and submitting one game frame's display list.
    //                      A pipeline compiled on first sight of a material lands here.
    //   Span::Screen       the present. Waiting on the display's refresh lands here, so this one
    //                      is EXPECTED to be long and is only interesting when it is much longer.
    //   Span::Submit       the gap between one display list arriving and the next. This is the
    //                      GAME thread's period as the graphics thread sees it: a spike here with
    //                      no spike in the other two means the game was late, not the renderer.
    //
    // Recorded as a band per line (count, mean, max) so a run of several minutes is a few dozen
    // lines, and each span over the hitch threshold ALSO writes one `[hitch]` line to stderr
    // immediately, because a mean hides exactly the thing being hunted.
    //   Span::Present      the gap between one PRESENTED frame and the next, taken in the render
    //                      hook, which RT64 calls once for every frame that reaches the display,
    //                      interpolated ones included. This is the picture's own cadence and the
    //                      only span here that a person can see directly: at the display's rate
    //                      its mean is the refresh interval, and a single gap of two intervals
    //                      IS the "I lose a frame or two" both reports describe. None of the
    //                      other three can show it, because the work they measure all happens on
    //                      other threads and finishes on time while the picture still stutters.
    //   Span::Interface    how long OUR render hook took, which is the one piece of this
    //                      program's own code that runs on the present thread. Everything the
    //                      interface does happens there, RmlUi's whole update and render
    //                      included, and it happens for every presented frame whether or not
    //                      anything is on screen. A spike here delays a present by exactly that
    //                      much, so it is a stutter this program caused rather than one it
    //                      inherited. Span::Present cannot tell the two apart; this can.
    enum class Span {
        DisplayList = 0,
        Screen = 1,
        Submit = 2,
        Present = 3,
        Interface = 4,
        Count = 5,
    };

    void note_span(Span which, uint64_t microseconds);

    // Once per VI. Writes a line when a second's worth have gone by.
    void tick();

    void close();

} // namespace oot::probe
