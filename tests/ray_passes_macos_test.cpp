#include "render/rt64_raytracing_passes.h"
#include <cstdio>
#include <initializer_list>

int main() {
    for (unsigned level : {0u, 1u, 2u}) {
        if (RT64::bouncePassEnabled(level, 1.0f) || RT64::scatterPassEnabled(level)) return 1;
    }
    if (RT64::bouncePassEnabled(3, 0.0f) || RT64::bouncePassEnabled(3, -1.0f)) return 1;
    if (!RT64::bouncePassEnabled(3, 0.25f) || !RT64::scatterPassEnabled(3)) return 1;
    // Returning from full lighting to level 2 must immediately disable optional work.
    if (RT64::bouncePassEnabled(2, 0.25f) || RT64::scatterPassEnabled(2)) return 1;
    std::puts("INACTIVE_RAY_EFFECTS_SKIPPED_OK");
}
