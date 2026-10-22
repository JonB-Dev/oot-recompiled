// The prompts. See prompts.h.

#include "ui/prompts/prompts.h"

#include <cstdio>

namespace oot::ui::prompts {

    using Device = oot::ui::input::Device;

    Family family_of(Device device) {
        switch (device) {
            case Device::Xbox:      return Family::Xbox;
            case Device::DualShock: return Family::DualShock;
            case Device::DualSense: return Family::DualSense;
            case Device::Nintendo:  return Family::Nintendo;
            case Device::OtherPad:  return Family::Xbox;      // a pad SDL maps as the Xbox layout
            case Device::Keyboard:
            case Device::Mouse:     return Family::Keyboard;
        }
        return Family::Keyboard;
    }

    Glyph glyph_for(Family family, Action action) {
        const auto face = [](const char* key, const char* shape, const char* tint) { return Glyph{ "face", key, shape, tint }; };
        const auto plate = [](const char* key) { return Glyph{ "plate", key, "", "" }; };
        // The pad prompts canvas's drawings (approved 2026-10-13): the d-pad in the medallion, and a
        // small system button whole, its shape over a stone carrying its word (assets/ui/prompts/).
        const auto medal = [](const char* shape) { return Glyph{ "medal", "", shape, "" }; };
        const auto art = [](const char* shape) { return Glyph{ "art", "", shape, "" }; };
        switch (family) {
            case Family::Keyboard:
                switch (action) {
                    case Action::Select:   return plate("Enter");
                    case Action::Back:     return plate("Esc");
                    case Action::Change:   return plate("\xE2\x86\x90 \xE2\x86\x92");   // the arrow keys
                    case Action::DialPrev: return plate("Q");
                    case Action::DialNext: return plate("E");
                    case Action::Close:    return plate("F1");
                    case Action::Type:     return plate("R");
                }
                break;
            case Family::Xbox:
                switch (action) {
                    case Action::Select:   return face("A", "", "xbox-a");
                    case Action::Back:     return face("B", "", "xbox-b");
                    case Action::Change:   return medal("dpad-left-right");
                    case Action::DialPrev: return plate("LB");
                    case Action::DialNext: return plate("RB");
                    case Action::Close:    return art("sys-menu");
                    case Action::Type:     return face("Y", "", "xbox-y");
                }
                break;
            case Family::DualShock:
            case Family::DualSense:
                switch (action) {
                    case Action::Select:   return face("", "cross", "ps-cross");
                    case Action::Back:     return face("", "circle", "ps-circle");
                    case Action::Change:   return medal("dpad-left-right");
                    case Action::DialPrev: return plate("L1");
                    case Action::DialNext: return plate("R1");
                    case Action::Close:    return art("sys-options");
                    case Action::Type:     return face("", "triangle", "ps-triangle");
                }
                break;
            case Family::Nintendo:
                switch (action) {
                    case Action::Select:   return face("A", "", "");
                    case Action::Back:     return face("B", "", "");
                    case Action::Change:   return medal("dpad-left-right");
                    case Action::DialPrev: return plate("L");
                    case Action::DialNext: return plate("R");
                    case Action::Close:    return art("sys-plus");
                    case Action::Type:     return face("X", "", "");
                }
                break;
        }
        return plate("?");
    }

    std::vector<Prompt> footer(Family family, const Here& here) {
        std::vector<Prompt> out;
        // The keyboard is up: its own words, and the shoulders move the caret (phase 110).
        if (here.typing) {
            out.push_back({ glyph_for(family, Action::Select), "Type", "accept" });
            out.push_back({ glyph_for(family, Action::Back), "Cancel", "back" });
            out.push_back({ glyph_for(family, Action::DialPrev), "", "dial-prev" });
            out.push_back({ glyph_for(family, Action::DialNext), "Move the caret", "dial-next" });
            return out;
        }
        // The dial's pair first, unless it is pinned at the dial's top corners (dial_pair, 2026-10-22),
        // which it is wherever there is a dial; the rail (the centered placement) keeps it here. A
        // footer too wide for a narrow panel takes a second line, and the start of a line is the one
        // place the two can never be split across it (phase 113).
        if (!here.dial_pinned) {
            out.push_back({ glyph_for(family, Action::DialPrev), "", "dial-prev" });
            // "Change setting" (the user, 2026-10-13: "Please rename turn the dial to instead be change setting").
            out.push_back({ glyph_for(family, Action::DialNext), "Change setting", "dial-next" });
        }
        out.push_back({ glyph_for(family, Action::Select), "Select", "accept" });
        if (here.change) {
            out.push_back({ glyph_for(family, Action::Change), "Change", "change" });
        }
        if (here.back) {
            out.push_back({ glyph_for(family, Action::Back), "Back", "back" });
        }
        if (here.type_path) {
            out.push_back({ glyph_for(family, Action::Type), "Type a path", "type" });
        }
        if (here.close) {
            out.push_back({ glyph_for(family, Action::Close), "Close", "close" });
        }
        return out;
    }

    std::vector<Prompt> dial_pair(Family family) {
        return { { glyph_for(family, Action::DialPrev), "", "dial-prev" },
                 { glyph_for(family, Action::DialNext), "", "dial-next" } };
    }

    bool selftest(int& passed) {
        bool ok = true;
        const auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  prompts: %s\n", what); ok = false; }
        };
        const Family families[] = { Family::Keyboard, Family::Xbox, Family::DualShock, Family::DualSense, Family::Nintendo };
        const Action actions[] = { Action::Select, Action::Back, Action::Change, Action::DialPrev, Action::DialNext, Action::Close, Action::Type };
        bool every = true;
        for (Family f : families) {
            for (Action a : actions) {
                const Glyph g = glyph_for(f, a);
                every = every && !g.kind.empty() && (!g.key.empty() || !g.shape.empty()) && g.key != "?";
            }
        }
        check(every, "every family has a glyph for every action");
        check(glyph_for(Family::DualSense, Action::Select).shape == "cross" &&
              glyph_for(Family::DualSense, Action::Back).shape == "circle", "PlayStation pads select with the cross, go back with the circle");
        check(glyph_for(Family::Xbox, Action::Select).tint == "xbox-a" && glyph_for(Family::Xbox, Action::Back).tint == "xbox-b",
              "Xbox's A and B carry their pastels");
        check(glyph_for(Family::Keyboard, Action::DialPrev).key == "Q" && glyph_for(Family::Keyboard, Action::DialNext).key == "E",
              "the keys turn the dial with Q and E");
        check(glyph_for(Family::Xbox, Action::Close).shape == "sys-menu" && glyph_for(Family::Nintendo, Action::Close).shape == "sys-plus",
              "each pad closes with its own small button, drawn");
        check(glyph_for(Family::DualSense, Action::Change).kind == "medal" && glyph_for(Family::DualSense, Action::Change).shape == "dpad-left-right",
              "a pad's Change is the d-pad, left and right lit");
        check(glyph_for(Family::Keyboard, Action::Close).kind == "plate", "the keyboard keeps its words on plates");

        // A run of inputs from one device and then another changes the family each time, so the
        // footer is redrawn in the new device's terms on the next input.
        const Device run[] = { Device::Xbox, Device::DualSense, Device::Keyboard, Device::DualShock, Device::Mouse };
        const Family want[] = { Family::Xbox, Family::DualSense, Family::Keyboard, Family::DualShock, Family::Keyboard };
        bool follows = true;
        std::string last;
        bool changed = true;
        for (int i = 0; i < 4; ++i) {
            const Family f = family_of(run[i]);
            follows = follows && f == want[i];
            const std::string first = footer(f, {}).front().glyph.key + footer(f, {}).front().glyph.shape;
            changed = changed && first != last;
            last = first;
        }
        follows = follows && family_of(run[4]) == want[4];
        check(follows, "each device's input gives its own family, the mouse the keyboard's");
        check(changed, "the footer's first prompt changes with each change of device");

        Here launcher_first;
        launcher_first.back = false;
        const std::vector<Prompt> top = footer(Family::Xbox, launcher_first);
        bool no_back = true;
        for (const Prompt& p : top) no_back = no_back && p.action != "back";
        check(no_back, "no Back prompt where Back does nothing");
        Here value;
        value.change = true;
        value.close = true;
        const std::vector<Prompt> on_value = footer(Family::DualShock, value);
        bool has_change = false;
        bool has_close = false;
        for (const Prompt& p : on_value) {
            has_change = has_change || p.action == "change";
            has_close = has_close || (p.action == "close" && p.glyph.kind == "art" && p.glyph.shape == "sys-options");
        }
        check(has_change && has_close, "a value row shows Change, and the open menu in the game shows its button closing it");

        // The dial's pair pinned at the dial's corners (2026-10-22): out of the footer, back then on,
        // and a plate on every family, which is all shell.rml draws there.
        Here pinned;
        pinned.dial_pinned = true;
        bool footer_without = true;
        bool plates = true;
        for (Family f : families) {
            for (const Prompt& p : footer(f, pinned)) {
                footer_without = footer_without && p.action != "dial-prev" && p.action != "dial-next";
            }
            const std::vector<Prompt> pair = dial_pair(f);
            plates = plates && pair.size() == 2 && pair[0].action == "dial-prev" && pair[1].action == "dial-next" &&
                     pair[0].glyph.kind == "plate" && pair[1].glyph.kind == "plate";
        }
        check(footer_without, "a pinned pair is not also in the footer");
        check(plates, "the pinned pair is back then on, a plate on every family");
        Here typing;
        typing.typing = true;
        typing.dial_pinned = true;
        bool caret = false;
        for (const Prompt& p : footer(Family::Xbox, typing)) {
            caret = caret || p.action == "dial-next";
        }
        check(caret, "typing, the shoulders move the caret in the footer whatever the pin");
        return ok;
    }

}
