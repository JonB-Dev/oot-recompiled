// Tag every actor drawn through the common path, so the renderer can interpolate it between game
// frames: the actor's own matrix, and each limb of a skeleton drawn to the opaque list.
//
// TWO TECHNIQUES, both the reference project's.
//
// 1. `Actor_Draw` is where every actor's drawing begins, and it does not know what the actor will
//    draw. So it reserves two no-op commands in each of the opaque and translucent lists before
//    calling the actor's draw, and afterward counts the matrices the actor wrote. An actor that
//    loaded exactly one matrix in a list (a bush, a sign, a rupee, most of what stands around in
//    a scene) gets that slot rewritten as a group with the actor's ID and a pop appended, and the
//    whole draw interpolates as one rigid thing. An actor that wrote several matrices is a
//    skeleton, and its limbs are tagged one by one by the functions below; an actor that wrote
//    none is left alone. The no-ops stay no-ops in those cases and cost nothing.
//
// 2. The four skeleton draws to the opaque list (`SkelAnime_DrawOpa` and `DrawFlexOpa` for the
//    root limb, `DrawLimbOpa` and `DrawFlexLimbOpa` for the rest, recursively) each load one
//    matrix per limb. Each load is wrapped in a group named by the actor's ID plus the limb's
//    index, and the limb's post-draw callback, which is where a held sword or a shield is drawn,
//    in a second group of its own. The `arg` these functions carry is the actor by convention,
//    and the native registry confirms it before an ID is handed out; a skeleton drawn with some
//    other `arg` goes out untagged, as it did before this plan.
//
// The Lod, Gfx and skin variants are phase 38's, in skeleton_tagging_lod.c, with these helpers.
//
// Every function is reproduced from the decompilation (z_actor.c and z_skelanime.c at the pinned
// commit) with the tagging lines marked `@recomp`; a patch REPLACES its function, so anything
// dropped here would simply stop happening.

#include "patches.h"
#include "tagging_helpers.h"
#include "effect_lights.h"

#include "actor.h"
#include "animation.h"
#include "effect.h"
#include "fault.h"
#include "gfx.h"
#include "light.h"
#include "play_state.h"
#include "printf.h"
#include "segmented_address.h"
#include "sys_matrix.h"
#include "translation.h"

// A real symbol in this revision's table, declared in no header (z_actor.c keeps it to itself).
void Actor_FaultPrint(Actor* actor, char* command);

// How many matrices the actor loaded at the top level of a list between `start` and `end`. The
// two word group commands the limb functions wrote inside that range begin with the renderer's
// own opcode and carry an ID in their second word, and neither reads as a matrix load.
static s32 count_matrix_loads(Gfx* start, Gfx* end) {
    s32 count = 0;
    Gfx* cur = start;

    while (cur != end) {
        if ((cur->words.w0 >> 24) == G_MTX) {
            count++;
        }
        cur++;
    }
    return count;
}

// Fill the slots `Actor_Draw` reserved when the actor's drawing was a single matrix per list.
static void tag_whole_actor(PlayState* play, Actor* actor, Gfx* opaSlot, Gfx* xluSlot) {
    s32 opaLoads;
    s32 xluLoads;
    u32 actorId;

    OPEN_DISPS(play->state.gfxCtx, "../z_actor.c", 0);

    opaLoads = count_matrix_loads(opaSlot, POLY_OPA_DISP);
    xluLoads = count_matrix_loads(xluSlot, POLY_XLU_DISP);

    // At least one matrix in total and at most one per list, or the actor is a skeleton (tagged
    // limb by limb already) or drew nothing that moves.
    if ((opaLoads == 1 || xluLoads == 1) && opaLoads <= 1 && xluLoads <= 1) {
        actorId = tag_actor_id(actor);
        if (actorId != TRANSFORM_ID_NONE) {
            if (opaLoads == 1) {
                // No increment: the slot is written in place, two words, and the pop goes at
                // the end of what the actor wrote.
                gEXMatrixGroupDecomposedNormal(opaSlot, actorId + ACTOR_TRANSFORM_WHOLE_OPA_OFFSET, G_EX_PUSH,
                                               G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
                gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);
            }
            if (xluLoads == 1) {
                gEXMatrixGroupDecomposedNormal(xluSlot, actorId + ACTOR_TRANSFORM_WHOLE_XLU_OFFSET, G_EX_PUSH,
                                               G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
                gEXPopMatrixGroup(POLY_XLU_DISP++, G_MTX_MODELVIEW);
            }
        }
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_actor.c", 0);
}

// @recomp Patched to reserve a tag slot in each list before the actor draws and fill it after.
RECOMP_PATCH void Actor_Draw(PlayState* play, Actor* actor) {
    FaultClient faultClient;
    Lights* lights;
    Gfx* opaSlot;
    Gfx* xluSlot;

    Fault_AddClient(&faultClient, Actor_FaultPrint, actor, "Actor_draw");

    OPEN_DISPS(play->state.gfxCtx, "../z_actor.c", 6035);

    lights = LightContext_NewLights(&play->lightCtx, play->state.gfxCtx);

    // @recomp Without the Sun's Song's and the blue warp's lights while the traced lights give
    // them (patches/effect_lights.c).
    EffectLights_BindAll(play, lights, play->lightCtx.listHead,
                         (actor->flags & ACTOR_FLAG_IGNORE_POINT_LIGHTS) ? NULL : &actor->world.pos);
    Lights_Draw(lights, play->state.gfxCtx);

    if (actor->flags & ACTOR_FLAG_IGNORE_QUAKE) {
        Matrix_SetTranslateRotateYXZ(actor->world.pos.x + play->mainCamera.quakeOffset.x,
                                     actor->world.pos.y +
                                         ((actor->shape.yOffset * actor->scale.y) + play->mainCamera.quakeOffset.y),
                                     actor->world.pos.z + play->mainCamera.quakeOffset.z, &actor->shape.rot);
    } else {
        Matrix_SetTranslateRotateYXZ(actor->world.pos.x, actor->world.pos.y + (actor->shape.yOffset * actor->scale.y),
                                     actor->world.pos.z, &actor->shape.rot);
    }

    Matrix_Scale(actor->scale.x, actor->scale.y, actor->scale.z, MTXMODE_APPLY);
    Actor_SetObjectDependency(play, actor);

    gSPSegment(POLY_OPA_DISP++, 0x06, play->objectCtx.slots[actor->objectSlot].segment);
    gSPSegment(POLY_XLU_DISP++, 0x06, play->objectCtx.slots[actor->objectSlot].segment);

    if (actor->colorFilterTimer != 0) {
        Color_RGBA8 color = { 0, 0, 0, 255 };

        if (actor->colorFilterParams & COLORFILTER_COLORFLAG_GRAY) {
            color.r = color.g = color.b = COLORFILTER_GET_COLORINTENSITY(actor->colorFilterParams) | 7;
        } else if (actor->colorFilterParams & COLORFILTER_COLORFLAG_RED) {
            color.r = COLORFILTER_GET_COLORINTENSITY(actor->colorFilterParams) | 7;
        } else {
            color.b = COLORFILTER_GET_COLORINTENSITY(actor->colorFilterParams) | 7;
        }

        if (actor->colorFilterParams & COLORFILTER_BUFFLAG_XLU) {
            func_80026860(play, &color, actor->colorFilterTimer, COLORFILTER_GET_DURATION(actor->colorFilterParams));
        } else {
            func_80026400(play, &color, actor->colorFilterTimer, COLORFILTER_GET_DURATION(actor->colorFilterParams));
        }
    }

    // @recomp Two no-ops in each list, the size of one group command, to be rewritten once it is
    // known what the actor drew.
    opaSlot = POLY_OPA_DISP;
    xluSlot = POLY_XLU_DISP;
    gDPNoOp(POLY_OPA_DISP++);
    gDPNoOp(POLY_OPA_DISP++);
    gDPNoOp(POLY_XLU_DISP++);
    gDPNoOp(POLY_XLU_DISP++);

    actor->draw(actor, play);

    // @recomp Fill the slots if the actor's drawing was one matrix per list.
    tag_whole_actor(play, actor, opaSlot, xluSlot);

    if (actor->colorFilterTimer != 0) {
        if (actor->colorFilterParams & COLORFILTER_BUFFLAG_XLU) {
            func_80026A6C(play);
        } else {
            func_80026608(play);
        }
    }

    if (actor->shape.shadowDraw != NULL) {
        actor->shape.shadowDraw(actor, lights, play);
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_actor.c", 6119);

    Fault_RemoveClient(&faultClient);
}

/**
 * Draw a limb of type `StandardLimb` to the polyOpa buffer
 */
// @recomp Patched to wrap the limb's matrix load, and its post-draw callback, in transform groups.
RECOMP_PATCH void SkelAnime_DrawLimbOpa(PlayState* play, s32 limbIndex, void** skeleton, Vec3s* jointTable,
                                        OverrideLimbDrawOpa overrideLimbDraw, PostLimbDrawOpa postLimbDraw,
                                        void* arg) {
    StandardLimb* limb;
    Gfx* dList;
    Vec3f pos;
    Vec3s rot;
    u32 actorId;

    OPEN_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1076);
    Matrix_Push();

    limb = (StandardLimb*)SEGMENTED_TO_VIRTUAL(skeleton[limbIndex]);
    limbIndex++;
    rot = jointTable[limbIndex];
    pos.x = limb->jointPos.x;
    pos.y = limb->jointPos.y;
    pos.z = limb->jointPos.z;
    dList = limb->dList;

    // @recomp The limb's group, opened before the override so a replaced limb is tagged too.
    actorId = tag_actor_id(arg);
    POLY_OPA_DISP = tag_push_limb(POLY_OPA_DISP, actorId, limbIndex);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, limbIndex, &dList, &pos, &rot, arg)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (dList != NULL) {
            MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_skelanime.c", 1103);
            gSPDisplayList(POLY_OPA_DISP++, dList);
        }
    }

    // @recomp Close the limb's group and open the one for whatever the post-draw callback draws.
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);
    POLY_OPA_DISP = tag_push_post_limb(POLY_OPA_DISP, actorId, limbIndex);

    if (postLimbDraw != NULL) {
        postLimbDraw(play, limbIndex, &dList, &rot, arg);
    }

    // @recomp
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);

    if (limb->child != LIMB_DONE) {
        SkelAnime_DrawLimbOpa(play, limb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg);
    }

    Matrix_Pop();

    if (limb->sibling != LIMB_DONE) {
        SkelAnime_DrawLimbOpa(play, limb->sibling, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg);
    }
    CLOSE_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1121);
}

/**
 * Draw all limbs of type `StandardLimb` in a given skeleton to the polyOpa buffer
 */
// @recomp Patched to wrap the root limb's matrix load, and its post-draw callback, in transform groups.
RECOMP_PATCH void SkelAnime_DrawOpa(PlayState* play, void** skeleton, Vec3s* jointTable,
                                    OverrideLimbDrawOpa overrideLimbDraw, PostLimbDrawOpa postLimbDraw, void* arg) {
    StandardLimb* rootLimb;
    s32 pad;
    Gfx* dList;
    Vec3f pos;
    Vec3s rot;
    u32 actorId;

    if (skeleton == NULL) {
        PRINTF_COLOR_RED();
        PRINTF(T("Si2_draw():skelがNULLです。\n", "Si2_draw(): skel is NULL.\n"));
        PRINTF_RST();
        return;
    }

    OPEN_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1148);

    Matrix_Push();
    rootLimb = (StandardLimb*)SEGMENTED_TO_VIRTUAL(skeleton[0]);

    pos.x = jointTable[0].x;
    pos.y = jointTable[0].y;
    pos.z = jointTable[0].z;

    rot = jointTable[1];
    dList = rootLimb->dList;

    // @recomp
    actorId = tag_actor_id(arg);
    POLY_OPA_DISP = tag_push_limb(POLY_OPA_DISP, actorId, 1);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, 1, &dList, &pos, &rot, arg)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (dList != NULL) {
            MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_skelanime.c", 1176);
            gSPDisplayList(POLY_OPA_DISP++, dList);
        }
    }

    // @recomp
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);
    POLY_OPA_DISP = tag_push_post_limb(POLY_OPA_DISP, actorId, 1);

    if (postLimbDraw != NULL) {
        postLimbDraw(play, 1, &dList, &rot, arg);
    }

    // @recomp
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);

    if (rootLimb->child != LIMB_DONE) {
        SkelAnime_DrawLimbOpa(play, rootLimb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg);
    }

    Matrix_Pop();

    CLOSE_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1190);
}

/**
 * Draw a limb of type `StandardLimb` contained within a flexible skeleton to the polyOpa buffer
 */
// @recomp Patched to wrap the limb's matrix load, and its post-draw callback, in transform groups.
RECOMP_PATCH void SkelAnime_DrawFlexLimbOpa(PlayState* play, s32 limbIndex, void** skeleton, Vec3s* jointTable,
                                            OverrideLimbDrawOpa overrideLimbDraw, PostLimbDrawOpa postLimbDraw,
                                            void* arg, Mtx** limbMatrices) {
    StandardLimb* limb;
    Gfx* newDList;
    Gfx* limbDList;
    Vec3f pos;
    Vec3s rot;
    u32 actorId;

    OPEN_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1214);

    Matrix_Push();

    limb = (StandardLimb*)SEGMENTED_TO_VIRTUAL(skeleton[limbIndex]);
    limbIndex++;
    rot = jointTable[limbIndex];

    pos.x = limb->jointPos.x;
    pos.y = limb->jointPos.y;
    pos.z = limb->jointPos.z;

    newDList = limbDList = limb->dList;

    // @recomp
    actorId = tag_actor_id(arg);
    POLY_OPA_DISP = tag_push_limb(POLY_OPA_DISP, actorId, limbIndex);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, limbIndex, &newDList, &pos, &rot, arg)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (newDList != NULL) {
            MATRIX_TO_MTX(*limbMatrices, "../z_skelanime.c", 1242);
            gSPMatrix(POLY_OPA_DISP++, *limbMatrices, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_OPA_DISP++, newDList);
            (*limbMatrices)++;
        } else if (limbDList != NULL) {
            if (1) {}
            MATRIX_TO_MTX(*limbMatrices, "../z_skelanime.c", 1249);
            (*limbMatrices)++;
        }
    }

    // @recomp
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);
    POLY_OPA_DISP = tag_push_post_limb(POLY_OPA_DISP, actorId, limbIndex);

    if (postLimbDraw != NULL) {
        postLimbDraw(play, limbIndex, &limbDList, &rot, arg);
    }

    // @recomp
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);

    if (limb->child != LIMB_DONE) {
        SkelAnime_DrawFlexLimbOpa(play, limb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg,
                                  limbMatrices);
    }

    Matrix_Pop();

    if (limb->sibling != LIMB_DONE) {
        SkelAnime_DrawFlexLimbOpa(play, limb->sibling, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg,
                                  limbMatrices);
    }
    CLOSE_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1265);
}

/**
 * Draw all limbs of type `StandardLimb` in a given flexible skeleton to the polyOpa buffer
 * Limbs in a flexible skeleton have meshes that can stretch to line up with other limbs.
 * An array of matrices is dynamically allocated so each limb can access any transform to ensure its meshes line up.
 */
// @recomp Patched to wrap the root limb's matrix load, and its post-draw callback, in transform groups.
RECOMP_PATCH void SkelAnime_DrawFlexOpa(PlayState* play, void** skeleton, Vec3s* jointTable, s32 dListCount,
                                        OverrideLimbDrawOpa overrideLimbDraw, PostLimbDrawOpa postLimbDraw,
                                        void* arg) {
    StandardLimb* rootLimb;
    s32 pad;
    Gfx* newDList;
    Gfx* limbDList;
    Vec3f pos;
    Vec3s rot;
    Mtx* mtx = GRAPH_ALLOC(play->state.gfxCtx, dListCount * sizeof(Mtx));
    u32 actorId;

    if (skeleton == NULL) {
        PRINTF_COLOR_RED();
        PRINTF(T("Si2_draw_SV():skelがNULLです。\n", "Si2_draw_SV(): skel is NULL.\n"));
        PRINTF_RST();
        return;
    }

    OPEN_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1294);

    gSPSegment(POLY_OPA_DISP++, 0xD, mtx);

    Matrix_Push();

    rootLimb = SEGMENTED_TO_VIRTUAL(skeleton[0]);

    pos.x = jointTable[0].x;
    pos.y = jointTable[0].y;
    pos.z = jointTable[0].z;

    rot = jointTable[1];

    newDList = limbDList = rootLimb->dList;

    // @recomp
    actorId = tag_actor_id(arg);
    POLY_OPA_DISP = tag_push_limb(POLY_OPA_DISP, actorId, 1);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, 1, &newDList, &pos, &rot, arg)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (newDList != NULL) {
            MATRIX_TO_MTX(mtx, "../z_skelanime.c", 1327);
            gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_OPA_DISP++, newDList);
            mtx++;
        } else if (limbDList != NULL) {
            MATRIX_TO_MTX(mtx, "../z_skelanime.c", 1334);
            mtx++;
        }
    }

    // @recomp
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);
    POLY_OPA_DISP = tag_push_post_limb(POLY_OPA_DISP, actorId, 1);

    if (postLimbDraw != NULL) {
        postLimbDraw(play, 1, &limbDList, &rot, arg);
    }

    // @recomp
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);

    if (rootLimb->child != LIMB_DONE) {
        SkelAnime_DrawFlexLimbOpa(play, rootLimb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg,
                                  &mtx);
    }

    Matrix_Pop();
    CLOSE_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1347);
}
