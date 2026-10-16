#pragma once

// THE GAME'S OWN MENU SOUNDS (the menus plan, phase 112): in the game, the menus ask the game to
// play its own system sounds rather than ours. This is the queue between them, and nothing else:
// the shell puts a cue in, and the patch (patches/menu_sounds.c) takes a few out each frame on the
// game thread and plays the game's sound for it from its own compiled table, in Cue's order. We
// never read, decode or store anything of the game's; the cue is an index.
//
// THE GAME TAKES A CUE ONLY WHILE IT IS TAKING THEM. The patch runs in the play state's frame, so
// on the title and the file select, in the launcher, or while nothing drains for any reason, the
// queue is not live and our own set plays instead. A cue left waiting too long is dropped rather
// than heard late, and an index out of range is dropped rather than played.
//
// Pure, with the clock passed in, so ui_tests checks it; the program's instance is below.

#include "ui/sound/ui_sound.h"

#include <cstdint>

namespace oot::ui::sound::game {

    class Queue {
    public:
        static constexpr int CAPACITY = 8;
        // How long after the last drain the game still counts as taking cues, and how long a cue
        // may wait before it is too late to be the sound of the press that made it.
        static constexpr double LIVE_SECONDS = 0.25;
        static constexpr double STALE_SECONDS = 0.25;

        // The game's side drained at `now`.
        void heard(double now) { last_drain_ = now; drained_ = true; }
        bool live(double now) const { return drained_ && now - last_drain_ <= LIVE_SECONDS; }
        // A cue offered at `now`: true when the game will play it (it was live and there was
        // room), false when ours should.
        bool offer(int cue, double now);
        // The next cue for the game, or -1: stale and out of range cues are dropped on the way.
        int take(double now);

    private:
        struct Entry {
            int cue;
            double at;
        };
        Entry entries_[CAPACITY] = {};
        int head_ = 0;
        int count_ = 0;
        double last_drain_ = 0.0;
        bool drained_ = false;
    };

    // The program's queue (thread safe): offered from the menus, taken by the export the patch
    // calls. take_for_game() also marks the drain, and returns the cue in the low byte and the
    // volume in thousandths above it, or -1 when there is none.
    bool offer(Cue cue);
    int32_t take_for_game();

    bool selftest(int& passed);

}
