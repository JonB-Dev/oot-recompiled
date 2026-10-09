// Tag the camera so the renderer can interpolate it between game frames, except when it cut.
//
// THE SINGLE TAG WITH THE LARGEST EFFECT. Every 3D thing on screen moves with the camera, so once
// the projection is a transform group the renderer can interpolate, a camera pan at the game's
// twenty updates a second is drawn at the display's rate with the world gliding between them.
//
// THE PART THAT CAN GO WRONG is a cut. A camera that jumps (a scene change, the pause menu's own
// view, a cutscene angle change, a warp) must NOT be interpolated, or the world smears from the
// old place to the new one over a display frame. So each frame decides: did the camera move the
// way a smooth camera moves, or did it jump? THE DECISION IS NATIVE (src/game/camera_cuts.cpp),
// reached like the actor registry is, and the reason is recorded there: two rules written here,
// with the previous frame kept in this patch's own storage, each called every frame of a first
// person pan a cut, and finding that out took a temporary fork of the renderer. Native, the
// inputs and the verdict are one line in the trace when the probe is on. This patch hands over
// the eye, the focus and the entrance index and emits the tag the answer calls for.
//
// `View_Apply` is reproduced from the decompilation's z_view.c: it is a nine line dispatcher in
// this revision, and the tag is emitted after the perspective view has loaded its matrices so it
// names the projection that is actually in place. The orthographic path (menus, the HUD) is not
// tagged: nothing in it moves with a camera.

#include "patches.h"
#include "transform_ids.h"

#include "gfx.h"
#include "save.h"
#include "sys_math.h"
#include "view.h"
#include "z_lib.h"

#include "rt64_extended_gbi.h"

s32 View_ApplyOrtho(View* view);
s32 View_ApplyPerspective(View* view);

// Native, through the dummy address in syms.ld. The eye and the focus go over as the emulated
// addresses of the game's own Vec3f, the entrance index as a plain integer; back comes 1 for a
// smooth move and 0 for a cut. Nothing native is handed a pointer it could follow blindly: the
// addresses are checked against the console's memory before they are read.
DECLARE_FUNC(s32, recomp_camera_smooth, u32 eye_address, u32 at_address, s32 entrance, u32 patch_calls);

// A counter in this patch's own static storage, handed across every call. Two versions of the
// cut rule kept their previous frame in storage like this and both called every frame of a pan
// a cut, while the same rule passes natively; the suspect is that a patch's statics do not
// persist. The native side prints this beside its own call count when the probe is on, so the
// question is answered by every trace rather than argued about, and any later phase that keeps
// state in a patch (billboard tracking, for one) can check the same line first.
static u32 sPatchCalls = 0;

// Whether any perspective view this frame was called a cut. Read by the billboard tracking
// (billboard_tagging.c), the skybox and the environment (environment_tagging.c), which must not
// interpolate against a camera that did not; cleared at the end of every frame from Graph_Update.
static s32 sCameraSkipped = 0;

s32 camera_was_skipped(void) {
    return sCameraSkipped;
}

void camera_clear_skipped(void) {
    sCameraSkipped = 0;
}

// A view that is not the play camera's (phase 45: the pause menu's cube view and its panel
// view, applied through the same function with eyes of their own) names its id and its verdict
// here before it applies itself; the next View_Apply uses them once and forgets them. Until
// then, and after, every view is the play camera's.
static u32 sNextViewId = 0;
static s32 sNextViewSmooth = 0;

void camera_next_view(u32 id, s32 smooth) {
    sNextViewId = id;
    sNextViewSmooth = smooth;
}

// @recomp Patched to tag the projection as the camera's transform group, interpolated unless the
// camera cut this frame.
RECOMP_PATCH s32 View_Apply(View* view, s32 mask) {
    s32 result;
    s32 interpolate;
    u32 id;
    GraphicsContext* gfxCtx;

    mask = (view->flags & mask) | (mask >> 4);

    if (mask & VIEW_PROJECTION_ORTHO) {
        return View_ApplyOrtho(view);
    }

    result = View_ApplyPerspective(view);

    // @recomp The addition. Smooth or cut is decided natively from where the camera was last
    // frame; a new entrance is always a cut. A view that named itself (the pause menu's) keeps
    // its own id and verdict, and does not move the play camera's record.
    if (sNextViewId != TRANSFORM_ID_NONE) {
        id = sNextViewId;
        interpolate = sNextViewSmooth;
        sNextViewId = TRANSFORM_ID_NONE;
    } else {
        id = CAMERA_TRANSFORM_ID;
        sPatchCalls++;
        interpolate = recomp_camera_smooth((u32)&view->eye, (u32)&view->at, gSaveContext.save.entranceIndex, sPatchCalls);
        if (!interpolate) {
            sCameraSkipped = 1;
        }
    }

    gfxCtx = view->gfxCtx;
    OPEN_DISPS(gfxCtx, "../z_view.c", 0);

    if (interpolate) {
        // Simple interpolation suits a camera: it orbits a focus, and decomposing the matrix
        // into position and rotation to interpolate each would swing it through the wrong arc.
        gEXMatrixGroupSimple(POLY_OPA_DISP++, id, G_EX_NOPUSH, G_MTX_PROJECTION,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE);
        gEXMatrixGroupSimple(POLY_XLU_DISP++, id, G_EX_NOPUSH, G_MTX_PROJECTION,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE,
            G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE);
    } else {
        // A cut: name the group so the renderer knows it is the same camera, but skip every
        // component this frame so nothing is drawn between the old place and the new one.
        gEXMatrixGroupSimple(POLY_OPA_DISP++, id, G_EX_NOPUSH, G_MTX_PROJECTION,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
        gEXMatrixGroupSimple(POLY_XLU_DISP++, id, G_EX_NOPUSH, G_MTX_PROJECTION,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE,
            G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
    }

    CLOSE_DISPS(gfxCtx, "../z_view.c", 0);

    return result;
}
