// Declarations the generated code needs and the generated header does not carry.
//
// Every recompiled file starts with whatever `recomp_include` says, which defaults to
// `#include "recomp.h"`. Our config points it here instead, and this includes that plus the four
// functions the generated `funcs.h` calls without declaring.
//
// Those four exist because the recompiler deliberately does not translate certain libultra
// functions, expecting librecomp to supply them natively. It still EMITS CALLS to them, and
// `funcs.h` only declares what was translated, so the calls compile as implicit declarations, which
// C99 and later reject.
//
// Two are implemented by librecomp (`librecomp/src/pi.cpp`) and two are implemented by us
// (`src/game/libultra_shims.cpp`), for the reason given in that file.

#ifndef OOT_RECOMP_EXTRA_DECLS_H
#define OOT_RECOMP_EXTRA_DECLS_H

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

// Implemented natively by librecomp, in librecomp/src/pi.cpp.
void __osPiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx);
void __osPiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx);

// Implemented natively by librecomp, in librecomp/src/ultra_translation.cpp. Both are no-ops there
// because there is no cache to write back or invalidate.
void osWritebackDCache_recomp(uint8_t* rdram, recomp_context* ctx);
void osInvalICache_recomp(uint8_t* rdram, recomp_context* ctx);

// Implemented by us, in src/game/libultra_shims.cpp.
//
// Both are 64DD entry points the retail build never reaches.
void osEPiWriteIo_recomp(uint8_t* rdram, recomp_context* ctx);
void osLeoDiskInit_recomp(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif // OOT_RECOMP_EXTRA_DECLS_H
