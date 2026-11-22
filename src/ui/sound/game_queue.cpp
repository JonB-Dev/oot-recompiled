// The queue between the menus and the game's own menu sounds. See game_queue.h.

#include "ui/sound/game_queue.h"

#include <cstdio>

namespace oot::ui::sound::game {

    bool Queue::offer(int cue, double now) {
        if (!live(now) || cue < 0 || cue >= CUE_COUNT) {
            return false;
        }
        if (count_ == CAPACITY) {
            // Full: the oldest gives way, so a held key never builds a backlog.
            head_ = (head_ + 1) % CAPACITY;
            --count_;
        }
        entries_[(head_ + count_) % CAPACITY] = Entry{ cue, now };
        ++count_;
        return true;
    }

    int Queue::take(double now) {
        while (count_ > 0) {
            const Entry e = entries_[head_];
            head_ = (head_ + 1) % CAPACITY;
            --count_;
            if (now - e.at <= STALE_SECONDS && e.cue >= 0 && e.cue < CUE_COUNT) {
                return e.cue;
            }
        }
        return -1;
    }

    bool selftest(int& passed) {
        bool ok = true;
        const auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  game sounds: %s\n", what); ok = false; }
        };
        Queue launcher;
        check(!launcher.offer(static_cast<int>(Cue::Select), 1.0) && launcher.take(1.0) == -1,
              "with nothing draining (the launcher) a cue is ours, and the queue stays empty");

        Queue q;
        q.heard(10.0);
        check(q.offer(static_cast<int>(Cue::Move), 10.05), "while the game drains, it takes a cue");
        check(q.take(10.06) == static_cast<int>(Cue::Move) && q.take(10.06) == -1, "the cue comes out once, as itself");
        bool each = true;
        for (int i = 0; i < CUE_COUNT; ++i) {
            q.heard(20.0);
            each = each && q.offer(i, 20.0) && q.take(20.0) == i;
        }
        check(each, "each action maps to its own index, in order");
        q.heard(30.0);
        check(!q.offer(CUE_COUNT, 30.0) && !q.offer(-1, 30.0) && q.take(30.0) == -1, "an index out of range is dropped");
        q.offer(static_cast<int>(Cue::Back), 30.0);
        check(q.take(30.0 + Queue::STALE_SECONDS + 0.1) == -1, "a cue left too long is dropped, not heard late");
        check(!q.offer(static_cast<int>(Cue::Back), 30.0 + Queue::LIVE_SECONDS + 0.1),
              "when the draining stops (the title, the file select) the next cue is ours");
        q.heard(40.0);
        for (int i = 0; i < Queue::CAPACITY + 3; ++i) {
            q.offer(i % CUE_COUNT, 40.0);
        }
        int taken = 0;
        while (q.take(40.0) != -1) ++taken;
        check(taken == Queue::CAPACITY, "a full queue keeps the newest and never grows");
        return ok;
    }

}
