// PHOTO MODE HIDES THE TEXT BOX WITH THE HUD (2026-10-09, the user: "a photo mode control for
// toggling the game UI HUD. So that way ... they can just get ... the actual game itself").
//
// The hearts, the buttons, the rupees and the map are Interface_Draw's (its patch in
// hud_anchoring.c returns at once while photo mode has the HUD hidden). A message box is drawn
// apart from them, by Message_Draw, right after it in Play_Draw, so it is left out here the same
// way. The function is the game's own, as the retail build compiles it (no debug text), with the
// one question added first.
#include "patches.h"

#include "gfx.h"
#include "gfxalloc.h"
#include "play_state.h"

// The program's side (src/game/recomp_api.cpp): 1 while photo mode has the game's HUD hidden.
s32 recomp_photo_hud_hidden(void);

// The game's own, from z_message.c.
void Message_DrawMain(PlayState* play, Gfx** p);

// @recomp The message box, left out while photo mode has the HUD hidden.
RECOMP_PATCH void Message_Draw(PlayState* play) {
    Gfx* plusOne;
    Gfx* polyOpaP;

    // @recomp The one addition.
    if (recomp_photo_hud_hidden()) {
        return;
    }

    OPEN_DISPS(play->state.gfxCtx, "../z_message_PAL.c", 3554);

    plusOne = Gfx_Open(polyOpaP = POLY_OPA_DISP);
    gSPDisplayList(OVERLAY_DISP++, plusOne);
    Message_DrawMain(play, &plusOne);
    gSPEndDisplayList(plusOne++);
    Gfx_Close(polyOpaP, plusOne);
    POLY_OPA_DISP = plusOne;
    CLOSE_DISPS(play->state.gfxCtx, "../z_message_PAL.c", 3582);
}
