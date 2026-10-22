// Track which matrices on the game's matrix stack derive from the billboard matrix, so their
// rotation is not interpolated on a frame where the camera cut.
//
// WHAT A BILLBOARD IS HERE. Once a frame, Play_Draw builds `play->billboardMtxF` from the inverse
// of the camera's rotation, so anything multiplied by it faces the viewer: a fairy's glow, a
// spark, a dust puff, the sun. The game reaches it two ways: through the matrix library
// (`Matrix_Mult(&play->billboardMtxF, MTXMODE_APPLY)` and its cousins, then a load from the
// stack) and directly, by loading `play->billboardMtx` through segment 1.
//
// WHY IT MATTERS FOR INTERPOLATION. On a smooth frame the billboard turns with the camera and
// interpolating its rotation keeps the sprite facing the interpolated camera, which is right. On
// a frame where the camera CUT, the camera's own group skips every component (camera_tagging.c),
// the view stays at the new frame's value for every generated frame, and a billboard whose
// rotation is still being interpolated turns from the old orientation to the new one while the
// camera does not: the sprite goes edge-on for a display frame. So on exactly those frames the
// billboard groups have their rotation skipped, which is the reference project's rule and the
// reason for the condition in `billboard_frame_end`.
//
// HOW THE TRACKING WORKS. The matrix library keeps a stack of twenty matrices; beside it this
// file keeps a stack of twenty flags, "this matrix has the billboard in it". Push copies the
// flag, Pop drops it, Put and Mult and ReplaceRotation set it when their source is the billboard
// matrix (Mult in APPLY mode only ever adds to it), and every setter that builds a fresh matrix
// (the NEW modes of translate, scale, the rotations, the axis rotation, and
// SetTranslateRotateYXZ) clears it. When a flagged matrix is converted for the display list
// (`Matrix_ToMtx`, which `Matrix_Finalize` calls), its address is remembered, and at the end of
// the frame each remembered address, plus `play->billboardMtx` itself for the direct loads, is
// given a group edit by address. That edit reaches a matrix inside an actor's limb group only
// because those groups are opened with G_EX_EDIT_ALLOW (tagging_helpers.h), which is why that
// flag changed from NONE in this phase.
//
// Every function is reproduced from the decompilation's sys_matrix.c at the pinned commit, with
// the tracking lines marked `@recomp`; a patch REPLACES its function. Two of the plan's names
// are not patched and the reason is recorded: `Matrix_MtxFCopy` is only ever handed the current
// matrix by Push and Put, which are patched, and `Matrix_TranslateRotateZYX` only applies, so
// neither can change the flag.

#include "patches.h"
#include "transform_ids.h"

#include "libc64/math64.h"
#include "gfx.h"
#include "sys_matrix.h"
#include "z_lib.h"
#include "game.h"
#include "play_state.h"
#include "skin_matrix.h"

#include "rt64_extended_gbi.h"

// From camera_tagging.c: whether this frame's camera verdict was a cut, and the reset.
s32 camera_was_skipped(void);
void camera_clear_skipped(void);

// The matrix library's stack, real data symbols in this revision's table, and the accessor
// `Matrix_Mult` uses, a real function that no header declares.
extern MtxF* sMatrixStack;
extern MtxF* sCurrentMatrix;
MtxF* Matrix_GetCurrent(void);

#define MATRIX_STACK_SIZE 20
#define MAX_TRACKED_BILLBOARDS 2048

// The play state's billboard matrix while a Play frame is being drawn, NULL otherwise. Set at the
// top of every frame from Graph_Update (frame_setup.c), which knows the game state.
static MtxF* sPlayBillboard = NULL;

// One flag per matrix stack slot, and the current one, kept in step with sCurrentMatrix.
static u8 sStackBillboard[MATRIX_STACK_SIZE] = { 0 };
static u8* sCurrentBillboard = sStackBillboard;

// The display list matrices converted from a flagged stack matrix this frame.
static Mtx* sTracked[MAX_TRACKED_BILLBOARDS];
static u32 sTrackedCount = 0;
static u32 sTrackedOverflow = 0;

// Called from Graph_Update before the game state runs: the billboard matrix to watch for.
//
// The play state is recognized by its DESTROY function, not its main one: Play_Main is patched
// (harness_warp.c), so in a patch the name resolves to the patch's own copy while the state
// holds the game's original address, and the two are never equal. Play_Destroy is untouched.
void billboard_frame_begin(GameState* gameState) {
    if (gameState->destroy == Play_Destroy) {
        sPlayBillboard = &((PlayState*)gameState)->billboardMtxF;
    } else {
        sPlayBillboard = NULL;
    }
    sTrackedCount = 0;
}

// Called from Graph_Update after the game state has drawn, before the lists are ended: on a
// frame where the camera cut, hold every billboard's rotation; then forget this frame's list.
void billboard_frame_end(GameState* gameState) {
    GraphicsContext* gfxCtx = gameState->gfxCtx;
    u32 i;

    if (sPlayBillboard != NULL && camera_was_skipped()) {
        PlayState* play = (PlayState*)gameState;

        OPEN_DISPS(gfxCtx, "../graph.c", 0);

        // The direct loads through segment 1.
        gEXEditGroupByAddress(POLY_XLU_DISP++, play->billboardMtx, G_EX_INTERPOLATE_DECOMPOSE, G_MTX_PUSH,
                              G_MTX_MODELVIEW, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP,
                              G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
                              G_EX_COMPONENT_SKIP, G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR);

        // The loads that came off the matrix stack with the billboard in them.
        for (i = 0; i < sTrackedCount; i++) {
            gEXEditGroupByAddress(POLY_XLU_DISP++, sTracked[i], G_EX_INTERPOLATE_DECOMPOSE, G_MTX_PUSH,
                                  G_MTX_MODELVIEW, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP,
                                  G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
                                  G_EX_COMPONENT_SKIP, G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR);
        }

        CLOSE_DISPS(gfxCtx, "../graph.c", 0);
    }

    camera_clear_skipped();
    sTrackedCount = 0;
}

// For the probe: how many billboard matrices this frame tracked and whether the list overflowed.
u32 billboard_tracked_count(void) {
    return sTrackedCount;
}

u32 billboard_overflow_count(void) {
    return sTrackedOverflow;
}

// @recomp Patched to reset the flag stack with the matrix stack.
RECOMP_PATCH void Matrix_Init(GameState* gameState) {
    s32 i;

    sCurrentMatrix = GAME_STATE_ALLOC(gameState, 20 * sizeof(MtxF), "../sys_matrix.c", 153);
    sMatrixStack = sCurrentMatrix;

    // @recomp
    for (i = 0; i < MATRIX_STACK_SIZE; i++) {
        sStackBillboard[i] = 0;
    }
    sCurrentBillboard = sStackBillboard;
    sTrackedCount = 0;
}

// @recomp Patched to push the flag with the matrix.
RECOMP_PATCH void Matrix_Push(void) {
    Matrix_MtxFCopy(sCurrentMatrix + 1, sCurrentMatrix);
    sCurrentMatrix++;

    // @recomp
    sCurrentBillboard[1] = sCurrentBillboard[0];
    sCurrentBillboard++;
}

// @recomp Patched to pop the flag with the matrix.
RECOMP_PATCH void Matrix_Pop(void) {
    sCurrentMatrix--;
    ASSERT(sCurrentMatrix >= sMatrixStack, "Matrix_now >= Matrix_stack", "../sys_matrix.c", 176);

    // @recomp
    sCurrentBillboard--;
}

// @recomp Patched to set the flag when the billboard matrix is put on the stack.
RECOMP_PATCH void Matrix_Put(MtxF* src) {
    Matrix_MtxFCopy(sCurrentMatrix, src);

    // @recomp
    *sCurrentBillboard = (src == sPlayBillboard);
}

// @recomp Patched to set the flag when the billboard matrix is multiplied in or loaded.
RECOMP_PATCH void Matrix_Mult(MtxF* mf, u8 mode) {
    MtxF* cmf = Matrix_GetCurrent();

    if (mode == MTXMODE_APPLY) {
        SkinMatrix_MtxFMtxFMult(cmf, mf, cmf);

        // @recomp Multiplying in never removes a billboard that is already there.
        *sCurrentBillboard = *sCurrentBillboard || (mf == sPlayBillboard);
    } else {
        Matrix_MtxFCopy(sCurrentMatrix, mf);

        // @recomp
        *sCurrentBillboard = (mf == sPlayBillboard);
    }
}

// @recomp Patched to clear the flag when a fresh matrix is set.
RECOMP_PATCH void Matrix_Translate(f32 x, f32 y, f32 z, u8 mode) {
    MtxF* cmf = sCurrentMatrix;
    f32 tx;
    f32 ty;

    if (mode == MTXMODE_APPLY) {
        tx = cmf->xx;
        ty = cmf->xy;
        cmf->xw += tx * x + ty * y + cmf->xz * z;
        tx = cmf->yx;
        ty = cmf->yy;
        cmf->yw += tx * x + ty * y + cmf->yz * z;
        tx = cmf->zx;
        ty = cmf->zy;
        cmf->zw += tx * x + ty * y + cmf->zz * z;
        tx = cmf->wx;
        ty = cmf->wy;
        cmf->ww += tx * x + ty * y + cmf->wz * z;
    } else {
        SkinMatrix_SetTranslate(cmf, x, y, z);

        // @recomp
        *sCurrentBillboard = 0;
    }
}

// @recomp Patched to clear the flag when a fresh matrix is set.
RECOMP_PATCH void Matrix_Scale(f32 x, f32 y, f32 z, u8 mode) {
    MtxF* cmf = sCurrentMatrix;

    if (mode == MTXMODE_APPLY) {
        cmf->xx *= x;
        cmf->yx *= x;
        cmf->zx *= x;
        cmf->xy *= y;
        cmf->yy *= y;
        cmf->zy *= y;
        cmf->xz *= z;
        cmf->yz *= z;
        cmf->zz *= z;
        cmf->wx *= x;
        cmf->wy *= y;
        cmf->wz *= z;
    } else {
        SkinMatrix_SetScale(cmf, x, y, z);

        // @recomp
        *sCurrentBillboard = 0;
    }
}

// @recomp Patched to clear the flag when a fresh matrix is set.
RECOMP_PATCH void Matrix_RotateX(f32 x, u8 mode) {
    MtxF* cmf;
    f32 sin;
    f32 cos;
    f32 temp1;
    f32 temp2;

    if (mode == MTXMODE_APPLY) {
        if (x != 0) {
            cmf = sCurrentMatrix;

            sin = sinf(x);
            cos = cosf(x);

            temp1 = cmf->xy;
            temp2 = cmf->xz;
            cmf->xy = temp1 * cos + temp2 * sin;
            cmf->xz = temp2 * cos - temp1 * sin;

            temp1 = cmf->yy;
            temp2 = cmf->yz;
            cmf->yy = temp1 * cos + temp2 * sin;
            cmf->yz = temp2 * cos - temp1 * sin;

            temp1 = cmf->zy;
            temp2 = cmf->zz;
            cmf->zy = temp1 * cos + temp2 * sin;
            cmf->zz = temp2 * cos - temp1 * sin;

            temp1 = cmf->wy;
            temp2 = cmf->wz;
            cmf->wy = temp1 * cos + temp2 * sin;
            cmf->wz = temp2 * cos - temp1 * sin;
        }
    } else {
        cmf = sCurrentMatrix;

        if (x != 0) {
            sin = sinf(x);
            cos = cosf(x);
        } else {
            sin = 0.0f;
            cos = 1.0f;
        }

        cmf->yx = 0.0f;
        cmf->zx = 0.0f;
        cmf->wx = 0.0f;
        cmf->xy = 0.0f;
        cmf->wy = 0.0f;
        cmf->xz = 0.0f;
        cmf->wz = 0.0f;
        cmf->xw = 0.0f;
        cmf->yw = 0.0f;
        cmf->zw = 0.0f;
        cmf->xx = 1.0f;
        cmf->ww = 1.0f;
        cmf->yy = cos;
        cmf->zz = cos;
        cmf->zy = sin;
        cmf->yz = -sin;

        // @recomp
        *sCurrentBillboard = 0;
    }
}

// @recomp Patched to clear the flag when a fresh matrix is set.
RECOMP_PATCH void Matrix_RotateY(f32 y, u8 mode) {
    MtxF* cmf;
    f32 sin;
    f32 cos;
    f32 temp1;
    f32 temp2;

    if (mode == MTXMODE_APPLY) {
        if (y != 0) {
            cmf = sCurrentMatrix;

            sin = sinf(y);
            cos = cosf(y);

            temp1 = cmf->xx;
            temp2 = cmf->xz;
            cmf->xx = temp1 * cos - temp2 * sin;
            cmf->xz = temp1 * sin + temp2 * cos;

            temp1 = cmf->yx;
            temp2 = cmf->yz;
            cmf->yx = temp1 * cos - temp2 * sin;
            cmf->yz = temp1 * sin + temp2 * cos;

            temp1 = cmf->zx;
            temp2 = cmf->zz;
            cmf->zx = temp1 * cos - temp2 * sin;
            cmf->zz = temp1 * sin + temp2 * cos;

            temp1 = cmf->wx;
            temp2 = cmf->wz;
            cmf->wx = temp1 * cos - temp2 * sin;
            cmf->wz = temp1 * sin + temp2 * cos;
        }
    } else {
        cmf = sCurrentMatrix;

        if (y != 0) {
            sin = sinf(y);
            cos = cosf(y);
        } else {
            sin = 0.0f;
            cos = 1.0f;
        }

        cmf->yx = 0.0f;
        cmf->wx = 0.0f;
        cmf->xy = 0.0f;
        cmf->zy = 0.0f;
        cmf->wy = 0.0f;
        cmf->yz = 0.0f;
        cmf->wz = 0.0f;
        cmf->xw = 0.0f;
        cmf->yw = 0.0f;
        cmf->zw = 0.0f;
        cmf->yy = 1.0f;
        cmf->ww = 1.0f;
        cmf->xx = cos;
        cmf->zz = cos;
        cmf->zx = -sin;
        cmf->xz = sin;

        // @recomp
        *sCurrentBillboard = 0;
    }
}

// @recomp Patched to clear the flag when a fresh matrix is set.
RECOMP_PATCH void Matrix_RotateZ(f32 z, u8 mode) {
    MtxF* cmf;
    f32 sin;
    f32 cos;
    f32 temp1;
    f32 temp2;

    if (mode == MTXMODE_APPLY) {
        if (z != 0) {
            cmf = sCurrentMatrix;

            sin = sinf(z);
            cos = cosf(z);

            temp1 = cmf->xx;
            temp2 = cmf->xy;
            cmf->xx = temp1 * cos + temp2 * sin;
            cmf->xy = temp2 * cos - temp1 * sin;

            temp1 = cmf->yx;
            temp2 = cmf->yy;
            cmf->yx = temp1 * cos + temp2 * sin;
            cmf->yy = temp2 * cos - temp1 * sin;

            temp1 = cmf->zx;
            temp2 = cmf->zy;
            cmf->zx = temp1 * cos + temp2 * sin;
            cmf->zy = temp2 * cos - temp1 * sin;

            temp1 = cmf->wx;
            temp2 = cmf->wy;
            cmf->wx = temp1 * cos + temp2 * sin;
            cmf->wy = temp2 * cos - temp1 * sin;
        }
    } else {
        cmf = sCurrentMatrix;

        if (z != 0) {
            sin = sinf(z);
            cos = cosf(z);
        } else {
            sin = 0.0f;
            cos = 1.0f;
        }

        cmf->zx = 0.0f;
        cmf->wx = 0.0f;
        cmf->zy = 0.0f;
        cmf->wy = 0.0f;
        cmf->xz = 0.0f;
        cmf->yz = 0.0f;
        cmf->wz = 0.0f;
        cmf->xw = 0.0f;
        cmf->yw = 0.0f;
        cmf->zw = 0.0f;
        cmf->zz = 1.0f;
        cmf->ww = 1.0f;
        cmf->xx = cos;
        cmf->yy = cos;
        cmf->yx = sin;
        cmf->xy = -sin;

        // @recomp
        *sCurrentBillboard = 0;
    }
}

/**
 * Rotate using ZYX Tait-Bryan angles.
 * This means a (column) vector is first rotated around X, then around Y, then around Z, then (if `mode` is
 * `MTXMODE_APPLY`) gets transformed according to whatever the matrix was before adding the ZYX rotation.
 * Original Name: Matrix_RotateXYZ, changed to reflect rotation order.
 */
// @recomp Patched to clear the flag when a fresh matrix is set.
RECOMP_PATCH void Matrix_RotateZYX(s16 x, s16 y, s16 z, u8 mode) {
    MtxF* cmf = sCurrentMatrix;
    f32 temp1;
    f32 temp2;
    f32 sin;
    f32 cos;

    if (mode == MTXMODE_APPLY) {
        sin = Math_SinS(z);
        cos = Math_CosS(z);

        temp1 = cmf->xx;
        temp2 = cmf->xy;
        cmf->xx = temp1 * cos + temp2 * sin;
        cmf->xy = temp2 * cos - temp1 * sin;

        temp1 = cmf->yx;
        temp2 = cmf->yy;
        cmf->yx = temp1 * cos + temp2 * sin;
        cmf->yy = temp2 * cos - temp1 * sin;

        temp1 = cmf->zx;
        temp2 = cmf->zy;
        cmf->zx = temp1 * cos + temp2 * sin;
        cmf->zy = temp2 * cos - temp1 * sin;

        temp1 = cmf->wx;
        temp2 = cmf->wy;
        cmf->wx = temp1 * cos + temp2 * sin;
        cmf->wy = temp2 * cos - temp1 * sin;

        if (y != 0) {
            sin = Math_SinS(y);
            cos = Math_CosS(y);

            temp1 = cmf->xx;
            temp2 = cmf->xz;
            cmf->xx = temp1 * cos - temp2 * sin;
            cmf->xz = temp1 * sin + temp2 * cos;

            temp1 = cmf->yx;
            temp2 = cmf->yz;
            cmf->yx = temp1 * cos - temp2 * sin;
            cmf->yz = temp1 * sin + temp2 * cos;

            temp1 = cmf->zx;
            temp2 = cmf->zz;
            cmf->zx = temp1 * cos - temp2 * sin;
            cmf->zz = temp1 * sin + temp2 * cos;

            temp1 = cmf->wx;
            temp2 = cmf->wz;
            cmf->wx = temp1 * cos - temp2 * sin;
            cmf->wz = temp1 * sin + temp2 * cos;
        }

        if (x != 0) {
            sin = Math_SinS(x);
            cos = Math_CosS(x);

            temp1 = cmf->xy;
            temp2 = cmf->xz;
            cmf->xy = temp1 * cos + temp2 * sin;
            cmf->xz = temp2 * cos - temp1 * sin;

            temp1 = cmf->yy;
            temp2 = cmf->yz;
            cmf->yy = temp1 * cos + temp2 * sin;
            cmf->yz = temp2 * cos - temp1 * sin;

            temp1 = cmf->zy;
            temp2 = cmf->zz;
            cmf->zy = temp1 * cos + temp2 * sin;
            cmf->zz = temp2 * cos - temp1 * sin;

            temp1 = cmf->wy;
            temp2 = cmf->wz;
            cmf->wy = temp1 * cos + temp2 * sin;
            cmf->wz = temp2 * cos - temp1 * sin;
        }
    } else {
        SkinMatrix_SetRotateZYX(cmf, x, y, z);

        // @recomp
        *sCurrentBillboard = 0;
    }
}

// @recomp Patched to clear the flag: this always sets a fresh matrix.
RECOMP_PATCH void Matrix_SetTranslateRotateYXZ(f32 translateX, f32 translateY, f32 translateZ, Vec3s* rot) {
    MtxF* cmf = sCurrentMatrix;
    f32 temp1 = Math_SinS(rot->y);
    f32 temp2 = Math_CosS(rot->y);
    f32 cos;
    f32 sin;

    cmf->xx = temp2;
    cmf->zx = -temp1;
    cmf->xw = translateX;
    cmf->yw = translateY;
    cmf->zw = translateZ;
    cmf->wx = 0.0f;
    cmf->wy = 0.0f;
    cmf->wz = 0.0f;
    cmf->ww = 1.0f;

    if (rot->x != 0) {
        sin = Math_SinS(rot->x);
        cos = Math_CosS(rot->x);

        cmf->zz = temp2 * cos;
        cmf->zy = temp2 * sin;
        cmf->xz = temp1 * cos;
        cmf->xy = temp1 * sin;
        cmf->yz = -sin;
        cmf->yy = cos;
    } else {
        cmf->zz = temp2;
        cmf->xz = temp1;
        cmf->yz = 0.0f;
        cmf->zy = 0.0f;
        cmf->xy = 0.0f;
        cmf->yy = 1.0f;
    }

    if (rot->z != 0) {
        sin = Math_SinS(rot->z);
        cos = Math_CosS(rot->z);

        temp1 = cmf->xx;
        temp2 = cmf->xy;
        cmf->xx = temp1 * cos + temp2 * sin;
        cmf->xy = temp2 * cos - temp1 * sin;

        temp1 = cmf->zx;
        temp2 = cmf->zy;
        cmf->zx = temp1 * cos + temp2 * sin;
        cmf->zy = temp2 * cos - temp1 * sin;

        temp2 = cmf->yy;
        cmf->yx = temp2 * sin;
        cmf->yy = temp2 * cos;
    } else {
        cmf->yx = 0.0f;
    }

    // @recomp
    *sCurrentBillboard = 0;
}

// @recomp Patched to record a converted matrix that carries the billboard.
RECOMP_PATCH Mtx* Matrix_ToMtx(Mtx* dest) {
    Mtx* ret = Matrix_MtxFToMtx(sCurrentMatrix, dest);

    // @recomp
    if (*sCurrentBillboard) {
        if (sTrackedCount < MAX_TRACKED_BILLBOARDS) {
            sTracked[sTrackedCount] = ret;
            sTrackedCount++;
        } else {
            sTrackedOverflow++;
        }
    }

    return ret;
}

// @recomp Patched to set the flag when the billboard's rotation replaces the current one.
RECOMP_PATCH void Matrix_ReplaceRotation(MtxF* mf) {
    MtxF* cmf = sCurrentMatrix;
    f32 acc;
    f32 temp;
    f32 curColNorm;

    // compute the Euclidean norm of the first column of the current matrix
    acc = cmf->xx;
    acc *= acc;
    temp = cmf->yx;
    acc += SQ(temp);
    temp = cmf->zx;
    acc += SQ(temp);
    curColNorm = sqrtf(acc);

    cmf->xx = mf->xx * curColNorm;
    cmf->yx = mf->yx * curColNorm;
    cmf->zx = mf->zx * curColNorm;

    // second column
    acc = cmf->xy;
    acc *= acc;
    temp = cmf->yy;
    acc += SQ(temp);
    temp = cmf->zy;
    acc += SQ(temp);
    curColNorm = sqrtf(acc);

    cmf->xy = mf->xy * curColNorm;
    cmf->yy = mf->yy * curColNorm;
    cmf->zy = mf->zy * curColNorm;

    // third column
    acc = cmf->xz;
    acc *= acc;
    temp = cmf->yz;
    acc += SQ(temp);
    temp = cmf->zz;
    acc += SQ(temp);
    curColNorm = sqrtf(acc);

    cmf->xz = mf->xz * curColNorm;
    cmf->yz = mf->yz * curColNorm;
    cmf->zz = mf->zz * curColNorm;

    // @recomp
    *sCurrentBillboard = (mf == sPlayBillboard);
}

// @recomp Patched to clear the flag when a fresh matrix is set.
RECOMP_PATCH void Matrix_RotateAxis(f32 angle, Vec3f* axis, u8 mode) {
    MtxF* cmf;
    f32 sin;
    f32 cos;
    f32 rCos;
    f32 temp1;
    f32 temp2;
    f32 temp3;
    f32 temp4;

    if (mode == MTXMODE_APPLY) {
        if (angle != 0) {
            cmf = sCurrentMatrix;

            sin = sinf(angle);
            cos = cosf(angle);

            temp1 = cmf->xx;
            temp2 = cmf->xy;
            temp3 = cmf->xz;
            temp4 = (axis->x * temp1 + axis->y * temp2 + axis->z * temp3) * (1.0f - cos);
            cmf->xx = temp1 * cos + axis->x * temp4 + sin * (temp2 * axis->z - temp3 * axis->y);
            cmf->xy = temp2 * cos + axis->y * temp4 + sin * (temp3 * axis->x - temp1 * axis->z);
            cmf->xz = temp3 * cos + axis->z * temp4 + sin * (temp1 * axis->y - temp2 * axis->x);

            temp1 = cmf->yx;
            temp2 = cmf->yy;
            temp3 = cmf->yz;
            temp4 = (axis->x * temp1 + axis->y * temp2 + axis->z * temp3) * (1.0f - cos);
            cmf->yx = temp1 * cos + axis->x * temp4 + sin * (temp2 * axis->z - temp3 * axis->y);
            cmf->yy = temp2 * cos + axis->y * temp4 + sin * (temp3 * axis->x - temp1 * axis->z);
            cmf->yz = temp3 * cos + axis->z * temp4 + sin * (temp1 * axis->y - temp2 * axis->x);

            temp1 = cmf->zx;
            temp2 = cmf->zy;
            temp3 = cmf->zz;
            temp4 = (axis->x * temp1 + axis->y * temp2 + axis->z * temp3) * (1.0f - cos);
            cmf->zx = temp1 * cos + axis->x * temp4 + sin * (temp2 * axis->z - temp3 * axis->y);
            cmf->zy = temp2 * cos + axis->y * temp4 + sin * (temp3 * axis->x - temp1 * axis->z);
            cmf->zz = temp3 * cos + axis->z * temp4 + sin * (temp1 * axis->y - temp2 * axis->x);
        }
    } else {
        cmf = sCurrentMatrix;

        if (angle != 0) {
            sin = sinf(angle);
            cos = cosf(angle);
            rCos = 1.0f - cos;

            cmf->xx = axis->x * axis->x * rCos + cos;
            cmf->yy = axis->y * axis->y * rCos + cos;
            cmf->zz = axis->z * axis->z * rCos + cos;

            if (0) {}

            temp2 = axis->x * rCos * axis->y;
            temp3 = axis->z * sin;
            cmf->yx = temp2 + temp3;
            cmf->xy = temp2 - temp3;

            temp2 = axis->x * rCos * axis->z;
            temp3 = axis->y * sin;
            cmf->zx = temp2 - temp3;
            cmf->xz = temp2 + temp3;

            temp2 = axis->y * rCos * axis->z;
            temp3 = axis->x * sin;
            cmf->zy = temp2 + temp3;
            cmf->yz = temp2 - temp3;

            cmf->wx = cmf->wy = cmf->wz = cmf->xw = cmf->yw = cmf->zw = 0.0f;
            cmf->ww = 1.0f;
        } else {
            cmf->yx = 0.0f;
            cmf->zx = 0.0f;
            cmf->wx = 0.0f;
            cmf->xy = 0.0f;
            cmf->zy = 0.0f;
            cmf->wy = 0.0f;
            cmf->xz = 0.0f;
            cmf->yz = 0.0f;
            cmf->wz = 0.0f;
            cmf->xw = 0.0f;
            cmf->yw = 0.0f;
            cmf->zw = 0.0f;
            cmf->xx = 1.0f;
            cmf->yy = 1.0f;
            cmf->zz = 1.0f;
            cmf->ww = 1.0f;
        }

        // @recomp
        *sCurrentBillboard = 0;
    }
}
