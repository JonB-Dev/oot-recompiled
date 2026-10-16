// The header every patch includes.
//
// RECOMP_PATCH is the whole mechanism: it puts a function in the `.recomp_patch` section, and the
// recompiler treats a function found there as a REPLACEMENT for the game function of the same name.
// The substitution happens at link time in the main binary, which is why a patch needs no
// registration and no call site.
//
// RECOMP_FORCE_PATCH does the same without the recompiler checking that a function of that name
// exists in the game. Use it only for something genuinely new; the check is worth keeping, because
// a patch whose name is misspelled otherwise compiles, links, and silently never runs.

#ifndef PATCHES_H
#define PATCHES_H

// ---------------------------------------------------------------------------------------------
// LIBULTRA FUNCTIONS THE RUNTIME REPLACES, renamed so a patch reaches the runtime's version.
//
// THESE MUST COME BEFORE THE HEADERS THAT DECLARE THEM. A rename is a macro, so it rewrites
// whatever the preprocessor has not already read. Put after the include, the declaration stays
// `bzero` while the call site becomes `bzero_recomp`, and the build fails with "call to undeclared
// function 'bzero_recomp'" pointing at the patch rather than at the ordering.
//
// These are functions the recompiler deliberately does not translate, because librecomp implements
// them natively under a `_recomp` name. A patch calling the ordinary name links against a symbol
// that stays unresolved, which leaves the jump target at zero, so the recompiler has no address to
// look up and emits a call to the bare name that nothing defines.
//
// Only add a name librecomp actually implements: an alias to something absent trades a compile
// error for a link error.
// ---------------------------------------------------------------------------------------------

#define bzero              bzero_recomp
#define osWritebackDCache  osWritebackDCache_recomp
#define osInvalICache      osInvalICache_recomp
#define osGetTime          osGetTime_recomp
// The message queue and timer waits, for the frame gate in frame_setup.c (the first patch to
// block on a queue). All three are in librecomp/src/ultra_translation.cpp.
#define osCreateMesgQueue  osCreateMesgQueue_recomp
#define osRecvMesg         osRecvMesg_recomp
#define osSetTimer         osSetTimer_recomp
// The send, for fixes/rcp_watchdog.c, which reproduces the frame's hand-over to the scheduler.
// Also in ultra_translation.cpp.
#define osSendMesg         osSendMesg_recomp

// A DIFFERENT CASE, NOT handled here: the game's OWN sinf and cosf (libultra's, real recompiled
// functions) are written as sinf_recomp and cosf_recomp by the main recompilation because their
// names clash with the C library's, and the patch recompilation does not apply that rename. A
// patch calls them by their real names, as the decompilation does; the mapping to the renamed
// output lives on the native side, at the end of src/game/recomp_patch_decls.h, where only the
// generated patch code can see it. Renaming them HERE does not work: the recompiler resolves a
// patch's calls by name against the game's symbol table, which knows no sinf_recomp, and refuses
// the unresolved symbol outright. (Found by phase 39's matrix library patches, the first to take
// a sine in a patch.)

#include "patch_helpers.h"

#define RECOMP_PATCH       __attribute__((section(".recomp_patch")))
#define RECOMP_FORCE_PATCH __attribute__((section(".recomp_force_patch")))
#define RECOMP_EXPORT      __attribute__((section(".recomp_export")))

// Provided by the runtime, reached through the dummy addresses in syms.ld.
DECLARE_FUNC(void, recomp_puts, const char* str, u32 size);
DECLARE_FUNC(void, recomp_exit, void);

#endif // PATCHES_H
