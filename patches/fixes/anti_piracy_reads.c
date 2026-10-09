// The game's anti-piracy checks, and the one of them that crashes here.
//
// The retail game carries three checks that the decompilation compiles only for the console
// (`PLATFORM_N64`), each answering the question "did the real boot ROM run?" by looking at what
// it leaves behind in memory:
//
//   1. The bars of Ganon's Tower during the collapse (`func_808C0CD4`, z_bg_zg.c) read the word
//      at physical 0x2E8 through the uncached view, `IO_READ(0x000002E8)`, and refuse to open
//      unless it holds an instruction the boot ROM's own RSP code leaves there.
//   2. Zelda's hair (`func_80B4FEA4`, z_en_zl2.c) is drawn misshapen unless `osCicId` is 6105.
//   3. The fishing pond (`Fishing_Init`, z_fishing.c) locks the reel unless `gCICBootMagic0`,
//      which the boot code reads from physical 0x2FB1F4, holds the value the boot ROM deposits.
//
// Two of the three reads go through the uncached view (`IO_READ` is a load at 0xA0000000 plus
// the physical address), and in this recompilation the uncached view is not memory at all: the
// runtime commits the console's eight megabytes behind the cached addresses and leaves everything
// past them protected, so that a bad address traps instead of reading whatever happens to be
// there (the PHASE-26 reasoning, in .scaffold/execution/issues.md). So:
//
//   - The bars' read crashes the game thread the moment the collapse interior loads, with the
//     features on or off (phase 46's regression run found it, at entrance 0x179, which the parity
//     sweep had never visited).
//   - The boot code's read never runs: `CIC6105_SaveBootMagicValues` is one of the functions the
//     recompiler configuration stubs, because it reads what only a boot ROM leaves, so
//     `gCICBootMagic0` stays zero and the fishing pond locks its reel for as long as the game runs.
//   - `osCicId` is an ordinary variable the application deposits before the game's first
//     instruction (src/game/game_init.cpp), so Zelda's hair is drawn as drawn.
//
// THE FIX for the bars is the decompilation's own non-console body: the check is dropped and
// the function otherwise does what it always did, which is to open the bars when the switch flag
// the actor watches is set. That is what the decompilation compiles for every platform that is
// not the console, so this is not an interpretation. THE FIX for the fishing pond is the boot
// function with the boot ROM's answer written in: the value it would have read is the constant
// the game compares against (`CIC_BOOT_MAGIC0_IS_CORRECT`, cic6105.h), so the function stores
// that instead of reading it; the second word is compared by nothing in this game and is left
// as the stub left it, zero, which is said here so nobody looks for a missing value. Everything
// else is the original, line for line; a patch REPLACES its function, so anything dropped here
// would simply stop happening.

#include "patches.h"
#include "play_state.h"
#include "overlays/actors/ovl_Bg_Zg/z_bg_zg.h"

s32 func_808C0C98(BgZg* this, PlayState* play);
void func_808C0C50(BgZg* this);

extern u32 gCICBootMagic0;
extern u32 gCICBootMagic1;

// @recomp Patched to drop the console-only read of the boot ROM's leftovers through the uncached
// view, which is not memory here.
RECOMP_PATCH void func_808C0CD4(BgZg* this, PlayState* play) {
    if (func_808C0C98(this, play) != 0) {
        this->action = 1;
        func_808C0C50(this);
    }
}

// @recomp Patched to store what the boot ROM would have left for the game to read, since the
// read itself is stubbed by the recompiler configuration and would trap if it were not.
RECOMP_PATCH void CIC6105_SaveBootMagicValues(void) {
    gCICBootMagic0 = 0xAD090010;
    gCICBootMagic1 = 0;
}
