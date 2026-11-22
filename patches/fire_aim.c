// FIRE WHILE AIMING (the user, 2026-11-22: "set a specific key, button to shoot when something like
// longshot, hookshot, bow, boomerang, slingshot or any other aimable item ... when aiming ... more like
// a modern trigger like R2/right trigger"; "if fire while aiming isnt set, it works as it does, and if
// set, it will be an override for all aiming fire actions"; "must also work for those items while z
// targeting too"; "its up and ready, press down on the key/button pulls back/holds it and let go
// fires").
//
// How the game fires a drawn item (z_player.c): its button pressed with the item up and ready starts
// the draw (Player_ProcessItemButtons, sUseHeldItem), and the draw fires once it is drawn either by
// itself (unk_860 below zero, which the ready pose func_8083501C sets) or, when the button is still held
// after a shot, once the button is let go (unk_860 above zero, func_808353D8; func_808351D4 fires on
// `unk_860 < 0 || !sHeldItemButtonIsHeldDown`). The same code runs in first person and Z targeting.
//
// With the control unbound nothing here touches anything. With it bound, while an aimable item is up
// and ready, the control stands in for the item's own button: the game sees the button held while the
// control is held and pressed when it is pressed, and never the button itself, so the button cannot
// fire it. While the control is held the draw is the waiting kind, so pressing pulls back and holds,
// and letting go fires. Called from the patched Play_Main (harness_warp.c) before the update reads the
// frame's input.
#include "patches.h"

#include "array_count.h"
#include "controller.h"
#include "pause.h"
#include "play_state.h"
#include "player.h"

// The program's side (src/game/recomp_api.cpp, main/input.h): bit 0 while the binding is held, bit 1
// on the frame it is pressed, bit 2 while it is bound at all. A hold that began while a menu held the
// game is not reported until it is let go.
DECLARE_FUNC(u32, recomp_fire_aim, void);
#define FIRE_HELD 1
#define FIRE_PRESSED 2
#define FIRE_BOUND 4

// The player's own (z_actor.c and z_player.c): an aimable item up and ready, the bow, the slingshot,
// the hookshots and the boomerang, as the first person aim itself tests (func_8084ABD8).
int func_8002DD78(Player* player);
int func_808334B4(Player* this);

// The buttons the player's items are used from, by Player.heldItemButton (z_player.c, sItemButtons).
static const u16 sFireItemButtons[] = { BTN_B, BTN_CLEFT, BTN_CDOWN, BTN_CRIGHT };

void FireAim_Input(PlayState* play) {
    u32 fire = recomp_fire_aim();
    Player* player;
    Input* input;
    u16 button;

    if (((fire & FIRE_BOUND) == 0) || (play->pauseCtx.state != PAUSE_STATE_OFF) ||
        (play->msgCtx.msgMode != MSGMODE_NONE)) {
        return;
    }
    player = GET_PLAYER(play);
    if ((player == NULL) || (!func_8002DD78(player) && !func_808334B4(player)) ||
        (player->heldItemButton < 0) || (player->heldItemButton >= ARRAY_COUNT(sFireItemButtons))) {
        return;
    }
    button = sFireItemButtons[player->heldItemButton];
    input = &play->state.input[0];

    // The item's own button is the control's now.
    input->cur.button &= ~button;
    input->press.button &= ~button;
    if (fire & FIRE_HELD) {
        input->cur.button |= button;
        // Held, the draw waits to be let go rather than firing itself once drawn.
        if (player->unk_860 < 0) {
            player->unk_860 = -player->unk_860;
        }
    }
    if (fire & FIRE_PRESSED) {
        input->press.button |= button;
    }
}
