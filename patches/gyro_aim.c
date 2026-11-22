// GYRO AIMING (a player's suggestion, 2026-11-22: "gyroscope-based aiming with compatible
// controllers").
//
// func_8084ABD8 is the player's first person look and aim. Its first half is the free look with
// nothing held (C up), where the stick sets where the view points; its second is the aim of the
// bow, the slingshot, the hookshot and the boomerang, where the stick turns the aim at a speed. The
// pad's turn is added to the aim in the second half, before the game's own clamps, so the aim
// follows the pad as it turns and stops where the game stops it; in the first half too when the
// pad's Gyro look row is on (the user, 2026-11-22). The program sums the turn (src/main/gyro.cpp)
// and gives nothing while a row is off, so with them off this is the game's own function. The
// reference project adds its gyro to the same function of its own game.
//
// The function below is the game's own, line for line, with the gyro's additions marked @recomp.
#include "patches.h"

#include "player.h"
#include "play_state.h"
#include "z_lib.h"
#include "z_math.h"

// The program's side (src/game/recomp_api.cpp): the pad's turn since the last call, in the game's
// angle units, heading in the low half (plus turns the aim left) and tilt in the high half (plus
// tilts it down). `look` is 1 while looking around with C up, which turns only when the pad's Gyro
// look row is on (the user, 2026-11-22), and 0 while aiming an item.
DECLARE_FUNC(s32, recomp_gyro_aim, s32 look);

// GYRO LOOK'S TILT. C up's tilt eases toward where the stick points it each frame, so a turn added
// to the tilt itself would be eased straight back; the pad's tilt is summed here instead and added
// to where the tilt eases to, from the frame the look began (gameplayFrames running on unbroken).
static s32 sGyroLookTilt = 0;
static u32 sGyroLookFrame = 0;
#define GYRO_LOOK_TILT_MAX 14000

// The player's own, from z_player.c. `sControlInput` is the overlay's static, shared through
// game_static_syms.toml so this reads exactly the input the rest of the player's code reads.
extern Input* sControlInput;
int func_8002DD78(Player* player);
int func_808334B4(Player* this);
s32 func_80836AB8(Player* this, s32 arg1);

RECOMP_PATCH s16 func_8084ABD8(PlayState* play, Player* this, s32 arg2, s16 arg3) {
    s32 temp1;
    s16 temp2;
    s16 temp3;

    if (!func_8002DD78(this) && !func_808334B4(this) && !arg2) {
        // @recomp Gyro look: the pad's turn while looking around with C up, nothing unless its row
        // is on, and nothing while the player is locked.
        s32 look = recomp_gyro_aim(1);
        s16 look_yaw = (s16)(look & 0xFFFF);
        s16 look_pitch = (s16)(look >> 16);
        s32 stick_tilt;
        s32 tilt_limit;

        if (this->stateFlags1 & (PLAYER_STATE1_5 | PLAYER_STATE1_29)) {
            look_yaw = 0;
            look_pitch = 0;
        }
        if (play->gameplayFrames != sGyroLookFrame + 1) {
            sGyroLookTilt = 0;
        }
        sGyroLookFrame = play->gameplayFrames;
        sGyroLookTilt = CLAMP(sGyroLookTilt + look_pitch, -GYRO_LOOK_TILT_MAX, GYRO_LOOK_TILT_MAX);

        // @recomp The stick's tilt plus the pad's, never past the aim's own limit unless the stick
        // alone goes further; with no pad tilt this is the stick's tilt exactly, as before.
        stick_tilt = (s32)(sControlInput->rel.stick_y * 240.0f);
        tilt_limit = MAX(ABS(stick_tilt), GYRO_LOOK_TILT_MAX);
        temp2 = CLAMP(stick_tilt + sGyroLookTilt, -tilt_limit, tilt_limit);
        Math_SmoothStepToS(&this->actor.focus.rot.x, temp2, 14, 4000, 30);

        temp2 = sControlInput->rel.stick_x * -16.0f;
        temp2 = CLAMP(temp2, -3000, 3000);
        // @recomp + look_yaw
        this->actor.focus.rot.y += temp2 + look_yaw;
    } else {
        // @recomp The pad's turn, taken every aiming frame so none of it builds up. Nothing while
        // the player is locked, when the game's own input is all zeros too.
        s32 gyro = recomp_gyro_aim(0);
        s16 gyro_yaw = (s16)(gyro & 0xFFFF);
        s16 gyro_pitch = (s16)(gyro >> 16);

        if (this->stateFlags1 & (PLAYER_STATE1_5 | PLAYER_STATE1_29)) {
            gyro_yaw = 0;
            gyro_pitch = 0;
        }

        temp1 = (this->stateFlags1 & PLAYER_STATE1_23) ? 3500 : 14000;
        temp3 = ((sControlInput->rel.stick_y >= 0) ? 1 : -1) *
                (s32)((1.0f - Math_CosS(sControlInput->rel.stick_y * 200)) * 1500.0f);
        // @recomp + gyro_pitch
        this->actor.focus.rot.x += temp3 + gyro_pitch;
        this->actor.focus.rot.x = CLAMP(this->actor.focus.rot.x, -temp1, temp1);

        temp1 = 19114;
        temp2 = this->actor.focus.rot.y - this->actor.shape.rot.y;
        temp3 = ((sControlInput->rel.stick_x >= 0) ? 1 : -1) *
                (s32)((1.0f - Math_CosS(sControlInput->rel.stick_x * 200)) * -1500.0f);
        // @recomp + gyro_yaw
        temp2 += temp3 + gyro_yaw;
        this->actor.focus.rot.y = CLAMP(temp2, -temp1, temp1) + this->actor.shape.rot.y;
    }

    this->unk_6AE_rotFlags |= UNK6AE_ROT_FOCUS_Y;
    return func_80836AB8(this, (play->shootingGalleryStatus != 0) || func_8002DD78(this) || func_808334B4(this)) - arg3;
}
