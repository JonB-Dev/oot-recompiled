// Two libultra functions that nothing else provides.
//
// THE GAP, precisely. The recompiler keeps a list of libultra functions it deliberately does not
// translate, because librecomp implements them natively:
// `N64Recomp/src/symbol_lists.cpp` names both `osEPiWriteIo` and `osLeoDiskInit`. librecomp
// implements neither. So the recompiled code contains calls to `osEPiWriteIo_recomp` and
// `osLeoDiskInit_recomp`, the generated `funcs.h` does not declare them (they were not translated),
// and nothing anywhere defines them.
//
// This is an upstream gap rather than a mistake in our configuration, and it is one the reference
// project never hits: Majora's Mask does not reference these, and this game does.
//
// WHY A NO-OP IS THE RIGHT ANSWER HERE. Every caller in the generated output is 64DD code:
// `leomain`, `leoSetUA_MEDIUM_CHANGED`, `leoSet_mseq`, `leoSend_asic_cmd_i`,
// `LeoCACreateLeoManager`, `func_801C8E70`. The 64DD shipped only in Japan and this game never
// drives it; the library is present in the retail ROM as dead code. These paths do not execute in
// normal play.
//
// They report the first time each is reached rather than staying silent, because "dead code does
// not run" is a claim, and if it turns out to be wrong the useful thing is to find out at that
// moment instead of debugging whatever happens next.

#include <cstdio>

#include "recomp.h"

namespace {
    bool warned_write_io = false;
    bool warned_disk_init = false;
}

// A NOTE ON bzero, because it was implemented here and then removed.
//
// bzero appears in the recompiler's symbol_lists.cpp alongside osEPiWriteIo and osLeoDiskInit,
// which made it look like the same case: skipped by the recompiler and unimplemented by librecomp.
// It is not. The recompiler DOES translate it, and funcs_0.c defines bzero_recomp. Implementing it
// here produced `lld-link: error: duplicate symbol: bzero_recomp`.
//
// The lesson worth keeping: appearing in that list does not mean a function is missing. The only
// reliable check is which definitions actually exist, which is a grep over the generated output and
// librecomp's sources rather than a reading of the list.

extern "C" void osEPiWriteIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    if (!warned_write_io) {
        warned_write_io = true;
        std::fprintf(stderr,
                     "[shim] osEPiWriteIo was called. This is 64DD code that should be unreachable;\n"
                     "       see src/game/libultra_shims.cpp.\n");
    }
}

extern "C" void osLeoDiskInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    if (!warned_disk_init) {
        warned_disk_init = true;
        std::fprintf(stderr,
                     "[shim] osLeoDiskInit was called. This is 64DD code that should be unreachable;\n"
                     "       see src/game/libultra_shims.cpp.\n");
    }
}
