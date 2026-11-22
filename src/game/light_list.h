// The game's light list, read out of its memory for the ray traced local lights (phase 55).
//
// The game keeps every point light of the scene (a torch, a candle, a fire, a glowing thing) in
// a list on its play state, but hands the microcode only a directional stand-in per actor, so
// the renderer cannot see where a light is from the display list (patches/light_list.c says why
// at length). A patch publishes the list's address when a scene makes it and withdraws it when
// the scene ends; this reads the list once a frame, treating the game's memory as hostile input:
// every pointer is range checked and aligned, the walk is bounded by the game's own pool size,
// and a bad link ends the walk rather than the program.
#pragma once

#include <cstdint>
#include <vector>

namespace oot::light_list {

    struct Light {
        float x, y, z;         // the game's world units
        float r, g, b;         // 0 to 1
        float radius;          // where the game's own falloff (1 - (d / radius)^2) reaches zero
        bool glow;             // the game draws a glow sprite at it (a flame)
    };

    // The LightContext's address in the game's memory, or 0 for none. Called from the game
    // thread by the patch through recomp_light_context.
    void bind_context(uint32_t address);

    // Fills `out` with the list's point lights of positive radius, in list order (newest
    // first, as the game inserts at the head). Empty when no context is bound or the list is
    // unreadable. Safe to call from any thread once the game's memory exists.
    void read(const uint8_t* rdram, std::vector<Light>& out);

    // The environment's live light settings (play->envCtx.lightSettings): the ambient and the two
    // directional lights the game blends for the hour, which are the scene's own sun and fill.
    // Directions point toward the light, as the game gives them to the microcode, normalized.
    struct Environment {
        float ambient[3];
        float dir1[3];
        float color1[3];
        float dir2[3];
        float color2[3];
    };

    // The address of the live settings in the game's memory, or 0 for none. Published with the
    // light context by the same patch.
    void bind_environment(uint32_t address);

    // Reads the live settings; false when none is bound or the address is unreadable.
    bool read_environment(const uint8_t* rdram, Environment& out);

    // The address of play->envCtx.lightMode, published with the two above by the same patch. It
    // says whether the place follows the time of day, which is the game's own answer to whether
    // there is a sun here to cast shadows. The renderer used to guess it from the draw list.
    void bind_light_mode(uint32_t address);

    // True into `out` when the place follows the time of day (LIGHT_MODE_TIME). The return says
    // whether it could be read at all.
    bool read_sun_follows_time(const uint8_t* rdram, bool& out);

    // What the place's own surfaces give as light, published with the context by the same patch
    // (patches/light_list.c): bit 0, its lattice windows are daylight (renderer patch 0030).
    // Zero when the list is torn down.
    void bind_scene_lights(uint32_t flags);
    bool windows_give_light();

} // namespace oot::light_list
