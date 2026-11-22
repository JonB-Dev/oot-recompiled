// The display list pools, and the two values graph.c keeps to itself.
//
// Shared by frame_setup.c (which checks the pools each frame, as the game does) and gfx_pools.c
// (which replaces them with bigger ones). One definition, because the two files must agree on
// what a pool is and the game's own graph.c keeps these as file locals that no header offers.

#ifndef OOT_RECOMP_GFX_POOL_H
#define OOT_RECOMP_GFX_POOL_H

#include "gfx.h"

// File local in the decompilation's graph.c, so they are repeated here with the same values.
#define GFXPOOL_HEAD_MAGIC 0x1234
#define GFXPOOL_TAIL_MAGIC 0x5678

// THE POOLS THE GAME BUILDS ITS FRAME IN, MADE BIGGER. The game's own are sized for the lists the
// console drew (gfx.h: 0x17E0 opaque commands, 0x800 translucent, 0x400 overlay, 0x80 work). Our
// patches add to every list (a transform group per tagged matrix for the frame interpolation; the
// render distance drawing actors the game would have culled), so the scenes that were closest to
// full on the console overflow here. See gfx_pools.c for what an overflow does to the game, which
// is the reason these exist, and frame_setup.c for how one is counted if it still happens.
//
// Sizes: opaque a little over five times the game's, the others four to eight times. Memory is
// not the constraint it was on the console (these live in the runtime's extra RAM, see
// patches.ld), and the reference project goes larger still for its game.
typedef struct {
    Gfx polyOpaBuffer[0x10000];   // doubled 2026-09-24: the traced lighting draws the room and the actors behind the camera too
    Gfx polyXluBuffer[0x4000];
    Gfx overlayBuffer[0x1000];
    Gfx workBuffer[0x400];
} BiggerGfxPool;

extern BiggerGfxPool gBiggerGfxPools[2];

#endif // OOT_RECOMP_GFX_POOL_H
