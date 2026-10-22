#pragma once

#include <cstdint>
#include <vector>

// The bodies near the eye that the dust collides with (2026-10-08; patches/air_movers.c fills the
// table, renderer patch 0042 keeps the motes out of them). Each is an actor's own collision
// cylinder, standing or moving.
//
// The patch publishes the table's address once per update; this side reads it once a frame, with
// the address and every count checked, and turns the game's fixed point into world units.
namespace oot::air_movers {

    struct Mover {
        float x, y, z;         // the cylinder's foot, world units
        float radius, height;  // world units
        float dx, dy, dz;      // world units moved in the game's last update (zero when it stood still)
    };

    // The table's address in the game's memory, or 0 for none.
    void bind(uint32_t address);

    // The bodies as the table holds them now; empty when nothing is bound or it cannot be read.
    void read(const uint8_t* rdram, std::vector<Mover>& out);

} // namespace oot::air_movers
