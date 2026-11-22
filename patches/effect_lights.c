// THE SUN'S SONG AND THE BLUE WARP LIGHT WHAT THEY REACH, NOT A WHOLE FIGURE (2026-10-08).
//
// Both effects work the same way on the console. The scene is dimmed (the Sun's Song through
// Environment_AdjustLights, the warp through the environment's adjust colors), and two point
// lights are placed at the player: the Sun's Song's above his head and just in front of him, the
// warp's above and below. A point light in this game never reaches the room; Lights_BindPoint
// turns it into ONE directional light per actor, so the player lights up whole from one side,
// the parts the light could never reach included (the user, 2026-10-08: "This same exact kind of
// thing happens when playing ... songs and teleporting ... or using the sun song").
//
// The traced local lights already read those same two lights out of the game's light list and
// light everything near them, the floor and the player alike, with their shadows. So while they
// run, the actors' own binding leaves these lights out and the traced light is the only one they
// give: the dim stays the game's, the light is real. Every other light keeps the game's binding,
// and the binding for the room geometry and the effects is untouched (they are not bound here).
//
// Recognized by WHERE the light lives rather than by its numbers: each effect keeps its two
// LightInfo inside its own instance, so a light is the effect's when its address is one of those
// fields of a live effect actor. The song effects and the warp are both item action actors, a
// list that holds a handful at most.
#include "patches.h"
#include "effect_lights.h"

#include "actor.h"
#include "light.h"
#include "play_state.h"
#include "overlays/actors/ovl_Door_Warp1/z_door_warp1.h"
#include "overlays/actors/ovl_Oceff_Spot/z_oceff_spot.h"

s32 recomp_traced_light_effects(void);

void Lights_BindPoint(Lights* lights, LightParams* params, Vec3f* vec);
void Lights_BindDirectional(Lights* lights, LightParams* params, Vec3f* vec);

static s32 EffectLights_IsTraced(PlayState* play, LightInfo* info) {
    Actor* actor;

    for (actor = play->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; actor != NULL; actor = actor->next) {
        if (actor->id == ACTOR_OCEFF_SPOT) {
            OceffSpot* spot = (OceffSpot*)actor;

            if ((info == &spot->lightInfo1) || (info == &spot->lightInfo2)) {
                return true;
            }
        } else if (actor->id == ACTOR_DOOR_WARP1) {
            DoorWarp1* warp = (DoorWarp1*)actor;

            if ((info == &warp->upperLightInfo) || (info == &warp->lowerLightInfo)) {
                return true;
            }
        }
    }
    return false;
}

void EffectLights_BindAll(PlayState* play, Lights* lights, LightNode* listHead, Vec3f* vec) {
    const s32 traced = recomp_traced_light_effects();
    LightInfo* info;

    while (listHead != NULL) {
        info = listHead->info;
        if (!traced || (info->type == LIGHT_DIRECTIONAL) || !EffectLights_IsTraced(play, info)) {
            if (info->type == LIGHT_DIRECTIONAL) {
                Lights_BindDirectional(lights, &info->params, vec);
            } else {
                Lights_BindPoint(lights, &info->params, vec);
            }
        }
        listHead = listHead->next;
    }
}
