// The interface's input stream, the pure part. See ui_input_core.h.

#include "ui_input_core.h"

#include "ui_tokens.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace oot::ui::input {

    Schedule token_schedule() {
        return Schedule{ oot::ui::tokens::repeat_delay, oot::ui::tokens::repeat_start, oot::ui::tokens::repeat_fastest, 1.0 };
    }

    void Stream::update(int source, Device device, Held held, double now) {
        Source& s = sources_[source];
        const Held pressed = held & ~s.held;
        if (pressed != 0) {
            last_ = device;
            last_source_ = source;
        }
        for (int a = 0; a <= static_cast<int>(Action::SectionNext); ++a) {
            const Action action = static_cast<Action>(a);
            const Held b = bit(action);
            if (pressed & b) {
                sink_(action, device);
                if (is_direction(action)) {
                    s.repeats[a] = Repeat{ now, now + schedule_.delay };
                }
            }
            else if ((held & b) && is_direction(action)) {
                Repeat& r = s.repeats[a];
                // At most one repeat per update: a long gap between pumps (a stall) must not
                // dump a burst of moves that sends the focus flying past where the person meant.
                if (now >= r.next_at) {
                    sink_(action, device);
                    const double since = now - (r.first_at + schedule_.delay);
                    const double k = std::clamp(since / schedule_.ramp, 0.0, 1.0);
                    const double interval = schedule_.start + (schedule_.fastest - schedule_.start) * k;
                    r.next_at = std::max(r.next_at + interval, now + schedule_.fastest);
                }
            }
            else {
                s.repeats.erase(a);
            }
        }
        s.held = held;
    }

    void Stream::forget(int source) {
        sources_.erase(source);
    }

    bool selftest(int& passed) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  input: %s\n", what); ok = false; }
        };

        struct Fired { Action action; Device device; double at; };
        std::vector<Fired> fired;
        double clock = 0.0;
        const Schedule sch{ 0.38, 0.11, 0.045, 1.0 };
        Stream stream(sch, [&](Action a, Device d) { fired.push_back({ a, d, clock }); });

        // A press fires once, at once.
        clock = 0.0;
        stream.update(0, Device::Keyboard, bit(Action::Down), clock);
        check(fired.size() == 1 && fired[0].action == Action::Down, "a press fires at once");

        // Held: nothing before the delay, then the first repeat at the delay.
        for (clock = 0.01; clock < 0.379; clock += 0.01) stream.update(0, Device::Keyboard, bit(Action::Down), clock);
        check(fired.size() == 1, "nothing repeats before the delay");
        clock = 0.38;
        stream.update(0, Device::Keyboard, bit(Action::Down), clock);
        check(fired.size() == 2, "the first repeat comes at the delay");

        // Then the intervals shorten from 0.11 toward 0.045 over a second.
        for (clock = 0.385; clock <= 2.5; clock += 0.005) stream.update(0, Device::Keyboard, bit(Action::Down), clock);
        std::vector<double> gaps;
        for (size_t i = 2; i < fired.size(); ++i) gaps.push_back(fired[i].at - fired[i - 1].at);
        check(!gaps.empty() && gaps.front() >= 0.105 && gaps.front() <= 0.12, "the second repeat follows by about 0.11 s");
        check(!gaps.empty() && gaps.back() >= 0.04 && gaps.back() <= 0.055, "repeats settle at about 0.045 s");
        bool shortening = true;
        for (size_t i = 1; i < gaps.size(); ++i) if (gaps[i] > gaps[i - 1] + 0.006) shortening = false;
        check(shortening, "the intervals never lengthen while held");

        // Released: no more.
        const size_t before = fired.size();
        clock = 2.6;
        stream.update(0, Device::Keyboard, 0, clock);
        for (clock = 2.61; clock < 3.5; clock += 0.01) stream.update(0, Device::Keyboard, 0, clock);
        check(fired.size() == before, "a release stops the repeat");

        // Accept fires once and never repeats.
        fired.clear();
        for (clock = 4.0; clock < 6.0; clock += 0.01) stream.update(0, Device::Keyboard, bit(Action::Accept), clock);
        check(fired.size() == 1, "accept fires once however long it is held");

        // A stall between pumps gives one repeat, not a burst.
        fired.clear();
        stream.update(0, Device::Keyboard, 0, 6.0);
        stream.update(0, Device::Keyboard, bit(Action::Left), 6.0);
        stream.update(0, Device::Keyboard, bit(Action::Left), 9.0);
        check(fired.size() == 2, "a stall gives one repeat, not a burst");

        // The last device follows whoever pressed last, and a second pad counts as its own source.
        stream.update(0, Device::Keyboard, 0, 10.0);
        stream.update(100, Device::Xbox, bit(Action::Down), 10.1);
        check(stream.last() == Device::Xbox, "an Xbox press makes Xbox the last device");
        stream.update(200, Device::DualSense, bit(Action::Accept), 10.2);
        check(stream.last() == Device::DualSense, "a DualSense press makes DualSense the last device");
        stream.update(0, Device::Keyboard, bit(Action::Back), 10.3);
        check(stream.last() == Device::Keyboard, "a key makes the keyboard the last device");
        stream.update(300, Device::DualShock, bit(Action::SectionNext), 10.4);
        check(stream.last() == Device::DualShock, "a DualShock press makes DualShock the last device");
        stream.note(Device::Mouse);
        check(stream.last() == Device::Mouse, "the mouse becomes the last device when it moves");

        // Merely holding (no new press) does not steal the last device back.
        stream.update(100, Device::Xbox, bit(Action::Down), 10.5);
        check(stream.last() == Device::Mouse, "a button still held from before does not take over");

        // A pad pulled out stops repeating.
        fired.clear();
        stream.forget(100);
        stream.update(100, Device::Xbox, 0, 11.0);
        check(fired.empty(), "a forgotten pad fires nothing");

        // Two sources hold the same direction: each repeats on its own schedule.
        fired.clear();
        Stream two(sch, [&](Action a, Device d) { fired.push_back({ a, d, clock }); });
        clock = 0.0;
        two.update(1, Device::Keyboard, bit(Action::Up), clock);
        two.update(2, Device::Xbox, bit(Action::Up), clock);
        check(fired.size() == 2, "two devices pressing at once both fire");
        clock = 1.0;
        two.update(2, Device::Xbox, 0, clock);
        two.update(2, Device::Xbox, bit(Action::Accept), clock);
        check(two.last_source() == 2 && two.last() == Device::Xbox, "the pad that pressed last is the source in use");
        two.update(1, Device::Keyboard, 0, clock);
        two.update(1, Device::Keyboard, bit(Action::Accept), clock);
        check(two.last_source() == 1, "then the keyboard, once it presses");

        return ok;
    }

}
