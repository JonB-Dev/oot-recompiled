// Tell the renderer, once per frame, that it may use its extended commands, that this frame's
// lists live above the console's memory, and what rate the game is running at. The first and the
// last are the two commands the whole post-parity plan rests on; the middle one arrived with the
// bigger display list pools (gfx_pools.c).
//
// WHY HERE. `Graph_Update` is the one function every game state's frame passes through: it resets
// the frame's display list pools (`Graph_InitTHGA`), runs the game state, closes the lists and
// hands the task to the scheduler. The renderer executes the task from `workBuffer`, and after
// `Graph_InitTHGA` the work list is empty, so a command appended there first is the first command
// the renderer sees in the frame. `gEXEnable` has to be that command: RT64 ignores every other
// extended command until it has seen it, each frame.
//
// `gEXSetRefreshRate` tells RT64 how many game frames a second the game is producing, which is
// what it interpolates between when the frame rate setting asks for the display's rate. This game
// runs its logic every `R_UPDATE_RATE` VIs: 3 in play (twenty a second), 1 in some menus. The value
// is read at the start of the frame, so a change is seen one frame late, which is invisible.
//
// Everything else in the function is the original, line for line, from the decompilation's
// `graph.c` with the debug blocks compiled away as they are in this revision. A patch REPLACES
// its function, so anything dropped here would simply stop happening; the pool corruption checks
// and the timing counters are kept for that reason even though nothing here reads them.

#include "patches.h"
#include "gfx_pool.h"

#include "audio.h"
#include "buffers.h"
#include "fault.h"
#include "game.h"
#include "gfx.h"
#include "line_numbers.h"
#include "printf.h"
#include "regs.h"
#include "speed_meter.h"
#include "terminal.h"
#include "thga.h"
#include "translation.h"
#include "ultra64.h"

// After the decomp's own gbi.h, which it builds on.
#include "rt64_extended_gbi.h"

void Graph_InitTHGA(GraphicsContext* gfxCtx);
void Graph_TaskSet00(GraphicsContext* gfxCtx);

// billboard_tagging.c (phase 39).
void billboard_frame_begin(GameState* gameState);
void billboard_frame_end(GameState* gameState);

// prerender_aspect.c: whether the room on screen is a fixed camera over a background picture, in
// which case the view is held at the console's 4:3. Evaluated here rather than in the room draw
// because this runs for every frame of every game state, so leaving such a room, or leaving play
// altogether, puts the view back.
void prerender_frame_begin(GameState* gameState);

// culling.c: the render distance row, read once a frame rather than once per actor, because the
// culling test it feeds runs for every actor of every frame.
void culling_frame_begin(void);

extern OSTime sGraphPrevUpdateEndTime;

// A frame was dropped because a display list pool overflowed: which pools (bit 0 opaque, bit 1
// translucent, bit 2 overlay, bit 3 work) and the worst overrun in bytes. Counted natively so the
// frame recorder can write the count beside every picture, and logged there.
void recomp_note_dropped_frame(u32 which, s32 overrun);

// ----------------------------------------------------------------------------------------------
// THE DROPPED FRAME WAITS.
//
// WHAT THE GAME DOES WITH AN OVERFLOWED FRAME. When a display list pool overflows, the frame below
// sets `problem` and skips Graph_TaskSet00. That is the right call, the lists are not safe to
// draw, but Graph_TaskSet00 is also the only call in the loop that WAITS, so a dropped frame is
// followed by the next Graph_Update immediately, and by the next, for as long as the scene keeps
// overflowing. On the console that state was designed never to be reached. Here it was reached
// in Kakariko by day (see gfx_pools.c), and the game ran thousands of updates a second with the
// picture frozen on its last submitted frame: the teleporting, the camera jumps, the clock racing
// and the broken audio were all this one loop.
//
// gfx_pools.c removes the cause. This is the belt to its braces: if a pool ever overflows again,
// the dropped frame costs the time a drawn one would have, so the worst case is a frozen picture
// at the game's real speed rather than a runaway.
//
// WHY A TIMER AND NOT A NATIVE SLEEP. The runtime emulates the console's single processor:
// exactly one game thread runs at a time. A native sleep called from this thread would not pause
// the game, it would pause EVERY game thread, the audio thread and the scheduler included. Waiting
// on a message queue is what the console's own waits were, and the runtime hands the processor to
// the next thread while this one blocks.
//
// WHY ONLY ON A DROP. A build of 2026-09-23 ran this wait on every frame, as a gate. It paced the
// game correctly (the recorder showed exactly twenty updates a second through a four second
// freeze), but the pictures came unevenly (16.5, 19.6 and 13.9 ms apart where they had been 16.6
// each), because the game's own gate, the scheduler swapping framebuffers on the retrace, is a
// clock of its own and the two beat against each other. The game's gate is the console's and it
// works; it is only the drop path that lacks one.
// ----------------------------------------------------------------------------------------------

// One video retrace in osGetTime units: 46,875,000 counts a second at sixty a second.
#define FRAME_GATE_RETRACE 781250

static OSMesgQueue sFrameGateQueue;
static OSMesg sFrameGateMsg[1];
static OSTimer sFrameGateTimer;
static u8 sFrameGateReady = false;

static void dropped_frame_wait(s32 updateRate) {
    if (!sFrameGateReady) {
        osCreateMesgQueue(&sFrameGateQueue, sFrameGateMsg, 1);
        sFrameGateReady = true;
    }
    // The timer's message is the only thing ever sent to this queue, and it is always consumed,
    // so the queue can never hold a stale one.
    osSetTimer(&sFrameGateTimer, (OSTime)updateRate * FRAME_GATE_RETRACE, 0, &sFrameGateQueue, NULL);
    osRecvMesg(&sFrameGateQueue, NULL, OS_MESG_BLOCK);
}

// @recomp Patched to enable the extended GBI and report the game's frame rate every frame.
RECOMP_PATCH void Graph_Update(GraphicsContext* gfxCtx, GameState* gameState) {
    u32 problem;
    s32 updateRate;

    gameState->inPreNMIState = false;
    Graph_InitTHGA(gfxCtx);

    // @recomp The three additions, at the head of the frame's first list so nothing precedes them.
    // R_UPDATE_RATE is a register the game writes; guard the division against a value it has not
    // set yet during boot, when three (the play rate) is the honest default.
    updateRate = R_UPDATE_RATE;
    if (updateRate < 1) {
        updateRate = 3;
    }
    OPEN_DISPS(gfxCtx, "../graph.c", 966);
    gEXEnable(WORK_DISP++);
    // The lists, matrices and vertices of this very frame live in the bigger pools of
    // gfx_pools.c, which are in the runtime's extra RAM above the console's 8 MB. Without this
    // the renderer masks every address to the console's 8 MB, reads the wrong memory, and walks a
    // garbage list off the end of what is mapped: the crash of 2026-09-23 18:09, first thing
    // after the pools moved. It must come before the first address the renderer follows, and the
    // branch to the opaque list at the end of this one is the first such address.
    gEXSetRDRAMExtended(WORK_DISP++, 1);
    gEXSetRefreshRate(WORK_DISP++, 60 / updateRate);
    CLOSE_DISPS(gfxCtx, "../graph.c", 975);

    GameState_ReqPadData(gameState);

    // @recomp The billboard tracking (billboard_tagging.c) learns which state is running and
    // forgets last frame's matrices before the game draws, and after it has drawn writes its
    // group edits into the translucent list, before the lists are closed below.
    billboard_frame_begin(gameState);
    prerender_frame_begin(gameState);
    culling_frame_begin();
    GameState_Update(gameState);
    billboard_frame_end(gameState);

    OPEN_DISPS(gfxCtx, "../graph.c", 999);

    gSPBranchList(WORK_DISP++, gfxCtx->polyOpaBuffer);
    gSPBranchList(POLY_OPA_DISP++, gfxCtx->polyXluBuffer);
    gSPBranchList(POLY_XLU_DISP++, gfxCtx->overlayBuffer);
    gDPPipeSync(OVERLAY_DISP++);
    gDPFullSync(OVERLAY_DISP++);
    gSPEndDisplayList(OVERLAY_DISP++);

    CLOSE_DISPS(gfxCtx, "../graph.c", 1028);

    problem = false;

    {
        GfxPool* pool = &gGfxPools[gfxCtx->gfxPoolIdx & 1];

        if (pool->headMagic != GFXPOOL_HEAD_MAGIC) {
            //! @bug (?) : "problem = true;" may be missing
            PRINTF("%c", BEL);
            PRINTF(VT_COL(RED, WHITE) T("ダイナミック領域先頭が破壊されています\n", "Dynamic area head is destroyed\n")
                       VT_RST);
            Fault_AddHungupAndCrash("../graph.c", LN4(937, 940, 951, 1067, 1070));
        }

        if (pool->tailMagic != GFXPOOL_TAIL_MAGIC) {
            problem = true;
            PRINTF("%c", BEL);
            PRINTF(VT_COL(RED, WHITE)
                       T("ダイナミック領域末尾が破壊されています\n", "Dynamic region tail is destroyed\n") VT_RST);
            Fault_AddHungupAndCrash("../graph.c", LN4(943, 946, 957, 1073, 1076));
        }
    }

    if (THGA_IsCrash(&gfxCtx->polyOpa)) {
        problem = true;
        PRINTF("%c", BEL);
        PRINTF(VT_COL(RED, WHITE) T("ゼルダ0は死んでしまった(graph_alloc is empty)\n",
                                    "Zelda 0 is dead (graph_alloc is empty)\n") VT_RST);
    }
    if (THGA_IsCrash(&gfxCtx->polyXlu)) {
        problem = true;
        PRINTF("%c", BEL);
        PRINTF(VT_COL(RED, WHITE) T("ゼルダ1は死んでしまった(graph_alloc is empty)\n",
                                    "Zelda 1 is dead (graph_alloc is empty)\n") VT_RST);
    }
    if (THGA_IsCrash(&gfxCtx->overlay)) {
        problem = true;
        PRINTF("%c", BEL);
        PRINTF(VT_COL(RED, WHITE) T("ゼルダ4は死んでしまった(graph_alloc is empty)\n",
                                    "Zelda 4 is dead (graph_alloc is empty)\n") VT_RST);
    }

    if (!problem) {
        Graph_TaskSet00(gfxCtx);
        gfxCtx->gfxPoolIdx++;
        gfxCtx->fbIdx++;
    } else {
        // @recomp A dropped frame is counted, named, and made to cost the time a drawn one would
        // have. See THE DROPPED FRAME WAITS above. Which pools, and the worst overrun in bytes:
        // THGA_GetRemaining is tail minus head, so it is negative by the amount overrun.
        u32 which = 0;
        s32 worst = 0;
        s32 left;

        left = THGA_GetRemaining(&gfxCtx->polyOpa);
        if (left < 0) { which |= 1; if (left < worst) { worst = left; } }
        left = THGA_GetRemaining(&gfxCtx->polyXlu);
        if (left < 0) { which |= 2; if (left < worst) { worst = left; } }
        left = THGA_GetRemaining(&gfxCtx->overlay);
        if (left < 0) { which |= 4; if (left < worst) { worst = left; } }
        left = THGA_GetRemaining(&gfxCtx->work);
        if (left < 0) { which |= 8; if (left < worst) { worst = left; } }

        recomp_note_dropped_frame(which, -worst);
        dropped_frame_wait(updateRate);
    }

    Audio_Update();

    {
        OSTime timeNow = osGetTime();
        s32 pad;

        gRSPGfxTimeTotal = gRSPGfxTimeAcc;
        gRSPAudioTimeTotal = gRSPAudioTimeAcc;
        gRDPTimeTotal = gRDPTimeAcc;
        gRSPGfxTimeAcc = 0;
        gRSPAudioTimeAcc = 0;
        gRDPTimeAcc = 0;

        if (sGraphPrevUpdateEndTime != 0) {
            gGraphUpdatePeriod = timeNow - sGraphPrevUpdateEndTime;
        }
        sGraphPrevUpdateEndTime = timeNow;
    }
}
