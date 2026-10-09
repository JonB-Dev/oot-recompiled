// THE GAME'S RAYS THROUGH THE SIDE WINDOWS GO WHERE OURS STAND IN (2026-10-09).
//
// The user, in the Temple of Time: "I don't see our real lighting. I only see pretend beams and
// they need to be removed and instead replaced with working real light beams". The hall's side
// windows are an actor's (Bg_Toki_Hikari), and for a child it draws two scrolling ray pieces
// through them besides the glass; an adult gets the glass alone, which is why the beams were seen
// in one age and not the other. Renderer patch 0051 lights the air under every window with the
// window's own traced light, so while those shafts are on, the painted rays are left out and the
// glass is drawn as the game draws it. With the shafts off the hall is the game's own, as in 0.4.0,
// and the Debugging menu's Drawn beam row brings the rays back beside the shafts to compare.
//
// The function below is the game's own, line for line, with the one question added.
#include "patches.h"

#include "gfx.h"
#include "gfx_setupdl.h"
#include "sys_matrix.h"
#include "play_state.h"
#include "save.h"

#include "overlays/actors/ovl_Bg_Toki_Hikari/z_bg_toki_hikari.h"

// The program's side (src/game/recomp_api.cpp): 1 while the windows' traced shafts stand in for
// the painted rays.
s32 recomp_window_rays_hidden(void);

// The actor's display lists in its object, object_toki_objects (segment 6), at the offsets the
// decomp's object description gives (assets/xml/objects/object_toki_objects.xml): the windows'
// glass for an adult, the glass for a child, and the child's scrolling rays.
#define TOKI_WINDOWS_ADULT_DL ((Gfx*)0x06008190)
#define TOKI_WINDOWS_CHILD_DL ((Gfx*)0x06007E20)
#define TOKI_WINDOW_RAYS_DL ((Gfx*)0x06007EE0)

// @recomp The windows, with the child's rays asked of the shafts.
RECOMP_PATCH void func_808BA018(Actor* thisx, PlayState* play) {
    PlayState* play2 = (PlayState*)play;

    OPEN_DISPS(play->state.gfxCtx, "../z_bg_toki_hikari.c", 246);
    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_bg_toki_hikari.c", 252);

    if (LINK_IS_ADULT) {
        gSPDisplayList(POLY_OPA_DISP++, TOKI_WINDOWS_ADULT_DL);
    } else {
        gSPDisplayList(POLY_OPA_DISP++, TOKI_WINDOWS_CHILD_DL);
        // @recomp The rays, only while no traced shaft stands in for them.
        if (!recomp_window_rays_hidden()) {
            Gfx_SetupDL_25Xlu(play->state.gfxCtx);
            gSPSegment(POLY_XLU_DISP++, 8, Gfx_TexScroll(play->state.gfxCtx, 0, play2->gameplayFrames % 128, 64, 32));

            gSPSegment(POLY_XLU_DISP++, 9, Gfx_TexScroll(play->state.gfxCtx, 0, play2->gameplayFrames % 128, 64, 32));

            MATRIX_FINALIZE_AND_LOAD(POLY_XLU_DISP++, play->state.gfxCtx, "../z_bg_toki_hikari.c", 278);

            gSPDisplayList(POLY_XLU_DISP++, TOKI_WINDOW_RAYS_DL);
        }
    }
    CLOSE_DISPS(play->state.gfxCtx, "../z_bg_toki_hikari.c", 284);
}
