// Runtime functions a patch may call.
//
// Each of these is implemented natively in src/game/recomp_api.cpp and reached through a fake
// address in syms.ld. DECLARE_FUNC gives the declaration the right shape on both sides: a normal
// prototype here in the MIPS world, and the (rdram, ctx) form on the native side.

#ifndef MISC_FUNCS_H
#define MISC_FUNCS_H

#include "patch_helpers.h"

// Tell the runtime that `size` bytes of ROM at `rom` are now in RAM at `ram`, so it can map the
// recompiled code for that region. Called wherever the game loads code: the code segment at boot,
// and every overlay after that.
DECLARE_FUNC(void, recomp_load_overlays, u32 rom, void* ram, u32 size);

#endif // MISC_FUNCS_H
