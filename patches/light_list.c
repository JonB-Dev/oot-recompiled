// Publish the game's light list to the program, for the ray traced local lights (upgrades phase 55).
//
// THIS GAME NEVER HANDS THE MICROCODE A POINT LIGHT. Lights_BindPoint (z_lights.c) turns each
// point light of the scene into a directional light per actor, its color scaled by the
// distance and its direction from the actor toward the light, and skips point lights entirely
// for the room geometry (Lights_BindAll with no reference position). So the renderer's light
// manager, which reads the RSP light state, sees only directional lights and can never recover
// where a torch stands. The truth is the light list itself: play->lightCtx.listHead, a list of
// LightNode each pointing at an actor's LightInfo (type, position, color, radius, glow).
//
// The program reads that list out of the game's memory once a frame (src/game/light_list.cpp),
// walking it with every pointer checked. All it needs from this side is WHERE the LightContext
// is, which is what these two patches tell it: the context's address when a scene's list is
// made, and zero when it is torn down. Both functions are two lines in the decompilation and
// are reproduced here from z_lights.c at the pinned commit with the one `@recomp` line each.

#include "patches.h"

#include "light.h"
#include "play_state.h"
#include "scene.h"

// The program's side (src/game/recomp_api.cpp). Both addresses are the game's own, as plain
// numbers: the LightContext, and the environment's live light settings (play->envCtx.lightSettings:
// the ambient and the two directional lights the game blends for the hour), which are the scene's
// sun and ambient. The renderer used to search the RSP light state for the strongest directional
// light instead, and Navi's per actor stand-in could win that search and swing the whole scene's
// sun as she flew (the user, 2026-09-24).
// The third is play->envCtx.lightMode, which says whether the place follows the time of day
// (outdoors, a sun that moves with the hour) or holds a fixed setting (indoors). The renderer used
// to work that out for itself by sniffing the draw list for a sky, and that guess is wrong in both
// directions: it called the Deku Tree an outdoor scene, so the room's own fill light was traced as
// a sun to the horizon and every wall threw a hard edged shadow the length of the room across the
// floor (the user, 2026-09-24: "the other seems polygonal"); and it flipped with the camera in
// Kakariko, putting every sun shadow in the picture out at once ("windowmill shadow cuts out
// completely at certain camera angles").
// The fourth is what this place's own surfaces give as light (2026-10-08): bit 0 says its lattice
// windows are daylight, for a scene whose windows are glass painted on the wall where no traced
// sun can come through (renderer patch 0030). The Temple of Time is the one so far.
void recomp_light_context(u32 address, u32 environment, u32 lightMode, u32 sceneLights);

#define SCENE_LIGHTS_WINDOWS (1 << 0)

static u32 LightList_SceneLights(PlayState* play) {
    return (play->sceneId == SCENE_TEMPLE_OF_TIME) ? SCENE_LIGHTS_WINDOWS : 0;
}

// @recomp Publishes the context when a scene's light list is made.
RECOMP_PATCH void LightContext_InitList(PlayState* play, LightContext* lightCtx) {
    lightCtx->listHead = NULL;
    recomp_light_context((u32)lightCtx, (u32)&play->envCtx.lightSettings, (u32)&play->envCtx.lightMode,
                         LightList_SceneLights(play));
}

// @recomp Withdraws it when the list is torn down, so nothing reads a scene that is gone.
RECOMP_PATCH void LightContext_DestroyList(PlayState* play, LightContext* lightCtx) {
    while (lightCtx->listHead != NULL) {
        LightContext_RemoveLight(play, lightCtx, lightCtx->listHead);
        lightCtx->listHead = lightCtx->listHead->next;
    }
    recomp_light_context(0, 0, 0, 0);
}
