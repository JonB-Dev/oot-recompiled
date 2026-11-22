// Tell the native side, once a frame, whether the room on screen is one the game draws as a
// fixed camera over a background PICTURE rather than as scene geometry, so the view can be held
// at the console's 4:3 while it is.
//
// THE BUG, reported by the user on 2026-09-20: "during the fixed width scenes where it kind of
// draws the background, such as right as soon as you enter High Rules Castle area or things like
// inside of Link's house, the larger aspect ratio is stretching it out and causing the
// interactions and Link's positioning to be different. So it actually needs to force these to
// maintain a four x three ratio."
//
// They are right about the cause as well as the fix. Such a room draws a 320 by 240 image
// through the S2DEX microcode and then puts the actors and a little geometry in 3D on top of it.
// Widen the view and the two are widened DIFFERENTLY: the renderer stretches a 2D rect that
// spans the frame to the frame's full width, while a 3D projection is widened by showing more at
// the sides. The backdrop and the world then disagree about where anything is, which is why Link
// stands in the wrong place against the picture and why what he can reach is not what it looks
// like he can reach. Nothing here can make a 4:3 photograph into a 16:9 one, so the answer is to
// stop widening while one is on screen.
//
// WHAT COUNTS AS ONE, arrived at by being wrong twice and measuring each time:
//
//   the room's shape is ROOM_SHAPE_TYPE_IMAGE, the game's own statement that this room is drawn
//   over a picture rather than built from geometry,
//   OR the active camera is on one of the three PRERENDER settings, which is the game's own
//   statement that this area is built around a fixed, console-framed view.
//
// EITHER, not both, and the two failures are the reason. The first version required the camera
// to be CAM_SET_PREREND_FIXED, copied from the condition the game uses to decide whether to draw
// a backdrop (z_room.c), and matched almost nothing: this revision has THREE prerender settings,
// FIXED (fixed position and rotation), PIVOT (fixed position, free to turn in yaw) and
// SIDE_SCROLL, whose own comment in the decompilation reads "Only used in castle courtyard with
// the guards". The second version dropped the camera entirely and keyed on the room's shape,
// which caught Link's house (an image room on a PIVOT camera) and then switched OFF in the
// streets of the Market, which the user had named as the worst case: those streets are ordinary
// geometry viewed through a PIVOT camera, so they are framed for 4:3 without being a picture.
// Both signals are needed because neither place has both.
//
// A scene can mix these room by room, so it is read every frame rather than once per scene.
//
// WHY IT IS EVALUATED IN Graph_Update rather than in the room draw. Room_Draw only runs for a
// room that is being drawn, so it can say "this room is prerendered" and can never say "no room
// is". Graph_Update runs for every frame of every game state, so the answer is fresh even when
// the game is in a menu, a file select or a cutscene, and leaving play puts the view back.

#include "patches.h"

#include "camera.h"
#include "play_state.h"
#include "room.h"

// Native, through the dummy address in syms.ld: one integer in, nothing out. The native side
// ignores a value that has not changed, so this costs a call a frame and no more.
DECLARE_FUNC(void, recomp_prerendered_room, s32 on, s32 cam_setting);

// The play state is recognized by its DESTROY function, not its main one, for the reason
// billboard_tagging.c records: Play_Main is patched (harness_warp.c), so in a patch the name
// resolves to the patch's own copy while the state holds the game's original address, and the
// two are never equal. Play_Destroy is untouched.
void Play_Destroy(GameState* thisx);

void prerender_frame_begin(GameState* gameState) {
    PlayState* play;
    Room* room;
    Camera* cam;
    s32 prerendered = 0;
    s32 camSetting = -1;

    if (gameState->destroy == Play_Destroy) {
        play = (PlayState*)gameState;
        room = &play->roomCtx.curRoom;

        // roomShape is NULL between a room being unloaded and the next being ready, which happens
        // every time Link walks through a door, so it is checked rather than assumed.
        // roomShape is NULL between a room being unloaded and the next being ready, which happens
        // every time Link walks through a door, so it is checked rather than assumed.
        if ((room->roomShape != NULL) && (room->roomShape->base.type == ROOM_SHAPE_TYPE_IMAGE)) {
            prerendered = 1;
        }

        // GET_ACTIVE_CAM is `cameraPtrs[activeCamId]` with no bounds check, which is fine where
        // the game uses it (inside its own draw, after everything is set up) and is not fine
        // here: this runs at the head of EVERY frame of every state, the first frames of a scene
        // load included, where the id has held whatever was there before.
        if ((play->activeCamId >= 0) && (play->activeCamId < NUM_CAMS)) {
            cam = play->cameraPtrs[play->activeCamId];
            if (cam != NULL) {
                camSetting = cam->setting;
                if ((camSetting == CAM_SET_PREREND_FIXED) || (camSetting == CAM_SET_PREREND_PIVOT) ||
                    (camSetting == CAM_SET_PREREND_SIDE_SCROLL)) {
                    prerendered = 1;
                }
            }
        }
    }

    recomp_prerendered_room(prerendered, camSetting);
}
