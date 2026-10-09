#include "ui/ui_lighting.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "main/keyvalue.h"

namespace {

    oot::ui::lighting::Lighting g_live;
    std::string g_path;

    struct Row {
        const char* key;
        const char* label;
        // Sized for the longest row, which is the Experiment one: it carries the shader variants
        // and the cut-out ladder (2026-09-30). Raise this when a row outgrows it; the compiler
        // says "excess elements in array initializer" and names the line, so it cannot pass
        // quietly.
        // Sized well clear of the longest row rather than exactly to it: this array has now been
        // the build error three times in one evening as the Experiment row grew, and the cost of
        // the slack is a few pointers.
        const char* options[40];
        int count;
    };

    // The order here IS the order on screen and in the file. The words are what a person reads;
    // the numbers each means are in `values` below, one table per row, in the same order. Every
    // feature that can be on has its own row with Off in it (ui_lighting.h says why).
    const Row ROWS[] = {
        { "raytracing",      "Ray traced lighting", { "Off", "On" }, 2 },
        { "shadows",         "Shadows",           { "Off", "On" }, 2 },
        { "shadowsoftness",  "Shadow softness",   { "Sharp", "Original", "Soft", "Softer",
                                                   "Very soft", "Hazy" }, 6 },
        // How much of the world behind and beside the camera the game keeps in the traced scene
        // so its shadow reaches the picture (the user, 2026-09-24, the windmill's shadow gone
        // as the camera turned: "add a setting to remove it or reduce it"). Game's own is the
        // console's culling; Near is the day's builds; Everything keeps every actor and every
        // room piece within its distance, at that cost.
        // How many rays a lit pixel spends on the sun's disc, a moving surface twice that. The
        // soft edge of a shadow is where the noise lives, and one ray across a moving edge is
        // what painted the windmill's shadow (the user, 2026-09-24). More rays cost frame rate
        // and buy a cleaner edge, which is a person's trade to make.
        { "shadowrays",      "Shadow rays",       { "1", "2", "4", "8" }, 4 },
        { "casters",         "Shadow casters",    { "Game's own", "Near", "Far", "Everything" }, 4 },
        // HOW DARK A SHADOW GOES INDOORS, as a share of a full sun's darkening. Indoors the
        // shadow is cast by the room's own light, and where a room's settings carry no
        // directional light at all this supplies one overhead so there is something to cast with.
        // It adds no light and moves no ambient: it only darkens where something is in the way,
        // within the short reach the indoor shadow has always used, since a ray traced further up
        // meets the ceiling and calls the whole place shadowed (the user, 2026-09-24: "it makes
        // the whole cave darker rather than just this shadow").
        { "indoorlight",     "Indoor light",      { "Off", "25%", "50%", "75%", "100%" }, 5 },
        // How much of the game's own ambient is taken away where no traced light falls (the
        // user, 2026-09-24: "realistic shadows not just glows"): a torch or a fairy then lights
        // a pool and what stands in it throws a shadow, and a room is dark but for its lights.
        // Outdoors under a sky the sun counts as light; indoors only the local lights do.
        { "darkness",        "Darkness",          { "Off", "Light", "Deep" }, 3 },
        // Darkness's opposite (the user, 2026-10-08): how far a traced light may lift a surface
        // above the game's own full light, so a lit patch is brighter than the floor was rather
        // than only the floor with its shadow taken off. Normal is the sum as it always was.
        { "lightbrightness", "Light brightness",  { "Normal", "Bright", "Brighter", "Harsh" }, 4 },
        { "occlusion",       "Ambient occlusion", { "Off", "Light", "Original", "Strong", "Full" }, 5 },
        { "occlusionradius", "Occlusion radius",  { "20 units", "40 units", "80 units", "160 units" }, 4 },
        { "occlusionquality", "Occlusion rays",   { "4", "8", "16" }, 3 },
        { "lights",          "Local lights",      { "Off", "On" }, 2 },
        // Twelve steps each, fine at the bottom and reaching well past the game's own light at
        // the top. THE NUMBERS MEAN THE GAME'S OWN LIGHT NOW: 100% is the radius and the power
        // the game gives a torch or a fairy, and the row goes on to three times that.
        //
        // They were capped at three quarters of the game's own (the user, 2026-09-24 14:45:
        // "cap at 75% ... while showing 0 to 100"), because a strong light on a sunlit wall ran
        // past the shade cap and read as white. The knee above the game's full light (the direct
        // pass, 18:38) took that away, and the cap then only stopped a torch from lighting its
        // own room: at three quarters the light a wall blocks is too small to read as a shadow
        // on the floor (the user, 19:38: "we just need the light reach and intensity caps
        // raised now"). So the cap is gone and the labels say what they mean.
        { "lightreach",      "Light reach",       { "0%", "5%", "10%", "20%", "30%", "45%", "60%", "80%", "100%", "150%", "200%", "300%" }, 12 },
        { "lightstrength",   "Light strength",    { "0%", "5%", "10%", "20%", "30%", "45%", "60%", "80%", "100%", "150%", "200%", "300%" }, 12 },
        { "bounce",          "Bounced light",     { "Off", "Light", "Original", "Strong", "Full" }, 5 },
        { "bouncerange",     "Bounce range",      { "500 units", "1500 units", "3000 units", "6000 units" }, 4 },
        { "reflections",     "Reflections",       { "Off", "Faint", "Original", "Strong", "Mirror" }, 5 },
        { "reflectionrange", "Reflection range",  { "Half the view", "The view", "Twice the view" }, 3 },
        // The glow in the air around a light: its density and its size, each its own, and neither
        // scaled by the strength any more (the user, 2026-09-24: "control the reach and the actual
        // radius visibility of a light and its opacity all that independent").
        { "glow",            "Glow density",      { "Off", "Faint", "Original", "Strong", "Thick" }, 5 },
        { "glowsize",        "Glow size",         { "Quarter", "Half", "Whole", "Double" }, 4 },
        // Motes of dust that show only where light reaches them, and not at all in shadow (the
        // user, 2026-10-08: "really, really subtle particles that float through light areas
        // only"), indoors and outdoors each their own ("outside versus inside to have their own
        // dust settings"), and a wind that only the outdoors has ("interior rooms should not have
        // that").
        { "indoordust",      "Indoor dust",       { "Off", "Very sparse", "Sparse", "Some", "Thick", "Very thick" }, 6 },
        { "indoordustvisibility", "Indoor dust visibility", { "Faint", "Light", "Visible", "Bright" }, 4 },
        { "indoordustsize",  "Indoor dust size",  { "Tiny", "Small", "Medium", "Large" }, 4 },
        { "indoordustdrift", "Indoor dust drift", { "Still", "Slow", "Lively" }, 3 },
        { "outdoordust",     "Outdoor dust",      { "Off", "Very sparse", "Sparse", "Some", "Thick", "Very thick" }, 6 },
        { "outdoordustvisibility", "Outdoor dust visibility", { "Faint", "Light", "Visible", "Bright" }, 4 },
        { "outdoordustsize", "Outdoor dust size", { "Tiny", "Small", "Medium", "Large" }, 4 },
        { "wind",            "Wind",              { "Calm", "Breeze", "Wind", "Strong" }, 4 },
        { "windgusts",       "Wind gusts",        { "None", "Light", "Strong" }, 3 },
        // Varied wanders round the compass on its own, with its own gusts and lulls (the user,
        // 2026-10-08: "randomly choose a direction and randomly choose the strength of the gust").
        { "winddirection",   "Wind from",         { "North", "East", "South", "West", "Varied" }, 5 },
        // Where the ambience's long fade with distance ends, indoors and outdoors each its own; it
        // always begins at a fifth of that (the user, 2026-10-08: "It still needs to be gradual
        // regardless").
        { "indoorambiencedistance", "Indoor ambience distance", { "Near", "Normal", "Far", "Very far" }, 4 },
        { "outdoorambiencedistance", "Outdoor ambience distance", { "Near", "Normal", "Far", "Very far" }, 4 },
        { "haze",            "Sun haze",          { "Off", "Faint", "Light", "Thick" }, 4 },
        { "hazerange",       "Haze range",        { "800 units", "1400 units", "2500 units", "5000 units" }, 4 },
        { "fogfade",         "Fade with the fog", { "Off", "On" }, 2 },
        { "distancefade",    "Fade with distance", { "Off", "Far", "Middle", "Near" }, 4 },
        { "smoothing",       "Smoothing",         { "Off", "Light", "Original", "Heavy" }, 4 },
        { "filter",          "Filter",            { "Off", "One step", "Two steps" }, 3 },
        // Shadows is the direct light as the picture uses it (history and filter applied), with a
        // factor above one shown warm; Raw shadows is the single sample before them; History is
        // how many frames each pixel's history holds, red for none to green for many; Hits is
        // what the first ray made of each pixel: green a hit, red a hit the depth compare
        // dropped, magenta one dropped as a genuine cut-out, black no hit at all.
        { "inspect",         "Inspect one pass",  { "Off", "Shadows", "Raw shadows", "Occlusion", "Lights", "Bounce", "Reflections", "History", "Fog share", "Hits", "Normals" }, 11 },
        // Each variant undoes ONE piece of the 2026-09-24 light math, so the person can see
        // which piece a look comes from: 1 holds every surface at its raster brightness (the
        // factor capped at one, as before that day); 2 fades the factor toward one by the fog
        // instead of relighting the unfogged part only; 3 uses the game's own falloff dome in
        // place of the inverse square core; 4 switches the lights' glow in the air off; 5 holds
        // the shade to one as the console does (the default is twice that, so a local light
        // shows on a surface the game already lit in full); 6 turns off the compare of each
        // first hit against the game's own depth, which drops the hit where they disagree (the
        // 2026-09-24 seam under Z targeting is suspected of being that).
        // The last option puts patch 0006's rule back: a tree, a vine or a fence in the
        // translucent z mode traced by nothing and drawn after the composite, which is what it
        // did before patch 0010. Here so the two can be put side by side, not because either is
        // in question.
        //
        // IT IS NAMED FOR WHAT IT TAKES AWAY, and that wording was bought the hard way. It read
        // "Cut-outs translucent" for one build, which describes the mechanism truthfully and
        // sounds, to anyone who has just been told a cut-out fix shipped, like the switch that
        // turns the fix ON. The user selected it, it persists, and he then tested the fix with
        // the fix disabled and reported it still broken. Every other option on this row is a
        // deviation from normal with Off as normal, so the label has to say which side of that
        // line it sits on. "untraced (old)" cannot be read as the new thing.
        //
        // THE CUT-OUT LADDER (8 to 13) is his ask of 2026-09-30, after patch 0010's one
        // discriminator changed nothing on his trees: every rule the microcode actually offers
        // for deciding that a draw in the translucent z mode is really solid geometry, so the
        // picture can answer instead of me guessing again. Off is the shipping rule (the
        // coverage cut or the alpha compare). "everything" is the control, not a fix: it is
        // wrong for water and every glow, and it is there to say whether the mechanism is the
        // classification at all.
        { "experiment",      "Experiment",        { "Off", "Surfaces capped", "Fog by lerp", "Game's falloff", "Glow off", "Console clamp", "Depth test off", "Coverage averaged",
                                                    "Cut-outs untraced (old)", "Solid if writes depth", "Solid if reads depth", "Solid if coverage alpha", "Solid if not forced blend", "Solid: everything",
                                                    "Reads depth, no shadow", "Reads depth, many tris", "Reads depth, many tris, no shadow",
                                                    "Alpha tested, no shadow", "Alpha tested, shadows", "Alpha tested, strict, shadows",
                                                    "Alpha cut, full light", "Alpha cut, full light, shadows",
                                                    "Retired A", "Retired B",
                                                    "Alpha cut, keep hit", "Alpha cut, keep hit, shadows",
                                                    "Coverage or compare (old)",
                                                    "Reflections: both bits", "Reflections: wider probe", "Reflections: both ways",
                                                    "Water stays water",
                                                    "Shade: local after knee", "Shade: local lifts shade", "Shade: both",
                                                    "Cut-out edge unscaled (old)",
                                                    "Cut-out quads untraced (old)",
                                                    "Depthless glows lit (old)",
                                                    "Light effects untraced (old)" }, 38 },
        // THE WINDOWS' AIM (the user, 2026-10-08, of the Master Sword room's painted bright patch:
        // "adjust the alignment of the real ray tracing to actually just match up with that
        // perfectly ... allow me to cycle through positioning in a debug setting"; "adjust the angle
        // of the lighting and the depth ... vertically and horizontally ... on the x and y and z
        // axis"). Renderer patch 0044: which window (numbered as the log lists them), from its own
        // slope or the old beam's direction, whole degrees steeper or flatter and turned, and its
        // light moved along the world's axes. Saved, so a setting being tried survives a restart.
        { "windowaim",       "Aim which window",  { "Every window", "Window 1", "Window 2", "Window 3", "Window 4", "Window 5", "Window 6", "Window 7", "Window 8" }, 9 },
        { "windowaimfrom",   "Aim from",          { "Its own slope", "The old beam" }, 2 },
        { "windowtilt",      "Aim up or down",    { "15 flatter", "14 flatter", "13 flatter", "12 flatter", "11 flatter", "10 flatter", "9 flatter", "8 flatter", "7 flatter", "6 flatter", "5 flatter", "4 flatter", "3 flatter", "2 flatter", "1 flatter", "As built", "1 steeper", "2 steeper", "3 steeper", "4 steeper", "5 steeper", "6 steeper", "7 steeper", "8 steeper", "9 steeper", "10 steeper", "11 steeper", "12 steeper", "13 steeper", "14 steeper", "15 steeper" }, 31 },
        { "windowturn",      "Aim left or right", { "15 left", "14 left", "13 left", "12 left", "11 left", "10 left", "9 left", "8 left", "7 left", "6 left", "5 left", "4 left", "3 left", "2 left", "1 left", "As built", "1 right", "2 right", "3 right", "4 right", "5 right", "6 right", "7 right", "8 right", "9 right", "10 right", "11 right", "12 right", "13 right", "14 right", "15 right" }, 31 },
        { "windowmovex",     "Move along x",      { "-160 units", "-120 units", "-80 units", "-60 units", "-40 units", "-30 units", "-20 units", "-10 units", "-5 units", "None", "+5 units", "+10 units", "+20 units", "+30 units", "+40 units", "+60 units", "+80 units", "+120 units", "+160 units" }, 19 },
        { "windowmovey",     "Move along y",      { "-160 units", "-120 units", "-80 units", "-60 units", "-40 units", "-30 units", "-20 units", "-10 units", "-5 units", "None", "+5 units", "+10 units", "+20 units", "+30 units", "+40 units", "+60 units", "+80 units", "+120 units", "+160 units" }, 19 },
        { "windowmovez",     "Move along z",      { "-160 units", "-120 units", "-80 units", "-60 units", "-40 units", "-30 units", "-20 units", "-10 units", "-5 units", "None", "+5 units", "+10 units", "+20 units", "+30 units", "+40 units", "+60 units", "+80 units", "+120 units", "+160 units" }, 19 },
        // CANDIDATES TO CYCLE (the user, 2026-10-08, in the Fire Temple: "adds some debugging
        // options to cycle through so I can test a bunch of different options to see which one
        // fixes it without regressions"; "my current 4.0 is really good so i neevr want to
        // accidentally regress here"). Each row ends with 0.4.0's own behavior, named for it.
        //
        // Light through stone (renderer patch 0049): whether a torch's or Navi's light goes on
        // through the stone round it. The first stops it at the first surface it meets, its glow
        // in the air too; the second leaves that glow as it was; the last is the 48 unit
        // clearance every non white light had, which let a flame in a niche shine through it.
        //
        // Glow test (patches/light_glow.c, renderer patch 0050): how a light's round glow, a flat
        // card turned to the camera, is hidden. By its center: hidden whole when the light's center
        // is covered, as on the console, Navi's core still masked per pixel (0046) or, the second,
        // not; per pixel, which cut the card wherever stone stood in front of it (0.4.0); or never.
        // Saved, unlike the other Debugging rows, so a candidate being tried survives a restart.
        { "lightstone",      "Light through stone", { "Stops at stone", "Stops, glow passes", "Passes 48 units (0.4.0)" }, 3 },
        { "glowtest",        "Glow test",         { "By center", "By center, Navi too", "Per pixel (0.4.0)", "Never (0.3.9)" }, 4 },
        // THE WINDOWS' SHAFTS OF LIGHT (renderer patch 0051, the user, 2026-10-09: "make our own
        // visible light that starts at window and fades closer to the floor but looks way more
        // natural and uses actual lighting somehow"). The air in each window's traced beam,
        // brightest at the glass and fading with the distance the light has come, cut by what
        // stands in it. Appended, and shown under Glow size (rows_for in ui_shell.cpp). Off by
        // default, like everything that costs time (the user, 2026-10-08: "I want the defaults to
        // just be off"). Drawn beam is the Debugging menu's: the game's own painted beam, which
        // 0034/0039 hide where the windows give light, shown again beside the shafts to compare.
        { "windowshafts",    "Window shafts",     { "Off", "Faint", "Soft", "Medium", "Strong", "Bright" }, 6 },
        { "shaftlength",     "Shaft length",      { "Short", "Medium", "Long", "Very long" }, 4 },
        { "drawnbeam",       "Drawn beam",        { "Hidden (0.4.0)", "Shown" }, 2 },
        // 37 (2026-10-08) puts back the light effects as the game draws them before renderer
        // patches 0029 and 0030: a glowing shaft that lights nothing, the floor under the Temple
        // of Time's pedestal beam switching the whole scene brighter, the Sun's Song and the blue
        // warp lighting a figure whole from one side, and the Temple of Time's windows giving no
        // light (patches/beam_zone.c and patches/effect_lights.c ask for this rung through
        // recomp_traced_light_effects).
        // 36 (2026-10-08) puts back how a blended draw with no depth test and no depth write was
        // sorted before renderer patch 0028: into the traced scene and the raster before the
        // composite, where Navi's core was multiplied by the shadow behind it and dimmed in shade.
        // 35 (2026-10-08) puts back the sprite rule as it was before renderer patch 0027: a
        // translucent draw of two triangles or fewer stays out of the traced scene whatever its
        // texture, which is what left a patch of vines unlit over the wall behind it.
        // 34 (2026-10-07) puts back the traced alpha test's old reading of a texture: the raw
        // coordinate, without the raster's tile scale. With a texture pack that reads the
        // replacement at a quarter of the right place, which is the cliff top drawn above where
        // its shadow and Navi's light stop. Named "(old)" for the reason the note above gives.
        // It is the first rung after the shade rungs, and patch 0023 made sure it differs from
        // Off in nothing else.
        // THE REFLECTION RUNGS CAME OFF WITH THE FEATURE (2026-09-30). Six candidates were
        // built, every one was looked at, and the verdict was that the picture is better without
        // reflections at all. Rungs that can no longer do anything are the same defect as a
        // setting that does nothing, one level down, so they go. Everything below 31 is the
        // foliage work and keeps its number, so nothing a person might be sitting on moves.
    };

    constexpr int ROW_COUNT = static_cast<int>(sizeof(ROWS) / sizeof(ROWS[0]));

    // The numbers, in each row's order.
    // "refraction" is the slot the direct pass writes the fog's share of each pixel into (nothing
    // refracts yet): the Fog share view, to see how much of a surface the lighting may touch.
    // "normal" is the surface normal each pixel is shaded with, and it was added to this row on
    // 2026-09-24 to answer a question the other views could not: a whole surface reading one flat
    // value in the Occlusion view, bounded exactly by its own polygon edge and unchanged when the
    // occlusion radius is cut from a hundred and sixty units to twenty, is not a pass misbehaving.
    // It is that surface's normal. The renderer has carried the view all along.
    constexpr const char* VIEWS[] = { "final", "shadow", "raw", "ao", "lights", "indirect", "reflection", "history", "refraction", "hits", "normal" };
    // How many times the sun's own disc the shadow ray's cone is. A sun is half a degree across,
    // so its penumbra near a blocker is about a pixel however many rays are spent on it, which is
    // what reads as a hard polygonal edge along a wall (the user, 2026-09-24, in the Forest
    // Temple's courtyard). The two wide steps were added for that; the indoor shadow, the one they
    // called soft, is six times the sun's disc, so Very soft is a little softer than that and Hazy
    // is well past it. Added at the END, so a settings file keeps the step it named.
    constexpr float SOFTNESS[] = { 0.25f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
    constexpr float DARKNESS[] = { 0.0f, 0.4f, 0.7f };   // the share of the ambient taken away where no light falls
    constexpr float LIGHT_BRIGHTNESS[] = { 0.0f, 0.33f, 0.66f, 1.0f };   // 0 the knee as it was, 1 none (renderer patch 0033)
    // The dust (renderer patches 0035 and 0036): the share of the air's forty unit cells that hold
    // a mote; a lit mote's brightness as a share of the light on it; the indoor motes' pace; the
    // wind's speed in world units a second; its gusts; and the way it blows over the ground, as
    // world x and z, for wind FROM the north (the world's -z), east, south and west.
    // Motes to a forty unit cell since renderer patch 0038: the old Thick is Very sparse now.
    constexpr float DUST_DENSITY[] = { 0.0f, 0.6f, 1.5f, 3.0f, 5.0f, 8.0f };
    constexpr float DUST_SIZE[] = { 0.15f, 0.3f, 0.6f, 1.2f };   // the median mote's radius in world units (patch 0038)
    constexpr float DUST_VISIBILITY[] = { 0.15f, 0.3f, 0.5f, 0.8f };
    constexpr float DUST_DRIFT[] = { 0.0f, 1.0f, 2.5f };
    constexpr float WIND_SPEED[] = { 0.0f, 15.0f, 45.0f, 110.0f };
    constexpr float WIND_GUSTS[] = { 0.0f, 0.5f, 1.0f };
    // The last, Varied, is no direction at all, which asks the renderer for its wandering wind.
    constexpr float WIND_X[] = { 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
    constexpr float WIND_Z[] = { 1.0f, 0.0f, -1.0f, 0.0f, 0.0f };
    constexpr float AMBIENCE_DISTANCE[] = { 600.0f, 1200.0f, 2400.0f, 4000.0f };   // world units where the fade ends (renderer patch 0037)
    // The windows' aim (renderer patch 0044): degrees steeper (or turned) for each step of those
    // rows, and world units for each step of the moves.
    constexpr float AIM_DEGREES[] = { -15.0f, -14.0f, -13.0f, -12.0f, -11.0f, -10.0f, -9.0f, -8.0f, -7.0f, -6.0f, -5.0f, -4.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f };
    constexpr float MOVE_UNITS[] = { -160.0f, -120.0f, -80.0f, -60.0f, -40.0f, -30.0f, -20.0f, -10.0f, -5.0f, 0.0f, 5.0f, 10.0f, 20.0f, 30.0f, 40.0f, 60.0f, 80.0f, 120.0f, 160.0f };
    constexpr int SHADOW_RAYS[] = { 1, 2, 4, 8 };        // samples over the sun's disc per lit pixel
    // How dark a figure's shadow goes indoors, as a share of a full sun's darkening, in the
    // quarters the user asked for (2026-09-24). The fixed value this replaced was 0.35.
    constexpr float INDOOR_LIGHT[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    constexpr float GLOW_SIZE[] = { 0.25f, 0.5f, 1.0f, 2.0f };   // the glow's radius over the game's own light radius
    constexpr float OCCLUSION[] = { 0.0f, 0.35f, 0.7f, 0.85f, 1.0f };
    constexpr float OCCLUSION_RADIUS[] = { 20.0f, 40.0f, 80.0f, 160.0f };
    constexpr int OCCLUSION_RAYS[] = { 4, 8, 16 };
    // The row's label IS the number here: 1.0 is the game's own radius and the game's own power,
    // and the last three steps are half as much again, twice, three times (2026-09-24, the caps
    // raised so a torch can light its room and cast a shadow across it).
    constexpr float REACH[] = { 0.0f, 0.05f, 0.1f, 0.2f, 0.3f, 0.45f, 0.6f, 0.8f, 1.0f, 1.5f, 2.0f, 3.0f };
    constexpr float STRENGTH[] = { 0.0f, 0.05f, 0.1f, 0.2f, 0.3f, 0.45f, 0.6f, 0.8f, 1.0f, 1.5f, 2.0f, 3.0f };
    constexpr float BOUNCE[] = { 0.0f, 0.12f, 0.25f, 0.5f, 1.0f };
    constexpr float BOUNCE_RANGE[] = { 500.0f, 1500.0f, 3000.0f, 6000.0f };
    constexpr float REFLECTIONS[] = { 0.0f, 0.25f, 0.5f, 0.8f, 1.0f };
    constexpr float REFLECTION_RANGE[] = { 0.5f, 1.0f, 2.0f };
    // Ten times fainter since 2026-09-24: with the falloff's core at the light's own radius the
    // glow along a ray through a torch is about its core times pi times this, and the old
    // numbers, tuned when no light reached the passes in play, saturated it.
    constexpr float GLOW[] = { 0.0f, 0.0001f, 0.0002f, 0.0004f, 0.0008f };
    // The windows' shafts (renderer patch 0051): the air's brightness per world unit of beam
    // crossed, on the glow's own scale, and the share of each window's own fall, from its glass to
    // the first surface its light meets, over which the shaft fades to a third (renderer patch
    // 0052, 2026-10-09: in world units the Master Sword room's high window had faded to almost
    // nothing before its shaft came into view).
    constexpr float SHAFTS[] = { 0.0f, 0.0002f, 0.0004f, 0.0008f, 0.0016f, 0.003f };
    constexpr float SHAFT_LENGTH[] = { 0.25f, 0.45f, 0.75f, 1.2f };
    // Four times fainter since 2026-09-24: at Thick over 5000 units the sunlit air added about
    // half of white to the far scene, which read as "harsh blowout" at a distance.
    constexpr float HAZE[] = { 0.0f, 0.0000025f, 0.0000075f, 0.000025f };
    constexpr float HAZE_RANGE[] = { 800.0f, 1400.0f, 2500.0f, 5000.0f };
    constexpr float DISTANCE_FADE[] = { 0.0f, 0.5f, 0.3f, 0.15f };
    constexpr float SMOOTHING[] = { 1.0f, 0.3f, 0.12f, 0.06f };

    int clamp_int(int value, int low, int high) {
        return std::max(low, std::min(high, value));
    }

    template <typename T, int N>
    T pick(const T (&table)[N], int index) {
        return table[clamp_int(index, 0, N - 1)];
    }

    // One place that knows the fields by number, for the reading, the writing and the clamping.
    int* field(oot::ui::lighting::Lighting& l, int row) {
        switch (row) {
            case 0:  return &l.raytracing;
            case 1:  return &l.shadows;
            case 2:  return &l.shadow_softness;
            case 3:  return &l.shadow_rays;
            case 4:  return &l.casters;
            case 5:  return &l.indoor_light;
            case 6:  return &l.darkness;
            case 7:  return &l.light_brightness;   // inserted 2026-10-08; every row below moved down one
            case 8:  return &l.occlusion;
            case 9:  return &l.occlusion_radius;
            case 10: return &l.occlusion_quality;
            case 11: return &l.lights;
            case 12: return &l.light_reach;
            case 13: return &l.light_strength;
            case 14: return &l.bounce;
            case 15: return &l.bounce_range;
            case 16: return &l.reflections;
            case 17: return &l.reflection_range;
            case 18: return &l.glow;
            case 19: return &l.glow_size;
            // The dust rows and the ambience's distances, inserted 2026-10-08; every row below them
            // moved down twelve.
            case 20: return &l.indoor_dust;
            case 21: return &l.indoor_dust_visibility;
            case 22: return &l.indoor_dust_size;
            case 23: return &l.indoor_dust_drift;
            case 24: return &l.outdoor_dust;
            case 25: return &l.outdoor_dust_visibility;
            case 26: return &l.outdoor_dust_size;
            case 27: return &l.wind;
            case 28: return &l.wind_gusts;
            case 29: return &l.wind_direction;
            case 30: return &l.indoor_ambience_distance;
            case 31: return &l.outdoor_ambience_distance;
            case 32: return &l.haze;
            case 33: return &l.haze_range;
            case 34: return &l.fog_fade;
            case 35: return &l.distance_fade;
            case 36: return &l.smoothing;
            case 37: return &l.filter;
            case 38: return &l.inspect;
            case 39: return &l.experiment;
            // The windows' aim, added at the end 2026-10-08.
            case 40: return &l.window_aim;
            case 41: return &l.window_aim_from;
            case 42: return &l.window_tilt;
            case 43: return &l.window_turn;
            case 44: return &l.window_move_x;
            case 45: return &l.window_move_y;
            case 46: return &l.window_move_z;
            // The candidates, added at the end 2026-10-08.
            case 47: return &l.light_stone;
            case 48: return &l.glow_test;
            // The windows' shafts, added at the end 2026-10-09.
            case 49: return &l.window_shafts;
            case 50: return &l.shaft_length;
            case 51: return &l.drawn_beam;
            default: return nullptr;
        }
    }

    using oot::keyvalue::parse_line;
    using oot::keyvalue::as_int;

} // namespace

namespace oot::ui::lighting {

    bool Lighting::operator==(const Lighting& o) const {
        Lighting a = *this;
        Lighting b = o;
        for (int row = 0; row < ROW_COUNT; ++row) {
            if (*field(a, row) != *field(b, row)) {
                return false;
            }
        }
        return true;
    }

    int row_count() {
        return ROW_COUNT;
    }

    const char* row_label(int row) {
        return (row >= 0 && row < ROW_COUNT) ? ROWS[row].label : "";
    }

    const char* row_key(int row) {
        return (row >= 0 && row < ROW_COUNT) ? ROWS[row].key : "";
    }

    int row_index(const char* key) {
        for (int row = 0; row < ROW_COUNT; ++row) {
            if (std::strcmp(ROWS[row].key, key) == 0) {
                return row;
            }
        }
        return -1;
    }

    int option_count(int row) {
        return (row >= 0 && row < ROW_COUNT) ? ROWS[row].count : 0;
    }

    const char* option_label(int row, int value) {
        if (row < 0 || row >= ROW_COUNT) {
            return "";
        }
        return ROWS[row].options[clamp_int(value, 0, ROWS[row].count - 1)];
    }

    int get_row(const Lighting& l, int row) {
        Lighting copy = l;
        const int* f = field(copy, row);
        return f != nullptr ? *f : 0;
    }

    void set_row(Lighting& l, int row, int value) {
        if (int* f = field(l, row)) {
            *f = value;
        }
        clamp(l);
    }

    void clamp(Lighting& l) {
        // THE REFLECTIONS ARE OFF AND STAY OFF (2026-09-30). The rows are gone from the menus,
        // and this is what makes that true of the PASS as well: a lighting file written before
        // today has a reflection value in it, and without this the pass would go on running with
        // no way left to turn it off.
        l.reflections = 0;
        l.reflection_range = 0;
        for (int row = 0; row < ROW_COUNT; ++row) {
            if (int* f = field(l, row)) {
                *f = clamp_int(*f, 0, ROWS[row].count - 1);
            }
        }
    }

    const Lighting& current() {
        return g_live;
    }

    Lighting defaults() {
        return Lighting{};
    }

    oot::renderer::LightingValues values_of(const Lighting& l) {
        oot::renderer::LightingValues v;

        // The level is what the switches need, not a choice of its own: shadows want level 1,
        // the occlusion and the lights level 2, the bounce, the reflections, the glow and the
        // haze level 3. Off is off whatever else is on.
        int level = 0;
        if (l.raytracing != 0) {
            level = 1;
            if (pick(OCCLUSION, l.occlusion) > 0.0f || l.lights != 0) {
                level = 2;
            }
            if (pick(BOUNCE, l.bounce) > 0.0f || pick(REFLECTIONS, l.reflections) > 0.0f ||
                pick(GLOW, l.glow) > 0.0f || pick(HAZE, l.haze) > 0.0f ||
                pick(DUST_DENSITY, l.indoor_dust) > 0.0f || pick(DUST_DENSITY, l.outdoor_dust) > 0.0f) {
                level = 3;
            }
        }
        v.level = level;
        v.view = pick(VIEWS, l.inspect);
        v.shadow_strength = (l.shadows != 0) ? 1.0f : 0.0f;
        v.shadow_softness = pick(SOFTNESS, l.shadow_softness);
        v.occlusion_strength = pick(OCCLUSION, l.occlusion);
        v.occlusion_radius = pick(OCCLUSION_RADIUS, l.occlusion_radius);
        v.occlusion_samples = pick(OCCLUSION_RAYS, l.occlusion_quality);
        v.light_strength = (l.lights != 0) ? pick(STRENGTH, l.light_strength) : 0.0f;
        v.light_reach = pick(REACH, l.light_reach);
        v.bounce_strength = pick(BOUNCE, l.bounce);
        v.bounce_range = pick(BOUNCE_RANGE, l.bounce_range);
        v.reflection_strength = pick(REFLECTIONS, l.reflections);
        v.reflection_range_scale = pick(REFLECTION_RANGE, l.reflection_range);
        v.light_glow = pick(GLOW, l.glow);
        v.sun_haze = pick(HAZE, l.haze);
        v.haze_range = pick(HAZE_RANGE, l.haze_range);
        v.fog_fade = (l.fog_fade != 0) ? 1.0f : 0.0f;
        v.distance_fade = pick(DISTANCE_FADE, l.distance_fade);
        v.history_blend = pick(SMOOTHING, l.smoothing);
        v.filter_steps = l.filter;
        v.experiment = l.experiment;
        v.caster_reach = l.casters;
        v.darkness = pick(DARKNESS, l.darkness);
        v.light_brightness = pick(LIGHT_BRIGHTNESS, l.light_brightness);
        v.shadow_rays = pick(SHADOW_RAYS, l.shadow_rays);
        v.indoor_light = pick(INDOOR_LIGHT, l.indoor_light);
        v.glow_size = pick(GLOW_SIZE, l.glow_size);
        v.indoor_dust_density = pick(DUST_DENSITY, l.indoor_dust);
        v.indoor_dust_visibility = pick(DUST_VISIBILITY, l.indoor_dust_visibility);
        v.indoor_dust_drift = pick(DUST_DRIFT, l.indoor_dust_drift);
        v.indoor_dust_size = pick(DUST_SIZE, l.indoor_dust_size);
        v.outdoor_dust_size = pick(DUST_SIZE, l.outdoor_dust_size);
        v.outdoor_dust_density = pick(DUST_DENSITY, l.outdoor_dust);
        v.outdoor_dust_visibility = pick(DUST_VISIBILITY, l.outdoor_dust_visibility);
        v.wind_speed = pick(WIND_SPEED, l.wind);
        v.wind_gusts = pick(WIND_GUSTS, l.wind_gusts);
        v.wind_x = pick(WIND_X, l.wind_direction);
        v.wind_z = pick(WIND_Z, l.wind_direction);
        v.indoor_ambience_distance = pick(AMBIENCE_DISTANCE, l.indoor_ambience_distance);
        v.outdoor_ambience_distance = pick(AMBIENCE_DISTANCE, l.outdoor_ambience_distance);
        v.window_aim = l.window_aim;
        v.window_aim_from = l.window_aim_from;
        v.window_tilt = pick(AIM_DEGREES, l.window_tilt);
        v.window_turn = pick(AIM_DEGREES, l.window_turn);
        v.window_move_x = pick(MOVE_UNITS, l.window_move_x);
        v.window_move_y = pick(MOVE_UNITS, l.window_move_y);
        v.window_move_z = pick(MOVE_UNITS, l.window_move_z);
        v.light_stone = l.light_stone;
        v.glow_test = l.glow_test;
        v.shaft_strength = pick(SHAFTS, l.window_shafts);
        v.shaft_length = pick(SHAFT_LENGTH, l.shaft_length);
        v.drawn_beam = l.drawn_beam;
        return v;
    }

    oot::renderer::LightingValues values() {
        return values_of(g_live);
    }

    void load(const std::string& path) {
        g_path = path;
        std::ifstream file(path);
        if (!file) {
            // No file is the first run, and the defaults stand.
            oot::renderer::apply_lighting(values());
            return;
        }
        Lighting loaded;
        std::string line;
        while (std::getline(file, line)) {
            std::string key, value;
            if (!parse_line(line, key, value)) {
                continue;
            }
            for (int row = 0; row < ROW_COUNT; ++row) {
                if (key == ROWS[row].key) {
                    set_row(loaded, row, as_int(value, get_row(loaded, row)));
                    break;
                }
            }
        }
        clamp(loaded);
        // Inspect does not survive a launch, like the recorder and the counter: a session that
        // left one pass up must not open on it next time.
        loaded.inspect = 0;
        loaded.experiment = 0;
        // The windows' aim starts as built too (2026-10-08): the values the user found for the
        // Temple of Time's tall window are built into the renderer now (patch 0045), and kept in
        // the file they would be applied a second time on top of it.
        const Lighting starts;
        loaded.window_aim = starts.window_aim;
        loaded.window_aim_from = starts.window_aim_from;
        loaded.window_tilt = starts.window_tilt;
        loaded.window_turn = starts.window_turn;
        loaded.window_move_x = starts.window_move_x;
        loaded.window_move_y = starts.window_move_y;
        loaded.window_move_z = starts.window_move_z;
        g_live = loaded;
        oot::renderer::apply_lighting(values());
    }

    bool apply(const Lighting& next) {
        Lighting copy = next;
        clamp(copy);
        g_live = copy;
        oot::renderer::apply_lighting(values());

        if (g_path.empty()) {
            return false;
        }
        std::ofstream file(g_path, std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "[ui] the lighting file could not be written to %s\n", g_path.c_str());
            return false;
        }
        file << "# OoT: Recompiled lighting. Edit by hand if you like; every value is clamped when\n";
        file << "# it is read, and anything unrecognized is ignored.\n";
        for (int row = 0; row < ROW_COUNT; ++row) {
            file << ROWS[row].key << " = " << get_row(g_live, row) << "\n";
        }
        return file.good();
    }

} // namespace oot::ui::lighting
