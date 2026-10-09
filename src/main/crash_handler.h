// Crash diagnostics, installed before anything that can crash.

#pragma once

namespace oot {
    namespace crash {
        // Installs every handler: the unhandled exception filter for hardware exceptions, and
        // the CRT's own endings the filter never sees (an invalid parameter to a CRT function,
        // an uncaught C++ exception, abort, a pure virtual call). Call once, first thing in main,
        // after the log exists so the reports have somewhere to go.
        void install();

        // Makes one ending happen on purpose, so a check can prove its report reaches the log:
        // "invalid-parameter" (reported, then the program goes on and this returns true),
        // "terminate", "abort", "purecall", "access-violation" (each ends the program).
        // Returns false for a name it does not know.
        bool self_test(const char* kind);
    }
}
