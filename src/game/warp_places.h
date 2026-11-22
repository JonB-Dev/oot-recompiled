#pragma once

// WHERE A MOMENT WAS TAKEN, IN WORDS (the user, 2026-10-13: "we could actually put a user friendly
// location along with it: Fire Temple, Forest Temple, Inside Jabu-Jabu, Lake Hylia, Temple of Time,
// Mask Shop"). The warp catalog already names every place an entrance leads into
// (warp_table.ntsc-1.0.h, generated from the decompilation's entrance table), so an entrance index
// finds its place there, the same way the Warps page marks where the player stands.
namespace oot::warps {

    struct Named {
        const char* place = nullptr;    // "Lake Hylia", "Temple of Time"
        const char* region = nullptr;   // the region the catalog files it under
    };

    // The place an entrance leads into, or nulls when the catalog has none. The catalog lists each
    // group of the entrance table by its first row, and the game's index can be a later row of the
    // same group (the layer for the age and the time of day, four rows at most), so the nearest
    // listed row at or below it, within those four, is the same place.
    Named place_of(int entrance);

} // namespace oot::warps
