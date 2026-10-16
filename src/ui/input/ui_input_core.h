#pragma once

// ONE INPUT STREAM FOR THE INTERFACE (the menus plan, phase 90): the pure part, no SDL, no RmlUi,
// so it is tested headless (ui_tests). The SDL glue (ui_input.cpp) feeds it what every device
// holds, every pump; it hands back interface actions, repeating a held direction on the tokens'
// schedule, and it remembers which device spoke last, BY FAMILY, so the prompts can redraw the
// moment a different device is used (the user, 2026-10-12: "must detect in real time so if I
// switch mid menu navigation, it switched the UI to show the proper controls").

#include <cstdint>
#include <functional>
#include <map>

namespace oot::ui::input {

    // Who spoke last. The pad families are SDL's controller types grouped by what is printed on
    // the buttons: a DualShock 4 (and a DualShock 3) and a DualSense name some buttons
    // differently (Share and Options, Create and Options), so they are told apart.
    enum class Device { Keyboard, Mouse, Xbox, DualShock, DualSense, Nintendo, OtherPad };

    // What the interface hears. Directions repeat while held; the rest fire once per press.
    enum class Action { Up, Down, Left, Right, Accept, Back, Clear, Default, SectionPrev, SectionNext };

    // A device's held state as a bit per action (1 << Action).
    using Held = uint32_t;
    constexpr Held bit(Action a) { return Held{ 1 } << static_cast<int>(a); }
    constexpr bool is_direction(Action a) { return static_cast<int>(a) <= static_cast<int>(Action::Right); }

    // The repeat schedule: the first repeat after `delay`, then every `start`, shortening
    // steadily to `fastest` over `ramp` seconds. Seconds throughout.
    struct Schedule {
        double delay;
        double start;
        double fastest;
        double ramp = 1.0;
    };
    Schedule token_schedule();   // the tokens' (tokens.rcss, through the generated header)

    class Stream {
    public:
        using Sink = std::function<void(Action, Device)>;

        Stream(Schedule schedule, Sink sink) : schedule_(schedule), sink_(std::move(sink)) {}

        // A source's whole held state at time `now`. A source is one device instance: the
        // keyboard, or a pad by SDL's instance id. A newly held action fires at once; a held
        // direction repeats; anything newly held makes this source's device the last one.
        void update(int source, Device device, Held held, double now);

        // A source gone (a pad pulled out): forget what it held, so nothing repeats from it.
        void forget(int source);

        // The mouse moved or clicked: it becomes the last device, nothing fires.
        void note(Device device) { last_ = device; }

        Device last() const { return last_; }
        // The source that pressed something last (a pad's instance id, or the keyboard's), for the
        // Controls page, which opens on the very pad in use (2026-10-13).
        int last_source() const { return last_source_; }

    private:
        struct Repeat {
            double first_at = 0.0;   // when the direction was pressed
            double next_at = 0.0;    // when it fires again
        };
        struct Source {
            Held held = 0;
            std::map<int, Repeat> repeats;   // by Action, directions only
        };

        Schedule schedule_;
        Sink sink_;
        std::map<int, Source> sources_;
        Device last_ = Device::Keyboard;
        int last_source_ = 0;
    };

    // The headless tests of this file's logic (ui_tests). Prints a line per failure; the count of
    // checks passed through `passed`. Returns true when all pass.
    bool selftest(int& passed);

}
