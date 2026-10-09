// The game's random numbers, with a seed the harness can fix (phase 52).
//
// WHY. Play_Init seeds the game's generator from the clock (Rand_Seed((u32)osGetTime())) on every
// scene load, so two runs of the same scene spawn and animate its actors differently, and the A
// against B captures the lighting phases rest on (.scaffold/upgrades/lighting-design.md §9) could
// not be compared: two runs of one build differed by up to five percent of the pixels. With a
// fixed seed at every scene load and the same number of updates after arriving, they are the same
// picture.
//
// HOW. The generator is three tiny functions over one static word (libc64/qrand.c). The static is
// not reachable from a patch, so the three functions are replaced together over a word of their
// own, with the same multiplier, increment and 23 bit sampling the original has, so with no seed
// asked for the game's numbers are exactly the game's numbers. Rand_Seed asks the native side for
// a fixed seed first and uses the clock's only when there is none, which is the case in play: the
// flag exists for the harness and nothing else sets it. The _Variable family keeps its own state
// through a pointer and is left alone.
//
// The original samples the LOW 23 bits into the float, which shortens the period; that quirk is
// kept on purpose, because a pattern of numbers the game was tuned against is part of the game.

#include "patches.h"

#define HARNESS_RAND_MULTIPLIER 1664525
#define HARNESS_RAND_INCREMENT 1013904223

// The seed the harness asked for with --seed, or 0 for none. One call per scene load.
u32 recomp_take_seed(void);

static u32 sHarnessRandInt = 1;

typedef union {
    u32 i;
    f32 f;
} HarnessFloatInt;

RECOMP_PATCH u32 Rand_Next(void) {
    u32 next = sHarnessRandInt * HARNESS_RAND_MULTIPLIER + HARNESS_RAND_INCREMENT;

    sHarnessRandInt = next;
    return next;
}

RECOMP_PATCH void Rand_Seed(u32 seed) {
    u32 forced = recomp_take_seed();

    sHarnessRandInt = (forced != 0) ? forced : seed;
}

RECOMP_PATCH f32 Rand_ZeroOne(void) {
    HarnessFloatInt v;
    f32 vf;

    v.i = (Rand_Next() & 0x007FFFFF) | 0x3F800000;
    vf = v.f - 1.0f;
    return vf;
}
