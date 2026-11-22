// THE LOW HEALTH BEEP CAN BE TURNED OFF (2026-10-08).
//
// The user: "add the ability as a settings to turn off the low health beeps". The reference
// project does the same for its game by replacing the function that times the beating heart and
// asking a setting before the alarm; this is that technique on this game's own function, which
// sounds NA_SE_SY_HITPOINT_ALARM each time the heart's beat bottoms out while health is critical.
// The heart still beats and still turns red: only the sound is the row's.
//
// The function below is the game's own, line for line, with the one question added.
#include "patches.h"

#include "lifemeter.h"
#include "pause.h"
#include "play_state.h"
#include "player.h"
#include "sfx.h"
#include "z_lib.h"

// The program's side (src/game/recomp_api.cpp): 1 while the Low health beep row is On.
s32 recomp_low_health_beep(void);

// @recomp The game's beating heart, with the beep asked of the Low health beep row.
RECOMP_PATCH void Health_UpdateBeatingHeart(PlayState* play) {
    InterfaceContext* interfaceCtx = &play->interfaceCtx;

    if (interfaceCtx->beatingHeartOscillatorDirection != 0) {
        interfaceCtx->beatingHeartOscillator--;
        if (interfaceCtx->beatingHeartOscillator <= 0) {
            interfaceCtx->beatingHeartOscillator = 0;
            interfaceCtx->beatingHeartOscillatorDirection = 0;
            // @recomp The row is asked last, so the game's own conditions read as they did.
            if (!Player_InCsMode(play) && !IS_PAUSED(&play->pauseCtx) && Health_IsCritical() && !Play_InCsMode(play) &&
                recomp_low_health_beep()) {
                Sfx_PlaySfxCentered(NA_SE_SY_HITPOINT_ALARM);
            }
        }
    } else {
        interfaceCtx->beatingHeartOscillator++;
        if (interfaceCtx->beatingHeartOscillator >= 10) {
            interfaceCtx->beatingHeartOscillator = 10;
            interfaceCtx->beatingHeartOscillatorDirection = 1;
        }
    }
}
