#ifndef ACTOR_HEAP_H
#define ACTOR_HEAP_H

#include "ultra64.h"

struct PlayState;

// A hookshot in hand with no hook, put right (patches/actor_heap.c). Called once per update, after it.
void ActorHeap_HookshotGuard(struct PlayState* play);

// The hookshot guard's record for the log: the hooks it has put back, and 0x10000 while one is
// still missing because the game had no room to make it.
s32 ActorHeap_HookshotNote(void);

#endif
