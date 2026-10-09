// The lighting controls: the model behind the Lighting sub menu (2026-09-24, the user: "a sub
// menu for controls on all ambient occlusion, ray tracing, volumetric lighting, falloff controls
// and distance", "both to enable and disable, as well as refine and change levels of them",
// "control the radius", "how far the lights or shadows reach").
//
// THE RULE OF THIS MENU (the user, 2026-09-24: "anything that has multiple enablable options must
// have switchers"): a thing that can be on is its own Off/On row, never one choice among several.
// So the shadows, the occlusion, the local lights, the bounce, the reflections, the glow and the
// haze are each switched on their own row (the ones with a strength carry Off as their first
// step), and the renderer's level is DERIVED from what is on rather than chosen: the structures
// and the shadows are level 1, the occlusion and the lights level 2, the rest level 3. The one
// choice among several that remains is Inspect, which replaces the picture with ONE pass to look
// at, and is truly one at a time.
//
// The numbers each choice means are tables here, so the labels and the values cannot drift
// apart, and the renderer is handed the numbers (game/render.h, LightingValues). Kept in its own
// file beside the settings (lighting.txt), same grammar, same clamping, same tolerance of an
// unknown key.
#pragma once

#include <string>

#include "game/render.h"

namespace oot::ui::lighting {

    struct Lighting {
        int raytracing = 0;        // Off, On: the master switch
        int shadows = 1;           // Off, On
        int shadow_softness = 1;   // Sharp, Original, Soft, Softer
        // How many rays each lit pixel spends on the sun's disc: 1, 2, 4, 8 (a moving surface
        // takes twice). A soft edge is where the noise lives, and one ray across it is what
        // painted the windmill's turning shadow (the user, 2026-09-24). Two by default.
        //
        // EVERY ROW THAT COSTS FRAME RATE STARTS LOW (the user, 2026-10-08: "I want the defaults
        // to just be off, and I want the defaults to be like low settings, and then they can
        // increase from there", choosing "Low, still looks right" from three). The master switch
        // was already Off; these are what a person gets when they first turn it on: two shadow
        // rays (was four), the game's own casters (was Near), light occlusion from four rays (was
        // the original strength from eight), no bounce (was the original). Local lights stay on,
        // and nothing that only changes the look moved. A file that holds a row keeps its value.
        int shadow_rays = 1;       // the index: 0 is one ray, 1 is two
        // How much of the world behind and beside the camera stays in the traced scene so its
        // shadow reaches the picture (the user, 2026-09-24, Kakariko's windmill culled and its
        // shadow gone with it): Game's own, Near, Far, Everything. Near is the reach the day's
        // builds used; Everything keeps every room piece and every actor within its distance.
        // The game's own by default since 2026-10-08 (see shadow_rays).
        int casters = 0;
        // A LIGHT OF OUR OWN WHERE THE PLACE HAS NO SKY (the user, 2026-09-24, in the Deku
        // Tree: "i do not see shadow unless navi is out ... add a default yet not super bright
        // or strong skybox light in these arease", "it should act like a sun or moonlight but be
        // like half as intense", "i do NOT want it using the games original ugly simulated
        // one"). Off, then quarters of a full sun's darkening: how dark a figure's shadow goes
        // indoors. It adds no light and takes nothing off the ambient except where something is
        // actually in the way, because a version that moved the ambient turned the whole cave
        // dark instead of drawing one shadow. Half by default, which is about the fixed weight it
        // replaced.
        int indoor_light = 2;
        // THE DARKNESS (the user, 2026-09-24: "navi and torches would also cause the same floor
        // shadow like the sun or moon does ... realistic shadows not just glows"; "replacing
        // all lighting with skybox points would achieve this"). Off, Light, Deep: how much of
        // the game's own ambient is taken away where no traced light falls, so a room is dark
        // but for what its torches and a fairy reach, and what stands between a light and the
        // floor throws a shadow there. Off by default: it changes every dark place's look.
        int darkness = 0;
        // THE LIGHT BRIGHTNESS, Darkness's opposite (the user, 2026-10-08: "I would love a
        // setting that also amplifies how bright lights can light up a ground area ... right now
        // it just lightens it to like its default base brightness rather than adding any
        // additional light"). Normal, Bright, Brighter, Harsh: how far a traced light may lift a
        // surface above the game's own full light (renderer patch 0033). Normal is the sum as it
        // was, so it is the default.
        int light_brightness = 0;
        int occlusion = 1;         // Off, Light, Original, Strong, Full (Light by default since 2026-10-08)
        int occlusion_radius = 1;  // 20, 40, 80, 160 world units
        int occlusion_quality = 0; // 4, 8, 16 rays (4 by default since 2026-10-08)
        int lights = 1;            // Off, On: the game's own local lights, traced
        int light_reach = 8;       // twelve steps, 0 to 300 percent of the game's own radius; 8 is 100%
        int light_strength = 8;    // the same, on every local light's power
        int bounce = 0;            // Off, Light, Original, Strong, Full (Off by default since 2026-10-08)
        int bounce_range = 2;      // 500, 1500, 3000, 6000 world units; 3000 since 2026-09-24 ("bounce at original and 3000 units looks great")
        // OFF, AND THE DEFAULT SAYS SO (2026-10-08). The reflections were removed on 2026-09-30
        // and clamp() forces both to zero, but a first run has no file, so the defaults were used
        // unclamped: a new player who turned tracing on got the removed reflections until they
        // moved any lighting row. Found by the first run check of the low defaults.
        int reflections = 0;       // Off, Faint, Original, Strong, Mirror
        int reflection_range = 0;  // Half the view, The view, Twice the view
        int glow = 2;              // Off, Faint, Original, Strong, Thick: the glow's density
        // The glow's SIZE, apart from the reach and the strength (the user, 2026-09-24: "control
        // the reach and the actual radius visibility of a light and its opacity all that
        // independent"): Quarter, Half, Whole, Double of the game's own light radius. Half is
        // about what the reach's default gave before the two were separated.
        int glow_size = 1;
        // DUST IN THE LIGHT, indoors and out (the user, 2026-10-08: "really, really subtle
        // particles that float through light areas only"; "outside versus inside to have their
        // own dust settings"; "Full control over exactly how that interacts both indoors and
        // outdoors"). Renderer patches 0035 and 0036. Each place: how much dust (Off, Sparse,
        // Some, Thick) and how visible a lit mote is (Faint, Light, Visible, Bright). Indoors the
        // motes drift in place (Still, Slow, Lively); outdoors the wind carries them (Calm,
        // Breeze, Wind, Strong), with gusts (None, Light, Strong), from a direction (North, East,
        // South, West). The dust is Off by default in both, like every look that is not the
        // game's own, so the rest only matter once it is on.
        // DENSER AND EACH MOTE ITS OWN (the user, 2026-10-08: Thick "shows almost no particles
        // ... this thick setting should probably be a new "very sparse" setting"; "the particle
        // size would then control ... the median size"). The amount runs Off, Very sparse,
        // Sparse, Some, Thick, Very thick, and each place has a size (Tiny, Small, Medium, Large)
        // that each mote varies around (renderer patch 0038).
        int indoor_dust = 0;
        int indoor_dust_visibility = 1;
        int indoor_dust_size = 1;
        int indoor_dust_drift = 1;
        int outdoor_dust = 0;
        int outdoor_dust_visibility = 1;
        int outdoor_dust_size = 1;
        int wind = 1;
        int wind_gusts = 1;
        int wind_direction = 0;
        // THE AMBIENCE'S DISTANCE (the user, 2026-10-08: "a distance setting on how far out the
        // stuff renders ... a gradual fade out rather than a harsh cutoff"; then "indoor ambience
        // distance and outdoor ambience distance settings so that they can control
        // independently"). Near, Normal, Far, Very far: where the dust's long fade ends (renderer
        // patches 0037 and 0041), for every ambience to come as well.
        int indoor_ambience_distance = 1;
        int outdoor_ambience_distance = 1;
        int haze = 0;              // Off, Faint, Light, Thick
        int haze_range = 1;        // 800, 1400, 2500, 5000 world units
        int fog_fade = 1;          // Off, On
        int distance_fade = 0;     // Off, Far, Middle, Near
        int smoothing = 2;         // Off, Light, Original, Heavy
        int filter = 2;            // Off, One step, Two steps
        int inspect = 0;           // Off, then one pass to look at instead of the picture
        // Off, then a numbered variant of one disputed sum in the shaders, so the person can
        // cycle them in play and say which looks right (the user's idea, 2026-09-24). Not saved,
        // like Inspect. The variants are listed beside the row in ui_lighting.cpp.
        int experiment = 0;
        // THE WINDOWS' AIM (2026-10-08, renderer patch 0044): which window (0 every one), from its
        // own slope or the old beam's direction, degrees steeper or flatter and turned (15 is as
        // built), and its light moved along x, y and z (9 is no move). Debugging rows; written to
        // the file like the rest but put back as built at every launch, since the aim found for the
        // Temple of Time's tall window is built into the renderer (patch 0045).
        int window_aim = 0;
        int window_aim_from = 0;
        int window_tilt = 15;
        int window_turn = 15;
        int window_move_x = 9;
        int window_move_y = 9;
        int window_move_z = 9;
        // CANDIDATES TO CYCLE (2026-10-08, renderer patches 0049 and 0050): Light through stone
        // (0 stops at stone, 1 stops but the glow in the air passes, 2 the 48 units of 0.4.0) and
        // Glow test (0 by center, 1 by center with Navi's core too, 2 per pixel as 0.4.0, 3 never
        // as 0.3.9). Debugging rows, and saved, so a candidate survives a restart.
        // THE DEFAULTS ARE THE USER'S PICK (2026-10-08, from the test build): "'stops, glow passes'
        // and 'By center' fixes them without rgressing navi". The glow in the air stays unshadowed.
        int light_stone = 1;
        int glow_test = 0;
        // THE WINDOWS' SHAFTS (2026-10-09, renderer patch 0051): Window shafts (0 off, then Faint
        // to Bright), Shaft length (Short, Medium, Long, Very long), and the Debugging menu's
        // Drawn beam (0 hidden as in 0.4.0, 1 the game's painted beam shown again).
        int window_shafts = 0;
        int shaft_length = 1;
        int drawn_beam = 0;

        bool operator==(const Lighting& other) const;
        bool operator!=(const Lighting& other) const { return !(*this == other); }
    };

    // The table, the same shape as the settings rows: labels on screen, keys in the file.
    int row_count();
    const char* row_label(int row);
    const char* row_key(int row);
    // The row with this key, or -1: so a menu can place a row by name, not by number.
    int row_index(const char* key);
    int option_count(int row);
    const char* option_label(int row, int value);
    int get_row(const Lighting& l, int row);
    void set_row(Lighting& l, int row, int value);
    void clamp(Lighting& l);

    const Lighting& current();

    // Load, clamping everything; a missing file leaves the design's defaults.
    void load(const std::string& path);

    // Replace the live controls, hand the renderer the numbers, write the file. Returns false only
    // when the write failed; the change still took effect.
    bool apply(const Lighting& next);

    // The design's defaults, for the Reset row.
    Lighting defaults();

    // The numbers the live controls mean, for the renderer, the level derived from the switches.
    oot::renderer::LightingValues values();
    // The same for any Lighting, for the sweep to try a row's option without touching the live
    // values or the file (main/sweep.cpp).
    oot::renderer::LightingValues values_of(const Lighting& l);

} // namespace oot::ui::lighting
