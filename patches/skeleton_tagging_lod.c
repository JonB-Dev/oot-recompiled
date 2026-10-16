// Tag the remaining skeleton paths, with the helpers phase 37 wrote: the level of detail variants
// (the player's own path, through `Player_Draw`), the variants that write to a display list the
// caller hands in rather than to the opaque list (a fairy, anything drawn translucent), and the
// skin meshes (the horses), whose limbs are deformed on the CPU rather than posed by a matrix.
//
// The Lod and Gfx variants are the phase 37 functions with a different limb type or a different
// destination, and the tagging is the same: a group around each limb's matrix load, a second
// around its post-draw callback. Two of the originals opened their display list pointers only
// inside the block that loads the matrix; here the pointers are opened for the whole function so
// the tags can be written either side of that block, which changes nothing about what is written
// (outside a debug build the macros are a local copy of a pointer and no more).
//
// A SKIN MESH is different. `Skin_DrawImpl` loads the actor's one matrix before its loop and then
// draws every limb under it from vertices `Skin_ApplyAnimTransformations` already moved into
// place, so there is no per-limb matrix to name. The reference project's answer, reproduced
// here: inside each limb's group, push the identity onto the matrix stack (a load the renderer
// can attach the group to, leaving the transform as it was), draw the limb, pop it. The group
// interpolates the actor's position and rotation AND the vertices, because the limb's motion
// lives in its vertices; a mesh whose body slid smoothly while its legs jumped would tear at the
// joints. The plan's phrase for this path, "position and rotation skipped", describes the
// reference's treatment of a teleporting actor, not its skin path; recorded in issues.md.
//
// Every function is reproduced from the decompilation (z_skelanime.c and z_skin.c at the pinned
// commit) with the tagging lines marked `@recomp`.

#include "patches.h"
#include "tagging_helpers.h"

#include "actor.h"
#include "animation.h"
#include "gfx.h"
#include "play_state.h"
#include "printf.h"
#include "segmented_address.h"
#include "skin.h"
#include "skin_matrix.h"
#include "sys_matrix.h"
#include "translation.h"

// Defined in z_skin.c and declared in no header: the per-limb matrices of the skin being drawn.
extern MtxF gSkinLimbMatrices[];
// The game's own (z_player_lib.c): which of the two display lists the things in Link's hands are
// drawn from, set by his draw from the level of detail it chose.
extern s32 sDListsLodOffset;

/**
 * Draw a limb of type `LodLimb`
 * Near or far display list is specified via `lod`
 */
// @recomp Patched to wrap the limb's matrix load, and its post-draw callback, in transform groups.
RECOMP_PATCH void SkelAnime_DrawLimbLod(PlayState* play, s32 limbIndex, void** skeleton, Vec3s* jointTable,
                                        OverrideLimbDrawOpa overrideLimbDraw, PostLimbDrawOpa postLimbDraw,
                                        void* arg, s32 lod) {
    LodLimb* limb;
    Gfx* dList;
    Vec3f pos;
    Vec3s rot;
    u32 actorId;

    OPEN_DISPS(play->state.gfxCtx, "../z_skelanime.c", 773);

    Matrix_Push();
    limb = (LodLimb*)SEGMENTED_TO_VIRTUAL(skeleton[limbIndex]);
    limbIndex++;
    rot = jointTable[limbIndex];

    pos.x = limb->jointPos.x;
    pos.y = limb->jointPos.y;
    pos.z = limb->jointPos.z;

    dList = limb->dLists[lod];

    // @recomp
    actorId = tag_actor_id(arg);
    POLY_OPA_DISP = tag_push_limb(POLY_OPA_DISP, actorId, limbIndex);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, limbIndex, &dList, &pos, &rot, arg)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (dList != NULL) {
            MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_skelanime.c", 805);
            gSPDisplayList(POLY_OPA_DISP++, dList);
        }
    }

    // @recomp
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);
    POLY_OPA_DISP = tag_push_post_limb(POLY_OPA_DISP, actorId, limbIndex);

    if (postLimbDraw != NULL) {
        postLimbDraw(play, limbIndex, &dList, &rot, arg);
    }

    // @recomp
    POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);

    if (limb->child != LIMB_DONE) {
        SkelAnime_DrawLimbLod(play, limb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, lod);
    }

    Matrix_Pop();

    if (limb->sibling != LIMB_DONE) {
        SkelAnime_DrawLimbLod(play, limb->sibling, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, lod);
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_skelanime.c", 821);
}

/**
 * Draw all limbs of type `LodLimb` in a given skeleton
 * Near or far display list is specified via `lod`
 */
// @recomp Patched to wrap the root limb's matrix load, and its post-draw callback, in transform groups.
RECOMP_PATCH void SkelAnime_DrawLod(PlayState* play, void** skeleton, Vec3s* jointTable,
                                    OverrideLimbDrawOpa overrideLimbDraw, PostLimbDrawOpa postLimbDraw, void* arg,
                                    s32 lod) {
    LodLimb* rootLimb;
    s32 pad;
    Gfx* dList;
    Vec3f pos;
    Vec3s rot;
    u32 actorId;

    if (skeleton == NULL) {
        PRINTF_COLOR_RED();
        PRINTF(T("Si2_Lod_draw():skelがNULLです。\n", "Si2_Lod_draw(): skel is NULL.\n"));
        PRINTF_RST();
        return;
    }

    OPEN_DISPS(play->state.gfxCtx, "../z_skelanime.c", 849);

    Matrix_Push();

    rootLimb = (LodLimb*)SEGMENTED_TO_VIRTUAL(skeleton[0]);
    pos.x = jointTable[0].x;
    pos.y = jointTable[0].y;
    pos.z = jointTable[0].z;

    rot = jointTable[1];
    dList = rootLimb->dLists[lod];

    // @recomp
    actorId = tag_actor_id(arg);
    POLY_OPA_DISP = tag_push_limb(POLY_OPA_DISP, actorId, 1);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, 1, &dList, &pos, &rot, arg)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (dList != NULL) {
            MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_skelanime.c", 881);
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
        SkelAnime_DrawLimbLod(play, rootLimb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, lod);
    }

    Matrix_Pop();

    CLOSE_DISPS(play->state.gfxCtx, "../z_skelanime.c", 894);
}

/**
 * Draw a limb of type `LodLimb` contained within a flexible skeleton
 * Near or far display list is specified via `lod`
 */
// @recomp Patched to wrap the limb's matrix load, and its post-draw callback, in transform groups.
// The display list pointers are opened for the whole function rather than for the load alone.
RECOMP_PATCH void SkelAnime_DrawFlexLimbLod(PlayState* play, s32 limbIndex, void** skeleton, Vec3s* jointTable,
                                            OverrideLimbDrawOpa overrideLimbDraw, PostLimbDrawOpa postLimbDraw,
                                            void* arg, s32 lod, Mtx** mtx) {
    LodLimb* limb;
    Gfx* newDList;
    Gfx* limbDList;
    Vec3f pos;
    Vec3s rot;
    u32 actorId;

    OPEN_DISPS(play->state.gfxCtx, "../z_skelanime.c", 946);

    Matrix_Push();

    limb = (LodLimb*)SEGMENTED_TO_VIRTUAL(skeleton[limbIndex]);
    limbIndex++;

    rot = jointTable[limbIndex];

    pos.x = limb->jointPos.x;
    pos.y = limb->jointPos.y;
    pos.z = limb->jointPos.z;

    newDList = limbDList = limb->dLists[lod];

    // @recomp
    actorId = tag_actor_id(arg);
    POLY_OPA_DISP = tag_push_limb(POLY_OPA_DISP, actorId, limbIndex);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, limbIndex, &newDList, &pos, &rot, arg)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (newDList != NULL) {
            MATRIX_TO_MTX(*mtx, "../z_skelanime.c", 945);
            gSPMatrix(POLY_OPA_DISP++, *mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_OPA_DISP++, newDList);
            (*mtx)++;
        } else if (limbDList != NULL) {
            if (1) {}
            MATRIX_TO_MTX(*mtx, "../z_skelanime.c", 954);
            (*mtx)++;
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
        SkelAnime_DrawFlexLimbLod(play, limb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, lod,
                                  mtx);
    }

    Matrix_Pop();

    if (limb->sibling != LIMB_DONE) {
        SkelAnime_DrawFlexLimbLod(play, limb->sibling, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, lod,
                                  mtx);
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_skelanime.c", 949);
}

/**
 * Draws all limbs of type `LodLimb` in a given flexible skeleton
 * Limbs in a flexible skeleton have meshes that can stretch to line up with other limbs.
 * An array of matrices is dynamically allocated so each limb can access any transform to ensure its meshes line up.
 */
// @recomp Patched to wrap the root limb's matrix load, and its post-draw callback, in transform groups.
RECOMP_PATCH void SkelAnime_DrawFlexLod(PlayState* play, void** skeleton, Vec3s* jointTable, s32 dListCount,
                                        OverrideLimbDrawOpa overrideLimbDraw, PostLimbDrawOpa postLimbDraw,
                                        void* arg, s32 lod) {
    LodLimb* rootLimb;
    s32 pad;
    Gfx* newDList;
    Gfx* limbDList;
    Vec3f pos;
    Vec3s rot;
    Mtx* mtx = GRAPH_ALLOC(play->state.gfxCtx, dListCount * sizeof(Mtx));
    u32 actorId;

    // @recomp LINK AT HIS NEAR DETAIL, WHATEVER THE DISTANCE (the user, 2026-09-24: "links LOD
    // drops res when he is far away from camera ... he should always be whatever res he needs to
    // be. and not change"). His skeleton carries two display lists per limb and the game picks
    // the far one past about two of his own heights (Player_Draw, the projected z against 160),
    // which on a console at 320 by 240 nobody saw and on a large screen is a step down in the
    // middle of the picture. This function is where the choice lands, and it has exactly one
    // caller in the game, Player_DrawImpl, so it reaches Link and nothing else. The offset for
    // the things in his hands is set from the same choice by that caller, before this, and is
    // set back here so his sword and shield are near detail with him.
    lod = 0;
    sDListsLodOffset = 0;

    if (skeleton == NULL) {
        PRINTF_COLOR_RED();
        PRINTF(T("Si2_Lod_draw_SV():skelがNULLです。\n", "Si2_Lod_draw_SV(): skel is NULL.\n"));
        PRINTF_RST();
        return;
    }

    OPEN_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1000);

    gSPSegment(POLY_OPA_DISP++, 0xD, mtx);
    Matrix_Push();

    rootLimb = (LodLimb*)SEGMENTED_TO_VIRTUAL(skeleton[0]);
    pos.x = jointTable[0].x;
    pos.y = jointTable[0].y;
    pos.z = jointTable[0].z;

    rot = jointTable[1];

    newDList = limbDList = rootLimb->dLists[lod];

    // @recomp
    actorId = tag_actor_id(arg);
    POLY_OPA_DISP = tag_push_limb(POLY_OPA_DISP, actorId, 1);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, 1, &newDList, &pos, &rot, arg)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (newDList != NULL) {
            MATRIX_TO_MTX(mtx, "../z_skelanime.c", 1033);
            gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(POLY_OPA_DISP++, newDList);
            mtx++;
        } else if (limbDList != NULL) {
            MATRIX_TO_MTX(mtx, "../z_skelanime.c", 1040);
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
        SkelAnime_DrawFlexLimbLod(play, rootLimb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, lod,
                                  &mtx);
    }

    Matrix_Pop();

    CLOSE_DISPS(play->state.gfxCtx, "../z_skelanime.c", 1053);
}

/**
 * Draw a limb of type `StandardLimb` to the specified display buffer
 */
// @recomp Patched to wrap the limb's matrix load, and its post-draw callback, in transform groups
// written to the caller's buffer.
RECOMP_PATCH Gfx* SkelAnime_DrawLimb(PlayState* play, s32 limbIndex, void** skeleton, Vec3s* jointTable,
                                     OverrideLimbDraw overrideLimbDraw, PostLimbDraw postLimbDraw, void* arg,
                                     Gfx* gfx) {
    StandardLimb* limb;
    Gfx* dList;
    Vec3f pos;
    Vec3s rot;
    u32 actorId;

    Matrix_Push();

    limb = (StandardLimb*)SEGMENTED_TO_VIRTUAL(skeleton[limbIndex]);
    limbIndex++;

    rot = jointTable[limbIndex];

    pos.x = limb->jointPos.x;
    pos.y = limb->jointPos.y;
    pos.z = limb->jointPos.z;

    dList = limb->dList;

    // @recomp
    actorId = tag_actor_id(arg);
    gfx = tag_push_limb(gfx, actorId, limbIndex);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, limbIndex, &dList, &pos, &rot, arg, &gfx)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (dList != NULL) {
            MATRIX_FINALIZE_AND_LOAD(gfx++, play->state.gfxCtx, "../z_skelanime.c", 1489);
            gSPDisplayList(gfx++, dList);
        }
    }

    // @recomp
    gfx = tag_pop(gfx, actorId);
    gfx = tag_push_post_limb(gfx, actorId, limbIndex);

    if (postLimbDraw != NULL) {
        postLimbDraw(play, limbIndex, &dList, &rot, arg, &gfx);
    }

    // @recomp
    gfx = tag_pop(gfx, actorId);

    if (limb->child != LIMB_DONE) {
        gfx = SkelAnime_DrawLimb(play, limb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, gfx);
    }

    Matrix_Pop();

    if (limb->sibling != LIMB_DONE) {
        gfx = SkelAnime_DrawLimb(play, limb->sibling, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, gfx);
    }

    return gfx;
}

/**
 * Draw all limbs of type `StandardLimb` in a given skeleton to the specified display buffer
 */
// @recomp Patched to wrap the root limb's matrix load, and its post-draw callback, in transform groups
// written to the caller's buffer.
RECOMP_PATCH Gfx* SkelAnime_Draw(PlayState* play, void** skeleton, Vec3s* jointTable, OverrideLimbDraw overrideLimbDraw,
                                 PostLimbDraw postLimbDraw, void* arg, Gfx* gfx) {
    StandardLimb* rootLimb;
    s32 pad;
    Gfx* dList;
    Vec3f pos;
    Vec3s rot;
    u32 actorId;

    if (skeleton == NULL) {
        PRINTF_COLOR_RED();
        PRINTF(T("Si2_draw2():skelがNULLです。NULLを返します。\n", "Si2_draw2(): skel is NULL. Returns NULL.\n"));
        PRINTF_RST();
        return NULL;
    }

    Matrix_Push();

    rootLimb = (StandardLimb*)SEGMENTED_TO_VIRTUAL(skeleton[0]);

    pos.x = jointTable[0].x;
    pos.y = jointTable[0].y;
    pos.z = jointTable[0].z;

    rot = jointTable[1];

    dList = rootLimb->dList;

    // @recomp
    actorId = tag_actor_id(arg);
    gfx = tag_push_limb(gfx, actorId, 1);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, 1, &dList, &pos, &rot, arg, &gfx)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (dList != NULL) {
            MATRIX_FINALIZE_AND_LOAD(gfx++, play->state.gfxCtx, "../z_skelanime.c", 1558);
            gSPDisplayList(gfx++, dList);
        }
    }

    // @recomp
    gfx = tag_pop(gfx, actorId);
    gfx = tag_push_post_limb(gfx, actorId, 1);

    if (postLimbDraw != NULL) {
        postLimbDraw(play, 1, &dList, &rot, arg, &gfx);
    }

    // @recomp
    gfx = tag_pop(gfx, actorId);

    if (rootLimb->child != LIMB_DONE) {
        gfx = SkelAnime_DrawLimb(play, rootLimb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, gfx);
    }

    Matrix_Pop();

    return gfx;
}

/**
 * Draw a limb of type `StandardLimb` contained within a flexible skeleton to the specified display buffer
 */
// @recomp Patched to wrap the limb's matrix load, and its post-draw callback, in transform groups
// written to the caller's buffer.
RECOMP_PATCH Gfx* SkelAnime_DrawFlexLimb(PlayState* play, s32 limbIndex, void** skeleton, Vec3s* jointTable,
                                         OverrideLimbDraw overrideLimbDraw, PostLimbDraw postLimbDraw, void* arg,
                                         Mtx** mtx, Gfx* gfx) {
    StandardLimb* limb;
    Gfx* newDList;
    Gfx* limbDList;
    Vec3f pos;
    Vec3s rot;
    u32 actorId;

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
    gfx = tag_push_limb(gfx, actorId, limbIndex);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, limbIndex, &newDList, &pos, &rot, arg, &gfx)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (newDList != NULL) {
            MATRIX_TO_MTX(*mtx, "../z_skelanime.c", 1623);
            gSPMatrix(gfx++, *mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(gfx++, newDList);
            (*mtx)++;
        } else if (limbDList != NULL) {
            MATRIX_TO_MTX(*mtx, "../z_skelanime.c", 1630);
            (*mtx)++;
        }
    }

    // @recomp
    gfx = tag_pop(gfx, actorId);
    gfx = tag_push_post_limb(gfx, actorId, limbIndex);

    if (postLimbDraw != NULL) {
        postLimbDraw(play, limbIndex, &limbDList, &rot, arg, &gfx);
    }

    // @recomp
    gfx = tag_pop(gfx, actorId);

    if (limb->child != LIMB_DONE) {
        gfx = SkelAnime_DrawFlexLimb(play, limb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg, mtx,
                                     gfx);
    }

    Matrix_Pop();

    if (limb->sibling != LIMB_DONE) {
        gfx = SkelAnime_DrawFlexLimb(play, limb->sibling, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg,
                                     mtx, gfx);
    }

    return gfx;
}

/**
 * Draw all limbs of type `StandardLimb` in a given flexible skeleton to the specified display buffer
 * Limbs in a flexible skeleton have meshes that can stretch to line up with other limbs.
 * An array of matrices is dynamically allocated so each limb can access any transform to ensure its meshes line up.
 */
// @recomp Patched to wrap the root limb's matrix load, and its post-draw callback, in transform groups
// written to the caller's buffer.
RECOMP_PATCH Gfx* SkelAnime_DrawFlex(PlayState* play, void** skeleton, Vec3s* jointTable, s32 dListCount,
                                     OverrideLimbDraw overrideLimbDraw, PostLimbDraw postLimbDraw, void* arg,
                                     Gfx* gfx) {
    StandardLimb* rootLimb;
    s32 pad;
    Gfx* newDList;
    Gfx* limbDList;
    Vec3f pos;
    Vec3s rot;
    Mtx* mtx = GRAPH_ALLOC(play->state.gfxCtx, dListCount * sizeof(*mtx));
    u32 actorId;

    if (skeleton == NULL) {
        PRINTF_COLOR_RED();
        PRINTF(T("Si2_draw2_SV():skelがNULLです。NULLを返します。\n", "Si2_draw2_SV(): skel is NULL. Returns NULL.\n"));
        PRINTF_RST();
        return NULL;
    }

    gSPSegment(gfx++, 0xD, mtx);
    Matrix_Push();
    rootLimb = (StandardLimb*)SEGMENTED_TO_VIRTUAL(skeleton[0]);

    pos.x = jointTable[0].x;
    pos.y = jointTable[0].y;
    pos.z = jointTable[0].z;

    rot = jointTable[1];

    newDList = limbDList = rootLimb->dList;

    // @recomp
    actorId = tag_actor_id(arg);
    gfx = tag_push_limb(gfx, actorId, 1);

    if ((overrideLimbDraw == NULL) || !overrideLimbDraw(play, 1, &newDList, &pos, &rot, arg, &gfx)) {
        Matrix_TranslateRotateZYX(&pos, &rot);
        if (newDList != NULL) {
            MATRIX_TO_MTX(mtx, "../z_skelanime.c", 1710);
            gSPMatrix(gfx++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gSPDisplayList(gfx++, newDList);
            mtx++;
        } else if (limbDList != NULL) {
            MATRIX_TO_MTX(mtx, "../z_skelanime.c", 1717);
            mtx++;
        }
    }

    // @recomp
    gfx = tag_pop(gfx, actorId);
    gfx = tag_push_post_limb(gfx, actorId, 1);

    if (postLimbDraw != NULL) {
        postLimbDraw(play, 1, &limbDList, &rot, arg, &gfx);
    }

    // @recomp
    gfx = tag_pop(gfx, actorId);

    if (rootLimb->child != LIMB_DONE) {
        gfx = SkelAnime_DrawFlexLimb(play, rootLimb->child, skeleton, jointTable, overrideLimbDraw, postLimbDraw, arg,
                                     &mtx, gfx);
    }

    Matrix_Pop();

    return gfx;
}

// @recomp Patched to give each skin limb a group of its own: the identity pushed onto the matrix
// stack inside the group is the load the renderer attaches the group to, and the vertices drawn
// under it are interpolated along with the actor's transform.
RECOMP_PATCH void Skin_DrawImpl(Actor* actor, PlayState* play, Skin* skin, SkinPostDraw postDraw,
                                SkinOverrideLimbDraw overrideLimbDraw, s32 setTranslation, s32 arg6, s32 drawFlags) {
    s32 i;
    s32 segmentType;
    SkinLimb** skeleton;
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Mtx* mtx;
    u32 actorId;

    OPEN_DISPS(gfxCtx, "../z_skin.c", 471);

    if (!(drawFlags & SKIN_DRAW_FLAG_CUSTOM_TRANSFORMS)) {
        Skin_ApplyAnimTransformations(skin, gSkinLimbMatrices, actor, setTranslation);
    }

    skeleton = SEGMENTED_TO_VIRTUAL(skin->skeletonHeader->segment);

    if (!(drawFlags & SKIN_DRAW_FLAG_CUSTOM_MATRIX)) {
        gSPMatrix(POLY_OPA_DISP++, &gIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        mtx = SkinMatrix_MtxFToNewMtx(gfxCtx, &skin->mtx);

        if (mtx == NULL) {
            goto close_disps;
        }

        gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }

    // @recomp
    actorId = tag_actor_id(actor);

    for (i = 0; i < skin->skeletonHeader->limbCount; i++) {
        s32 shouldDraw = true;

        if (overrideLimbDraw != NULL) {
            shouldDraw = overrideLimbDraw(actor, play, i, skin);
        }

        segmentType = ((SkinLimb*)SEGMENTED_TO_VIRTUAL(skeleton[i]))->segmentType;

        // @recomp Open the limb's group around a push of the identity, which leaves the
        // transform as it was and gives the group a matrix load to belong to.
        if (actorId != TRANSFORM_ID_NONE) {
            gSPMatrix(POLY_OPA_DISP++, &gIdentityMtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
        }
        POLY_OPA_DISP = tag_push_skin_limb(POLY_OPA_DISP, actorId, i);

        if (segmentType == SKIN_LIMB_TYPE_ANIMATED && shouldDraw == true) {
            Skin_DrawAnimatedLimb(gfxCtx, skin, i, arg6, drawFlags);
        } else if (segmentType == SKIN_LIMB_TYPE_NORMAL && shouldDraw == true) {
            Skin_DrawLimb(gfxCtx, skin, i, NULL, drawFlags);
        }

        // @recomp Pop both.
        if (actorId != TRANSFORM_ID_NONE) {
            gSPPopMatrix(POLY_OPA_DISP++, G_MTX_MODELVIEW);
        }
        POLY_OPA_DISP = tag_pop(POLY_OPA_DISP, actorId);
    }

    if (postDraw != NULL) {
        postDraw(actor, play, skin);
    }

close_disps:
    CLOSE_DISPS(gfxCtx, "../z_skin.c", 534);
}
