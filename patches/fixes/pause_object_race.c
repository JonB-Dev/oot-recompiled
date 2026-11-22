// The pause menu overwrites memory the renderer may still be reading. Wait for it first.
//
// THE MECHANISM, traced in .scaffold/execution/issues.md under PHASE-26.
//
// Opening the pause menu has the game borrow the object space for its own use: the equipment
// screen renders Link from a fresh copy of gameplay_keep and the link object, and that copy is
// DMA'd straight over the live gameplay_keep, shifted 0x3800 bytes along to make room for a render
// texture (`Player_InitPauseDrawData`, z_player_lib.c). Closing the menu reloads every object
// over the top again (`Object_ReloadAll`, z_scene.c).
//
// Both overwrites are synchronous and both happen in `Play_Update`. Neither waits for the RCP.
// The frame drawn just before the menu opens still draws the minimap's compass arrow out of
// gameplay_keep (`gCompassArrowDL`, at 0xC820 in that object), and the frame drawn just before
// the menu closes still draws Link out of the borrowed copy. So on every pause and every unpause
// there is a window in which the display list already submitted refers to memory the game is
// about to replace.
//
// On the console that window is closed by timing. The RSP starts reading the list the moment the
// scheduler hands it over, and it has consumed a HUD-only frame long before the CPU has decoded
// the first 0x9000 bytes of a compressed object file. It is a race the hardware always wins, so
// the game never needed to wait.
//
// Here the renderer is a host thread. It takes the task later, interprets the list itself, and for
// the pause backdrop it also has to bring the framebuffer back from the GPU. It can still be inside
// that list when the game thread overwrites gameplay_keep, and then the compass arrow command
// resolves to the middle of the shifted copy, which is not a display list at all. The renderer
// reads a G_DL there that points 260MB past the end of console memory, follows it, never finds a
// G_ENDDL, and walks off the end of mapped memory. That is the crash.
//
// THE FIX is to make explicit the wait the console got for free: flush the scheduler's task queue
// before either overwrite, with the same `Sched_FlushTaskQueue` the game itself uses in
// `Play_Draw` before it reads the pause backdrop back out of the framebuffer. The runtime only
// reports the RDP finished once the renderer has fully read the list, so the flush is a real
// barrier. It costs one wait per pause and one per unpause, on the order of a frame, at a moment
// the game is already stalled on a synchronous DMA.
//
// Everything else in both functions is the original, line for line. A patch REPLACES its
// function, so anything dropped here would simply stop happening.

#include "patches.h"

#include "alignment.h"
#include "animation.h"
#include "dma.h"
#include "object.h"
#include "pause.h"
#include "play_state.h"
#include "player.h"
#include "save.h"
#include "sched.h"
#include "segmented_address.h"

#include "assets/objects/gameplay_keep/player_anim_headers.h"

// @recomp Patched to wait for the RCP before overwriting gameplay_keep in place.
RECOMP_PATCH u32 Player_InitPauseDrawData(PlayState* play, u8* segment, SkelAnime* skelAnime) {
    s16 linkObjectId = gLinkObjectIds[gSaveContext.save.linkAge];
    u32 size;
    void* ptr;

    // @recomp The one addition. The previous frame's list still draws the compass arrow out of
    // the memory the DMA below is about to replace.
    Sched_FlushTaskQueue();

    // Note that since gameplay_keep is typically a compressed segment and due to constraints in the DMA manager,
    // the entire segment is loaded even when only the first bytes up to PAUSE_PLAYER_SEGMENT_GAMEPLAY_KEEP_BUFFER_SIZE
    // are kept for later use.
    size = gObjectTable[OBJECT_GAMEPLAY_KEEP].vromEnd - gObjectTable[OBJECT_GAMEPLAY_KEEP].vromStart;
    ptr = PAUSE_PLAYER_SEGMENT_GAMEPLAY_KEEP_START(segment);
    DmaMgr_RequestSync(ptr, gObjectTable[OBJECT_GAMEPLAY_KEEP].vromStart, size);

    size = gObjectTable[linkObjectId].vromEnd - gObjectTable[linkObjectId].vromStart;
    ptr = PAUSE_PLAYER_SEGMENT_LINK_OBJECT(segment);
    DmaMgr_RequestSync(ptr, gObjectTable[linkObjectId].vromStart, size);

    // Joint tables are placed after the link object
    ptr = (void*)ALIGN16((uintptr_t)ptr + size);

    gSegments[4] = OS_K0_TO_PHYSICAL(PAUSE_PLAYER_SEGMENT_GAMEPLAY_KEEP_START(segment));
    gSegments[6] = OS_K0_TO_PHYSICAL(PAUSE_PLAYER_SEGMENT_LINK_OBJECT(segment));

    SkelAnime_InitLink(play, skelAnime, gPlayerSkelHeaders[gSaveContext.save.linkAge],
                       &gPlayerAnim_link_normal_wait, 9, ptr, ptr, PLAYER_LIMB_MAX);

    return PAUSE_PLAYER_SEGMENT_TOTAL_SIZE(size);
}

// @recomp Patched to wait for the RCP before reloading every object over the pause menu's copy.
RECOMP_PATCH void Object_ReloadAll(ObjectContext* objectCtx) {
    s32 i;
    s32 id;
    u32 size;

    // @recomp The one addition. The menu's last frame still draws Link and the item icons out of
    // the memory the reload below is about to replace.
    Sched_FlushTaskQueue();

    for (i = 0; i < objectCtx->numEntries; i++) {
        id = objectCtx->slots[i].id;
        size = gObjectTable[id].vromEnd - gObjectTable[id].vromStart;
        DmaMgr_RequestSync(objectCtx->slots[i].segment, gObjectTable[id].vromStart, size);
    }
}
