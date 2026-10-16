// HOW FULL THE SCENE IS, AND WHERE THE SUN'S SONG STANDS, IN THE LOG (2026-10-12).
//
// The game refuses every spawn once its scene holds more than 200 actors (Actor_Spawn,
// ACTOR_NUMBER_MAX), without a word. That is how the Sun's Song stopped ending: the ring and tree
// spawners, reaching as far as the Render distance and Shadow casters rows ask, filled Hyrule Field,
// the song's effect (Oceff_Spot) was refused, and only the effect starts the change of time and
// releases the player, so Link stood holding the song's pose while the clock crept on (the user,
// 2026-10-12). The spawners now leave room (patches/culling.c), which the user confirmed: "so is sun
// sung fix!".
//
// What stays is the note, so the next time something is refused it shows: the scene's actor count
// against the limit, the largest block left in the game's actor memory, and the song's state, sent
// to the program every update, which writes a line only when something worth reading changes
// (src/game/actor_room.cpp).
#include "patches.h"
#include "actor_note.h"

#include "actor.h"
#include "message.h"
#include "ocarina.h"
#include "play_state.h"
#include "player.h"
#include "save.h"
#include "zelda_arena.h"

// The program's side (src/game/recomp_api.cpp).
DECLARE_FUNC(void, recomp_note_actors, s32 total, s32 largestFree, s32 song);

static s32 ActorNote_EffectAlive(PlayState* play) {
    Actor* actor;

    for (actor = play->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; actor != NULL; actor = actor->next) {
        if (actor->id == ACTOR_OCEFF_SPOT) {
            return true;
        }
    }
    return false;
}

void ActorNote_Update(PlayState* play) {
    MessageContext* msgCtx = &play->msgCtx;
    Player* player = GET_PLAYER(play);
    u32 largestFree = 0;
    u32 free = 0;
    u32 allocated = 0;
    s32 effect = ActorNote_EffectAlive(play);
    // Holding the song's pose after the Sun's Song with no effect to end it: what the user saw.
    s32 stranded = (msgCtx->lastPlayedSong == OCARINA_SONG_SUNS) && (msgCtx->ocarinaMode == OCARINA_MODE_01) &&
                   (msgCtx->msgMode == MSGMODE_NONE) && (gSaveContext.sunsSongState == SUNSSONG_INACTIVE) &&
                   (player != NULL) && ((player->stateFlags2 & PLAYER_STATE2_USING_OCARINA) != 0) && !effect;

    ZeldaArena_GetSizes(&largestFree, &free, &allocated);
    // The song as bits: 1 stranded, 2 the effect alive, 4 to 16 the time change's state, 32 and up
    // the pose's mode, so any change in any of them is a line in the log.
    recomp_note_actors(play->actorCtx.total, (s32)largestFree,
                       (stranded ? 1 : 0) | (effect ? 2 : 0) | ((gSaveContext.sunsSongState & 7) << 2) | ((msgCtx->ocarinaMode & 0x3F) << 5));
}
