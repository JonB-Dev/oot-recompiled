#ifndef ACTOR_NOTE_H
#define ACTOR_NOTE_H

#include "ultra64.h"

struct PlayState;

// How full the scene is and where the Sun's Song stands, written to the program's log
// (patches/actor_note.c). Called once per update, after it.
void ActorNote_Update(struct PlayState* play);

// The actor heap's largest free block at the last update, for the spawners (patches/culling.c).
u32 ActorNote_LargestFree(void);

#endif
