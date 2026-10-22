// THE ARROW THAT IS NOT THERE AND THE HOOKSHOT THAT NEVER COMES (the user, 2026-10-21: "I have
// arrows and ... it basically acts like I don't have arrows and just shoots an empty bow", and
// "when I open up and use my long shot my character just freezes ... I'm able to move the camera
// but I'm unable to move my character ... I have to close the whole game and reopen it").
//
// WHAT HAPPENED. Both are actors Link spawns for himself: the arrow each time he nocks one
// (z_player.c, func_8083442C) and the hook once, as he takes the hookshot out
// (Player_InitHookshotIA). A spawn needs memory from the game's actor heap (ZeldaArena), for the
// actor and, the first time, for its code. The log of the user's last session in Hyrule Field
// (2026-10-17) shows that heap all but gone: 145 to 164 actors, under the limit of 200 that
// patches/culling.c guards, but the largest free block down to between 96 bytes and 5 KB. The
// spawners (trees, rings of shrubs and rocks) reaching out to the Render distance and Shadow
// casters rows had used it up. So the arrow was refused and Link drew an empty bow, firing
// nothing and spending nothing; and the hook was refused, and Link's upper body waits for a hook
// that is never made (func_808353D8 returns at `Player_HoldsHookshot(this) && !func_80834FBC(this)`
// every frame), which is the freeze. Nothing ever spawns the hook again, so only a restart helped.
//
// THREE FIXES, built as candidates each with its own Debugging switch and all confirmed by the
// user with all three on ("both of the issues that I brought up have been addressed", 2026-10-21),
// so all three are the plain behavior now and the switches are gone:
//
//   The bigger actor heap (here): the heap is given the memory above the game's framebuffers.
//   The spawners leave memory (patches/culling.c): they reach out only while memory is left.
//   The hookshot guard (here): a hookshot in hand with no hook gets its hook.
//
// In the user's test the heap alone did it: 4091 KB, its largest free block never under 3.9 MB
// in Hyrule Field with the same settings, and the guard never had to act. The other two stay as
// the net under it, since a stranded player is the worst way this can fail.

#include "patches.h"
#include "actor_heap.h"

#include "actor.h"
#include "libc64/os_malloc.h"
#include "play_state.h"
#include "player.h"
#include "sys_cfb.h"

// The game's actor heap (z_malloc.c), in the exported data symbols.
extern Arena sZeldaArena;

static s32 sHooksPutBack = 0;
static s32 sHookMissing = 0;

// ---------------------------------------------------------------------------------------------
// THE BIGGER HEAP.
//
// Retail OoT uses the first 4 MB of RAM and nothing above, even with 8 MB present: SysCfb_Init
// ends the framebuffers at 0x80400000 unless the 64DD is attached (sys_cfb.c), and the system
// heap stops below them (main.c). The runtime gives the game 8 MB (librecomp, osMemSize), its
// own handles start at 0x80800000 and our patches at 0x80801000 (librecomp addresses.hpp), and
// nothing of ours writes between. So the heap is given everything from the end of the
// framebuffers, wherever the game put it, up to 0x80800000: about 4 MB, against a heap that
// had roughly 60 KB free in Hyrule Field. Still under 8 MB, so the program's own readers of
// actors (src/game/actor_registry.cpp, light_list.cpp, camera_cuts.cpp), which take nothing
// past 8 MB, see every actor as before. Saved moments keep the save and re-enter the scene,
// so where the heap lives is nothing to them.
//
// The block the game carved for the heap from its scene memory is left unused. If the upper
// memory were ever smaller than that block (it cannot be in retail), the game's own is used.
#define UPPER_RAM_END 0x80800000

// @recomp Patched to give the actor heap the memory the game leaves unused. The original is
// `__osMallocInit(&sZeldaArena, start, size)`, which is still the fallback.
RECOMP_PATCH void ZeldaArena_Init(void* start, u32 size) {
    uintptr_t from = ((uintptr_t)SysCfb_GetFbEnd() + 0xF) & ~(uintptr_t)0xF;

    if ((from != 0) && (from < UPPER_RAM_END) && ((UPPER_RAM_END - from) > size)) {
        __osMallocInit(&sZeldaArena, (void*)from, (s32)(UPPER_RAM_END - from));
        return;
    }
    __osMallocInit(&sZeldaArena, start, size);
}

// ---------------------------------------------------------------------------------------------
// THE HOOKSHOT GUARD.
//
// The game makes the hook once, as the hookshot comes out, and never again; if that spawn is
// refused, Link is stuck holding it. So once an update: a hookshot in hand, no hook on Link, and
// no hook anywhere in the scene (one flying or pulling Link is not missing, nor one killed and
// awaiting deletion), means the spawn was refused, and it is made now exactly as
// Player_InitHookshotIA makes it. While memory is still short the attempt fails and is made
// again the next update, so Link is free the moment there is room.
void ActorHeap_HookshotGuard(PlayState* play) {
    Player* player = GET_PLAYER(play);
    Actor* actor;

    if ((player == NULL) || !Player_HoldsHookshot(player) || (player->actor.child != NULL) ||
        (player->heldActor != NULL)) {
        sHookMissing = 0;
        return;
    }
    for (actor = play->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; actor != NULL; actor = actor->next) {
        if (actor->id == ACTOR_ARMS_HOOK) {
            sHookMissing = 0;
            return;
        }
    }

    player->heldActor =
        Actor_SpawnAsChild(&play->actorCtx, &player->actor, play, ACTOR_ARMS_HOOK, player->actor.world.pos.x,
                           player->actor.world.pos.y, player->actor.world.pos.z, 0, player->actor.shape.rot.y, 0, 0);
    if (player->heldActor != NULL) {
        sHooksPutBack++;
        sHookMissing = 0;
    } else {
        sHookMissing = 1;
    }
}

s32 ActorHeap_HookshotNote(void) {
    return (sHooksPutBack & 0xFFFF) | (sHookMissing ? 0x10000 : 0);
}
