// A POINT LIGHT'S GLOW IS HIDDEN BY ITS CENTER, NOT CUT BY ITS PLANE (2026-10-08).
//
// The user, in the Fire Temple, of the torches and the flames in the carved faces: "the radial
// glows that are around ... things like torches or navi ... rotate. They're just a flat surface that
// rotates around to basically always face the camera. So they're not an actual 3D sphere ... at
// certain angles, that plane starts to get clipped by the geometry around it"; and from the Inspect
// view of a torch in a recess: "you can see its that the outer glow is clipping behing the walls and
// ceiling geometry".
//
// Every glow light's glow is drawn here: one quad turned to the eye by the billboard matrix, about
// 230 units across at a torch's radius, with no depth test at all (G_RM_CLD_SURF). The console
// decided once per light, in Lights_GlowCheck, whether the pixel at the light's center was covered,
// and drew the whole quad or none of it. Since renderer patch 0046 (for Navi's core, in 0.4.0) the
// renderer depth tests every such draw per pixel, so stone standing in front of the quad's plane cut
// hard straight edges out of it: a flame in a recess showed its glow only through the opening.
//
// THE REFERENCE PROJECT'S TECHNIQUE, applied to this game's own function: each glow is drawn inside
// RT64's vertex Z test on a vertex at the light's center, so the renderer hides the whole glow when
// that point is covered in its own depth buffer, at the real frame's width and on the frame it is
// drawn, and the renderer leaves a draw inside that test out of its per pixel test (renderer patch
// 0050). Lights_GlowCheck is NOT replaced, unlike the reference project: the program's light list
// reads each glow light's drawGlow byte as its glow flag (src/game/light_list.cpp), which decides
// how far Navi's light reaches, so the game's own check keeps setting it exactly as before.
//
// The Debugging menu's Glow test row picks between the candidates (the user: "adds some debugging
// options to cycle through so I can test a bunch of different options to see which one fixes it
// without regressions"): 0 and 1 draw through the center test, 2 and 3 draw exactly as the game does.
// The drawing below is the game's own, line for line, around the test.
#include "patches.h"

#include "gfx.h"
#include "gfx_setupdl.h"
#include "light.h"
#include "play_state.h"
#include "sys_matrix.h"
#include "z_math.h"

#include "rt64_extended_gbi.h"

// The program's side (src/game/recomp_api.cpp).
s32 recomp_glow_test(void);

// The glow's quad and its texture, in gameplay_keep.
extern Gfx gGlowCircleTextureLoadDL[];
extern Gfx gGlowCircleDL[];

// The glow's own origin, which its matrix carries to the light's position: the point tested.
static Vtx sGlowCenterVtx = VTX(0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);

// @recomp The game's glow drawing, with each glow inside the renderer's center test (rows 0 and 1).
RECOMP_PATCH void Lights_DrawGlow(PlayState* play) {
    LightNode* node = play->lightCtx.listHead;
    const s32 centerTest = (recomp_glow_test() <= 1);

    OPEN_DISPS(play->state.gfxCtx, "../z_lights.c", 887);

    POLY_XLU_DISP = func_800947AC(POLY_XLU_DISP++);
    gDPSetAlphaDither(POLY_XLU_DISP++, G_AD_NOISE);
    gDPSetColorDither(POLY_XLU_DISP++, G_CD_MAGICSQ);
    gSPDisplayList(POLY_XLU_DISP++, gGlowCircleTextureLoadDL);

    while (node != NULL) {
        if (node->info->type == LIGHT_POINT_GLOW) {
            LightPoint* params = &node->info->params.point;

            if (params->drawGlow) {
                f32 scale = SQ(params->radius) * 0.0000026f;

                gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, params->color[0], params->color[1], params->color[2], 50);
                Matrix_Translate(params->x, params->y, params->z, MTXMODE_NEW);
                Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
                MATRIX_FINALIZE_AND_LOAD(POLY_XLU_DISP++, play->state.gfxCtx, "../z_lights.c", 918);
                if (centerTest) {
                    // @recomp The quad loads its own four vertices over this one; the test has
                    // already taken the center's screen position and depth by then.
                    gSPVertex(POLY_XLU_DISP++, &sGlowCenterVtx, 1, 0);
                    gEXVertexZTest(POLY_XLU_DISP++, 0);
                    gSPDisplayList(POLY_XLU_DISP++, gGlowCircleDL);
                    gEXEndVertexZTest(POLY_XLU_DISP++);
                } else {
                    gSPDisplayList(POLY_XLU_DISP++, gGlowCircleDL);
                }
            }
        }

        node = node->next;
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_lights.c", 927);
}
