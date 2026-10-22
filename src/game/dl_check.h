#pragma once

#include <cstdint>

#include "game/game_state.h"

// Walk a display list before the renderer does, and name any address in it that would resolve
// outside the memory the runtime maps.
//
// WHY THIS EXISTS. A faulting address tells you where a read went. The register dump tells you
// which pointer was already computed. Neither tells you WHICH COMMAND put the bad address there,
// and that is the only thing that identifies the game code responsible. This is the same move as
// printing the emulated RAM base, one level further in: stop inferring, read it.
//
// It found the command, and the command led to the cause: the pause menu overwriting gameplay_keep
// while the renderer still had a list that drew from it. That is fixed where it belongs, in
// patches/fixes/pause_object_race.c. The walk stays on as a guard (below), and the reporting stays
// behind the probe flag, because the walk is a few thousand comparisons a frame and the report is
// not.
namespace oot::dl_check {

    // Walk the list and NEUTRALIZE any command that would send the renderer off the end of
    // memory, returning how many were found.
    //
    // THIS IS A GUARD, NOT JUST A CHECK, and it is deliberately always on.
    //
    // A display list that references an object the game has since moved contains a G_DL whose
    // target is not a display list at all. The renderer has no iteration limit and no plausibility
    // check: it follows that pointer, reads whatever is there as commands, never meets a G_ENDDL,
    // and advances until it leaves the memory the runtime maps. That is a hard crash.
    //
    // Replacing such a command with a no-op costs one object going undrawn for one frame. That is
    // a far better outcome than the program dying, and it is done HERE rather than in the renderer
    // because the renderer is vendored and because this is our memory model's problem: a console
    // would have faulted on the bad pointer immediately instead of walking 260MB first.
    //
    // It does not fix the cause. The list should not have contained that pointer, and why it does
    // is recorded in .scaffold/execution/issues.md under PHASE-26.
    int scan(uint8_t* rdram, uint32_t physical_start);

    // Off by default. Turned on alongside the probe, because both exist for hunting and
    // neither should cost anything in an ordinary run.
    void enable(bool on);
    bool enabled();

    // Record every graphics task handed to the renderer, and print the last few when the
    // program dies. ALWAYS ON, unlike the walk: it is three stores per frame, and the one
    // time it is needed is a crash nobody was expecting.
    //
    // Addresses only, never contents, which keeps the rule the crash handler was built
    // around. What it answers is the question the faulting address cannot: what was the
    // renderer actually given, just before it went wrong.
    void note_task(uint32_t ucode, uint32_t data_ptr, uint32_t data_size);

    // A cheap checksum over the head of a display list. Taken before and after the
    // renderer walks it: if it changes underneath, something is writing while it reads,
    // which would explain both the intermittency and why every static check of the list
    // comes back clean. Proving a race is worth more than another clean scan.
    uint32_t checksum(const uint8_t* rdram, uint32_t physical_start);
    void note_mismatch(uint32_t before, uint32_t after);

    // The segment table either side of the renderer's walk. A change means the game moved
    // its object bank while the renderer was still reading a list that refers to it, which
    // is the level the checksum above does not reach: the list's bytes stay put, the memory
    // they point at does not.
    void note_segment_move(const oot::game_state::Snapshot& before,
                           const oot::game_state::Snapshot& after);
    void report_recent_tasks();

} // namespace oot::dl_check
