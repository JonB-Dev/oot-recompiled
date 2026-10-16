// Tag the environment: the skybox, the sun and the moon, and the lens flares.
//
// THE SKYBOX is a box of faces drawn around the camera's eye, so it moves exactly with the
// camera and, untagged, it steps once per game frame while the tagged camera glides between
// them: the horizon jitters against the ground. Its one matrix gets the skybox group, and on a
// frame where the camera cut every component is skipped, for the same reason the billboards
// are (billboard_tagging.c): the camera's own group skips on a cut, and anything still
// interpolating against it moves while the world does not.
//
// THE SUN AND THE MOON are two matrices built from the eye and the time of day in one function;
// each gets a group of its own, interpolated except on a cut.
//
// THE LENS FLARE is a 206 line function this file does not reproduce. Its two callers, the sun's
// flare and the custom flare a cutscene can place, are a dozen lines each and the flare draws
// entirely to the translucent list, so the group is opened around the call in each caller and
// closed after it, which is what the reference project does. The plan sized this phase for
// reproducing the flare; wrapping the call needs nothing inside it.
//
// Every reproduced function is from the decompilation (z_vr_box_draw.c and z_kankyo.c at the
// pinned commit) with the tagging lines marked `@recomp`.

#include "patches.h"
#include "tagging_helpers.h"

#include "environment.h"
#include "gfx.h"
#include "gfx_setupdl.h"
#include "play_state.h"
#include "save.h"
#include "skybox.h"
#include "sys_math.h"
#include "sys_matrix.h"
#include "z_lib.h"

#include "rt64_extended_gbi.h"

// From camera_tagging.c: whether this frame's camera verdict was a cut.
s32 camera_was_skipped(void);

// Globals of the two source files that no header declares, real data symbols in this revision.
extern Mtx* sSkyboxDrawMatrix;
extern s16 sLensFlareUnused;

// Sun and moon display lists, in gameplay_keep.
extern Gfx gSunDL[];
extern Gfx gMoonDL[];

// A fixed thing's group on the opaque list: interpolated, or held on a cut.
static Gfx* tag_fixed_opa(Gfx* dlist, u32 id) {
    if (camera_was_skipped()) {
        gEXMatrixGroupDecomposedSkipAll(dlist++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    } else {
        gEXMatrixGroupDecomposedNormal(dlist++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    }
    return dlist;
}

// @recomp Patched to tag the skybox's matrix, held on a cut.
RECOMP_PATCH void Skybox_Draw(SkyboxContext* skyboxCtx, GraphicsContext* gfxCtx, s16 skyboxId, s16 blend, f32 x, f32 y,
                              f32 z) {
    OPEN_DISPS(gfxCtx, "../z_vr_box_draw.c", 52);

    Gfx_SetupDL_40Opa(gfxCtx);

    gSPSegment(POLY_OPA_DISP++, 0x7, skyboxCtx->staticSegments[0]);
    gSPSegment(POLY_OPA_DISP++, 0x8, skyboxCtx->staticSegments[1]);
    gSPSegment(POLY_OPA_DISP++, 0x9, skyboxCtx->palettes);

    gDPSetPrimColor(POLY_OPA_DISP++, 0x00, 0x00, 0, 0, 0, blend);
    gSPTexture(POLY_OPA_DISP++, 0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON);

    // Prepare matrix
    sSkyboxDrawMatrix = GRAPH_ALLOC(gfxCtx, sizeof(Mtx));
    Matrix_Translate(x, y, z, MTXMODE_NEW);
    Matrix_Scale(1.0f, 1.0f, 1.0f, MTXMODE_APPLY);
    Matrix_RotateX(skyboxCtx->rot.x, MTXMODE_APPLY);
    Matrix_RotateY(skyboxCtx->rot.y, MTXMODE_APPLY);
    Matrix_RotateZ(skyboxCtx->rot.z, MTXMODE_APPLY);
    MATRIX_TO_MTX(sSkyboxDrawMatrix, "../z_vr_box_draw.c", 76);

    // @recomp The group, opened before the load it names.
    POLY_OPA_DISP = tag_fixed_opa(POLY_OPA_DISP, SKYBOX_TRANSFORM_ID_START);

    gSPMatrix(POLY_OPA_DISP++, sSkyboxDrawMatrix, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    // Enable magic square RGB dithering and bilinear filtering
    gDPSetColorDither(POLY_OPA_DISP++, G_CD_MAGICSQ);
    gDPSetTextureFilter(POLY_OPA_DISP++, G_TF_BILERP);

    // All skyboxes use CI8 textures with an RGBA16 palette
    gDPLoadTLUT_pal256(POLY_OPA_DISP++, skyboxCtx->palettes[0]);
    gDPSetTextureLUT(POLY_OPA_DISP++, G_TT_RGBA16);

    // Enable texture filtering RDP pipeline stages for bilinear filtering
    gDPSetTextureConvert(POLY_OPA_DISP++, G_TC_FILT);

    if (skyboxCtx->drawType != SKYBOX_DRAW_128) {
        // 256x256 textures, per-face palettes
        // 2, 3 or 4 faces

        gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[0]); // -z face upper
        gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[1]); // -z face lower

        gDPPipeSync(POLY_OPA_DISP++);
        gDPLoadTLUT_pal256(POLY_OPA_DISP++, skyboxCtx->palettes[1]);
        gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[2]); // +x face upper
        gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[3]); // +x face lower

        if (skyboxId != SKYBOX_BAZAAR) {
            if (skyboxId < SKYBOX_KOKIRI_SHOP || skyboxId > SKYBOX_BOMBCHU_SHOP) {
                // Skip remaining faces for most shop skyboxes

                gDPPipeSync(POLY_OPA_DISP++);
                gDPLoadTLUT_pal256(POLY_OPA_DISP++, skyboxCtx->palettes[2]);
                gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[4]); // +z face upper
                gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[5]); // +z face lower

                // Note this pipesync is slightly misplaced and would be better off inside the condition
                gDPPipeSync(POLY_OPA_DISP++);

                if (skyboxCtx->drawType != SKYBOX_DRAW_256_3FACE) {
                    gDPLoadTLUT_pal256(POLY_OPA_DISP++, skyboxCtx->palettes[3]);
                    gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[6]); // -x face upper
                    gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[7]); // -x face lower
                }
            }
        }
    } else {
        // 128x128 and 128x64 textures
        // 5 or 6 faces

        // Draw each face
        gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[0]); // -z face
        gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[2]); // +z face
        gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[4]); // -x face
        gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[6]); // +x face
        gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[8]); // +y face
        if (skyboxId == SKYBOX_CUTSCENE_MAP) {
            // Skip the bottom face in the cutscene map
            gSPDisplayList(POLY_OPA_DISP++, skyboxCtx->dListBuf[10]); // -y face
        }
    }

    gDPPipeSync(POLY_OPA_DISP++);

    // @recomp
    gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);

    CLOSE_DISPS(gfxCtx, "../z_vr_box_draw.c", 125);
}

// @recomp Patched to tag the sun's and the moon's matrices, held on a cut.
RECOMP_PATCH void Environment_DrawSunAndMoon(PlayState* play) {
    f32 alpha;
    f32 color;
    f32 y;
    f32 scale;
    f32 temp;

    OPEN_DISPS(play->state.gfxCtx, "../z_kankyo.c", 2266);

    if (play->csCtx.state != CS_STATE_IDLE) {
        Math_SmoothStepToF(&play->envCtx.sunPos.x,
                           -(Math_SinS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 120.0f) * 25.0f,
                           1.0f, 0.8f, 0.8f);
        Math_SmoothStepToF(&play->envCtx.sunPos.y,
                           (Math_CosS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 120.0f) * 25.0f, 1.0f,
                           0.8f, 0.8f);
        //! @bug This should be z.
        Math_SmoothStepToF(&play->envCtx.sunPos.y,
                           (Math_CosS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 20.0f) * 25.0f, 1.0f,
                           0.8f, 0.8f);
    } else {
        play->envCtx.sunPos.x = -(Math_SinS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 120.0f) * 25.0f;
        play->envCtx.sunPos.y = +(Math_CosS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 120.0f) * 25.0f;
        play->envCtx.sunPos.z = +(Math_CosS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 20.0f) * 25.0f;
    }

    if (gSaveContext.save.entranceIndex != ENTR_HYRULE_FIELD_0 || ((void)0, gSaveContext.sceneLayer) != 5) {
        Matrix_Translate(play->view.eye.x + play->envCtx.sunPos.x, play->view.eye.y + play->envCtx.sunPos.y,
                         play->view.eye.z + play->envCtx.sunPos.z, MTXMODE_NEW);

        y = play->envCtx.sunPos.y / 25.0f;
        temp = y / 80.0f;

        alpha = temp * 255.0f;
        if (alpha < 0.0f) {
            alpha = 0.0f;
        }
        if (alpha > 255.0f) {
            alpha = 255.0f;
        }

        alpha = 255.0f - alpha;

        color = temp;
        if (color < 0.0f) {
            color = 0.0f;
        }

        if (color > 1.0f) {
            color = 1.0f;
        }

        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, (u8)(color * 75.0f) + 180, (u8)(color * 155.0f) + 100, 255);
        gDPSetEnvColor(POLY_OPA_DISP++, 255, (u8)(color * 255.0f), (u8)(color * 255.0f), alpha);

        scale = (color * 2.0f) + 10.0f;
        Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);

        // @recomp The sun.
        POLY_OPA_DISP = tag_fixed_opa(POLY_OPA_DISP, SUN_MOON_TRANSFORM_ID);
        MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_kankyo.c", 2364);
        Gfx_SetupDL_54Opa(play->state.gfxCtx);
        gSPDisplayList(POLY_OPA_DISP++, gSunDL);
        gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);

        Matrix_Translate(play->view.eye.x - play->envCtx.sunPos.x, play->view.eye.y - play->envCtx.sunPos.y,
                         play->view.eye.z - play->envCtx.sunPos.z, MTXMODE_NEW);

        color = -y / 120.0f;
        color = CLAMP_MIN(color, 0.0f);

        scale = -15.0f * color + 25.0f;
        Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);

        temp = -y / 80.0f;
        temp = CLAMP_MAX(temp, 1.0f);

        alpha = temp * 255.0f;

        if (alpha > 0.0f) {
            // @recomp The moon.
            POLY_OPA_DISP = tag_fixed_opa(POLY_OPA_DISP, MOON_TRANSFORM_ID);
            MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_kankyo.c", 2406);
            Gfx_SetupDL_51Opa(play->state.gfxCtx);
            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 240, 255, 180, alpha);
            gDPSetEnvColor(POLY_OPA_DISP++, 80, 70, 20, alpha);
            gSPDisplayList(POLY_OPA_DISP++, gMoonDL);
            gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);
        }
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_kankyo.c", 2429);
}

// A flare's group on the translucent list, around the whole flare: every element of it is one
// matrix, paired in order, interpolated except on a cut.
static void tag_flare_begin(GraphicsContext* gfxCtx, u32 id) {
    OPEN_DISPS(gfxCtx, "../z_kankyo.c", 0);
    if (camera_was_skipped()) {
        gEXMatrixGroupDecomposedSkipAll(POLY_XLU_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    } else {
        gEXMatrixGroupDecomposedNormal(POLY_XLU_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    }
    CLOSE_DISPS(gfxCtx, "../z_kankyo.c", 0);
}

static void tag_flare_end(GraphicsContext* gfxCtx) {
    OPEN_DISPS(gfxCtx, "../z_kankyo.c", 0);
    gEXPopMatrixGroup(POLY_XLU_DISP++, G_MTX_MODELVIEW);
    CLOSE_DISPS(gfxCtx, "../z_kankyo.c", 0);
}

// @recomp Patched to tag the sun's lens flare around the call that draws it.
RECOMP_PATCH void Environment_DrawSunLensFlare(PlayState* play, EnvironmentContext* envCtx, View* view,
                                               GraphicsContext* gfxCtx, Vec3f pos, s32 unused) {
    if ((play->envCtx.precipitation[PRECIP_RAIN_CUR] == 0) && (play->envCtx.skyboxConfig == 0)) {
        // @recomp
        tag_flare_begin(play->state.gfxCtx, LENS_FLARE_TRANSFORM_ID_START + 0);
        Environment_DrawLensFlare(play, &play->envCtx, &play->view, play->state.gfxCtx, pos, 2000, 370,
                                  Math_CosS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 120.0f, 400,
                                  true);
        // @recomp
        tag_flare_end(play->state.gfxCtx);
    }
}

// @recomp Patched to tag the custom lens flare around the call that draws it.
RECOMP_PATCH void Environment_DrawCustomLensFlare(PlayState* play) {
    Vec3f pos;

    if (gCustomLensFlareOn) {
        pos.x = gCustomLensFlarePos.x;
        pos.y = gCustomLensFlarePos.y;
        pos.z = gCustomLensFlarePos.z;

        // @recomp
        tag_flare_begin(play->state.gfxCtx, LENS_FLARE_TRANSFORM_ID_START + 1);
        Environment_DrawLensFlare(play, &play->envCtx, &play->view, play->state.gfxCtx, pos, sLensFlareUnused,
                                  gLensFlareScale, gLensFlareColorIntensity, gLensFlareGlareStrength, false);
        // @recomp
        tag_flare_end(play->state.gfxCtx);
    }
}
