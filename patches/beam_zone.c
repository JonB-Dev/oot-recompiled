// THE BEAM IS A LIGHT, NOT A SWITCH (2026-10-08).
//
// The game fakes the beam of light over the pedestal in the Temple of Time with the floor: the
// floor under the beam carries its own light setting (word 1 of its surface type), and Player
// hands it to this function, so the WHOLE scene's lighting blends to a brighter setting while
// the player stands there. Every lit actor brightens at once, and a figure half inside the beam
// lights up whole (the user, 2026-10-08: "he's partially inside of it, but his whole body is lit
// up, even the parts that are outside of it").
//
// With the traced light effects on, the renderer turns the beam's own drawn shaft into a light
// that reaches only what stands inside it, from the direction the shaft falls (renderer patch
// 0029), so the switch is left out here. An inventory of every scene's floor found this is the
// one zone in the game that sits under a beam: the other small light zones (a fairy fountain's,
// Ganon's tower's) have no beam over them and keep their switch. The only other caller of this
// function is a Forest Temple object, never in this scene. Everything else about the function
// is the original.
//
// AND THE MASTER SWORD ROOM TAKES THE HALL'S LIGHT (2026-10-08). The same inventory shows the
// hall's floor selects setting 0 and the Master Sword room's floor setting 3, which the game
// lights well below the hall's, so with the traced windows lighting both, the room read as the
// darker of the two (the user: "this master sword room is still darker too"). Of every way tried
// in play (a test row of brighter windows, more ambient, and the game's own settings), the user
// chose this one: "the hall's light setting, which actually makes both rooms pretty consistent
// with each other". So while the traced light effects stand, setting 3 in this scene is the
// hall's setting 0.
#include "patches.h"

#include "environment.h"
#include "play_state.h"
#include "scene.h"

s32 recomp_traced_light_effects(void);

// The Temple of Time's floor settings: the hall's, the beam's under the pedestal, and the
// Master Sword room's.
#define HALL_SETTING 0
#define BEAM_ZONE_SETTING 2
#define PEDESTAL_ROOM_SETTING 3

RECOMP_PATCH void Environment_ChangeLightSetting(PlayState* play, u32 lightSetting) {
    if ((play->sceneId == SCENE_TEMPLE_OF_TIME) && recomp_traced_light_effects()) {
        if (lightSetting == BEAM_ZONE_SETTING) {
            return;
        }
        if (lightSetting == PEDESTAL_ROOM_SETTING) {
            lightSetting = HALL_SETTING;
        }
    }

    if ((play->envCtx.lightSetting != lightSetting) && (play->envCtx.lightBlend >= 1.0f) &&
        (play->envCtx.lightSettingOverride == LIGHT_SETTING_OVERRIDE_NONE)) {
        if (lightSetting >= LIGHT_SETTING_MAX) {
            lightSetting = 0;
        }

        play->envCtx.lightBlend = 0.0f;
        play->envCtx.prevLightSetting = play->envCtx.lightSetting;
        play->envCtx.lightSetting = lightSetting;
    }
}
