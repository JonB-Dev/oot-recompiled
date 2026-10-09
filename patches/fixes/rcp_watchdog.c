// The game's "RCP is HUNG UP" watchdog, and why it halts the game here.
//
// Every frame, `Graph_TaskSet00` (graph.c) waits for the previous frame's graphics task to come
// back before it builds the next one, and it does not wait forever: it sets a three second timer,
// and if the timer's message arrives first it calls `Fault_AddHungupAndCrashImpl("RCP is HUNG
// UP!!", "Oh! MY GOD!!")`. In this revision there is no retry. On the console that is a guard
// against the graphics hardware locking up, and the fault screen it leads to is a developer's.
//
// In this recompilation the "RCP" is the renderer, and the renderer can legitimately be busy for
// longer than three seconds. The user's report of 2026-10-07, "The texture pack loading is
// actually causing the game to crash", was exactly this: a texture pack switched on in play takes
// three and a half to four seconds to load (its 45,000 files are walked and its 9 MB database is
// read), no frame can finish meanwhile, and the game decided the hardware had hung and stopped
// itself in the fault screen. The picture froze with the game thread still running, and closing
// the window then crashed the runtime's timer thread. Read off the frozen process with
// tools/thread_stacks.py: every renderer thread idle and starved, and the game's graphics thread
// in Graph_TaskSet00 -> Fault_AddHungupAndCrashImpl -> Fault_WaitForButtonCombo.
//
// THE FIX is the wait without the timer: the task is waited for however long it takes. That is
// what the reference project does (upstream/Zelda64Recomp, patches/input_latency.c, "@recomp
// Disable the wait here"), and it is safe for the same reason there: a real hardware hang cannot
// happen to a renderer, and a renderer that genuinely stopped would freeze the picture whether or
// not the game then also stopped itself, with the fault screen helping nobody who is playing.
// Everything else is the original, line for line, with the debug blocks compiled away as they are
// in this revision; a patch REPLACES its function, so anything dropped here would simply stop
// happening, which is why the timing counters nothing here reads are kept.

#include "patches.h"

#include "array_count.h"
#include "buffers.h"
#include "gfx.h"
#include "regs.h"
#include "sched.h"
#include "speed_meter.h"
#include "sys_ucode.h"
#include "ultra64.h"
#include "versions.h"

extern OSTime sGraphPrevTaskTimeStart;

RECOMP_PATCH void Graph_TaskSet00(GraphicsContext* gfxCtx) {
    OSTask_t* task = &gfxCtx->task.list.t;
    OSScTask* scTask = &gfxCtx->task;

    gGfxTaskSentToNextReadyMinusAudioThreadUpdateTime =
        osGetTime() - sGraphPrevTaskTimeStart - gAudioThreadUpdateTimeAcc;

    {
        OSMesg msg;

        // @recomp No three second timer and no fault screen: wait for the previous task to come
        // back, however long the renderer takes (see the note at the top).
        osRecvMesg(&gfxCtx->queue, &msg, OS_MESG_BLOCK);

        osRecvMesg(&gfxCtx->queue, &msg, OS_MESG_NOBLOCK);
    }

    if (gfxCtx->callback != NULL) {
        gfxCtx->callback(gfxCtx, gfxCtx->callbackParam);
    }

    {
        OSTime timeNow = osGetTime();

        if (gAudioThreadUpdateTimeStart != 0) {
            // The audio thread update is running
            // Add the time already spent to the accumulator and leave the rest for the next cycle

            gAudioThreadUpdateTimeAcc += timeNow - gAudioThreadUpdateTimeStart;
            gAudioThreadUpdateTimeStart = timeNow;
        }
        gAudioThreadUpdateTimeTotalPerGfxTask = gAudioThreadUpdateTimeAcc;
        gAudioThreadUpdateTimeAcc = 0;

        sGraphPrevTaskTimeStart = osGetTime();
    }

    task->type = M_GFXTASK;
    task->flags = OS_SC_DRAM_DLIST;
    task->ucode_boot = SysUcode_GetUCodeBoot();
    task->ucode_boot_size = SysUcode_GetUCodeBootSize();
    task->ucode = SysUcode_GetUCode();
    task->ucode_data = SysUcode_GetUCodeData();
    task->ucode_size = SP_UCODE_SIZE;
    task->ucode_data_size = SP_UCODE_DATA_SIZE;
    task->dram_stack = gGfxSPTaskStack;
    task->dram_stack_size = sizeof(gGfxSPTaskStack);
    task->output_buff = gGfxSPTaskOutputBuffer;
    task->output_buff_size = gGfxSPTaskOutputBuffer + ARRAY_COUNT(gGfxSPTaskOutputBuffer);
    task->data_ptr = (u64*)gfxCtx->workBuffer;

    OPEN_DISPS(gfxCtx, "../graph.c", 828);
    task->data_size = (uintptr_t)WORK_DISP - (uintptr_t)gfxCtx->workBuffer;
    CLOSE_DISPS(gfxCtx, "../graph.c", 830);

    task->yield_data_ptr = gGfxSPTaskYieldBuffer;

    task->yield_data_size = sizeof(gGfxSPTaskYieldBuffer);

    scTask->next = NULL;
    scTask->flags = OS_SC_NEEDS_RSP | OS_SC_NEEDS_RDP | OS_SC_SWAPBUFFER | OS_SC_LAST_TASK;
    if (R_GRAPH_TASKSET00_FLAGS & 1) {
        R_GRAPH_TASKSET00_FLAGS &= ~1;
        scTask->flags &= ~OS_SC_SWAPBUFFER;
        gfxCtx->fbIdx--;
    }

    scTask->msgQueue = &gfxCtx->queue;
    scTask->msg = NULL;

    {
        static CfbInfo sGraphCfbInfos[3];
        static s32 sGraphCfbInfoIdx = 0;
        CfbInfo* cfb;

        cfb = &sGraphCfbInfos[sGraphCfbInfoIdx];

        sGraphCfbInfoIdx = (sGraphCfbInfoIdx + 1) % ARRAY_COUNT(sGraphCfbInfos);
        cfb->framebuffer = gfxCtx->curFrameBuffer;
        cfb->swapBuffer = gfxCtx->curFrameBuffer;

        cfb->viMode = gfxCtx->viMode;
        cfb->viFeatures = gfxCtx->viFeatures;
#if OOT_VERSION >= PAL_1_0
        cfb->xScale = gfxCtx->xScale;
        cfb->yScale = gfxCtx->yScale;
#endif
        cfb->unk_10 = 0;
        cfb->updateRate = R_UPDATE_RATE;

        scTask->framebuffer = cfb;
    }

    gfxCtx->schedMsgQueue = &gScheduler.cmdQueue;

    osSendMesg(&gScheduler.cmdQueue, (OSMesg)scTask, OS_MESG_BLOCK);
    Sched_Notify(&gScheduler);
}
