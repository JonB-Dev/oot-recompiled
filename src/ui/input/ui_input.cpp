// The interface's input stream, the SDL side. See ui_input.h.

#include "ui/input/ui_input.h"

#include "main/input.h"
#include "ui/ui_shell.h"

#include <atomic>
#include <set>

namespace oot::ui::input {

    namespace {

        constexpr int KEYBOARD_SOURCE = -1000;

        Held g_keys = 0;
        std::atomic<int> g_last{ static_cast<int>(Device::Keyboard) };
        std::atomic<int> g_last_source{ -1 };   // the source that pressed last (last_pad_instance)
        std::set<int> g_pads_seen;

        void deliver(Action action) {
            using Nav = oot::ui::shell::Nav;
            switch (action) {
                case Action::Up:          oot::ui::shell::post_nav(Nav::Up); break;
                case Action::Down:        oot::ui::shell::post_nav(Nav::Down); break;
                case Action::Left:        oot::ui::shell::post_nav(Nav::Left); break;
                case Action::Right:       oot::ui::shell::post_nav(Nav::Right); break;
                case Action::Accept:      oot::ui::shell::post_nav(Nav::Accept); break;
                case Action::Back:        oot::ui::shell::post_nav(Nav::Back); break;
                case Action::Clear:       oot::ui::shell::post_nav(Nav::Clear); break;
                case Action::Default:     oot::ui::shell::post_nav(Nav::Default); break;
                case Action::SectionPrev: oot::ui::shell::post_nav(Nav::SectionPrev); break;
                case Action::SectionNext: oot::ui::shell::post_nav(Nav::SectionNext); break;
            }
        }

        Stream& stream() {
            static Stream s(token_schedule(), [](Action action, Device) { deliver(action); });
            return s;
        }

        Held key_action(SDL_Scancode code) {
            switch (code) {
                case SDL_SCANCODE_UP:
                case SDL_SCANCODE_W:        return bit(Action::Up);
                case SDL_SCANCODE_DOWN:
                case SDL_SCANCODE_S:        return bit(Action::Down);
                case SDL_SCANCODE_LEFT:
                case SDL_SCANCODE_A:        return bit(Action::Left);
                case SDL_SCANCODE_RIGHT:
                case SDL_SCANCODE_D:        return bit(Action::Right);
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                case SDL_SCANCODE_X:        return bit(Action::Accept);
                case SDL_SCANCODE_ESCAPE:
                case SDL_SCANCODE_C:        return bit(Action::Back);
                case SDL_SCANCODE_DELETE:
                case SDL_SCANCODE_BACKSPACE: return bit(Action::Clear);
                case SDL_SCANCODE_R:        return bit(Action::Default);
                case SDL_SCANCODE_Q:
                case SDL_SCANCODE_PAGEUP:   return bit(Action::SectionPrev);
                case SDL_SCANCODE_E:
                case SDL_SCANCODE_PAGEDOWN: return bit(Action::SectionNext);
                default:                    return 0;
            }
        }

        Device device_of(oot::input::devices::PadKind kind) {
            using K = oot::input::devices::PadKind;
            switch (kind) {
                case K::Xbox:      return Device::Xbox;
                case K::DualShock: return Device::DualShock;
                case K::DualSense: return Device::DualSense;
                case K::Nintendo:  return Device::Nintendo;
                case K::Other:     return Device::OtherPad;
            }
            return Device::OtherPad;
        }

    }

    void key(SDL_Scancode code, bool down) {
        const Held b = key_action(code);
        g_keys = down ? (g_keys | b) : (g_keys & ~b);
        if (down) {
            // Any key at all is the keyboard speaking, a game key included: the prompts follow
            // the hands, not only the menu's own keys.
            g_last.store(static_cast<int>(Device::Keyboard), std::memory_order_relaxed);
            stream().note(Device::Keyboard);
        }
    }

    void release_keys() {
        g_keys = 0;
    }

    void mouse() {
        stream().note(Device::Mouse);
        g_last.store(static_cast<int>(Device::Mouse), std::memory_order_relaxed);
    }

    void pump() {
        const double now = static_cast<double>(SDL_GetTicks64()) / 1000.0;
        Stream& s = stream();
        s.update(KEYBOARD_SOURCE, Device::Keyboard, g_keys, now);

        std::set<int> seen;
        const int count = oot::input::devices::pad_count();
        for (int slot = 0; slot < count; ++slot) {
            oot::input::devices::UiPad pad;
            if (!oot::input::devices::ui_pad(slot, pad)) {
                continue;
            }
            seen.insert(pad.instance);
            s.update(pad.instance, device_of(pad.kind), pad.held, now);
        }
        for (int gone : g_pads_seen) {
            if (seen.count(gone) == 0) {
                s.forget(gone);
            }
        }
        g_pads_seen = std::move(seen);
        g_last.store(static_cast<int>(s.last()), std::memory_order_relaxed);
        g_last_source.store(s.last_source(), std::memory_order_relaxed);
    }

    Device last_device() {
        return static_cast<Device>(g_last.load(std::memory_order_relaxed));
    }

    int last_pad_instance() {
        const Device d = last_device();
        if (d == Device::Keyboard || d == Device::Mouse) {
            return -1;
        }
        const int source = g_last_source.load(std::memory_order_relaxed);
        return source == KEYBOARD_SOURCE ? -1 : source;
    }

}
