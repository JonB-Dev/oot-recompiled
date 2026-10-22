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

    bool Echoes::echo(int what, int source, double now) {
        const auto it = last_.find(what);
        if (it != last_.end() && it->second.source != source && (now - it->second.at) < ECHO_WINDOW) {
            return true;
        }
        last_[what] = Last{ source, now };
        return false;
    }

    void Stream::update(int source, Device device, Held held, double now) {
        Source& s = sources_[source];
        const Held pressed = held & ~s.held;
        bool spoke = false;
        for (int a = 0; a <= static_cast<int>(Action::SectionNext); ++a) {
            const Action action = static_cast<Action>(a);
            const Held b = bit(action);
            if (pressed & b) {
                // Another source's press of the same thing a moment ago: this is its echo, so it
                // neither fires nor repeats, nor makes its device the last one (a Steam pad echoing
                // a DualSense would otherwise flip the prompts to its own), until it is let go.
                if (echoes_.echo(a, source, now)) {
                    s.echoed |= b;
                    continue;
                }
                spoke = true;
                sink_(action, device);
                if (is_direction(action)) {
                    s.repeats[a] = Repeat{ now, now + schedule_.delay };
                }
            }
            else if (s.echoed & b) {
                if (!(held & b)) {
                    s.echoed &= ~b;
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
        if (spoke) {
            last_ = device;
            last_source_ = source;
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

        // Two sources press the same thing at once: one press, the second is its echo (2026-10-21:
        // one controller arriving twice, DS4Windows or Steam's virtual pad, doubled every press;
        // until then this case said both fire). The echo does not repeat on its own while held.
        fired.clear();
        Stream two(sch, [&](Action a, Device d) { fired.push_back({ a, d, clock }); });
        clock = 0.0;
        two.update(1, Device::Keyboard, bit(Action::Up), clock);
        two.update(2, Device::Xbox, bit(Action::Up), clock);
        check(fired.size() == 1 && fired[0].device == Device::Keyboard, "two devices pressing at once fire once");
        check(two.last() == Device::Keyboard, "an echo does not make its device the last one");
        two.update(1, Device::Keyboard, 0, 0.02);
        for (clock = 0.02; clock < 0.9; clock += 0.01) two.update(2, Device::Xbox, bit(Action::Up), clock);
        check(fired.size() == 1, "an echo held on its own never repeats");
        clock = 1.0;
        two.update(2, Device::Xbox, 0, clock);
        two.update(2, Device::Xbox, bit(Action::Accept), clock);
        check(two.last_source() == 2 && two.last() == Device::Xbox, "the pad that pressed last is the source in use");
        clock = 1.2;   // past the echo window, or the keyboard's Accept would be the pad's echo
        two.update(1, Device::Keyboard, 0, clock);
        two.update(1, Device::Keyboard, bit(Action::Accept), clock);
        check(two.last_source() == 1, "then the keyboard, once it presses");

        // The echo window: a twin's press a few milliseconds behind is dropped, another person's
        // press later is not, and a quick double press on one pad counts twice.
        fired.clear();
        Stream twins(sch, [&](Action a, Device d) { fired.push_back({ a, d, clock }); });
        clock = 5.0;
        twins.update(10, Device::DualSense, bit(Action::Accept), clock);
        twins.update(11, Device::Xbox, bit(Action::Accept), clock + 0.004);
        check(fired.size() == 1, "a twin pad's press 4 ms behind is dropped");
        twins.update(10, Device::DualSense, 0, 5.1);
        twins.update(11, Device::Xbox, 0, 5.1);
        twins.update(11, Device::Xbox, bit(Action::Accept), 5.3);
        check(fired.size() == 2 && fired[1].device == Device::Xbox, "another pad pressing later fires");
        twins.update(11, Device::Xbox, 0, 5.32);
        twins.update(11, Device::Xbox, bit(Action::Accept), 5.34);
        check(fired.size() == 3, "a quick double press on one pad counts twice");

        Echoes echoes;
        check(!echoes.echo(3, 0, 1.0), "the first press is not an echo");
        check(echoes.echo(3, 1, 1.0 + ECHO_WINDOW * 0.5), "another source inside the window is an echo");
        check(!echoes.echo(3, 1, 1.0 + ECHO_WINDOW * 2.0), "another source after the window is a press");
        check(!echoes.echo(4, 0, 1.0 + ECHO_WINDOW * 2.0), "a different thing is never an echo");

        return ok;
    }

}
