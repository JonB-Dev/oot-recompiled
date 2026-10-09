// Tell the native registry when an actor is born, dies, or the whole scene is torn down, so the
// tagging patches can give each actor an identity the renderer can interpolate by.
//
// WHY A REGISTRY. RT64 interpolates a transform group by ID between two consecutive frames. An
// actor's address is the obvious ID and the wrong one: the game frees and reallocates actors at
// the same address constantly, and a new actor would inherit a dead one's last transform and fly
// in from wherever that was. A spawn index that never repeats is the identity that works, and the
// native side keeps it (src/game/actor_registry.cpp) because there is no spare field in `Actor`
// in this revision and widening the struct would change every offset the recompiler and the
// decompilation agree on.
//
// THE THREE FUNCTIONS are reproduced line for line from the decompilation's z_actor.c with one
// call each. `func_80031C3C` is what the decompilation annotates as Actor_CleanupContext in this
// revision: the name is not yet matched, the body is.
//
// A patch REPLACES its function, so anything dropped here would simply stop happening.

#include "patches.h"

#include "libc64/math64.h"
#include "array_count.h"
#include "attributes.h"
#include "printf.h"
#include "sfx.h"
#include "translation.h"
#include "z_actor_dlftbls.h"
#include "z_lib.h"
#include "zelda_arena.h"
#include "actor.h"
#include "audio.h"
#include "camera.h"
#include "play_state.h"
#include "player.h"
#include "save.h"

// Native, through the dummy addresses in syms.ld. The address goes over as a plain integer and
// comes back as one; nothing native is ever handed a pointer it could follow.
DECLARE_FUNC(u32, recomp_actor_register, u32 actor_address);
DECLARE_FUNC(void, recomp_actor_release, u32 actor_address);
DECLARE_FUNC(void, recomp_actor_reset, void);

// File local in z_actor.c in the decompilation. In this revision it prints nothing.
#define ACTOR_DEBUG_PRINTF \
    if (0)                 \
    PRINTF

// Static in the decompilation's z_actor.c, so no header declares them, but they are real
// functions with their own symbols in OoTRecompSyms, and the recompiler resolves a call by name.
// Declared here exactly as z_actor.c defines them.
void Actor_SetWorldToHome(Actor* actor);
void Actor_SetShapeRotToWorld(Actor* actor);
void Actor_Destroy(Actor* actor, PlayState* play);
Actor* Actor_RemoveFromCategory(PlayState* play, ActorContext* actorCtx, Actor* actorToRemove);
void Actor_FreeOverlay(ActorOverlay* entry);
void func_80030488(PlayState* play);

// A ring spawner's reach by the Render distance row (culling.c).
void culling_mure2_reach(Actor* thisx, PlayState* play);

// @recomp Patched to register the actor with the native registry once it is initialized.
RECOMP_PATCH void Actor_Init(Actor* actor, PlayState* play) {
    Actor_SetWorldToHome(actor);
    Actor_SetShapeRotToWorld(actor);
    Actor_SetFocus(actor, 0.0f);
    Math_Vec3f_Copy(&actor->prevPos, &actor->world.pos);
    Actor_SetScale(actor, 0.01f);
    actor->attentionRangeType = ATTENTION_RANGE_3;
    actor->minVelocityY = -20.0f;
    actor->xyzDistToPlayerSq = MAXFLOAT;
    actor->naviEnemyId = NAVI_ENEMY_NONE;
    actor->cullingVolumeDistance = 1000.0f;
    actor->cullingVolumeScale = 350.0f;
    actor->cullingVolumeDownward = 700.0f;
    CollisionCheck_InitInfo(&actor->colChkInfo);
    actor->floorBgId = BGCHECK_SCENE;
    ActorShape_Init(&actor->shape, 0.0f, NULL, 0.0f);

    // @recomp The one addition. Before the actor's own init, so an actor that draws from inside
    // its init (none is known to) already has an identity.
    recomp_actor_register((u32)actor);

    if (Object_IsLoaded(&play->objectCtx, actor->objectSlot)) {
        Actor_SetObjectDependency(play, actor);
        actor->init(actor, play);
        actor->init = NULL;
        // @recomp The second addition: a ring of shrubs or rocks reaches as far as the Render
        // distance row says from the moment it exists (culling.c, culling_mure2_reach). Its
        // object is the one always loaded, so its init is always this one.
        if (actor->id == ACTOR_OBJ_MURE2) {
            culling_mure2_reach(actor, play);
        }
    }
}

// @recomp Patched to release the actor's identity as it is deleted.
RECOMP_PATCH Actor* Actor_Delete(ActorContext* actorCtx, Actor* actor, PlayState* play) {
    PlayState* play2 = (PlayState*)play;
    Player* player;
    Actor* newHead;
    ActorOverlay* overlayEntry;
    UNUSED_NDEBUG char* name;

    player = GET_PLAYER(play);

    overlayEntry = actor->overlayEntry;

    name = "";

    ACTOR_DEBUG_PRINTF(T("アクタークラス削除 [%s]\n", "Actor class deleted [%s]\n"), name);

    if ((player != NULL) && (player->focusActor == actor)) {
        Player_ReleaseLockOn(player);
        Camera_RequestMode(Play_GetCamera(play2, Play_GetActiveCamId(play2)), CAM_MODE_NORMAL);
    }

    if (actorCtx->attention.naviHoverActor == actor) {
        actorCtx->attention.naviHoverActor = NULL;
    }

    if (actorCtx->attention.forcedLockOnActor == actor) {
        actorCtx->attention.forcedLockOnActor = NULL;
    }

    if (actorCtx->attention.bgmEnemy == actor) {
        actorCtx->attention.bgmEnemy = NULL;
    }

    Audio_StopSfxByPos(&actor->projectedPos);
    Actor_Destroy(actor, play2);

    newHead = Actor_RemoveFromCategory(play2, actorCtx, actor);

    // @recomp The one addition, before the memory goes back to the arena.
    recomp_actor_release((u32)actor);

    ZELDA_ARENA_FREE(actor, "../z_actor.c", 7242);

    if (overlayEntry->vramStart == NULL) {
        ACTOR_DEBUG_PRINTF(T("オーバーレイではありません\n", "Not an overlay\n"));
    } else {
        ASSERT(overlayEntry->loadedRamAddr != NULL, "actor_dlftbl->allocp != NULL", "../z_actor.c", 7251);
        ASSERT(overlayEntry->numLoaded > 0, "actor_dlftbl->clients > 0", "../z_actor.c", 7252);
        overlayEntry->numLoaded--;
        Actor_FreeOverlay(overlayEntry);
    }

    return newHead;
}

// @recomp Patched to reset the registry after every actor in the scene has been deleted.
// Actor_CleanupContext in the decompilation's own words; unmatched name in this revision.
RECOMP_PATCH void func_80031C3C(ActorContext* actorCtx, PlayState* play) {
    Actor* actor;
    s32 i;

    for (i = 0; i < ARRAY_COUNT(actorCtx->actorLists); i++) {
        actor = actorCtx->actorLists[i].head;
        while (actor != NULL) {
            Actor_Delete(actorCtx, actor, play);
            actor = actorCtx->actorLists[i].head;
        }
    }

    ACTOR_DEBUG_PRINTF(T("絶対魔法領域解放\n", "Absolute magic field deallocation\n"));

    if (actorCtx->absoluteSpace != NULL) {
        ZELDA_ARENA_FREE(actorCtx->absoluteSpace, "../z_actor.c", 6731);
        actorCtx->absoluteSpace = NULL;
    }

    // @recomp The one addition. Every actor has been deleted through Actor_Delete above, so the
    // registry should already be empty; the reset prints its counters, which is how a leak shows.
    recomp_actor_reset();

    Play_SaveSceneFlags(play);
    func_80030488(play);
    ActorOverlayTable_Cleanup();
}
