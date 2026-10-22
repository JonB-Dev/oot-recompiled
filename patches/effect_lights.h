#ifndef EFFECT_LIGHTS_H
#define EFFECT_LIGHTS_H

#include "light.h"

struct PlayState;

// Lights_BindAll for an actor, leaving out the lights of the Sun's Song and the blue warp while
// the traced local lights give them instead (patches/effect_lights.c).
void EffectLights_BindAll(struct PlayState* play, Lights* lights, LightNode* listHead, Vec3f* vec);

#endif
