// Macros shared by every patch.
//
// A patch file is compiled TWICE in spirit: once here as MIPS, where it is ordinary C that calls
// game functions directly, and once conceptually on the native side, where the recompiler has
// turned it into a function taking (rdram, ctx). DECLARE_FUNC exists so a declaration can be
// written once and mean the right thing in both worlds.
//
// The two compilation worlds do not mix, per CLAUDE.md. This header is part of the MIPS world.

#ifndef PATCH_HELPERS_H
#define PATCH_HELPERS_H

#ifdef MIPS
#include "ultra64.h"
#else
#include "recomp.h"
#endif

#ifdef __cplusplus
#   define EXTERNC extern "C"
#else
#   define EXTERNC
#endif

#ifdef MIPS
#    define DECLARE_FUNC(type, name, ...) \
        EXTERNC type name(__VA_ARGS__)
#else
#    define DECLARE_FUNC(type, name, ...) \
        EXTERNC void name(uint8_t* rdram, recomp_context* ctx)
#endif

#endif // PATCH_HELPERS_H
