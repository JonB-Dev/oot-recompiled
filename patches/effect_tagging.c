// Tag the effects the game draws from its two tables: the soft sprite particles (dust, sparkles,
// bubbles, a hundred slots) and the three vertex effects (sparks, sword trails, shield sparks).
//
// PARTICLES are identified by their slot in the table, which is the only identity they have, and
// a slot is reused the moment a particle dies. Keyed by slot alone, a new particle would be drawn
// sweeping from wherever the old one ended, so `EffectSs_Reset` (which every death and every
// clear goes through) marks the slot, and the slot's next draw skips every component and clears
// the mark. That is the reference project's rule, and it is why this file patches a reset
// function in a phase about drawing.
//
// The tag wraps the particle's own draw callback, on both lists, because a particle draws to
// whichever list its type uses and the group has to be on the one it wrote to; the other gets an
// empty group, which costs the renderer nothing.
//
// THE VERTEX EFFECTS are tagged around their draw call in `Effect_DrawAll`, so none of the three
// draw functions themselves has to be reproduced (the plan sized this phase for reproducing the
// largest of them; wrapping the call is what the reference does and it needs nothing inside).
// Sparks and shield particles interpolate. A sword trail does NOT: its vertices are rebuilt each
// frame from the sword's recent positions and their count changes as it grows, which the
// renderer's vertex interpolation cannot pair, and the reference skips it for that reason; the
// plan's phrase "the vertex-interpolating mode the reference uses for trails" is recorded in
// issues.md as not what the reference does.
//
// Every function is reproduced from the decompilation (z_effect_soft_sprite.c and z_effect.c at
// the pinned commit) with the tagging lines marked `@recomp`.

#include "patches.h"
#include "tagging_helpers.h"

#include "array_count.h"
#include "effect.h"
#include "gfx.h"
#include "play_state.h"

#include "rt64_extended_gbi.h"

extern EffectSsInfo sEffectSsInfo;
extern EffectContext sEffectContext;
extern EffectInfo sEffectInfoTable[];

// One mark per particle slot. The game sizes its table per scene (a hundred entries in this
// revision) and the range in transform_ids.h holds 256, so a larger table would still be tagged
// for its first 256 slots and untagged past them rather than written out of bounds.
static u8 sParticleReset[PARTICLE_TRANSFORM_ID_COUNT];

// @recomp Patched to mark the slot so its next draw is not interpolated from the last occupant.
RECOMP_PATCH void EffectSs_Reset(EffectSs* effectSs) {
    u32 i;
    u32 index;

    effectSs->type = EFFECT_SS_TYPE_MAX;
    effectSs->accel.x = effectSs->accel.y = effectSs->accel.z = 0;
    effectSs->velocity.x = effectSs->velocity.y = effectSs->velocity.z = 0;
    effectSs->vec.x = effectSs->vec.y = effectSs->vec.z = 0;
    effectSs->pos.x = effectSs->pos.y = effectSs->pos.z = 0;
    effectSs->life = -1;
    effectSs->flags = 0;
    effectSs->priority = 128;
    effectSs->draw = NULL;
    effectSs->update = NULL;
    effectSs->gfx = NULL;
    effectSs->actor = NULL;

    for (i = 0; i < ARRAY_COUNT(effectSs->regs); i++) {
        effectSs->regs[i] = 0;
    }

    // @recomp The slot this entry occupies, when it is in the table at all (a reset can also be
    // handed a stack copy being prepared for insertion, which has no slot yet).
    if (sEffectSsInfo.table != NULL && effectSs >= sEffectSsInfo.table) {
        index = (u32)(effectSs - sEffectSsInfo.table);
        if (index < PARTICLE_TRANSFORM_ID_COUNT) {
            sParticleReset[index] = 1;
        }
    }
}

// @recomp Patched to tag the particle's draw with its slot, skipped on the first draw after a reset.
RECOMP_PATCH void EffectSs_Draw(PlayState* play, s32 index) {
    EffectSs* effectSs = &sEffectSsInfo.table[index];
    u32 id;
    s32 tagged;

    // @recomp
    tagged = (index >= 0) && ((u32)index < PARTICLE_TRANSFORM_ID_COUNT);
    if (tagged) {
        id = PARTICLE_TRANSFORM_ID_START + (u32)index;
        OPEN_DISPS(play->state.gfxCtx, "../z_effect_soft_sprite.c", 0);
        if (sParticleReset[index]) {
            gEXMatrixGroupDecomposedSkipAll(POLY_OPA_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
            gEXMatrixGroupDecomposedSkipAll(POLY_XLU_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
            sParticleReset[index] = 0;
        } else {
            gEXMatrixGroupDecomposedNormal(POLY_OPA_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
            gEXMatrixGroupDecomposedNormal(POLY_XLU_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
        }
        CLOSE_DISPS(play->state.gfxCtx, "../z_effect_soft_sprite.c", 0);
    }

    if (effectSs->draw != NULL) {
        effectSs->draw(play, index, effectSs);
    }

    // @recomp
    if (tagged) {
        OPEN_DISPS(play->state.gfxCtx, "../z_effect_soft_sprite.c", 0);
        gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup(POLY_XLU_DISP++, G_MTX_MODELVIEW);
        CLOSE_DISPS(play->state.gfxCtx, "../z_effect_soft_sprite.c", 0);
    }
}

// A vertex effect's group on both lists: interpolated for sparks and shield sparks, every
// component held for a trail. The translucent list's copy of the id sits in the upper half of
// the effect range so the two never collide.
static void tag_effect(GraphicsContext* gfxCtx, u32 id, s32 interpolate) {
    OPEN_DISPS(gfxCtx, "../z_effect.c", 0);
    if (interpolate) {
        gEXMatrixGroupDecomposedNormal(POLY_OPA_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_ALLOW);
        gEXMatrixGroupDecomposedNormal(POLY_XLU_DISP++, id + EFFECT_TRANSFORM_XLU_OFFSET, G_EX_PUSH, G_MTX_MODELVIEW,
                                       G_EX_EDIT_ALLOW);
    } else {
        gEXMatrixGroupDecomposedSkipAll(POLY_OPA_DISP++, id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gEXMatrixGroupDecomposedSkipAll(POLY_XLU_DISP++, id + EFFECT_TRANSFORM_XLU_OFFSET, G_EX_PUSH, G_MTX_MODELVIEW,
                                        G_EX_EDIT_NONE);
    }
    CLOSE_DISPS(gfxCtx, "../z_effect.c", 0);
}

static void untag_effect(GraphicsContext* gfxCtx) {
    OPEN_DISPS(gfxCtx, "../z_effect.c", 0);
    gEXPopMatrixGroup(POLY_OPA_DISP++, G_MTX_MODELVIEW);
    gEXPopMatrixGroup(POLY_XLU_DISP++, G_MTX_MODELVIEW);
    CLOSE_DISPS(gfxCtx, "../z_effect.c", 0);
}

// @recomp Patched to tag each vertex effect's draw with its slot in its table.
RECOMP_PATCH void Effect_DrawAll(GraphicsContext* gfxCtx) {
    s32 i;

    for (i = 0; i < SPARK_COUNT; i++) {
        if (!sEffectContext.sparks[i].status.active) {
            continue;
        }
        tag_effect(gfxCtx, EFFECT_TRANSFORM_ID_START + EFFECT_TRANSFORM_SPARK_OFFSET + (u32)i, 1);
        sEffectInfoTable[EFFECT_SPARK].draw(&sEffectContext.sparks[i].effect, gfxCtx);
        untag_effect(gfxCtx);
    }

    for (i = 0; i < BLURE_COUNT; i++) {
        if (!sEffectContext.blures[i].status.active) {
            continue;
        }
        tag_effect(gfxCtx, EFFECT_TRANSFORM_ID_START + EFFECT_TRANSFORM_BLURE_OFFSET + (u32)i, 0);
        sEffectInfoTable[EFFECT_BLURE1].draw(&sEffectContext.blures[i].effect, gfxCtx);
        untag_effect(gfxCtx);
    }

    for (i = 0; i < SHIELD_PARTICLE_COUNT; i++) {
        if (!sEffectContext.shieldParticles[i].status.active) {
            continue;
        }
        tag_effect(gfxCtx, EFFECT_TRANSFORM_ID_START + EFFECT_TRANSFORM_SHIELD_OFFSET + (u32)i, 1);
        sEffectInfoTable[EFFECT_SHIELD_PARTICLE].draw(&sEffectContext.shieldParticles[i].effect, gfxCtx);
        untag_effect(gfxCtx);
    }
}
