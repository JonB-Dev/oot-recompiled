// Bigger display list pools, so a dense scene cannot overflow them.
//
// THE FAULT THIS FIXES, measured on 2026-09-23 with the frame recorder writing the game's own
// update count and the runtime's retrace count beside every drawn frame. In Kakariko Village by
// day the game would, now and then, run a hundred updates between two pictures, and once one
// thousand nine hundred and thirty four; Link teleported, the camera jumped, the clock raced when
// he left, and the audio broke, all in the same instant. With a wall clock gate holding the game
// to its twenty updates a second the same moments became a frozen picture four seconds long, with
// the game running on underneath at exactly twenty a second: 83 updates against 247 retraces, and
// Link at the gate before it and deep in the village after.
//
// SO THE GAME WAS RUNNING FRAMES AND SUBMITTING NOTHING. graph.c does that on purpose: when any
// of the frame's display list pools overflows (THGA_IsCrash), the frame sets `problem` and skips
// Graph_TaskSet00, which is the only call in the whole loop that waits. No task is submitted, the
// picture keeps its last frame, and Graph_Update runs again at once. On the console that state
// was never reached, because the pools were sized for the lists the console drew. Here our
// patches add to every list (a transform group per tagged matrix, the render distance drawing
// actors the game would have culled), and the densest scene in the game, a dozen villagers and
// their buildings by day and nobody at all by night, is the one that overflows. Which is exactly
// where, and exactly when, it happened.
//
// THE FIX is the reference project's for its game: point the frame's arenas at bigger buffers of
// our own. Graph_InitTHGA is the one place the game binds a frame to its pool, so the patch
// reproduces it line for line with the pointers swapped. The game's own pools stay where they are
// and still carry the head and tail magic that Graph_Update checks, which can no longer fail
// (nothing writes there now) and is kept because dropping a check the original made is how a
// patch introduces a bug that looks like a game bug.

#include "patches.h"
#include "gfx_pool.h"

#include "buffers.h"
#include "sys_cfb.h"
#include "thga.h"

BiggerGfxPool gBiggerGfxPools[2];

// @recomp Patched to build each frame in the bigger pools above.
RECOMP_PATCH void Graph_InitTHGA(GraphicsContext* gfxCtx) {
    GfxPool* pool = &gGfxPools[gfxCtx->gfxPoolIdx & 1];
    BiggerGfxPool* bigger = &gBiggerGfxPools[gfxCtx->gfxPoolIdx & 1];

    pool->headMagic = GFXPOOL_HEAD_MAGIC;
    pool->tailMagic = GFXPOOL_TAIL_MAGIC;
    THGA_Init(&gfxCtx->polyOpa, bigger->polyOpaBuffer, sizeof(bigger->polyOpaBuffer));
    THGA_Init(&gfxCtx->polyXlu, bigger->polyXluBuffer, sizeof(bigger->polyXluBuffer));
    THGA_Init(&gfxCtx->overlay, bigger->overlayBuffer, sizeof(bigger->overlayBuffer));
    THGA_Init(&gfxCtx->work, bigger->workBuffer, sizeof(bigger->workBuffer));

    gfxCtx->polyOpaBuffer = bigger->polyOpaBuffer;
    gfxCtx->polyXluBuffer = bigger->polyXluBuffer;
    gfxCtx->overlayBuffer = bigger->overlayBuffer;
    gfxCtx->workBuffer = bigger->workBuffer;

    // As the original: `% 2` on a signed index, with the decompilation's note that it would take
    // over a year of play to matter.
    gfxCtx->curFrameBuffer = SysCfb_GetFbPtr(gfxCtx->fbIdx % 2);
    gfxCtx->unk_014 = 0;
}
