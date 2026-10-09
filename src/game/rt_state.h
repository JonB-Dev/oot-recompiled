// What the game's patches may ask about the ray traced lighting (upgrades phase 57).
//
// The program owns the lighting level (the launcher's row, the --rt-level flag) and the game
// side owns what it draws; a patch that hides the game's own painted shadows under the actors
// when the traced shadows stand (patches/actor_shadow.c) asks here, through the export
// recomp_traced_shadows, once per shadow drawn.
#pragma once

namespace oot::rt_state {

    // The lighting level the renderer runs at: 0 off, 1 shadows, 2 occlusion and local lights,
    // 3 bounce, reflections and the air. Set once after setup from the command line and the
    // settings; read from the game thread.
    // A DEVICE THAT CANNOT TRACE REPORTS 0 HERE whatever the menu says (2026-09-26). The renderer
    // already refused to switch the traced passes on without the capability; the game did not know,
    // so it hid its own painted shadows for traced ones that never came and the picture had no
    // shadows in it at all.
    int level();

    // Whether the device can trace at all, and whether this build carries the path. False on both
    // counts means the lighting menu's rows cannot do anything, which the interface says out loud
    // rather than leaving it to the log.
    bool supported();

    // The Shadow casters row of the lighting menu: how much of the world behind and beside the
    // camera the game keeps in the traced scene (0 its own culling, 1 near, 2 far, 3
    // everything). Read once a frame from the game thread (patches/culling.c).
    int caster_reach();

    // Whether the traced lights stand in for the game's light effects: true while the traced
    // local lights run (level 2 and above, the Light strength row above nothing), unless the
    // Experiment row's 37 asks for the old picture. The renderer then turns a drawn beam into a
    // light of its own (renderer patch 0029), and the game leaves out what faked one: the Temple
    // of Time's beam zone (patches/beam_zone.c) and the whole body stand-in for the lights of the
    // Sun's Song and the blue warp (patches/effect_lights.c).
    bool light_effects_traced();

    // Whether the windows' traced shafts stand in for the game's painted rays through the Temple
    // of Time's side windows (patches/window_rays.c, 2026-10-09): the traced light effects stand,
    // the Window shafts row is on, and the Debugging menu's Drawn beam row is not showing the
    // game's own again. Read once a frame from the game thread.
    bool window_rays_hidden();

    // The Debugging menu's Glow test row (2026-10-08): 0 and 1 draw each point light's glow inside
    // the renderer's depth test at the light's center (patches/light_glow.c), 2 and 3 draw it as
    // the game did. The renderer reads the same row for whether it also tests per pixel (renderer
    // patch 0050). Read once a frame from the game thread.
    int glow_test();
} // namespace oot::rt_state
