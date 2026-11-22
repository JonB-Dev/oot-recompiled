#pragma once

// RESET (a player's suggestion, 2026-11-22: "a setting or button to reload back to the in-game menu
// opening as if the user reset their console"). The program starts itself again straight into the
// game, as the player found turning Resume on start off and starting it again does, and the old copy
// closes. A real start runs the game's boot from its first instruction with fresh memory, which is
// what a console's reset does; rewinding a running game's state in place would not be.
//
// The old copy closes the way Quit closes it, so with Save on quit on it keeps a moment first, as
// the window's close button does (the owner, 2026-11-22). The new copy waits for the old one to exit
// before it opens its log, skips the launcher (--play) and skips Resume on start for that one start.

namespace oot::reset {

    // Main thread: this copy starts itself again once it has shut down.
    void arm();
    bool armed();

    // The very end of main, after the window has gone: starts this executable again with
    // --reset-from and this process's id. False when nothing was armed or it would not start.
    bool relaunch_if_armed();

    // The very start of main, before the log is opened: when this start came from a reset, waits up
    // to fifteen seconds for the copy that started it to exit, so the two never hold the log at once.
    void wait_for_previous(int argc, char** argv);

    // The process id after --reset-from, or 0 when this start is not a reset's.
    unsigned long previous_id(int argc, char** argv);

    // The flag's reading, the command a reset starts, and the wait held on a real short-lived copy
    // of this program until it has exited. RESET_SELFTEST_OK or 1. The build runs it after every link.
    int selftest();

} // namespace oot::reset
