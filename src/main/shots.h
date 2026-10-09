// Deterministic captures for the harness (phase 52, .scaffold/upgrades/lighting-design.md §9).
//
// WHY. The lighting phases prove themselves by comparing captures: Off against a baseline must
// be byte identical, a pass against Off must differ where the pass draws. The first captures
// (phase 51) were not comparable at all: the game seeds its random numbers from the clock at
// every scene load, so two runs spawn the same scene differently, and the harness took its
// picture after a wall clock wait, so the animation phase differed too. Two runs of the same
// build differed by up to five percent of the pixels, which is more than a pass would change.
//
// TWO CONTROLS FIX THAT, both harness flags, both inert in play:
//
// --seed <n>     the seed the game's Rand_Seed takes instead of the clock, every scene load, so
//                every actor's randomness runs the same from the same start (patches/harness_rand.c).
// --freeze <entrance>,<ticks>
//                once the game has arrived at that entrance and updated <ticks> more times, its
//                update stops: the patched Play_Main draws without updating, so every presented
//                frame from then on is the same frame and a capture taken at any later moment is
//                the capture. The harness waits for the "[shot] frozen" line rather than a clock.
//
// A frozen game still draws, presents, and answers its window, so the harness closes it the
// way a person does.
#pragma once

#include <cstdint>

namespace oot::shots {

    // --seed. 0 means none, and the game keeps seeding from its clock.
    void set_seed(uint32_t seed);
    uint32_t seed();

    // --freeze. The entrance the harness warped to and the updates to allow after arriving there.
    void set_freeze(int32_t entrance, uint32_t ticks);

    // True when --freeze was given at all, frozen yet or not. The renderer bridge reads it at
    // setup to turn the renderer's per frame dither noise off for the run, so two captures of
    // one frozen frame are the same bytes and not the same picture under different noise.
    bool freeze_requested();

    // True once the freeze has happened, or while the program holds the game (set_hold); the
    // patched Play_Main skips the update while it is.
    bool frozen();

    // THE SAME FREEZE, HELD BY THE PROGRAM (2026-10-09, the comparison shots, main/screenshots.h):
    // the game draws its frame over and over without updating while this is on, exactly as under
    // --freeze, and carries on from that frame when it is turned off. Inert unless asked for.
    void set_hold(bool on);

    // AND BY PHOTO MODE (2026-10-09, main/photo.h), a hold of its own, so a comparison taken
    // inside photo mode lets go of its hold and leaves photo mode's standing. While it is on, the
    // game's camera is set aside for photo mode's (patches/photo_mode.c) and its sound is paused
    // (patches/photo_audio.c).
    void set_photo(bool on);
    bool photo();

    // Called once per game update from the patched Play_Main with the entrance the game is in
    // and whether the scene is READY: its room loaded and every object it asked for loaded.
    // Ready is reported on the trace with the update it happened on, because the runtime loads
    // a scene from the ROM file asynchronously and an actor that waits on a load can spawn a
    // frame apart from run to run; the trace is how that is told apart from a renderer change.
    // --freeze-clock. The game's 16 bit clock to set on arrival, or -1 for the file's own. The
    // clock reaches the entrance carrying however many updates the file's scene ran before the
    // warp, which the harness cannot make the same twice, so it is pinned on arrival.
    void set_clock(int32_t day_time);

    // What the patched Play_Main does this update.
    enum class Tick : int32_t {
        Run = 0,        // the ordinary update
        Hold = 1,       // hold the update: a load is in flight in the scene to be frozen
        Arrive = 2,     // the first ready update at the entrance: normalize the running counters,
                        // then the ordinary update
    };

    // Called once per game update from the patched Play_Main. Arms only in ordinary play, only
    // after the harness's warp has been taken and a NEW scene has begun since (state_frames
    // dropping back), and only at the entrance asked for. While a load is in flight there the
    // update is held (Hold), so every actor comes to begin on the same update in every run; the
    // patched Play_Main then drives only the load completion the update would have. The first
    // ready update is Arrive; the freeze count runs over the ready updates from it. Never holds
    // the first update of a scene: the game draws nothing before that one has run.
    Tick on_play_tick(int32_t entrance, int32_t mode, bool ready, int32_t gameplay_frames, int32_t state_frames);

} // namespace oot::shots
