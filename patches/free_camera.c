// The free camera on the right stick (upgrades phases 79 onward; the user, 2026-09-24: "build
// out the free aim camera for right stick (being sure to exclude from the static camera
// scenes)").
//
// WHAT. While the Free camera row is on, the right stick turns the camera around Link: left and
// right orbit, up and down tilt, at a fixed rate per game frame at full deflection. Once the
// stick has been touched the camera stays where it was put, the game's own follow (the swing to
// behind Link as he walks) standing aside, until the camera changes mode or setting (a Z target,
// a talk, a climb, a door), or the ocarina comes out; then the game's camera has it back, and
// the next touch of the stick starts from wherever the camera is then. The Camera axes row
// inverts either axis or both.
//
// THE EXCLUSION. This is a patch of Camera_Normal1 alone, the update the game runs for the
// normal, still, jump and free fall modes of every walking camera setting (Normal0 and 1, the
// dungeons, the bosses, the beans). A fixed camera (the prerendered rooms, the shops, the
// pivots, the market balcony), a cutscene, a Z target, a first person look, a talk, a climb, a
// ledge hang and the horse all run OTHER update functions, which are not touched, so the stick
// does nothing there and the scene's camera stays exactly the game's. The horse (Normal3) is a
// later phase.
//
// THE STICK'S C BUTTONS. The right stick's four directions are C buttons by default, and the
// game cannot have both. Each frame this patch tells the program whether the camera holds the
// stick (recomp_free_camera_holds), and the program leaves those bindings silent while it does,
// except with the ocarina out, when the notes need them. C up and C right already have face
// buttons as second bindings; C left and C down get the left bumper and the right trigger
// (src/main/input_bindings.cpp).
//
// THE TECHNIQUE is the reference project's (its camera_patches.c, an analog camera for the
// other game's Camera_Normal1): the game's own yaw and pitch step replaced by a pair the patch
// keeps while the stick is in charge. Every function here is this game's own, reproduced from
// the decompilation at the pinned commit; a patch replaces the whole function, so everything it
// did is here.

#include "patches.h"

#include "libc64/math64.h"

#include "actor.h"
#include "bgcheck.h"
#include "camera.h"
#include "olib.h"
#include "play_state.h"
#include "player.h"
#include "regs.h"
#include "save.h"
#include "z_lib.h"
#include "z_math.h"
#include "libc64/qrand.h"

// ---- the program's side (src/game/recomp_api.cpp) -----------------------------------------

// Bit 0: the Free camera row is on. Bit 1: left and right inverted. Bit 2: up and down inverted.
// Bits 3 and 4: the same two for the first person look and the aiming, from the Aiming axes row
// (the user, 2026-09-24: "Separate up and down ... for first person and/or the slingshot versus
// just normal free camera"). Bits 5 and 6: the Camera distance row, 0 Near (the game's own), 1
// Original, 2 Far. Bits 7 and 8: the same two again for the LEFT stick while aiming, which is the
// game's own aim and has its own row ("some like left and some like right"). Bit 9: the Camera
// drift row is OFF, so the distance is held instead of easing out as he runs.
DECLARE_FUNC(u32, recomp_free_camera_mode, void);
// The right stick, raw from the pad and past its deadzone: axis 0 is right positive, axis 1 is
// up positive, each as a 12 bit fraction (-4096 to 4096).
DECLARE_FUNC(s32, recomp_free_camera_stick, s32 axis);
// Whether the camera holds the stick this frame, so the program can leave the stick's C button
// bindings silent (1) or let them through (0).
DECLARE_FUNC(void, recomp_free_camera_holds, s32 holds);

// ---- the game's own, as z_camera.c has them ------------------------------------------------

// The retail constants of z_camera.c (DEBUG_FEATURES off), by the names it uses.
#define CAM_DATA_SCALED(x) ((x) * 0.01f)
#define GET_NEXT_RO_DATA(values) ((values++)->val)
#define GET_NEXT_SCALED_RO_DATA(values) CAM_DATA_SCALED(GET_NEXT_RO_DATA(values))
#define RELOAD_PARAMS(camera) (camera->animState == 0 || camera->animState == 10 || camera->animState == 20)
#define CAM_DEBUG_RELOAD_PARAMS true
#define CAM_DEBUG_RELOAD_PREG(camera) (void)0
#define CAM_XZ_OFFSET_UPDATE_RATE .05f
#define CAM_Y_OFFSET_UPDATE_RATE .05f
#define CAM_FOV_UPDATE_RATE .05f
#define CAM_PITCH_UPDATE_RATE_INV 16
#define CAM_R_UPDATE_RATE_INV 20
#define CAM_UPDATE_RATE_STEP_SCALE_XZ .50f
#define CAM_UPDATE_RATE_STEP_SCALE_Y .20f
#define CAM_GLOBAL_27 1800
#define CAM_YOFFSET_NORM -0.1f
#define CAM_GLOBAL_49 0.7f
#define CAM_GLOBAL_50 20
#define CAM_GLOBAL_51 20

// The camera setting tables, local to z_camera_data.inc.c (its own layout, eight bytes).
typedef struct CameraModeValue {
    s16 val;
    s16 dataType;
} CameraModeValue;

typedef struct CameraMode {
    s16 funcIdx;
    s16 valueCnt;
    CameraModeValue* values;
} CameraMode;

typedef struct CameraSetting {
    u32 bits;
    CameraMode* cameraModes;
} CameraSetting;

extern CameraSetting sCameraSettings[];
extern s32 sCameraInterfaceField;
extern s32 sUpdateCameraDirection;

f32 Camera_LERPCeilF(f32 target, f32 cur, f32 stepScale, f32 minDiff);
s16 Camera_LERPCeilS(s16 target, s16 cur, f32 stepScale, s16 minDiff);
Vec3f Camera_AddVecGeoToVec3f(Vec3f* a, VecGeo* geo);
s32 Camera_BGCheck(Camera* camera, Vec3f* from, Vec3f* to);
s16 Camera_GetPitchAdjFromFloorHeightDiffs(Camera* camera, s16 viewYaw, s16 initAndReturnZero);
f32 Camera_ClampLERPScale(Camera* camera, f32 maxLERPScale);
s32 Camera_CalcAtDefault(Camera* camera, VecGeo* eyeAtDir, f32 yOffset, s16 calcSlopeYAdj);
s32 func_800458D4(Camera* camera, VecGeo* eyeAtDir, f32 yOffset, f32* arg3, s16 calcSlopeYAdj);
s32 func_80045B08(Camera* camera, VecGeo* eyeAtDir, f32 yOffset, s16 arg3);
f32 Camera_ClampDist(Camera* camera, f32 dist, f32 minDist, f32 maxDist, s16 timer);
s16 Camera_CalcDefaultPitch(Camera* camera, s16 arg1, s16 arg2, s16 arg3);
s16 Camera_CalcDefaultYaw(Camera* camera, s16 cur, s16 target, f32 arg3, f32 accel);
void func_80046E20(Camera* camera, VecGeo* eyeAdjustment, f32 minDist, f32 arg3, f32* arg4, SwingAnimation* anim);

// ---- the free camera's own state -------------------------------------------------------------

// How far the camera turns per game frame at full deflection, in the game's angle units (65536
// to the turn): the reference project's rates, about eight degrees of yaw and three of pitch.
#define FREE_CAMERA_YAW_RATE 1500
#define FREE_CAMERA_PITCH_RATE 500
// The eye's pitch over the point it looks at, kept between looking a little up at Link and
// looking well down on him (the game's own normal camera allows -85 to 80 degrees; this stays
// short of the ground and the sky).
#define FREE_CAMERA_PITCH_MIN (-0x1000)
#define FREE_CAMERA_PITCH_MAX 0x3000
// How far above the floor the eye is kept when the stick asks for lower, world units.
#define FREE_CAMERA_FLOOR_CLEARANCE 20.0f
// The Camera distance row's own numbers are further down, beside Camera_ClampDist, because that
// is where they are applied now and they belong with the code that reads them.
#define FREE_CAMERA_WALL_CLEARANCE 10.0f    // off the wall the eye stops at
#define FREE_CAMERA_MIN_DISTANCE 40.0f      // never inside him; Link is about 60 tall
// The floor when something is in the way, which is SHORTER than the one above on purpose: the
// minimum distance is about where the camera looks right, and this is about not seeing through a
// wall, which wins. Only enough to keep the eye out of his own model.
#define FREE_CAMERA_MIN_HELD 12.0f

f32 Camera_GetFloorY(Camera* camera, Vec3f* pos);

static s16 sFreeYaw;
static s16 sFreePitch;
static u8 sFreeActive;
static s16 sFreeLastMode = -1;
static s16 sFreeLastSetting = -1;
static u32 sFreeLastFrame;

// THE CAMERA DISTANCE ROW, AND THE DRIFT ROW, WHERE EVERY PLAYER CAMERA PASSES (the user,
// 2026-09-24: "we also need to ensure this doesn't only affect free roam distance but affects
// both free camera and game camera which is the cam it uses after z targeting or if not uing
// free"; "the slight pull away with movement or as link gets closer can be an option to disable
// or enable").
//
// The row used to live inside free_camera_take, which only runs while the stick holds the view,
// so the game's own camera and the one Z targeting hands back to ignored it. Camera_ClampDist is
// the one place all of them meet: Normal0 to Normal3, Battle1, KeepOn and Parallel1 all hand it
// the setting's own near and far bounds and take back the distance to use. Scaling those two
// bounds there moves every one of them, and nothing else has to be patched to make it so.
//
// 1.0 is THE GAME'S OWN, and since 2026-09-26 the row calls that step "Original", which it did
// not: the labels were { Near, Original, Medium, Far } and the one called Original was a fifth
// further out than the console's camera. That was honest when written, because the row then had a
// step BELOW the game's own and Original sat in the middle; the nearer step was dropped for
// putting the eye inside him in tight places and the labels were never revisited. The row now
// reads { Original, A little further, Further, Furthest } and the scales below are untouched, so
// no camera moved. Four is the most the two bits carrying the row can hold, so a fifth step would
// need another bit.
static const f32 sDistanceScale[4] = { 1.0f, 1.2f, 1.3f, 1.4f };

// The row's bits for this camera, or 0 for any camera that is not the one the player is behind
// (a cutscene's subcamera keeps the distances its scene asked for).
static u32 camera_distance_bits(Camera* camera) {
    if ((camera == NULL) || (camera->camId != CAM_ID_MAIN)) {
        return 0;
    }
    return recomp_free_camera_mode();
}

// Whether the distance is held at one value rather than easing out as he runs and in as he
// stops. The row (bit 9) says so for the game's own camera; while the stick is holding the view
// it is always so, because the free camera solves its distance from Link every frame and a drift
// on top of that is the thing the user asked to be rid of ("drift also doesn't need to be in the
// free mode. only in original cam mode").
static s32 camera_distance_pinned(u32 bits) {
    return ((bits & 0x200) != 0) || (sFreeActive != 0);
}

// @recomp The game's own function with the row applied to the bounds it is given. Pinned, the
// near bound becomes the whole range, so the eye keeps one distance.
RECOMP_PATCH f32 Camera_ClampDist(Camera* camera, f32 dist, f32 minDist, f32 maxDist, s16 timer) {
    f32 distTarget;
    f32 rUpdateRateInvTarget;
    u32 bits = camera_distance_bits(camera);
    f32 scale = sDistanceScale[(bits >> 5) & 3];

    minDist *= scale;
    maxDist *= scale;
    if (camera_distance_pinned(bits)) {
        maxDist = minDist;
    }

    if (dist < minDist) {
        distTarget = minDist;

        rUpdateRateInvTarget = timer != 0 ? CAM_R_UPDATE_RATE_INV * 0.5f : CAM_R_UPDATE_RATE_INV;
    } else if (maxDist < dist) {
        distTarget = maxDist;

        rUpdateRateInvTarget = timer != 0 ? CAM_R_UPDATE_RATE_INV * 0.5f : CAM_R_UPDATE_RATE_INV;
    } else {
        distTarget = dist;

        rUpdateRateInvTarget = timer != 0 ? CAM_R_UPDATE_RATE_INV : 1.0f;
    }

    camera->rUpdateRateInv =
        Camera_LERPCeilF(rUpdateRateInvTarget, camera->rUpdateRateInv, CAM_UPDATE_RATE_STEP_SCALE_XZ, 0.1f);
    return Camera_LERPCeilF(distTarget, camera->dist, 1.0f / camera->rUpdateRateInv, 0.2f);
}

// Whether the stick has the eye this frame and, if so, the yaw and pitch it puts it at. Called
// by Camera_Normal1 once it has the game's own yaw and pitch (atEyeNext, the eye's present
// direction from the point it looks at) and is about to step them.
static s32 free_camera_take(Camera* camera, VecGeo* atEyeNextGeo, VecGeo* eyeAdjustment) {
    u32 mode = recomp_free_camera_mode();
    Player* player = (camera->play != NULL) ? GET_PLAYER(camera->play) : NULL;
    s32 ocarina = (player != NULL) && ((player->stateFlags2 & PLAYER_STATE2_USING_OCARINA) != 0);
    s32 holds = ((mode & 1) != 0) && !ocarina && (camera->camId == CAM_ID_MAIN);
    s32 x;
    s32 y;

    recomp_free_camera_holds(holds);
    if (!holds) {
        sFreeActive = 0;
        return 0;
    }

    // A change of mode or setting (a Z target begun or ended, a talk, a climb, a door) hands
    // the camera back to the game; the next touch starts from wherever it is then.
    //
    // THE FRAMES THIS FUNCTION DID NOT RUN COUNT AS A CHANGE TOO. While Z is held with nothing
    // to target the game runs its parallel camera instead of this one, which puts the eye
    // behind Link; when Z is released this function resumes with the mode it last saw, so the
    // mode test alone saw no change and the held yaw and pitch snapped the view back to where
    // the stick had left it (the user, 2026-09-24: "z down shows front correctly but release
    // switches back. z should remain faced forward and used as a centering"). A gap in the
    // frame count, or the game's own parameter reload on re-entry, drops the held pair, so Z
    // recenters and the camera stays behind Link until the stick is touched again.
    if ((camera->mode != sFreeLastMode) || (camera->setting != sFreeLastSetting) ||
        (camera->play->state.frames != sFreeLastFrame + 1) || RELOAD_PARAMS(camera)) {
        sFreeActive = 0;
    }
    sFreeLastMode = camera->mode;
    sFreeLastSetting = camera->setting;
    sFreeLastFrame = camera->play->state.frames;

    x = recomp_free_camera_stick(0);
    y = recomp_free_camera_stick(1);
    if (!sFreeActive) {
        sFreeYaw = atEyeNextGeo->yaw;
        sFreePitch = atEyeNextGeo->pitch;
        if ((x == 0) && (y == 0)) {
            return 0;
        }
        sFreeActive = 1;
    }

    if (mode & 2) {
        x = -x;
    }
    if (mode & 4) {
        y = -y;
    }

    // Stick right turns the view to the right, which moves the eye the other way round Link;
    // stick up tilts the view up, which lowers the eye. The Camera axes row flips either.
    sFreeYaw = (s16)(sFreeYaw - (s16)((x * FREE_CAMERA_YAW_RATE) >> 12));
    sFreePitch = (s16)(sFreePitch - (s16)((y * FREE_CAMERA_PITCH_RATE) >> 12));
    if (sFreePitch > FREE_CAMERA_PITCH_MAX) {
        sFreePitch = FREE_CAMERA_PITCH_MAX;
    }
    if (sFreePitch < FREE_CAMERA_PITCH_MIN) {
        sFreePitch = FREE_CAMERA_PITCH_MIN;
    }

    // THE FLOOR (the user, 2026-09-24: "if i free aim the camera to touch floor it prevents
    // turning rather than sliding across surface of ground"). The game's own collision keeps
    // the eye out of the floor by refusing the move, and a held pitch that asks for the floor
    // every frame had the eye refused every frame, so the yaw stopped with it. The held pitch
    // yields instead: where the eye the pitch asks for would be under the floor, the pitch is
    // raised to the one that keeps it a little above, and the eye slides over the ground as the
    // stick turns it.
    {
        Vec3f eye;
        f32 floorY;
        VecGeo asked;

        asked = *eyeAdjustment;
        asked.yaw = sFreeYaw;
        asked.pitch = sFreePitch;
        eye = Camera_AddVecGeoToVec3f(&camera->at, &asked);
        floorY = Camera_GetFloorY(camera, &eye);
        if ((floorY != BGCHECK_Y_MIN) && (eye.y < floorY + FREE_CAMERA_FLOOR_CLEARANCE)) {
            Vec3f lifted = eye;
            VecGeo over;

            lifted.y = floorY + FREE_CAMERA_FLOOR_CLEARANCE;
            over = OLib_Vec3fDiffToVecGeo(&camera->at, &lifted);
            if (sFreePitch < over.pitch) {
                sFreePitch = over.pitch;
            }
        }
    }

    // THE DISTANCE, AND THE WALL (2026-09-24). Turning the camera moves the point it looks at,
    // which is set ahead of Link along the view, so orbiting alone carried the eye further from
    // him than the game ever puts it ("the free camera move backs away from link too much").
    // The distance is taken from the game's own for this camera and this frame and scaled by
    // the row, so Original is exactly what the game would keep. And where something stands
    // between him and the eye the stick asks for, the eye comes IN toward him rather than the
    // camera swinging round the obstacle, which is the game's own answer and would throw away
    // the aim the stick just set.
    {
        VecGeo want;
        VecGeo unit;
        VecGeo hit;
        Vec3f eye;
        Vec3f origin;
        Vec3f direction;
        Vec3f toAt;
        f32 wanted;
        f32 along;
        f32 offset2;
        f32 under;

        // MEASURED FROM LINK, NOT FROM THE POINT THE CAMERA LOOKS AT (the user, 2026-09-24, with
        // the row on Original: the camera starts where the game's own would and then backs away
        // as he runs forward). That point is set ahead of him along the view and moves further
        // ahead the faster he goes, so a distance measured from it puts the eye further and
        // further from HIM while reading as unchanged. The distance the row means is his.
        //
        // The eye still sits on the line the stick's yaw and pitch name from that point, so the
        // view is the one the stick set; what is solved for is how far along that line the eye
        // must be for it to stand `wanted` from Link. One root of a quadratic, the one in front.
        // camera->dist ALREADY CARRIES THE ROW (the user, 2026-09-24: "it must affect both").
        // The scale moved into Camera_ClampDist, which every player camera passes through, so
        // the game's own camera and the one Z targeting hands back to stand where the row says
        // too. Scaling again here would square it.
        wanted = camera->dist;
        unit.r = 1.0f;
        unit.yaw = sFreeYaw;
        unit.pitch = sFreePitch;
        origin.x = 0.0f;
        origin.y = 0.0f;
        origin.z = 0.0f;
        direction = Camera_AddVecGeoToVec3f(&origin, &unit);
        toAt.x = camera->at.x - camera->playerPosRot.pos.x;
        toAt.y = camera->at.y - camera->playerPosRot.pos.y;
        toAt.z = camera->at.z - camera->playerPosRot.pos.z;
        along = (toAt.x * direction.x) + (toAt.y * direction.y) + (toAt.z * direction.z);
        offset2 = (toAt.x * toAt.x) + (toAt.y * toAt.y) + (toAt.z * toAt.z);
        under = (along * along) - (offset2 - (wanted * wanted));
        want.r = (under > 0.0f) ? (-along + sqrtf(under)) : wanted;

        // THE ROOT COMES BACK NEGATIVE MORE OFTEN THAN IT LOOKS, and a negative radius is not
        // "a bit too close": it MIRRORS the eye through the point the camera looks at (r scales
        // the unit direction, so a sign flip reverses it). That point is set AHEAD of Link along
        // the view, so a mirrored eye lands on the far side of him, which is the warp (the user,
        // 2026-09-30: "sometimes it gets stuck and warps to other side of link").
        //
        // WHEN: the root is -along + sqrt(along^2 - offset2 + wanted^2), which is negative
        // exactly when `wanted < offset2's root`, that is when the point the camera looks at is
        // FURTHER from Link than the distance we want the eye to stand at. The game throws that
        // point further ahead the faster he runs, so running fast is enough to cross it, with no
        // wall involved at all. `under > 0` does not catch it: under stays positive.
        //
        // Clamped HERE, before the wall test, rather than after it as it was. After was too late
        // twice over: the mirrored eye was the point handed to the collision check, so the wall
        // answer came from a ray pointing the wrong way, and the clamp then turned that answer
        // into a radius on the correct side. Garbage in, plausible number out.
        if (want.r < FREE_CAMERA_MIN_DISTANCE) {
            want.r = FREE_CAMERA_MIN_DISTANCE;
        }
        want.yaw = sFreeYaw;
        want.pitch = sFreePitch;
        eye = Camera_AddVecGeoToVec3f(&camera->at, &want);
        if (Camera_BGCheck(camera, &camera->at, &eye)) {
            hit = OLib_Vec3fDiffToVecGeo(&camera->at, &eye);
            want.r = hit.r - FREE_CAMERA_WALL_CLEARANCE;
            // AND THE ORDINARY MINIMUM MUST NOT APPLY TO A PULL OFF A WALL (the user,
            // 2026-09-30: "this would honestly simaltaneously prevent the ability to see through
            // walls"). Clamping back UP to 40 units after stopping at, say, 25 puts the eye
            // through the wall it was just pulled out of, which is the see-through. In front of
            // the wall beats standing off Link, and he settled the same tension for the game's
            // own cameras in the same breath: "these instances would now just be closer to
            // link". So the floor here is only enough to keep the eye out of his own model.
            if (want.r < FREE_CAMERA_MIN_HELD) {
                want.r = FREE_CAMERA_MIN_HELD;
            }
        }
        eyeAdjustment->r = want.r;
    }

    // THE LEDGE, AND IT IS THE FLOOR GUARD ABOVE TESTING A POINT THE EYE NEVER OCCUPIES (the user,
    // 2026-09-30: "if link is on ground floor, and eye is above a ledge floor and then sinks to be
    // inline with link it fall through the floor rather than scooting along floor and toward link
    // and then descending the wall").
    //
    // That guard runs before the block above and builds its test eye from the radius the GAME's
    // step happened to leave in eyeAdjustment. The block above then solves the real radius and
    // overwrites it. On open ground the two points are close enough that sampling the wrong one
    // costs nothing, which is why it has worked since it was written. On a LEDGE the floor height
    // changes by its whole height across a small horizontal step, so sampling a point even a
    // little away from the eye is the difference between finding the ledge and finding the ground
    // far below it, and the guard then sees nothing in the way.
    //
    // So the floor is checked again HERE, on the eye that actually results, and answered the way
    // he asked for and the way the wall already answers: the radius COMES IN, keeping the aim the
    // stick set, so the eye travels in over the ledge rather than through it. Once it has come
    // past the edge the floor under it is the low ground again, this stops applying, and it
    // descends the face on its own.
    //
    // The pitch is deliberately not touched here. Raising it is the answer to a different
    // question (the ground slide of 2026-09-24) and that guard above still owns it.
    {
        Vec3f eye;
        f32 floorY;
        VecGeo asked;
        s32 step;

        asked.yaw = sFreeYaw;
        asked.pitch = sFreePitch;
        // A few passes rather than one: pulling the eye in moves it horizontally as well as up,
        // so the floor under the new point may be different again (a stair, a stepped terrace).
        // Four is enough for anything this game builds and is bounded, which a while loop is not.
        for (step = 0; step < 4; step++) {
            asked.r = eyeAdjustment->r;
            eye = Camera_AddVecGeoToVec3f(&camera->at, &asked);
            floorY = Camera_GetFloorY(camera, &eye);
            if ((floorY == BGCHECK_Y_MIN) || (eye.y >= floorY + FREE_CAMERA_FLOOR_CLEARANCE)) {
                break;
            }
            // How far along the stick's line the eye has to come to stand a clearance above this
            // floor. sin(pitch) is its rise per unit of radius, so the radius that puts it at the
            // floor's height is the height difference over that rise. Below a hundredth the line
            // is level enough that no radius answers, and the ground slide guard has it instead.
            {
                f32 rise = Math_SinS(sFreePitch);
                f32 wanted;

                if (rise > -0.01f) {
                    break;
                }
                wanted = (floorY + FREE_CAMERA_FLOOR_CLEARANCE - camera->at.y) / rise;
                if (wanted < FREE_CAMERA_MIN_HELD) {
                    wanted = FREE_CAMERA_MIN_HELD;
                }
                if (wanted >= eyeAdjustment->r) {
                    break;   // already in front of it; nothing to do
                }
                eyeAdjustment->r = wanted;
            }
        }
    }

    eyeAdjustment->yaw = sFreeYaw;
    eyeAdjustment->pitch = sFreePitch;
    return 1;
}

// THE HORSE (the user, 2026-09-24: "we need same free camera on horse"). The horse's camera
// setting runs Camera_Normal3 for its normal mode, its own function with its own step for the
// eye's yaw and pitch; reproduced from the decompilation with the same one insertion, before
// the pitch clamp. The horse's own follow (the yaw swinging behind Epona as she runs) stands
// aside while the stick holds the camera, as Link's does.
#define CAM_MAX_PITCH 14500
#define CAM_MIN_PITCH_1 -5460
#define CAM_DEFAULT_ANIM_TIME 4
#define FC_ABS(x) ((x) >= 0 ? (x) : -(x))

s32 Camera_CalcAtForHorse(Camera* camera, VecGeo* eyeAtDir, f32 yOffset, f32* yPosOffset, s16 calcSlopeYAdj);

// @recomp Patched so the right stick can take the eye's yaw and pitch on the horse. Reproduced
// from the decompilation's z_camera.c; the one insertion is marked.
RECOMP_PATCH s32 Camera_Normal3(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    f32 sp98;
    f32 sp94;
    f32 sp90;
    f32 sp8C;
    VecGeo sp84;
    VecGeo sp7C;
    VecGeo sp74;
    PosRot* playerPosRot = &camera->playerPosRot;
    f32 temp_f0;
    s16 phi_a0;
    s16 t2;
    s32 freeHolds; // @recomp whether the stick took the eye this frame
    Normal3ReadOnlyData* roData = &camera->paramData.norm3.roData;
    Normal3ReadWriteData* rwData = &camera->paramData.norm3.rwData;
    f32 playerHeight;

    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || CAM_DEBUG_RELOAD_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->yOffset = GET_NEXT_RO_DATA(values) * CAM_DATA_SCALED(playerHeight);
        roData->distMin = GET_NEXT_RO_DATA(values) * CAM_DATA_SCALED(playerHeight);
        roData->distMax = GET_NEXT_RO_DATA(values) * CAM_DATA_SCALED(playerHeight);
        roData->pitchTarget = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->yawUpdateSpeed = GET_NEXT_RO_DATA(values);
        roData->unk_10 = GET_NEXT_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->maxAtLERPScale = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    CAM_DEBUG_RELOAD_PREG(camera);

    sp7C = OLib_Vec3fDiffToVecGeo(at, eye);
    sp74 = OLib_Vec3fDiffToVecGeo(at, eyeNext);

    sUpdateCameraDirection = true;
    sCameraInterfaceField = roData->interfaceField;
    switch (camera->animState) {
        case 0:
        case 10:
        case 20:
        case 25:
            rwData->swing.atEyePoly = NULL;
            rwData->curPitch = 0;
            rwData->unk_1C = 0.0f;
            rwData->unk_20 = camera->playerGroundY;
            rwData->swing.unk_16 = rwData->swing.unk_14 = rwData->swing.unk_18 = 0;
            rwData->swing.swingUpdateRate = roData->yawUpdateSpeed;
            rwData->yawUpdAmt = (s16)((s16)(playerPosRot->rot.y - 0x7FFF) - sp7C.yaw) * (1.0f / CAM_DEFAULT_ANIM_TIME);
            rwData->distTimer = 10;
            rwData->yawTimer = CAM_DEFAULT_ANIM_TIME;
            camera->animState = 1;
            rwData->swing.swingUpdateRateTimer = 0;
    }

    if (rwData->distTimer != 0) {
        rwData->distTimer--;
    }

    sp98 = CAM_UPDATE_RATE_STEP_SCALE_XZ * camera->speedRatio;
    sp94 = CAM_UPDATE_RATE_STEP_SCALE_Y * camera->speedRatio;

    if (rwData->swing.swingUpdateRateTimer != 0) {
        camera->yawUpdateRateInv = Camera_LERPCeilF(roData->yawUpdateSpeed + (rwData->swing.swingUpdateRateTimer * 2),
                                                    camera->yawUpdateRateInv, sp98, 0.1f);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF((f32)CAM_PITCH_UPDATE_RATE_INV + (rwData->swing.swingUpdateRateTimer * 2),
                             camera->pitchUpdateRateInv, sp94, 0.1f);
        rwData->swing.swingUpdateRateTimer--;
    } else {
        camera->yawUpdateRateInv = Camera_LERPCeilF(roData->yawUpdateSpeed, camera->yawUpdateRateInv, sp98, 0.1f);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF(CAM_PITCH_UPDATE_RATE_INV, camera->pitchUpdateRateInv, sp94, 0.1f);
    }

    camera->xzOffsetUpdateRate = Camera_LERPCeilF(CAM_XZ_OFFSET_UPDATE_RATE, camera->xzOffsetUpdateRate, sp98, 0.1f);
    camera->yOffsetUpdateRate = Camera_LERPCeilF(CAM_Y_OFFSET_UPDATE_RATE, camera->yOffsetUpdateRate, sp94, 0.1f);
    camera->fovUpdateRate = Camera_LERPCeilF(CAM_FOV_UPDATE_RATE, camera->fovUpdateRate, sp94, 0.1f);

    t2 = Camera_GetPitchAdjFromFloorHeightDiffs(camera, sp7C.yaw - 0x7FFF, true);
    sp94 = ((1.0f / roData->unk_10) * 0.5f);
    temp_f0 = (((1.0f / roData->unk_10) * 0.5f) * (1.0f - camera->speedRatio));
    rwData->curPitch = Camera_LERPCeilS(t2, rwData->curPitch, sp94 + temp_f0, 0xF);

    Camera_CalcAtForHorse(camera, &sp74, roData->yOffset, &rwData->unk_20, true);
    // @recomp The midpoint carries the Camera distance row like the bounds do (Camera_ClampDist
    // scales those), or the drift below would pull the eye back to the game's own distance a
    // little every frame and the row would do nothing on the horse.
    sp90 = (roData->distMax + roData->distMin) * 0.5f * sDistanceScale[(camera_distance_bits(camera) >> 5) & 3];
    sp84 = OLib_Vec3fDiffToVecGeo(at, eyeNext);
    camera->dist = sp84.r = Camera_ClampDist(camera, sp84.r, roData->distMin, roData->distMax, rwData->distTimer);
    // @recomp THE DRIFT ROW: the horse camera's own easing toward the middle of its range while
    // moving. Off pins the distance, and the stick holding the view pins it whatever the row says.
    if ((camera->xzSpeed > 0.001f) && !camera_distance_pinned(camera_distance_bits(camera))) {
        sp84.r += (sp90 - sp84.r) * 0.002f;
    }
    phi_a0 = roData->pitchTarget - rwData->curPitch;
    sp84.pitch = Camera_LERPCeilS(phi_a0, sp74.pitch, 1.0f / camera->pitchUpdateRateInv, 0xA);

    phi_a0 = playerPosRot->rot.y - (s16)(sp74.yaw - 0x7FFF);
    if (FC_ABS(phi_a0) > 0x2AF8) {
        if (phi_a0 > 0) {
            phi_a0 = 0x2AF8;
        } else {
            phi_a0 = -0x2AF8;
        }
    }

    sp90 = 1.0f;
    sp98 = 0.5;
    sp94 = camera->speedRatio;
    sp90 -= sp98;
    sp98 = sp98 + (sp94 * sp90);
    sp98 = (sp98 * phi_a0) / camera->yawUpdateRateInv;

    sp84.yaw = fabsf(sp98) > (150.0f * (1.0f - camera->speedRatio)) ? (s16)(sp74.yaw + sp98) : sp74.yaw;

    if (rwData->yawTimer > 0) {
        sp84.yaw += rwData->yawUpdAmt;
        rwData->yawTimer--;
    }

    // @recomp THE FREE CAMERA on the horse: the right stick's yaw and pitch in place of the
    // game's step while it holds them; the clamp below is the game's. The wall swing after is
    // NOT, for the reason written out in Camera_Normal1, and here it was worse: this camera
    // calls func_80046E20 on every active frame with no timer in front of it, so on horseback
    // the eye climbed over a wall every time rather than only while moving.
    freeHolds = free_camera_take(camera, &sp74, &sp84);

    if (sp84.pitch > CAM_MAX_PITCH) {
        sp84.pitch = CAM_MAX_PITCH;
    }
    if (sp84.pitch < CAM_MIN_PITCH_1) {
        sp84.pitch = CAM_MIN_PITCH_1;
    }

    *eyeNext = Camera_AddVecGeoToVec3f(at, &sp84);

    if (freeHolds) {
        rwData->swing.unk_18 = 0;
        *eye = *eyeNext;
    } else if (camera->status == CAM_STAT_ACTIVE) {
        func_80046E20(camera, &sp84, roData->distMin, roData->yawUpdateSpeed, &sp8C, &rwData->swing);
    } else {
        *eye = *eyeNext;
    }

    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->maxAtLERPScale);
    return 1;
}

// THE RIGHT STICK IN FIRST PERSON AND WHILE AIMING (the user, 2026-09-24, on 0.3.0: "going to
// first person and using right stick does nothing? it should honestly work the same"). The
// game's first person look and its aiming (the bow, the slingshot, the hookshot, the boomerang)
// read the LEFT stick, in the player's own code rather than the camera's, and with the free
// camera on the right stick's C up binding is silent, so the right stick did nothing there.
//
// Called from the patched Play_Main (harness_warp.c) before the update reads the frame's input:
// while the free camera holds the stick and the main camera is in one of those modes, the right
// stick's deflection is added to the left stick's in the frame's input, clamped to the range the
// game's own stick reaches, with the Camera axes row's inversions applied, so both sticks look
// and aim and neither doubles the other. The reference project does the same in its input
// patch. Outside those modes the input is left exactly as the pad gave it.
#define FREE_CAMERA_STICK_MAX 60

void FreeCamera_FirstPersonInput(PlayState* play) {
    u32 mode = recomp_free_camera_mode();
    Camera* camera;
    Input* input;
    Player* player;
    s32 x;
    s32 y;
    s32 sumX;
    s32 sumY;

    // THE LEFT STICK'S OWN INVERSIONS (bits 7 and 8) APPLY WITH THE FREE CAMERA OFF, since the
    // left stick is the game's own aim and a person may want it turned round without ever
    // touching the right one (the user, 2026-09-24: "some like left and some like right").
    // Only the right stick's part below needs the row.
    if (((mode & 1) == 0) && ((mode & (128u | 256u)) == 0)) {
        return;
    }
    // NOTHING IS TOUCHED WHILE SOMETHING ELSE IS READING THE STICK (the user, 2026-09-30: "the
    // aiming left stick inversion setting is also inverting pause menu navigation").
    //
    // The fault is that this function edits `input->rel.stick` IN PLACE, and the pause menu and
    // the dialog choices read that same field: the menu's cursor is driven by
    // `pauseCtx->stickAdjX = input->rel.stick_x` (z_kaleido_scope.c) and a yes or no choice by
    // `input->rel.stick_y` (z_message.c). So an inverted aim inverted the menu too.
    //
    // WHY THE CAMERA MODE DID NOT ALREADY PREVENT IT, which is the part worth writing down:
    // while the game is paused, Play_Update skips Camera_Update and every actor, so the main
    // camera's mode is FROZEN at whatever it was when the pause began. Pause while aiming a bow
    // or looking around in first person and the mode stays an aiming one for as long as the menu
    // is up, so the switch below passes on every one of those frames and the menu gets a flipped
    // stick. The mode test says where the camera is, never whether the player is in control.
    //
    // The ocarina test further down is the same bug found once before and patched at the one
    // place it showed. This is the general form of it, so that test stays only as the narrower
    // case it also covers (the ocarina is played with a message on screen anyway).
    //
    // The right stick's addition below needed this just as much: paused in first person with the
    // free camera on, it was moving the menu's cursor on its own.
    if (play->pauseCtx.state != PAUSE_STATE_OFF) {
        return;
    }
    if (play->msgCtx.msgMode != MSGMODE_NONE) {
        return;
    }
    camera = play->cameraPtrs[CAM_ID_MAIN];
    if (camera == NULL) {
        return;
    }
    switch (camera->mode) {
        case CAM_MODE_FIRST_PERSON:
        case CAM_MODE_AIM_ADULT:
        case CAM_MODE_AIM_CHILD:
        case CAM_MODE_AIM_BOOMERANG:
            break;
        default:
            return;
    }
    player = GET_PLAYER(play);
    if ((player != NULL) && ((player->stateFlags2 & PLAYER_STATE2_USING_OCARINA) != 0)) {
        return;
    }

    input = &play->state.input[0];

    // THE LEFT STICK FIRST, in place, before anything is added to it: in these modes it is the
    // look and the aim rather than a step, so turning it round is the aim's inversion and
    // nothing else moves. Its pair is its own (bits 7 and 8).
    if (mode & 128u) {
        input->rel.stick_x = (s8)(-input->rel.stick_x);
    }
    if (mode & 256u) {
        input->rel.stick_y = (s8)(-input->rel.stick_y);
    }

    // The right stick is this program's addition and needs the Free camera row.
    if ((mode & 1) == 0) {
        return;
    }

    // Twelve bit fractions to the game's stick units, and the AIMING row's inversions (bits 3
    // and 4), not the free camera's.
    x = (recomp_free_camera_stick(0) * 127) >> 12;
    y = (recomp_free_camera_stick(1) * 127) >> 12;
    if (mode & 8) {
        x = -x;
    }
    if (mode & 16) {
        y = -y;
    }
    if (x > FREE_CAMERA_STICK_MAX) x = FREE_CAMERA_STICK_MAX;
    if (x < -FREE_CAMERA_STICK_MAX) x = -FREE_CAMERA_STICK_MAX;
    if (y > FREE_CAMERA_STICK_MAX) y = FREE_CAMERA_STICK_MAX;
    if (y < -FREE_CAMERA_STICK_MAX) y = -FREE_CAMERA_STICK_MAX;
    if ((x == 0) && (y == 0)) {
        return;
    }

    sumX = input->rel.stick_x + x;
    sumY = input->rel.stick_y + y;
    if (sumX > FREE_CAMERA_STICK_MAX) sumX = FREE_CAMERA_STICK_MAX;
    if (sumX < -FREE_CAMERA_STICK_MAX) sumX = -FREE_CAMERA_STICK_MAX;
    if (sumY > FREE_CAMERA_STICK_MAX) sumY = FREE_CAMERA_STICK_MAX;
    if (sumY < -FREE_CAMERA_STICK_MAX) sumY = -FREE_CAMERA_STICK_MAX;
    // The relative stick is what the player's code reads: the raw stick past a deadzone of
    // seven and capped at sixty (PadUtils_UpdateRelXY), which is the range kept to here.
    input->rel.stick_x = sumX;
    input->rel.stick_y = sumY;
}

// @recomp Patched so the right stick can take the eye's yaw and pitch. Reproduced from the
// decompilation's z_camera.c; the one insertion is marked.
RECOMP_PATCH s32 Camera_Normal1(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    f32 spA0;
    f32 sp9C;
    f32 sp98;
    f32 sp94;
    Vec3f sp88;
    s16 wiggleAdj;
    s16 t;
    VecGeo eyeAdjustment;
    VecGeo atEyeGeo;
    VecGeo atEyeNextGeo;
    s32 freeHolds; // @recomp whether the stick took the eye this frame
    PosRot* playerPosRot = &camera->playerPosRot;
    Normal1ReadOnlyData* roData = &camera->paramData.norm1.roData;
    Normal1ReadWriteData* rwData = &camera->paramData.norm1.rwData;
    f32 playerHeight;
    f32 rate = 0.1f;

    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || CAM_DEBUG_RELOAD_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal = (1.0f + CAM_YOFFSET_NORM - CAM_YOFFSET_NORM * (68.0f / playerHeight));

        sp94 = yNormal * CAM_DATA_SCALED(playerHeight);

        roData->yOffset = GET_NEXT_RO_DATA(values) * sp94;
        roData->distMin = GET_NEXT_RO_DATA(values) * sp94;
        roData->distMax = GET_NEXT_RO_DATA(values) * sp94;
        roData->pitchTarget = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->unk_0C = GET_NEXT_RO_DATA(values);
        roData->unk_10 = GET_NEXT_RO_DATA(values);
        roData->unk_14 = GET_NEXT_SCALED_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->atLERPScaleMax = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    CAM_DEBUG_RELOAD_PREG(camera);

    sCameraInterfaceField = roData->interfaceField;

    atEyeGeo = OLib_Vec3fDiffToVecGeo(at, eye);
    atEyeNextGeo = OLib_Vec3fDiffToVecGeo(at, eyeNext);

    switch (camera->animState) {
        case 20:
            camera->yawUpdateRateInv = CAM_GLOBAL_27;
            camera->pitchUpdateRateInv = CAM_GLOBAL_27;
            // fallthrough (the decompilation's FALLTHROUGH, from attributes.h)
        case 0:
        case 10:
        case 25:
            rwData->swing.atEyePoly = NULL;
            rwData->slopePitchAdj = 0;
            rwData->unk_28 = 0xA;
            rwData->swing.unk_16 = rwData->swing.unk_14 = rwData->swing.unk_18 = 0;
            rwData->swing.swingUpdateRate = roData->unk_0C;
            rwData->yOffset = camera->playerPosRot.pos.y;
            rwData->unk_20 = camera->xzSpeed;
            rwData->swing.swingUpdateRateTimer = 0;
            rwData->swingYawTarget = atEyeGeo.yaw;
            sUpdateCameraDirection = 0;
            rwData->startSwingTimer = CAM_GLOBAL_50 + CAM_GLOBAL_51;
            break;
        default:
            break;
    }

    camera->animState = 1;
    sUpdateCameraDirection = 1;

    if (rwData->unk_28 != 0) {
        rwData->unk_28--;
    }

    if (camera->xzSpeed > 0.001f) {
        rwData->startSwingTimer = CAM_GLOBAL_50 + CAM_GLOBAL_51;
    } else if (rwData->startSwingTimer > 0) {
        if (rwData->startSwingTimer > CAM_GLOBAL_50) {
            rwData->swingYawTarget = atEyeGeo.yaw + ((s16)((s16)(camera->playerPosRot.rot.y - 0x7FFF) - atEyeGeo.yaw) /
                                                     rwData->startSwingTimer);
        }
        rwData->startSwingTimer--;
    }

    spA0 = camera->speedRatio * CAM_UPDATE_RATE_STEP_SCALE_XZ;
    sp9C = camera->speedRatio * CAM_UPDATE_RATE_STEP_SCALE_Y;
    sp98 = rwData->swing.unk_18 != 0 ? CAM_UPDATE_RATE_STEP_SCALE_XZ : spA0;

    sp94 = (camera->xzSpeed - rwData->unk_20) * (0.333333f);
    if (sp94 > 1.0f) {
        sp94 = 1.0f;
    }
    if (sp94 > -1.0f) {
        sp94 = -1.0f;
    }

    rwData->unk_20 = camera->xzSpeed;

    if (rwData->swing.swingUpdateRateTimer != 0) {
        camera->yawUpdateRateInv =
            Camera_LERPCeilF(rwData->swing.swingUpdateRate + (f32)(rwData->swing.swingUpdateRateTimer * 2),
                             camera->yawUpdateRateInv, sp98, rate);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF((f32)CAM_PITCH_UPDATE_RATE_INV + (f32)(rwData->swing.swingUpdateRateTimer * 2),
                             camera->pitchUpdateRateInv, sp9C, rate);
        rwData->swing.swingUpdateRateTimer--;
    } else {
        camera->yawUpdateRateInv =
            Camera_LERPCeilF(rwData->swing.swingUpdateRate - (rwData->swing.swingUpdateRate * CAM_GLOBAL_49 * sp94),
                             camera->yawUpdateRateInv, sp98, rate);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF(CAM_PITCH_UPDATE_RATE_INV, camera->pitchUpdateRateInv, sp9C, rate);
    }

    camera->pitchUpdateRateInv = Camera_LERPCeilF(CAM_PITCH_UPDATE_RATE_INV, camera->pitchUpdateRateInv, sp9C, rate);
    camera->xzOffsetUpdateRate = Camera_LERPCeilF(CAM_XZ_OFFSET_UPDATE_RATE, camera->xzOffsetUpdateRate, spA0, rate);
    camera->yOffsetUpdateRate = Camera_LERPCeilF(CAM_Y_OFFSET_UPDATE_RATE, camera->yOffsetUpdateRate, sp9C, rate);
    camera->fovUpdateRate =
        Camera_LERPCeilF(CAM_FOV_UPDATE_RATE, camera->yOffsetUpdateRate, camera->speedRatio * 0.05f, rate);

    if (roData->interfaceField & NORMAL1_FLAG_0) {
        t = Camera_GetPitchAdjFromFloorHeightDiffs(camera, atEyeGeo.yaw - 0x7FFF, false);
        sp9C = ((1.0f / roData->unk_10) * 0.5f) * (1.0f - camera->speedRatio);
        rwData->slopePitchAdj =
            Camera_LERPCeilS(t, rwData->slopePitchAdj, ((1.0f / roData->unk_10) * 0.5f) + sp9C, 0xF);
    } else {
        rwData->slopePitchAdj = 0;
        if (camera->playerGroundY == camera->playerPosRot.pos.y) {
            rwData->yOffset = camera->playerPosRot.pos.y;
        }
    }

    spA0 = ((rwData->swing.unk_18 != 0) && (roData->yOffset > -40.0f))
               ? (sp9C = Math_SinS(rwData->swing.unk_14), ((-40.0f * sp9C) + (roData->yOffset * (1.0f - sp9C))))
               : roData->yOffset;

    if (roData->interfaceField & NORMAL1_FLAG_7) {
        func_800458D4(camera, &atEyeNextGeo, spA0, &rwData->yOffset, roData->interfaceField & NORMAL1_FLAG_0);
    } else if (roData->interfaceField & NORMAL1_FLAG_5) {
        func_80045B08(camera, &atEyeNextGeo, spA0, rwData->slopePitchAdj);
    } else {
        Camera_CalcAtDefault(camera, &atEyeNextGeo, spA0, roData->interfaceField & NORMAL1_FLAG_0);
    }

    eyeAdjustment = OLib_Vec3fDiffToVecGeo(at, eyeNext);

    camera->dist = eyeAdjustment.r =
        Camera_ClampDist(camera, eyeAdjustment.r, roData->distMin, roData->distMax, rwData->unk_28);

    if (rwData->startSwingTimer <= 0) {
        eyeAdjustment.pitch = atEyeNextGeo.pitch;
        eyeAdjustment.yaw =
            Camera_LERPCeilS(rwData->swingYawTarget, atEyeNextGeo.yaw, 1.0f / camera->yawUpdateRateInv, 0xA);
    } else if (rwData->swing.unk_18 != 0) {
        eyeAdjustment.yaw =
            Camera_LERPCeilS(rwData->swing.unk_16, atEyeNextGeo.yaw, 1.0f / camera->yawUpdateRateInv, 0xA);
        eyeAdjustment.pitch =
            Camera_LERPCeilS(rwData->swing.unk_14, atEyeNextGeo.pitch, 1.0f / camera->yawUpdateRateInv, 0xA);
    } else {
        // rotate yaw to follow player.
        eyeAdjustment.yaw =
            Camera_CalcDefaultYaw(camera, atEyeNextGeo.yaw, camera->playerPosRot.rot.y, roData->unk_14, sp94);
        eyeAdjustment.pitch =
            Camera_CalcDefaultPitch(camera, atEyeNextGeo.pitch, roData->pitchTarget, rwData->slopePitchAdj);
    }

    // @recomp THE FREE CAMERA: the right stick's yaw and pitch in place of the game's step,
    // while it holds them. The pitch clamp below still applies to the eye the stick asked for.
    // THE GAME'S OWN WALL SWING DOES NOT, and the return value is what says so: see the block
    // after *eyeNext is set.
    freeHolds = free_camera_take(camera, &atEyeNextGeo, &eyeAdjustment);

    // set eyeAdjustment pitch from 79.65 degrees to -85 degrees
    if (eyeAdjustment.pitch > 0x38A4) {
        eyeAdjustment.pitch = 0x38A4;
    }
    if (eyeAdjustment.pitch < -0x3C8C) {
        eyeAdjustment.pitch = -0x3C8C;
    }

    *eyeNext = Camera_AddVecGeoToVec3f(at, &eyeAdjustment);
    if ((camera->status == CAM_STAT_ACTIVE) && !(roData->interfaceField & NORMAL1_FLAG_4)) {
        rwData->swingYawTarget = camera->playerPosRot.rot.y - 0x7FFF;
        // @recomp WHILE THE STICK HOLDS THE VIEW, THE GAME'S OWN SWING DOES NOT RUN (the user,
        // 2026-09-30, of the pull-closer: "this isnt working all the time ... sometimes it moves
        // upward to sit above a wall rather than staying level and moving closer").
        //
        // That was the whole of it, and the reason it was intermittent rather than broken is one
        // line further up in this function: `if (camera->xzSpeed > 0.001f) startSwingTimer =
        // CAM_GLOBAL_50 + CAM_GLOBAL_51`. The timer is reset to full on EVERY FRAME LINK IS
        // MOVING, so func_80046E20 below owned the eye whenever he walked, and only once he
        // stood still long enough for the timer to count down did our own solve get to keep it.
        //
        // And what it does with the eye is exactly what he does not want: its case 3 plants the
        // eye on the collision point pushed out along the wall's normal, then, when the hit is
        // close, adds a radius at `Math_SinS(geoNorm.pitch + 0x3FFF)`, which for a vertical wall
        // is straight UP. That is the eye climbing to sit on top of the wall. Its case 1 is no
        // better for us: it swings the YAW around the corner, throwing away the aim the stick
        // just set.
        //
        // So the two solves are alternatives, not layers, and ours already did the work: it has
        // shortened the radius until the eye stands clear in FRONT of whatever is in the way, on
        // the line the stick asked for. The eye is taken as solved. The game's swing state is
        // still left tidy (the rate and unk_18) so the frame it takes the camera back is not a
        // jolt.
        if (freeHolds) {
            rwData->swing.swingUpdateRate = camera->yawUpdateRateInv = roData->unk_0C * 2.0f;
            rwData->swing.unk_18 = 0;
            *eye = *eyeNext;
        } else if (rwData->startSwingTimer > 0) {
            func_80046E20(camera, &eyeAdjustment, roData->distMin, roData->unk_0C, &sp98, &rwData->swing);
        } else {
            sp88 = *eyeNext;
            rwData->swing.swingUpdateRate = camera->yawUpdateRateInv = roData->unk_0C * 2.0f;
            if (Camera_BGCheck(camera, at, &sp88)) {
                rwData->swingYawTarget = atEyeNextGeo.yaw;
                rwData->startSwingTimer = -1;
            } else {
                *eye = *eyeNext;
            }
            rwData->swing.unk_18 = 0;
        }

        if (rwData->swing.unk_18 != 0) {
            camera->inputDir.y =
                Camera_LERPCeilS(camera->inputDir.y + (s16)((s16)(rwData->swing.unk_16 - 0x7FFF) - camera->inputDir.y),
                                 camera->inputDir.y, 1.0f - (0.99f * sp98), 0xA);
        }

        if (roData->interfaceField & NORMAL1_FLAG_2) {
            camera->inputDir.x = -atEyeGeo.pitch;
            camera->inputDir.y = atEyeGeo.yaw - 0x7FFF;
            camera->inputDir.z = 0;
        } else {
            eyeAdjustment = OLib_Vec3fDiffToVecGeo(eye, at);
            camera->inputDir.x = eyeAdjustment.pitch;
            camera->inputDir.y = eyeAdjustment.yaw;
            camera->inputDir.z = 0;
        }

        // crit wiggle
        if (gSaveContext.save.info.playerData.health <= 16 && ((camera->play->state.frames % 256) == 0)) {
            wiggleAdj = Rand_ZeroOne() * 10000.0f;
            camera->inputDir.y = wiggleAdj + camera->inputDir.y;
        }
    } else {
        rwData->swing.swingUpdateRate = roData->unk_0C;
        rwData->swing.unk_18 = 0;
        sUpdateCameraDirection = 0;
        *eye = *eyeNext;
    }

    spA0 = (gSaveContext.save.info.playerData.health <= 16 ? 0.8f : 1.0f);
    camera->fov = Camera_LERPCeilF(roData->fovTarget * spA0, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->atLERPScaleMax);
    return 1;
}
