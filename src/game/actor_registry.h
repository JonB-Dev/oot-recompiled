#pragma once

#include <cstdint>

// Stable identity for the game's actors, so the renderer can interpolate their transforms.
//
// WHY THIS EXISTS. RT64 interpolates a transform group by ID between two consecutive frames. The
// obvious ID for an actor is its address, and it is wrong: the game frees actors and allocates new
// ones at the same address constantly, so a new actor would inherit a dead one's last transform and
// fly in from wherever that one was. The reference project solved this with a spawn index kept in
// native actor extension data. This is that index and nothing else.
//
// THE CONTRACT, which is also the security posture (.scaffold/post-parity/security/app-layer.md):
//   - keyed by an EMULATED address that is validated and never dereferenced
//   - bounded: a fixed table, FULL reported once, then index 0 which the patches treat as untagged
//   - reset at every scene start, so a missed release costs one slot until the next scene
//   - game thread only, like recomp_take_warp; no lock, and none pretended
//   - counters and names in the log, never memory contents
namespace oot::actor_registry {

    // Give a freshly initialized actor its index. Returns 0 when the address is implausible or
    // the table is full; a real index is always 1 or more.
    uint32_t register_actor(uint32_t actor_address);

    // The index a live actor was given, or 0. Asked every time an actor is drawn, so it is a hash
    // lookup rather than a search.
    uint32_t lookup(uint32_t actor_address);

    // The actor is being deleted. A miss is counted, not fatal: the game can delete an actor it
    // never fully initialized.
    void release_actor(uint32_t actor_address);

    // Scene teardown. Prints one line of counters and clears the table.
    void reset();

    // For the self test and the harness: how many are live right now, and the most that were.
    uint32_t live_count();
    uint32_t high_water();

    // --registry-selftest: fill past capacity, expect exactly one FULL report, reset, and return
    // 0 on success. Exercised without the game running.
    int self_test();

} // namespace oot::actor_registry
