// THE LENS OF TRUTH'S LIST HAS A GUARD (2026-10-21, found in the sweep before 0.6.1; the user: "fix").
//
// Actor_DrawAll gathers every actor the lens reveals (an invisible chest, a hidden Skulltula, a
// false wall's gate) into a list of INVISIBLE_ACTOR_MAX, twenty, on its own stack, and hands the
// list to Actor_DrawLensActors. The console's limit check is an ASSERT, compiled out of the retail
// build, so a twenty-first entry writes past the list into the rest of the stack frame. The game
// fills it with every revealing actor drawn this frame, the lens on or not, and those in other
// loaded rooms as well.
//
// Our culling (patches/culling.c) draws more actors than the console did: out to the Render
// distance, and behind and beside the camera as far as Shadow casters reaches. That raises how
// many reach this list. No room counted so far gets past fourteen (Bottom of the Well, room 0),
// so this is insurance: the list here holds LENS_LIST_MAX, and an actor past it is not gathered,
// so it is simply not revealed that frame, never written over the stack.
//
// Reproduced from the decompilation's z_actor.c, the retail build's path only: every
// `!DEBUG_FEATURES || ...` test there is true in retail, and gDPNoOpString is empty without
// GBI_DEBUG, as it is in this build. Actor_Draw and Lights_DrawGlow are our patched versions
// (patches/skeleton_tagging.c, patches/light_glow.c), reached exactly as the original reaches them.

#include "patches.h"

#include "actor.h"
#include "array_count.h"
#include "effect.h"
#include "gfx.h"
#include "light.h"
#include "play_state.h"
#include "player.h"
#include "regs.h"
#include "skin_matrix.h"

// z_actor.c's own, in the exported symbols (Actor_Draw is our patched one, patches/skeleton_tagging.c).
void Actor_Draw(PlayState* play, Actor* actor);
void TitleCard_Draw(PlayState* play, TitleCardContext* titleCtx);
s32 Actor_CullingCheck(PlayState* play, Actor* actor);
void Actor_UpdateFlaggedAudio(Actor* actor);
void Actor_DrawLensActors(PlayState* play, s32 numInvisibleActors, Actor** invisibleActors);
void Actor_DrawFaroresWindPointer(PlayState* play);

// Room for three times the console's list; the stack has it to spare (the original's frame held
// twenty pointers, this one sixty).
#define LENS_LIST_MAX (INVISIBLE_ACTOR_MAX * 3)

// @recomp Patched to bound the lens of truth's list (above). Otherwise the game's own.
RECOMP_PATCH void Actor_DrawAll(PlayState* play, ActorContext* actorCtx) {
    s32 invisibleActorCounter;
    Actor* invisibleActors[LENS_LIST_MAX];
    ActorListEntry* actorListEntry;
    Actor* actor;
    s32 i;

    invisibleActorCounter = 0;

    OPEN_DISPS(play->state.gfxCtx, "../z_actor.c", 6336);

    actorListEntry = &actorCtx->actorLists[0];

    for (i = 0; i < ARRAY_COUNT(actorCtx->actorLists); i++, actorListEntry++) {
        actor = actorListEntry->head;

        while (actor != NULL) {
            SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &actor->world.pos, &actor->projectedPos,
                                         &actor->projectedW);

            if (actor->sfx != 0) {
                Actor_UpdateFlaggedAudio(actor);
            }

            if (Actor_CullingCheck(play, actor)) {
                actor->flags |= ACTOR_FLAG_INSIDE_CULLING_VOLUME;
            } else {
                actor->flags &= ~ACTOR_FLAG_INSIDE_CULLING_VOLUME;
            }

            actor->isDrawn = false;

            if ((actor->init == NULL) && (actor->draw != NULL) &&
                (actor->flags & (ACTOR_FLAG_DRAW_CULLING_DISABLED | ACTOR_FLAG_INSIDE_CULLING_VOLUME))) {
                if ((actor->flags & ACTOR_FLAG_REACT_TO_LENS) &&
                    ((play->roomCtx.curRoom.lensMode == LENS_MODE_SHOW_ACTORS) || play->actorCtx.lensActive ||
                     (actor->room != play->roomCtx.curRoom.num))) {
                    // @recomp The bound. A full list leaves the actor out of it this frame.
                    if (invisibleActorCounter < LENS_LIST_MAX) {
                        invisibleActors[invisibleActorCounter] = actor;
                        invisibleActorCounter++;
                    }
                } else {
                    Actor_Draw(play, actor);
                    actor->isDrawn = true;
                }
            }

            actor = actor->next;
        }
    }

    Effect_DrawAll(play->state.gfxCtx);

    EffectSs_DrawAll(play);

    if (play->actorCtx.lensActive) {
        Actor_DrawLensActors(play, invisibleActorCounter, invisibleActors);
        if ((play->csCtx.state != CS_STATE_IDLE) || Player_InCsMode(play)) {
            Actor_DisableLens(play);
        }
    }

    Actor_DrawFaroresWindPointer(play);

    if (IREG(32) == 0) {
        Lights_DrawGlow(play);
    }

    TitleCard_Draw(play, &actorCtx->titleCtx);

    CLOSE_DISPS(play->state.gfxCtx, "../z_actor.c", 6563);
}
