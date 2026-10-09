// The bridge between the runtime and this game's specifics.
//
// src/game is the only place that knows both, per structure.md. Everything this game has to tell
// the runtime about itself is declared here.

#pragma once

#include <cstddef>
#include <cstdint>

#include "librecomp/game.hpp"

namespace oot {

    // Register the recompiled code's sections so the runtime can relocate overlays on load.
    void register_overlays();

    // Hand the runtime the patch binary and the patch's code sections, so a patch's own data
    // exists in the emulated memory and a pointer into patch code can be resolved. Before
    // register_overlays, before the runtime starts.
    void register_patches();

    // Both counts are asserted at startup rather than assumed. A silently short table is the kind
    // of defect that surfaces as a crash in unrelated code much later.
    size_t registered_section_count();
    size_t registered_overlay_count();

    // This game, as the runtime understands it. Consumed unmodified and registered as an additional
    // entry, per the OPEN-2 decision in .scaffold/scratch/stack-decisions.md.
    const recomp::GameEntry& game_entry();

} // namespace oot

// The recompiled entry point, generated from the ROM. Its name comes from the symbol table: the
// context dump renames the ELF's `entrypoint` at 0x80000400 to this.
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
