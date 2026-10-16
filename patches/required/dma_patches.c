// Phase 16: DMA and ROM access.
//
// THE PROBLEM THESE SOLVE. The game loads code at runtime: the whole `code` segment at boot, and
// every actor, scene and menu overlay after that. On the console it DMAs those bytes from the
// cartridge into RAM and jumps into them. Here, the bytes are still copied, but the CODE that runs
// is the recompiled C, and the runtime has to be told which recompiled functions correspond to the
// region the game just filled. Without that, the DMA succeeds, the game jumps to the new code, and
// there is nothing there.
//
// So each of these patches is the original function with ONE addition: a call to
// recomp_load_overlays before the copy. Everything else is preserved exactly, because these run on
// the boot path and any behavior change here is a change to how the game starts.
//
// The two functions were found by reading THIS game's decompilation, not the reference project's.
// Majora's Mask patches `Main_Init`; this game has no such function. Its code segment is loaded by
// `Main_ThreadEntry` in src/boot/idle.c, and the two games' overlay loaders have the same shape but
// live in different files with different logging.

#include "patches.h"
#include "misc_funcs.h"

// OverlayRelocationSection lives here in this game. The reference project includes
// "loadfragment.h", which does not exist in this decompilation.
#include "libu64/overlay.h"

// The code segment, as the linker script names it. These are addresses, not data: `_codeSegmentStart`
// is where the code lives in RAM and `_codeSegmentRomStart` is where it lives in the ROM.
extern u8 _codeSegmentStart[];
extern u8 _codeSegmentRomStart[];
extern u8 _codeSegmentRomEnd[];
extern u8 _codeSegmentBssStart[];
extern u8 _codeSegmentBssEnd[];

void DmaMgr_Init(void);
void DmaMgr_RequestSync(void* ram, uintptr_t vrom, size_t size);
void Main(void* arg);
// Overlay_Relocate is already declared by libu64/overlay.h, and it takes a void* vramStart
// rather than a uintptr_t. Redeclaring it here was a conflicting type.

// @recomp Patched to tell the runtime where the code segment landed.
//
// The original is in src/boot/idle.c. It initializes the DMA manager, copies the code segment from
// ROM to RAM, clears that segment's BSS, and calls Main. The logging and the timing calls around
// those steps are dropped: PRINTF compiles to nothing in a retail build, and osGetTime was only
// feeding a transfer-time message.
RECOMP_PATCH void Main_ThreadEntry(void* arg) {
    size_t code_size = (size_t)(_codeSegmentRomEnd - _codeSegmentRomStart);

    DmaMgr_Init();

    // @recomp The one addition. Must come BEFORE the copy: the runtime maps the region, then the
    // bytes arrive, then the game jumps into it.
    recomp_load_overlays((u32)(uintptr_t)_codeSegmentRomStart, _codeSegmentStart, (u32)code_size);

    DmaMgr_RequestSync(_codeSegmentStart, (uintptr_t)_codeSegmentRomStart, code_size);

    bzero(_codeSegmentBssStart, (size_t)(_codeSegmentBssEnd - _codeSegmentBssStart));

    Main(arg);
}

// @recomp Patched to tell the runtime where each overlay landed.
//
// The original is in src/libu64/loadfragment2_n64.c. Reproduced faithfully apart from the
// gOverlayLogSeverity blocks, which only called osSyncPrintf.
RECOMP_PATCH size_t Overlay_Load(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd,
                                 void* allocatedRamAddr) {
    s32 size = vromEnd - vromStart;
    uintptr_t end;
    OverlayRelocationSection* ovlRelocs;

    // @recomp The one addition, before the copy for the same reason as above.
    recomp_load_overlays((u32)vromStart, allocatedRamAddr, (u32)size);

    end = (uintptr_t)allocatedRamAddr + size;

    DmaMgr_RequestSync(allocatedRamAddr, vromStart, size);

    // The overlay file ends with a 32 bit offset from its end back to the relocation section.
    ovlRelocs = (OverlayRelocationSection*)(end - ((s32*)end)[-1]);

    Overlay_Relocate(allocatedRamAddr, ovlRelocs, vramStart);

    if ((s32)ovlRelocs->bssSize != 0) {
        bzero((void*)end, (s32)ovlRelocs->bssSize);
    }

    size = (uintptr_t)vramEnd - (uintptr_t)vramStart;

    osWritebackDCache(allocatedRamAddr, size);
    osInvalICache(allocatedRamAddr, size);

    return size;
}
