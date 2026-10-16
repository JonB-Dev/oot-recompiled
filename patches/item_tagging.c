// Tag the item the player holds up, and the actors that build matrices by hand: the bomb, the
// light switch, and the rain. The record of the sweep that chose them is at the end.
//
// COLLECTIBLES NEED NOTHING HERE. `EnItem00_Draw` (a rupee, a heart, ammunition, a key) loads
// exactly one matrix per frame, and phase 37's `Actor_Draw` already rewrites its reserved slot
// into the actor's own group in that case, so a rupee spins through the decomposed rotation
// like any other rigid actor. The same holds for every other actor that draws through
// `GetItem_Draw` (the shop's shelves, the chest game's prizes, the skulltula token, the
// ocarina, the letter in the bottle, the fire arrow): each is an actor of its own with one
// matrix. Tagging inside `GetItem_Draw` itself, as the plan names it, would nest a SHARED item
// id inside each of those actors' unique ids, and the innermost group is the one a matrix
// belongs to. The one draw of an item that is not an actor's own is the item the player holds
// up in the get-item pose: drawn inside the player's draw after his limbs' groups have closed,
// where the whole-actor rule cannot apply. That draw, `Player_DrawGetItemImpl`, is patched
// instead. Its group is keyed by the item's draw id in the item range, unique because one item
// is held up at a time, and the spin the game gives it (a thousand units a frame) is what the
// decomposed rotation interpolates.
//
// THE SWEEP. The reference project lists the actors of its game that build matrices outside
// the skeleton path; every one is that game's. For this game the decompilation was swept for
// actors that never call a skeleton draw and load two or more matrices, sixty of them, and
// each was read for whether the loads happen in ONE frame (several matrices, and the
// whole-actor rule steps aside) and whether the thing MOVES. The ones that needed a tag are
// here; the record is at the end of this file.

#include "patches.h"
#include "tagging_helpers.h"

#include "libc64/math64.h"
#include "draw.h"
#include "gfx.h"
#include "gfx_setupdl.h"
#include "play_state.h"
#include "player.h"
#include "rand.h"
#include "regs.h"
#include "save.h"
#include "segmented_address.h"
#include "sys_math.h"
#include "sys_matrix.h"
#include "z_lib.h"
#include "z_math.h"
#include "environment.h"
#include "camera.h"
#include "collision_check.h"
#include "overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "overlays/actors/ovl_Obj_Lightswitch/z_obj_lightswitch.h"

#include "rt64_extended_gbi.h"

// Overlay functions the light switch's dispatcher calls, and the rain's helper, declared in no
// header. All real symbols in this revision's table.
void ObjLightswitch_DrawOpa(Actor* thisx, PlayState* play);
void ObjLightswitch_DrawXlu(Actor* thisx, PlayState* play);
f32 Environment_RandCentered(void);

// Display lists in gameplay_keep.
extern Gfx gBombCapDL[];
extern Gfx gBombBodyDL[];
extern Gfx gRaindropDL[];
extern Gfx gEffShockwaveDL[];

// THE HELD ITEM. The player's draw places the item above his hands, spins it by the frame
// counter and hands the draw to the item table; the group is opened on both lists round that
// call, because which list an item draws to is the table entry's choice.
// @recomp Patched to give the item the player holds up a group of its own.
RECOMP_PATCH void Player_DrawGetItemImpl(PlayState* play, Player* this, Vec3f* refPos, s32 drawIdPlusOne) {
    f32 height = (this->exchangeItemId != EXCH_ITEM_NONE) ? 6.0f : 14.0f;
    // @recomp
    u32 id = ITEM_TRANSFORM_ID_START + (((u32)drawIdPlusOne - 1U) & (ITEM_TRANSFORM_ID_COUNT - 1U));

    OPEN_DISPS(play->state.gfxCtx, "../z_player_lib.c", 2401);

    gSegments[6] = OS_K0_TO_PHYSICAL(this->giObjectSegment);

    gSPSegment(POLY_OPA_DISP++, 0x06, this->giObjectSegment);
    gSPSegment(POLY_XLU_DISP++, 0x06, this->giObjectSegment);

    Matrix_Translate(refPos->x + (3.3f * Math_SinS(this->actor.shape.rot.y)), refPos->y + height,
                     refPos->z + ((3.3f + (IREG(90) / 10.0f)) * Math_CosS(this->actor.shape.rot.y)), MTXMODE_NEW);
    Matrix_RotateZYX(0, play->gameplayFrames * 1000, 0, MTXMODE_APPLY);
    Matrix_Scale(0.2f, 0.2f, 0.2f, MTXMODE_APPLY);

    // @recomp
    gEXMatrixGroupDecomposedNormal(POLY_OPA_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
    gEXMatrixGroupDecomposedNormal(POLY_XLU_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);

    GetItem_Draw(play, drawIdPlusOne - 1);

    // @recomp
    gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);
    gEXPopMatrixGroup(POLY_XLU_DISP++, G_MTX_MODELVIEW);

    CLOSE_DISPS(play->state.gfxCtx, "../z_player_lib.c", 2421);
}

// THE BOMB draws two matrices in one frame, the cap and the body, so the whole-actor rule steps
// aside: one group round both, paired in order. The body's rotation is the billboard's
// (Matrix_ReplaceRotation), which the phase 39 tracking holds on a cut.
// @recomp Patched to give the bomb's two matrices one group.
RECOMP_PATCH void EnBom_Draw(Actor* thisx, PlayState* play) {
    s32 pad;
    EnBom* this = (EnBom*)thisx;
    u32 actorId;

    OPEN_DISPS(play->state.gfxCtx, "../z_en_bom.c", 913);

    if (thisx->params == BOMB_BODY) {
        Gfx_SetupDL_25Opa(play->state.gfxCtx);
        Matrix_ReplaceRotation(&play->billboardMtxF);
        func_8002EBCC(thisx, play, 0);

        // @recomp
        actorId = tag_actor_id(thisx);
        if (actorId != TRANSFORM_ID_NONE) {
            gEXMatrixGroupDecomposedNormal(POLY_OPA_DISP++, actorId + ACTOR_TRANSFORM_WHOLE_OPA_OFFSET, G_EX_PUSH,
                                           G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
        }

        MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_en_bom.c", 928);
        gSPDisplayList(POLY_OPA_DISP++, gBombCapDL);
        Matrix_RotateZYX(0x4000, 0, 0, MTXMODE_APPLY);
        MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_en_bom.c", 934);
        gDPPipeSync(POLY_OPA_DISP++);
        gDPSetEnvColor(POLY_OPA_DISP++, (s16)this->flashIntensity, 0, 40, 255);
        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, (s16)this->flashIntensity, 0, 40, 255);
        gSPDisplayList(POLY_OPA_DISP++, gBombBodyDL);

        // @recomp
        POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);

        Collider_UpdateSpheres(0, &this->explosionCollider);
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_en_bom.c", 951);
}

// THE LIGHT SWITCH (the sun-faced floor switch) draws three matrices per list every frame, the
// face and two rings of flame turning against each other, so the whole-actor rule steps aside.
// Its dispatcher is reproduced with one group per list round the draw it calls; the two draws
// themselves, which read a texture table static to the overlay, are not touched.
// @recomp Patched to give the switch's three matrices one group per list.
RECOMP_PATCH void ObjLightswitch_Draw(Actor* thisx, PlayState* play) {
    ObjLightswitch* this = (ObjLightswitch*)thisx;
    s32 alpha = this->alpha >> 6 & 0xFF;
    u32 actorId;

    if (PARAMS_GET_U(this->actor.params, 0, 1) == 1) {
        Collider_UpdateSpheres(0, &this->collider);
    }

    // @recomp
    actorId = tag_actor_id(thisx);

    // The game's own condition reads `== OBJLIGHTSWITCH_TYPE_BURN && (alpha > 0 || alpha < 255)`
    // (z_obj_lightswitch.c), and the second half is true for every alpha, so the burn type always
    // takes the translucent draw. Written as what it evaluates to, which is what the game does, so
    // the compiler's always-true warning has nothing to say; `alpha` is still read for the same
    // reason the original reads it, which is none.
    (void)alpha;
    if (PARAMS_GET_U(this->actor.params, 4, 2) == OBJLIGHTSWITCH_TYPE_BURN) {
        if (actorId != TRANSFORM_ID_NONE) {
            OPEN_DISPS(play->state.gfxCtx, "../z_obj_lightswitch.c", 0);
            gEXMatrixGroupDecomposedNormal(POLY_XLU_DISP++, actorId + ACTOR_TRANSFORM_WHOLE_XLU_OFFSET, G_EX_PUSH,
                                           G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
            CLOSE_DISPS(play->state.gfxCtx, "../z_obj_lightswitch.c", 0);
        }
        ObjLightswitch_DrawXlu(thisx, play);
        if (actorId != TRANSFORM_ID_NONE) {
            OPEN_DISPS(play->state.gfxCtx, "../z_obj_lightswitch.c", 0);
            gEXPopMatrixGroup(POLY_XLU_DISP++, G_MTX_MODELVIEW);
            CLOSE_DISPS(play->state.gfxCtx, "../z_obj_lightswitch.c", 0);
        }
    } else {
        if (actorId != TRANSFORM_ID_NONE) {
            OPEN_DISPS(play->state.gfxCtx, "../z_obj_lightswitch.c", 0);
            gEXMatrixGroupDecomposedNormal(POLY_OPA_DISP++, actorId + ACTOR_TRANSFORM_WHOLE_OPA_OFFSET, G_EX_PUSH,
                                           G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
            CLOSE_DISPS(play->state.gfxCtx, "../z_obj_lightswitch.c", 0);
        }
        ObjLightswitch_DrawOpa(thisx, play);
        if (actorId != TRANSFORM_ID_NONE) {
            OPEN_DISPS(play->state.gfxCtx, "../z_obj_lightswitch.c", 0);
            gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);
            CLOSE_DISPS(play->state.gfxCtx, "../z_obj_lightswitch.c", 0);
        }
    }
}

// RAIN. Every drop and every ring on the ground is placed at RANDOM each frame, so there is
// nothing between two frames to interpolate toward: left to the renderer's own matching, two
// random positions with the same draw call get paired and every drop becomes a streak between
// them. The drops and the rings each get a group with every component held, so a drop is drawn
// where the game put it this frame and nowhere else. (The reference project's rain is the same
// answer for the same reason.) The version condition on the camera test is this revision's.
// @recomp Patched to hold the rain's random matrices.
RECOMP_PATCH void Environment_DrawRain(PlayState* play, View* view, GraphicsContext* gfxCtx) {
    s16 i;
    s32 pad;
    Vec3f vec;
    f32 temp1;
    f32 temp2;
    f32 temp3;
    f32 length;
    f32 rotX;
    f32 rotY;
    f32 x50;
    f32 y50;
    f32 z50;
    f32 x280;
    f32 z280;
    Vec3f unused = { 0.0f, 0.0f, 0.0f };
    Vec3f windDirection = { 0.0f, 0.0f, 0.0f };
    Player* player = GET_PLAYER(play);

    if (!(play->cameraPtrs[CAM_ID_MAIN]->stateFlags & CAM_STATE_CAMERA_IN_WATER)) {
        OPEN_DISPS(gfxCtx, "../z_kankyo.c", 2799);

        vec.x = view->at.x - view->eye.x;
        vec.y = view->at.y - view->eye.y;
        vec.z = view->at.z - view->eye.z;

        length = sqrtf(SQXYZ(vec));

        temp1 = vec.x / length;
        temp2 = vec.y / length;
        temp3 = vec.z / length;

        x50 = view->eye.x + temp1 * 50.0f;
        y50 = view->eye.y + temp2 * 50.0f;
        z50 = view->eye.z + temp3 * 50.0f;

        x280 = view->eye.x + temp1 * 280.0f;
        z280 = view->eye.z + temp3 * 280.0f;

        if (play->envCtx.precipitation[PRECIP_RAIN_CUR]) {
            gDPPipeSync(POLY_XLU_DISP++);
            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 150, 255, 255, 30);
            POLY_XLU_DISP = Gfx_SetupDL(POLY_XLU_DISP, SETUPDL_20);
        }

        // @recomp The drops' group: every component held.
        gEXMatrixGroupDecomposedSkipAll(POLY_XLU_DISP++, RAIN_TRANSFORM_ID, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);

        // draw rain drops
        for (i = 0; i < play->envCtx.precipitation[PRECIP_RAIN_CUR]; i++) {
            temp2 = Rand_ZeroOne();
            temp1 = Rand_ZeroOne();
            temp3 = Rand_ZeroOne();

            Matrix_Translate((temp2 - 0.7f) * 100.0f + x50, (temp1 - 0.7f) * 100.0f + y50,
                             (temp3 - 0.7f) * 100.0f + z50, MTXMODE_NEW);

            windDirection.x = play->envCtx.windDirection.x;
            windDirection.y = play->envCtx.windDirection.y;
            windDirection.z = play->envCtx.windDirection.z;

            vec.x = windDirection.x;
            vec.y = windDirection.y + 500.0f + Rand_ZeroOne() * 200.0f;
            vec.z = windDirection.z;
            length = sqrtf(SQXZ(vec));

            gSPMatrix(POLY_XLU_DISP++, &D_01000000, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);
            rotX = Math_Atan2F(length, -vec.y);
            rotY = Math_Atan2F(vec.z, vec.x);
            Matrix_RotateY(-rotY, MTXMODE_APPLY);
            Matrix_RotateX(M_PI / 2 - rotX, MTXMODE_APPLY);
            Matrix_Scale(0.4f, 1.2f, 0.4f, MTXMODE_APPLY);
            MATRIX_FINALIZE_AND_LOAD(POLY_XLU_DISP++, gfxCtx, "../z_kankyo.c", 2887);
            gSPDisplayList(POLY_XLU_DISP++, gRaindropDL);
        }

        // @recomp
        gEXPopMatrixGroup(POLY_XLU_DISP++, G_MTX_MODELVIEW);

        // draw droplet rings on the ground
        if (player->actor.world.pos.y < view->eye.y) {
            u8 materialFlag = false;

            // @recomp The rings' group: every component held.
            gEXMatrixGroupDecomposedSkipAll(POLY_XLU_DISP++, RAIN_RINGS_TRANSFORM_ID, G_EX_PUSH, G_MTX_MODELVIEW,
                                            G_EX_EDIT_NONE);

            for (i = 0; i < play->envCtx.precipitation[PRECIP_RAIN_CUR]; i++) {
                if (!materialFlag) {
                    Gfx_SetupDL_25Xlu(gfxCtx);
                    gDPSetEnvColor(POLY_XLU_DISP++, 155, 155, 155, 0);
                    gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 255, 120);
                    materialFlag++;
                }

                Matrix_Translate(Environment_RandCentered() * 280.0f + x280, player->actor.world.pos.y + 2.0f,
                                 Environment_RandCentered() * 280.0f + z280, MTXMODE_NEW);

                if ((LINK_IS_ADULT && ((player->actor.world.pos.y + 2.0f - view->eye.y) > -48.0f)) ||
                    (!LINK_IS_ADULT && ((player->actor.world.pos.y + 2.0f - view->eye.y) > -30.0f))) {
                    Matrix_Scale(0.02f, 0.02f, 0.02f, MTXMODE_APPLY);
                } else {
                    Matrix_Scale(0.1f, 0.1f, 0.1f, MTXMODE_APPLY);
                }

                MATRIX_FINALIZE_AND_LOAD(POLY_XLU_DISP++, gfxCtx, "../z_kankyo.c", 2940);
                gSPDisplayList(POLY_XLU_DISP++, gEffShockwaveDL);
            }

            // @recomp
            gEXPopMatrixGroup(POLY_XLU_DISP++, G_MTX_MODELVIEW);
        }

        CLOSE_DISPS(gfxCtx, "../z_kankyo.c", 2946);
    }
}

// THE SWEEP RECORD. Sixty actors in the decompilation never call a skeleton draw and load two
// or more matrices somewhere in their file. Each was read for whether the loads happen in ONE
// frame and whether the thing moves.
//
// Tagged here: the bomb (two matrices a frame, moves), the light switch (three a list, the
// flame rings turn), and the rain (random every frame, must be held; not an actor).
//
// Covered already by phase 37's whole-actor rule, because they load ONE matrix per frame and
// the other loads in their files are other states or lists: the collectibles (`EnItem00`), the
// shop's items (`En_GirlA`), signs (`En_Kanban`, until cut), doors (`Door_Shutter`), the web
// walls (`Bg_Ydan_Sp`, whole), torches (`Obj_Syokudai`), bushes and rocks, and most of the
// `Bg_` scenery pieces in the list.
//
// Left, with the reason: the cucco chicks (`En_Nwc`, four lists interleaved in one buffer with
// three loads per chick, a rewrite of the buffer layout for a flock that lives in one scene);
// the floor web's burning pieces (`Bg_Ydan_Sp`, eight matrices for forty frames, and the
// broken state is only recognizable by an action function pointer, which a patch cannot compare
// against safely; needs a Deku stick this save does not have); the sign's cut pieces
// (`En_Kanban`, needs a sword); the cutscene effect actors (`Demo_Effect`, `Demo_Kankyo`,
// `Demo_6K`, `Demo_Gt`, `Demo_Kekkai`, `Object_Kankyo`), which are cutscene-only and drawn
// through billboards the phase 39 tracking already holds on cuts; the bosses and their fires
// (`Boss_Mo`, `En_Fhg_Fire`, `Efc_Erupc`, `Magic_Dark`), which are adult-era scenes no run in
// this plan reaches; and the explosion (`En_Clear_Tag`, `En_Bombf`), effects drawn through
// billboards that the tracking holds.
