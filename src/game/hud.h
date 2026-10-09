#pragma once

#include <cstdint>

// THE HUD'S FADING (the user, 2026-10-09): "auto-dim/auto-hide hud when not in use (undims when
// using z targeting, using the c buttons, or talking), dim at all times, or hide/dim hud on mapped
// key or button. and if dimming, allow setting opacity and saturation", and for the rest: "More
// things to bring it back, Which parts hide, Fade speed, Delay before hiding, any other useful
// controls that users may like to work more modern-like. its intended to dim or hide depending on
// context or by click/button press".
//
// The rows are the HUD menu's (ui_settings, hudfade onward). The game reports once an update what
// it is doing (patches/hud_anchoring.c, the activity bits below); this module decides, by the
// rows, whether the HUD is wanted, and moves each part's opacity and color toward that over the
// Fade speed, on the wall clock. The game asks for each part's values as it draws it, and scales
// the part's own alpha (the one the game itself fades the HUD with in a cutscene) and its colors.
// The HUD binding (Controls, Tab by default) brings it back, or fades it, as the mode says.
namespace oot::hud {

    enum Part : int { Hearts = 0, Magic, Rupees, Buttons, Map, PartCount };

    // What the game is doing this update, from patches/hud_anchoring.c.
    enum Activity : uint32_t {
        Targeting = 1u << 0,      // Z: locked on, or the parallel camera
        CButtons = 1u << 1,       // a C button held
        Talking = 1u << 2,        // a message box open
        HealthMagic = 1u << 3,    // health or magic changed this update
        RupeesItems = 1u << 4,    // rupees, keys, an item on a button or its ammunition changed
        AnyButton = 1u << 5,      // any button pressed this update
        HealthLow = 1u << 6,      // health is critical
        Paused = 1u << 7,         // the pause menu is open
    };

    // Once an update, before the HUD is drawn.
    void note(uint32_t activity);

    // The HUD binding.
    void press_button();

    // A part's opacity (bits 0 to 8, 256 is the game's own) and color (bits 16 to 24, 256 full,
    // 0 gray), as they stand this update.
    uint32_t part(int part);

    // A PART'S SIZE (2026-10-09): bits 0 to 9 its size in percent of the game's own, by the Sizes
    // rows; the rest 0. The game scales the part about the corner it sits in, so a larger part
    // grows inward and stays on the screen.
    uint32_t layout(int part);

    // WHERE THE HUD SITS, CUSTOM (2026-10-09): 0 unless HUD sits is Custom; then bit 31 set and
    // the distances from the frame's left, right, top and bottom edges in whole percent, bits 0
    // to 7, 8 to 15, 16 to 23 and 24 to 30. The game moves each corner of the HUD to them.
    uint32_t place();

    // The frame's width over the console's 320, 16.16 fixed point (render.h, frame_aspect): what
    // a custom place's percent across is a share of.
    uint32_t frame_q16();

} // namespace oot::hud
