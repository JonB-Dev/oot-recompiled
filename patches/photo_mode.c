// PHOTO MODE'S CAMERA (2026-10-09).
//
// The user: "the ability to freeze the game at its current frame and then pause audio ... So I can
// lock the game at its exact spot, which helps me to get the same exact shot at any different
// setting level", and then "have the ability to use the free move of the camera while leaving
// everything else frozen ... everything in the game actually freezes, but the camera itself can
// still move around".
//
// The freeze is the one the comparison shots use (src/main/shots.h): the patched Play_Main
// (harness_warp.c) draws without updating, so actors, effects, animation and the clock all stand
// still. The game builds its view from the camera's eye and target when it updates, and draws
// from the view, so while frozen in photo mode this sets the view itself, every frame, from a
// camera of its own, before the draw. Which actors are on screen is decided again at draw time
// against the view (Actor_DrawAll), so moving the camera brings them into the picture as it would.
// When the freeze ends the game's update builds the view from its own camera again, which never
// moved, so the picture is back where it was.
//
// THE CONTROLS ARE PHOTO MODE'S OWN (the user, 2026-10-09: "a dedicated section within the controls
// panel ... those controls can be different than the ones that are used elsewhere"): the controls
// screen's Photo mode rows, bound per device like every other row, read by the program
// (main/photo.h, camera_input) and asked for here, not the controller the game sees. By default:
//   the left stick or WASD moves the camera across the ground, the way it faces;
//   the right stick or IJKL turns it around the point it looks at;
//   the shoulders, or R and E, raise and lower it;
//   the D-pad's up and down, or the arrows, bring it closer and further;
//   B's button, or Left Shift, puts it back where it was when the freeze began.
// The free camera's own axes row (Camera axes) inverts the turning, as it does for the free camera.

#include "patches.h"

#include "libc64/math64.h"

#include "olib.h"
#include "play_state.h"
#include "room.h"
#include "view.h"
#include "z_lib.h"
#include "z_math.h"

// The program's side (src/game/recomp_api.cpp): 1 while photo mode holds the game.
s32 recomp_photo_mode(void);
// Photo mode's camera from its own bindings, in 4096ths (main/photo.h, CameraInput): 0 across,
// 1 ahead, 2 turn across, 3 turn up, 4 up, 5 closer, 6 the put back press.
s32 recomp_photo_input(s32 which);
// The free camera's rows as bits; bits 1 and 2 invert left and right, up and down.
u32 recomp_free_camera_mode(void);
// Whether the frozen room is drawn over a picture, so the program can say why the camera stays.
void recomp_photo_picture_room(s32 on);

// Per game frame at full deflection.
#define PHOTO_TURN_STEP 0x2C0        // about four degrees
#define PHOTO_PITCH_LIMIT 0x3800     // short of straight up and down, where the view has no up
#define PHOTO_ZOOM_STEP 0.96f
#define PHOTO_MIN_DISTANCE 15.0f
#define PHOTO_MAX_DISTANCE 4000.0f
#define PHOTO_INPUT_FULL 4096.0f
#define PHOTO_MOVE_ACROSS 0
#define PHOTO_MOVE_AHEAD 1
#define PHOTO_TURN_ACROSS 2
#define PHOTO_TURN_UP 3
#define PHOTO_RISE 4
#define PHOTO_CLOSER 5
#define PHOTO_PUT_BACK 6

static s32 sPhotoActive = 0;
static Vec3f sPhotoAt;
static VecGeo sPhotoOffset;     // from the point looked at to the eye
static Vec3f sPhotoStartAt;
static VecGeo sPhotoStartOffset;

// How far the camera travels in a frame at full stick: a share of how far it stands from what it
// looks at, so it feels the same close up and far away.
static f32 PhotoMode_Speed(void) {
    f32 speed = sPhotoOffset.r * 0.04f;
    return (speed < 4.0f) ? 4.0f : speed;
}

// A ROOM DRAWN OVER A PICTURE KEEPS THE GAME'S CAMERA (the user, 2026-10-09, in Link's house: "Link
// actually moved off of the screen ... because you allowed the photo mode to move around inside of
// a fixed environment with a fixed background"). Such a room is a painted backdrop made for the
// game's one camera position, with Link and a little geometry drawn in 3D over it; moving the
// camera moves the 3D part against a picture that cannot move. The room's own shape says so
// (ROOM_SHAPE_TYPE_IMAGE, the test patches/prerender_aspect.c uses for the same reason). The
// Market's streets, 3D geometry seen through a turning camera, are not pictures and move freely.
static s32 PhotoMode_RoomIsPicture(PlayState* play) {
    Room* room = &play->roomCtx.curRoom;
    return (room->roomShape != NULL) && (room->roomShape->base.type == ROOM_SHAPE_TYPE_IMAGE);
}

void PhotoMode_Update(PlayState* play) {
    Vec3f eye;
    Vec3f up;
    Vec3f forward;
    f32 length;
    f32 speed;
    s32 turnX;
    s32 turnY;
    u32 mode;

    if (!recomp_photo_mode()) {
        sPhotoActive = 0;
        return;
    }

    if (PhotoMode_RoomIsPicture(play)) {
        recomp_photo_picture_room(1);
        sPhotoActive = 0;
        return;
    }
    recomp_photo_picture_room(0);

    // The first frozen frame: the camera starts exactly where the game's was.
    if (!sPhotoActive) {
        sPhotoAt = play->view.at;
        sPhotoOffset = OLib_Vec3fDiffToVecGeo(&play->view.at, &play->view.eye);
        if (sPhotoOffset.r < PHOTO_MIN_DISTANCE) {
            sPhotoOffset.r = PHOTO_MIN_DISTANCE;
        }
        sPhotoStartAt = sPhotoAt;
        sPhotoStartOffset = sPhotoOffset;
        sPhotoActive = 1;
    }

    speed = PhotoMode_Speed();
    mode = recomp_free_camera_mode();

    if (recomp_photo_input(PHOTO_PUT_BACK) != 0) {
        sPhotoAt = sPhotoStartAt;
        sPhotoOffset = sPhotoStartOffset;
    }

    turnX = (recomp_photo_input(PHOTO_TURN_ACROSS) * PHOTO_TURN_STEP) >> 12;
    turnY = (recomp_photo_input(PHOTO_TURN_UP) * PHOTO_TURN_STEP) >> 12;
    if (mode & 2) {
        turnX = -turnX;
    }
    if (mode & 4) {
        turnY = -turnY;
    }
    sPhotoOffset.yaw -= (s16)turnX;
    {
        // The same senses as the free camera (free_camera.c): both turns are taken away.
        s32 pitch = sPhotoOffset.pitch - turnY;
        if (pitch > PHOTO_PITCH_LIMIT) {
            pitch = PHOTO_PITCH_LIMIT;
        }
        if (pitch < -PHOTO_PITCH_LIMIT) {
            pitch = -PHOTO_PITCH_LIMIT;
        }
        sPhotoOffset.pitch = (s16)pitch;
    }

    // Closer and further, by a share of the distance a frame at full press.
    {
        f32 closer = (f32)recomp_photo_input(PHOTO_CLOSER) / PHOTO_INPUT_FULL;

        if (closer > 0.0f) {
            sPhotoOffset.r *= 1.0f - (1.0f - PHOTO_ZOOM_STEP) * closer;
        }
        else if (closer < 0.0f) {
            sPhotoOffset.r /= 1.0f + (1.0f - PHOTO_ZOOM_STEP) * closer;
        }
    }
    if (sPhotoOffset.r < PHOTO_MIN_DISTANCE) {
        sPhotoOffset.r = PHOTO_MIN_DISTANCE;
    }
    if (sPhotoOffset.r > PHOTO_MAX_DISTANCE) {
        sPhotoOffset.r = PHOTO_MAX_DISTANCE;
    }

    // Across the ground, the way the camera faces: forward is from the eye toward the point
    // looked at, level; right is forward turned a quarter clockwise seen from above.
    eye = OLib_VecGeoToVec3f(&sPhotoOffset);
    forward.x = -eye.x;
    forward.y = 0.0f;
    forward.z = -eye.z;
    length = sqrtf(SQ(forward.x) + SQ(forward.z));
    if (length > 0.001f) {
        f32 ahead = (f32)recomp_photo_input(PHOTO_MOVE_AHEAD) / PHOTO_INPUT_FULL;
        f32 aside = (f32)recomp_photo_input(PHOTO_MOVE_ACROSS) / PHOTO_INPUT_FULL;

        forward.x /= length;
        forward.z /= length;
        sPhotoAt.x += (forward.x * ahead - forward.z * aside) * speed;
        sPhotoAt.z += (forward.z * ahead + forward.x * aside) * speed;
    }

    // Up and down, by default on the two shoulders (the user, 2026-10-09: "L2 moves the camera down
    // vertically and R1 moves it up vertically ... either do both two buttons or both one buttons").
    sPhotoAt.y += speed * ((f32)recomp_photo_input(PHOTO_RISE) / PHOTO_INPUT_FULL);

    eye = OLib_VecGeoToVec3f(&sPhotoOffset);
    eye.x += sPhotoAt.x;
    eye.y += sPhotoAt.y;
    eye.z += sPhotoAt.z;
    up.x = 0.0f;
    up.y = 1.0f;
    up.z = 0.0f;
    View_LookAt(&play->view, &eye, &sPhotoAt, &up);
}
