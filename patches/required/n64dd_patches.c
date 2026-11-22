// Tell the game there is no 64DD, without asking the hardware.
//
// THE PROBLEM. `Main` calls `func_800AD410` early, which does three things: DMAs the `n64dd`
// segment into RAM, clears its BSS, and then calls into that freshly loaded code to ask whether a
// disk drive is attached. That last step reaches `LeoDriveExist`, which reads the drive's hardware
// registers, and there is no drive and no registers, so it faults.
//
// Found by booting: the crash handler's stack was
//
//     Main_ThreadEntry -> Main -> func_800AD410 -> func_801C6E80 -> LeoDriveExist
//
// THE ANSWER. Replace the whole function with its no-drive outcome. The 64DD shipped only in Japan,
// this is the NTSC-U build, and no setup this program supports has one. `Main` reads the result
// immediately afterward and picks a heap layout from it:
//
//     func_800AD410();
//     if (D_80121211 != 0) {  // a drive is present
//         systemHeapStart = (uintptr_t)_n64ddSegmentEnd;
//         SysCfb_Init(1);
//     } else {                // no drive, which is what we want
//         func_800AD488();
//         systemHeapStart = (uintptr_t)_buffersSegmentEnd;
//         SysCfb_Init(0);
//     }
//
// so reporting no drive is not merely safe, it selects the layout every real NTSC-U console used.
//
// Skipping the DMA as well as the probe is deliberate. Loading the n64dd segment would mean
// mapping its overlay through the runtime for code that is then never called, and the segment's
// absence is exactly what "no drive" means.

#include "patches.h"
#include "misc_funcs.h"

// Both live in the game's data. D_80121210 records that the check has run; D_80121211 records the
// answer, and is what Main reads.
extern u8 D_80121210;
extern u8 D_80121211;

// @recomp Patched to report no 64DD without touching the drive's registers.
RECOMP_PATCH void func_800AD410(void) {
    D_80121210 = true;    // the check has been performed
    D_80121211 = false;   // and there is no drive
}
