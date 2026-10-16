#pragma once

#include <cstdint>

// How full the scene is against the game's own limit of 200 actors, and where the Sun's Song
// stands (2026-10-12; patches/actor_note.c sends them once per update). Written to the log only
// when something worth reading changes, so one play of the song says whether its effect was
// refused for want of room.
namespace oot::actor_room {

    // total: the scene's actors; largest_free: the largest block left in the game's actor memory,
    // in bytes; song: the song's state as patches/actor_note.c packs it.
    void note(int32_t total, int32_t largest_free, int32_t song);

} // namespace oot::actor_room
