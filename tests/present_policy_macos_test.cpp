// Regression: native-rate traced workloads must not be rendered and then discarded.
#include "hle/rt64_present_policy.h"
#include <cassert>
#include <initializer_list>

int main() {
    // A slow renderer can have a newer present queued behind each completed workload.
    // Preserve every completed original-rate tracing frame, including a sustained backlog.
    unsigned presented = 0;
    for (unsigned frame = 0; frame < 120; ++frame) {
        if (!RT64::skipQueuedPresent(frame != 119, true, true)) ++presented;
    }
    assert(presented == 120);
    // Raster and interpolation retain the existing latest-frame policy.
    assert(RT64::skipQueuedPresent(true, false, true));
    assert(RT64::skipQueuedPresent(true, true, false));
    assert(RT64::skipQueuedPresent(true, false, false));
    for (bool tracing : {false, true}) {
        for (bool original : {false, true}) {
            assert(!RT64::skipQueuedPresent(false, tracing, original));
        }
    }
}
