// Hide the game's painted shadows under the actors when the ray traced shadows stand.
//
// The game draws a shadow under every actor as a textured decal on the floor: a circle (or a
// horse's blob) from ActorShadow_Draw, and one quad per foot from ActorShadow_DrawFoot for
// Link and the actors that walk. They are the game's stand-in for shadows it could not
// compute. With the traced shadows on (upgrades phase 57, the user's ask of 2026-09-24: "hide
// the fake shadows at Link's feet or anything others, it must just use the proper real ones"),
// the two draw functions return before drawing, and only outdoors: the traced sun casts
// shadows only under a sky (lighting-design.md, section 3), and a room without a sky keeps the
// game's own shadows until its fixtures become real lights. The game says whether the scene
// has a sky (play->skyboxId, SKYBOX_NONE for a room), and the program says whether the traced
// shadows stand (recomp_traced_shadows, src/game/recomp_api.cpp: 1 at lighting level 1 and
// above).
//
// Everything else in the two functions is reproduced line for line from the decompilation's
// z_actor.c at the pinned commit; only the early return is ours. ActorShadow_DrawFeet is left
// alone: it also sets the feet's floor flags the footstep effects read, and its drawing goes
// through ActorShadow_DrawFoot.

#include "patches.h"

#include "bgcheck.h"
#include "gfx.h"
#include "libc64/math64.h"
#include "gfx_setupdl.h"
#include "light.h"
#include "play_state.h"
#include "skybox.h"
#include "sys_math.h"
#include "sys_matrix.h"
#include "z_lib.h"

// The shadow display lists, in gameplay_keep.
extern Gfx gCircleShadowDL[];
extern Gfx gFootShadowDL[];

// The program's side (src/game/recomp_api.cpp).
s32 recomp_traced_shadows(void);

// The painted shadow is hidden where the traced one stands: the lighting on. Indoors as well
// since 2026-09-24, when the room's own light began casting a short contact shadow (the
// renderer's sunShadows of 2); before that the sky decided, and a room the game gave a sky
// (the Deku Tree) hid the painted shadow while the traced one did not exist ("link seems
// shadowless", the user).
static s32 painted_shadow_hidden(PlayState* play) {
    (void)play;
    return recomp_traced_shadows();
}

// @recomp Returns before drawing when the traced shadows stand.
RECOMP_PATCH void ActorShadow_Draw(Actor* actor, Lights* lights, PlayState* play, Gfx* dlist, Color_RGBA8* color) {
    f32 temp1;
    f32 temp2;
    MtxF sp60;

    if (actor->floorPoly == NULL) {
        return;
    }

    if (painted_shadow_hidden(play)) {
        return;
    }

    temp1 = actor->world.pos.y - actor->floorHeight;

    if (temp1 >= -50.0f && temp1 < 500.0f) {
        OPEN_DISPS(play->state.gfxCtx, "../z_actor.c", 1553);

        POLY_OPA_DISP = Gfx_SetupDL(POLY_OPA_DISP, SETUPDL_44);

        gDPSetCombineLERP(POLY_OPA_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, COMBINED, 0, 0, 0,
                          COMBINED);

        temp1 = (temp1 < 0.0f) ? 0.0f : ((temp1 > 150.0f) ? 150.0f : temp1);
        temp2 = 1.0f - (temp1 * (1.0f / 350));

        if (color != NULL) {
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, color->r, color->g, color->b,
                            (u32)(actor->shape.shadowAlpha * temp2) & 0xFF);
        } else {
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 0, 0, 0, (u32)(actor->shape.shadowAlpha * temp2) & 0xFF);
        }

        func_80038A28(actor->floorPoly, actor->world.pos.x, actor->floorHeight, actor->world.pos.z, &sp60);
        Matrix_Put(&sp60);

        if (dlist != gCircleShadowDL) {
            Matrix_RotateY(BINANG_TO_RAD(actor->shape.rot.y), MTXMODE_APPLY);
        }

        temp2 = (1.0f - (temp1 * (1.0f / 350)));
        temp2 *= actor->shape.shadowScale;
        Matrix_Scale(temp2 * actor->scale.x, 1.0f, temp2 * actor->scale.z, MTXMODE_APPLY);

        MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_actor.c", 1588);
        gSPDisplayList(POLY_OPA_DISP++, dlist);

        CLOSE_DISPS(play->state.gfxCtx, "../z_actor.c", 1594);
    }
}

// @recomp Returns before drawing when the traced shadows stand.
RECOMP_PATCH void ActorShadow_DrawFoot(PlayState* play, Light* light, MtxF* arg2, s32 arg3, f32 arg4, f32 arg5, f32 arg6) {
    s32 pad1;
    f32 sp58;
    f32 temp;
    s32 pad2;

    if (painted_shadow_hidden(play)) {
        return;
    }

    OPEN_DISPS(play->state.gfxCtx, "../z_actor.c", 1661);

    temp = arg3 * 0.00005f;
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 0, 0, 0, (u32)(arg4 * (temp > 1.0f ? 1.0f : temp)) & 0xFF);

    sp58 = Math_FAtan2F(light->l.dir[0], light->l.dir[2]);
    arg6 *= (4.5f - (light->l.dir[1] * 0.035f));
    if (arg6 < 1.0f) {
        arg6 = 1.0f;
    }
    Matrix_Put(arg2);
    Matrix_RotateY(sp58, MTXMODE_APPLY);
    Matrix_Scale(arg5, 1.0f, arg5 * arg6, MTXMODE_APPLY);

    MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_actor.c", 1687);
    gSPDisplayList(POLY_OPA_DISP++, gFootShadowDL);

    CLOSE_DISPS(play->state.gfxCtx, "../z_actor.c", 1693);
}
