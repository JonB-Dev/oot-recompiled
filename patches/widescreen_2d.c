// The 2D layer in a wide frame (phase 43): the two transitions drawn as 3D geometry sized for
// the console's 4:3 view, scaled by how much wider the frame is so they still cover it.
//
// WHY THIS FILE IS SHORT. The renderer already makes a rectangle that spans the console's frame
// span the wide one: a rectangle covering the whole scissor width is drawn at the target's full
// width, whatever the interface ratio setting (its own rule, rt64_framebuffer_renderer.cpp,
// "coversScissorWidth"). That takes care of the fade to black and the fade from white (one
// full-width rectangle each), the letterbox bars (two), the frame's base color fill, the
// desaturation filter's strips and the pause backdrop's copy. A rectangle that does not span
// the frame (the title card, the text box) is placed in the console's coordinates at the
// center, unstretched, which is what both want. None of those is patched; the survey captures
// are the record (execution/complete.md, phase 43) and the plan's candidates are accounted for
// in issues.md. The wipe is not here either: its mesh is far larger than the 4:3 view and the
// widened projection shows more of the same radial pattern, so it covers the wide frame as the
// game draws it (the survey has the numbers).
//
// The circle draws a textured mesh through a perspective projection and the triforce through
// an orthographic one, each scaled to just cover the 4:3 view. With the view set to expand, the
// renderer widens every projection by the frame's aspect, so a mesh that covered the console's
// frame covers only the middle of the wide one and the scene shows at the sides through the
// transition. Each draw is reproduced with its scale multiplied by the frame's width over 4:3,
// read from the native side on every frame (a window can be resized in the middle of a
// transition). At 4:3 the factor is exactly one and the draws are the game's. The reference
// project scales its circle the same way.

#include "patches.h"

#include "gfx.h"
#include "printf.h"
#include "z_math.h"
#include "transition_instances.h"
#include "transition_circle.h"
#include "transition_triforce.h"

// How much wider than 4:3 the frame is, 16.16 fixed point, exactly 65536 at 4:3. See
// src/game/recomp_api.cpp.
DECLARE_FUNC(u32, recomp_wide_frame_scale_q16, void);

// The meshes: statics of their files in the game, data symbols in this revision's table.
extern Gfx sTransCircleDL[];
extern Gfx sTransTriforceDL[];
extern Vtx sTransTriforceVtx[];

static f32 wide_frame_scale(void) {
    return (f32)recomp_wide_frame_scale_q16() * (1.0f / 65536.0f);
}

// @recomp Patched to scale the circle by the frame's width over 4:3.
RECOMP_PATCH void TransitionCircle_Draw(void* thisx, Gfx** gfxP) {
    Gfx* gfx = *gfxP;
    Mtx* modelView;
    TransitionCircle* this = (TransitionCircle*)thisx;
    Gfx* texScroll;
    // These variables are a best guess based on the other transition types.
    f32 tPos = 0.0f;
    f32 rot = 0.0f;
    f32 scale = 14.8f;

    // @recomp The scale carries the frame's width.
    scale *= wide_frame_scale();

    modelView = this->modelView[this->frame];

    this->frame ^= 1;
    gDPPipeSync(gfx++);
    texScroll = Gfx_BranchTexScroll(&gfx, this->texX, this->texY, 16, 64);
    gSPSegment(gfx++, 9, texScroll);
    gSPSegment(gfx++, 8, this->texture);
    gDPSetColor(gfx++, G_SETPRIMCOLOR, this->color.rgba);
    gDPSetColor(gfx++, G_SETENVCOLOR, this->color.rgba);
    gSPMatrix(gfx++, &this->projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPPerspNormalize(gfx++, this->normal);
    gSPMatrix(gfx++, &this->lookAt, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);

    if (scale != 1.0f) {
        guScale(&modelView[0], scale, scale, 1.0f);
        gSPMatrix(gfx++, &modelView[0], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }

    if (rot != 0.0f) {
        guRotate(&modelView[1], rot, 0.0f, 0.0f, 1.0f);
        gSPMatrix(gfx++, &modelView[1], G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    }

    if ((tPos != 0.0f) || (tPos != 0.0f)) {
        guTranslate(&modelView[2], tPos, tPos, 0.0f);
        gSPMatrix(gfx++, &modelView[2], G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    }
    gSPDisplayList(gfx++, sTransCircleDL);
    gDPPipeSync(gfx++);
    *gfxP = gfx;
}

// @recomp Patched to scale the triforce by the frame's width over 4:3.
RECOMP_PATCH void TransitionTriforce_Draw(void* thisx, Gfx** gfxP) {
    Gfx* gfx = *gfxP;
    Mtx* modelView;
    f32 scale;
    TransitionTriforce* this = (TransitionTriforce*)thisx;
    s32 pad;
    f32 rotation = this->transPos * 360.0f;

    modelView = this->modelView[this->frame];
    scale = this->transPos * 0.625f;
    // @recomp The scale carries the frame's width.
    scale *= wide_frame_scale();
    this->frame ^= 1;
    PRINTF("rate=%f tx=%f ty=%f rotate=%f\n", this->transPos, 0.0f, 0.0f, rotation);
    guScale(&modelView[0], scale, scale, 1.0f);
    guRotate(&modelView[1], rotation, 0.0f, 0.0f, 1.0f);
    guTranslate(&modelView[2], 0.0f, 0.0f, 0.0f);
    gDPPipeSync(gfx++);
    gSPDisplayList(gfx++, sTransTriforceDL);
    gDPSetColor(gfx++, G_SETPRIMCOLOR, this->color.rgba);
    gDPSetCombineMode(gfx++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gSPMatrix(gfx++, &this->projection, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix(gfx++, &modelView[0], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPMatrix(gfx++, &modelView[1], G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    gSPMatrix(gfx++, &modelView[2], G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    gSPVertex(gfx++, sTransTriforceVtx, 10, 0);

    if (!TransitionTriforce_IsDone(this)) {
        switch (this->type) {
            case TRANS_INSTANCE_TYPE_FILL_OUT:
                gSP2Triangles(gfx++, 0, 4, 5, 0, 4, 1, 3, 0);
                gSP1Triangle(gfx++, 5, 3, 2, 0);
                break;

            case TRANS_INSTANCE_TYPE_FILL_IN:
                gSP2Triangles(gfx++, 3, 4, 5, 0, 0, 2, 6, 0);
                gSP2Triangles(gfx++, 0, 6, 7, 0, 1, 0, 7, 0);
                gSP2Triangles(gfx++, 1, 7, 8, 0, 1, 8, 9, 0);
                gSP2Triangles(gfx++, 1, 9, 2, 0, 2, 9, 6, 0);
                break;
        }
    } else {
        switch (this->type) {
            case TRANS_INSTANCE_TYPE_FILL_OUT:
                break;

            case TRANS_INSTANCE_TYPE_FILL_IN:
                gSP1Quadrangle(gfx++, 6, 7, 8, 9, 0);
                break;
        }
    }
    gDPPipeSync(gfx++);
    *gfxP = gfx;
}
