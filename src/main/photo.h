#pragma once

#include <cstdint>

// PHOTO MODE (the user, 2026-10-09: "freeze the game at its current frame and then pause audio ...
// So I can lock the game at its exact spot, which helps me to get the same exact shot at any
// different setting level", "use the free move of the camera while leaving everything else
// frozen", and "when pressing the assigned keyboard shortcut or control shortcut on the controller
// for this particular photo mode ... it will freeze frame exactly wherever the user is. It will
// allow them to adjust settings ... move the camera around, but everything in the game remains
// frozen").
//
// While the Photo mode row is On, the Photo mode binding (Controls, F10 by default) freezes the game
// in play and lets it go again. Frozen, the game draws its frame over and over without updating
// (main/shots.h, the comparison shots' freeze), its sound is paused (patches/photo_audio.c), and
// the frame is drawn from a camera the player moves (patches/photo_mode.c). Every setting still
// applies to the frozen frame as it changes, so the Screenshot binding can take the same shot at
// each setting. A normal setting, not a Debugging one (the user: "Does not need to be a debug
// setting").
namespace oot::photo {

    // The Photo mode binding. Safe from any thread. Only in play: outside it there is no frame to
    // hold, and the press is ignored with a line in the log.
    void toggle();

    // Once per presented frame: photo mode ends if its row is turned Off or play is left.
    void tick();

    bool active();

    // PHOTO MODE'S CAMERA FROM ITS OWN BINDINGS (the user, 2026-10-09: "a dedicated section within
    // the controls panel ... those controls can be different than the ones that are used
    // elsewhere"): the controls screen's Photo mode rows, on the keyboard and every pad, not the
    // controller the game sees. What patches/photo_mode.c asks for, once a frozen frame, in
    // 4096ths: 0 across (right less left), 1 ahead (forward less back), 2 turn across, 3 turn up,
    // 4 up (camera up less down), 5 closer (closer less further); 6 is 4096 on the frame the Put
    // the camera back row is pressed. Nothing while one of the program's documents has the input.
    enum CameraInput : int {
        MoveAcross = 0, MoveAhead = 1, TurnAcross = 2, TurnUp = 3, Rise = 4, Closer = 5, PutBack = 6,
    };
    int32_t camera_input(int which);

    // THE GAME'S HUD, SHOWN OR HIDDEN (the user, 2026-10-09: "a photo mode control for toggling the
    // game UI HUD ... they can just get ... the actual game itself"): the Photo mode section's Show
    // or hide the HUD binding, while photo mode holds the game. Hidden, the game draws no hearts,
    // buttons, rupees, map or text box (patches/hud_anchoring.c, patches/photo_hud.c). It shows
    // again when photo mode ends.
    //
    // THREE STATES SINCE 2026-10-13 (the user: "What if the user wants the hud faded in photo mode?",
    // then "Own setting, 3 states"): photo mode starts the HUD as HUD in photo mode says (shown, dimmed
    // or hidden) and the control steps shown, dimmed, hidden, shown. Dimmed is the five parts at the
    // Dimmed to row's opacity (game/hud.cpp reads hud_opacity); hidden is the patch's, as before.
    void toggle_hud();
    bool hud_hidden();
    bool hud_dimmed();
    // The HUD parts' opacity in photo mode, 256 the game's own: 256 shown, the row's share dimmed.
    uint32_t hud_opacity();

    // THE BUTTON GUIDE (the user, 2026-10-13: "a row of buttons ... at the bottom of the screen when
    // enabling the photo mode ... the basic maneuvering of the camera controls based on their
    // obviously dynamically detected controller or keyboard setup", and "a button to be able to
    // hide that from view"): the row of prompts the shell draws while photo mode holds the game,
    // and the Photo mode section's Show or hide the button guide binding. Hidden stays hidden for
    // the rest of the run, so someone who knows the controls hides it once.
    void toggle_guide();
    bool guide_hidden();

    // From the game, each frozen frame: whether the room is drawn over a picture, where the
    // camera stays where the game put it (patches/photo_mode.c). Said once per photo mode.
    void note_picture_room(bool on);

} // namespace oot::photo
