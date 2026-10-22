#pragma once

#include <cstdint>

// How full the scene is against the game's own limit of 200 actors, and where the Sun's Song
// stands (2026-10-12; patches/actor_note.c sends them once per update). Written to the log only
// when something worth reading changes, so one play of the song says whether its effect was
// refused for want of room.
namespace oot::actor_room {

    // total: the scene's actors; largest_free: the largest block left in the game's actor memory,
    // in bytes; song: the song's state as patches/actor_note.c packs it; heap_size: that memory's
    // whole size, which says whether the bigger heap is in effect (2026-10-21); hookshot: the
    // hookshot guard's hooks put back, and 0x10000 while one is still missing (patches/actor_heap.c).
    void note(int32_t total, int32_t largest_free, int32_t song, int32_t heap_size, int32_t hookshot);

} // namespace oot::actor_room
