// Photo mode's button guide. See photo_guide.h.

#include "ui/prompts/photo_guide.h"

#include <algorithm>
#include <cstdio>

namespace oot::ui::prompts::photo_guide {

    using oot::input::Field;
    using oot::input::FieldKind;
    using oot::input::GameInput;

    namespace {

        // The arrows, as the footer already draws them (prompts.cpp, the keyboard's Change).
        constexpr const char* ARROW_UP = "\xE2\x86\x91";
        constexpr const char* ARROW_DOWN = "\xE2\x86\x93";
        constexpr const char* ARROW_LEFT = "\xE2\x86\x90";
        constexpr const char* ARROW_RIGHT = "\xE2\x86\x92";

        Glyph plate(const std::string& word) { return Glyph{ "plate", word, "", "" }; }
        Glyph face(const char* key, const char* shape, const char* tint) { return Glyph{ "face", key, shape, tint }; }
        // The pad prompts canvas's drawings (approved 2026-10-13, assets/ui/prompts/): the d-pad and the
        // sticks in the medallion, and the small system buttons whole, each over a stone with its word.
        Glyph medal(const std::string& shape) { return Glyph{ "medal", "", shape, "" }; }
        Glyph art(const std::string& name) { return Glyph{ "art", "", "sys-" + name, "" }; }

        bool playstation(Family f) { return f == Family::DualShock || f == Family::DualSense; }

        const char* pick(Family f, const char* xbox, const char* ps, const char* nintendo) {
            return playstation(f) ? ps : (f == Family::Nintendo ? nintendo : xbox);
        }

        // A pad button in the family's own terms, by its position in SDL's standard layout.
        Glyph button_glyph(Family f, int button) {
            switch (button) {
                case pad::A:
                    return playstation(f) ? face("", "cross", "ps-cross") : f == Family::Nintendo ? face("B", "", "") : face("A", "", "xbox-a");
                case pad::B:
                    return playstation(f) ? face("", "circle", "ps-circle") : f == Family::Nintendo ? face("A", "", "") : face("B", "", "xbox-b");
                case pad::X:
                    return playstation(f) ? face("", "square", "ps-square") : f == Family::Nintendo ? face("Y", "", "") : face("X", "", "xbox-x");
                case pad::Y:
                    return playstation(f) ? face("", "triangle", "ps-triangle") : f == Family::Nintendo ? face("X", "", "") : face("Y", "", "xbox-y");
                case pad::Back:
                    return art(f == Family::DualSense ? "create" : pick(f, "view", "share", "minus"));
                case pad::Guide:         return art(pick(f, "guide", "ps", "home"));
                case pad::Start:         return art(pick(f, "menu", "options", "plus"));
                // A stick's press: the cap pressed into its well, apart from the stick moving.
                case pad::LeftStick:     return medal("stick-l-press");
                case pad::RightStick:    return medal("stick-r-press");
                case pad::LeftShoulder:  return plate(pick(f, "LB", "L1", "L"));
                case pad::RightShoulder: return plate(pick(f, "RB", "R1", "R"));
                case pad::DUp:           return medal("dpad-up");
                case pad::DDown:         return medal("dpad-down");
                case pad::DLeft:         return medal("dpad-left");
                case pad::DRight:        return medal("dpad-right");
                case pad::Misc1:         return art(pick(f, "share-bar", "mute", "capture"));
                case pad::Paddle1: case pad::Paddle2: case pad::Paddle3: case pad::Paddle4:
                    return plate("P" + std::to_string(button - pad::Paddle1 + 1));
                case pad::Touchpad:
                    return playstation(f) ? Glyph{ "pad", "", "", "" } : plate("Touchpad");
                default:
                    return plate("Btn " + std::to_string(button));
            }
        }

        const char* direction_name(int axis, bool positive) {
            const bool across = axis == pad::LeftX || axis == pad::RightX;
            // SDL's y grows downward: the negative direction is up.
            return across ? (positive ? "right" : "left") : (positive ? "down" : "up");
        }

        bool on_stick(const Field& field, int x_axis, int y_axis) {
            return (field.kind == FieldKind::ControllerAxisPositive || field.kind == FieldKind::ControllerAxisNegative) &&
                   (field.index == x_axis || field.index == y_axis);
        }

        bool is_dpad(const Field& field) {
            return field.kind == FieldKind::ControllerButton && field.index >= pad::DUp && field.index <= pad::DRight;
        }

        // The d-pad's drawing with these arms lit, named in the drawings' order: up, down, left, right.
        Glyph dpad_lit(const std::vector<Field>& fields) {
            std::string name = "dpad";
            for (int button : { static_cast<int>(pad::DUp), static_cast<int>(pad::DDown), static_cast<int>(pad::DLeft),
                                static_cast<int>(pad::DRight) }) {
                for (const Field& field : fields) {
                    if (field.index == button) {
                        name += button == pad::DUp ? "-up" : button == pad::DDown ? "-down" : button == pad::DLeft ? "-left" : "-right";
                        break;
                    }
                }
            }
            return medal(name);
        }

        Glyph field_glyph(Family f, const Field& field, const KeyName& key_name) {
            switch (field.kind) {
                case FieldKind::Key:
                    return plate(short_key(key_name(field.index)));
                case FieldKind::ControllerButton:
                    return button_glyph(f, field.index);
                case FieldKind::ControllerAxisPositive:
                case FieldKind::ControllerAxisNegative: {
                    const bool positive = field.kind == FieldKind::ControllerAxisPositive;
                    switch (field.index) {
                        case pad::LeftX: case pad::LeftY:
                            return medal(std::string("stick-l-") + direction_name(field.index, positive));
                        case pad::RightX: case pad::RightY:
                            return medal(std::string("stick-r-") + direction_name(field.index, positive));
                        case pad::TriggerLeft:  return plate(pick(f, "LT", "L2", "ZL"));
                        case pad::TriggerRight: return plate(pick(f, "RT", "R2", "ZR"));
                        default: return plate("Axis " + std::to_string(field.index));
                    }
                }
                case FieldKind::JoystickButton:       return plate("Btn " + std::to_string(field.index));
                case FieldKind::JoystickAxisPositive:
                case FieldKind::JoystickAxisNegative: return plate("Axis " + std::to_string(field.index));
                case FieldKind::JoystickHat:          return plate("Hat " + std::to_string(field.index));
                default:                              return plate("?");
            }
        }

        // The glyphs for a group of inputs, in the group's order: one plate for a stick's or the
        // d-pad's directions or for keys alone, each its own glyph otherwise.
        std::vector<Glyph> group_glyphs(Family f, const std::vector<Field>& fields, const KeyName& key_name) {
            std::vector<Glyph> out;
            if (fields.empty()) {
                return out;
            }
            const auto all = [&fields](auto test) {
                for (const Field& field : fields) {
                    if (!test(field)) return false;
                }
                return true;
            };
            if (fields.size() > 1) {
                if (all([](const Field& x) { return on_stick(x, pad::LeftX, pad::LeftY); })) {
                    return { medal("stick-l") };
                }
                if (all([](const Field& x) { return on_stick(x, pad::RightX, pad::RightY); })) {
                    return { medal("stick-r") };
                }
                if (all(is_dpad)) {
                    return { dpad_lit(fields) };
                }
                if (all([](const Field& x) { return x.kind == FieldKind::Key; })) {
                    std::string word;
                    for (const Field& field : fields) {
                        word += (word.empty() ? "" : " ") + short_key(key_name(field.index));
                    }
                    return { plate(word) };
                }
            }
            for (const Field& field : fields) {
                out.push_back(field_glyph(f, field, key_name));
            }
            return out;
        }

    }

    std::string short_key(const std::string& name) {
        static const std::pair<const char*, const char*> SHORT[] = {
            { "Up", ARROW_UP }, { "Down", ARROW_DOWN }, { "Left", ARROW_LEFT }, { "Right", ARROW_RIGHT },
            { "Left Shift", "Shift" }, { "Right Shift", "R Shift" }, { "Left Ctrl", "Ctrl" }, { "Right Ctrl", "R Ctrl" },
            { "Left Alt", "Alt" }, { "Right Alt", "R Alt" }, { "Left GUI", "Win" }, { "Right GUI", "R Win" },
            { "Return", "Enter" }, { "Escape", "Esc" }, { "PageUp", "PgUp" }, { "PageDown", "PgDn" },
        };
        for (const auto& [from, to] : SHORT) {
            if (name == from) return to;
        }
        if (name.rfind("Keypad ", 0) == 0) {
            return "Num " + name.substr(7);
        }
        return name;
    }

    std::vector<Field> working(const Lookup& bindings, GameInput input) {
        std::vector<Field> out;
        for (const Field& field : bindings(input)) {
            if (field.kind == FieldKind::None || std::find(out.begin(), out.end(), field) != out.end()) {
                continue;
            }
            // Taken first by a control acted on before this one, read where this one is.
            bool taken = false;
            for (GameInput earlier : oot::input::DISPATCH_ORDER) {
                if (earlier == input) {
                    break;
                }
                if ((oot::input::read_in(earlier) & oot::input::read_in(input)) == 0) {
                    continue;
                }
                for (const Field& f : bindings(earlier)) {
                    taken = taken || f == field;
                }
            }
            if (!taken) {
                out.push_back(field);
            }
        }
        return out;
    }

    std::vector<Glyph> working_glyphs(Family family, const Lookup& bindings, GameInput input, const KeyName& key_name) {
        std::vector<Glyph> out;
        for (const Field& field : working(bindings, input)) {
            out.push_back(field_glyph(family, field, key_name));
        }
        return out;
    }

    std::vector<Prompt> rows(Family family, const Lookup& bindings, const KeyName& key_name, const Here& here) {
        struct Item {
            std::vector<GameInput> inputs;
            std::string label;
            const char* action;
        };
        const std::vector<Item> items = {
            { { GameInput::PhotoForward, GameInput::PhotoLeft, GameInput::PhotoBack, GameInput::PhotoRight }, "Move", "photo-move" },
            { { GameInput::PhotoTurnUp, GameInput::PhotoTurnLeft, GameInput::PhotoTurnDown, GameInput::PhotoTurnRight }, "Turn", "photo-turn" },
            { { GameInput::PhotoLower, GameInput::PhotoRaise }, "Lower, raise", "photo-height" },
            { { GameInput::PhotoCloser, GameInput::PhotoFurther }, "Closer, further", "photo-distance" },
            { { GameInput::PhotoReset }, "Put the camera back", "photo-reset" },
            // What the next press does: shown, dimmed, hidden, shown (photo.h).
            { { GameInput::PhotoHud }, here.hud_hidden ? "Show the HUD" : here.hud_dimmed ? "Hide the HUD" : "Dim the HUD", "photo-hud" },
            // Photo mode's own picture control, or the Screenshot control when that is unbound here.
            { { GameInput::PhotoShot }, "Take a picture", "photo-shot" },
            { { GameInput::PhotoGuide }, "Hide this guide", "photo-guide" },
            { { GameInput::PhotoMode }, "Leave photo mode", "photo-leave" },
        };
        std::vector<Prompt> out;
        for (const Item& item : items) {
            // A control on its own shows every binding that reaches it, each its own glyph; a group
            // (a stick's or the d-pad's directions, a pair) the first that reaches each of its own.
            std::vector<Glyph> glyphs;
            if (item.inputs.size() == 1) {
                glyphs = working_glyphs(family, bindings, item.inputs[0], key_name);
                if (glyphs.empty() && item.inputs[0] == GameInput::PhotoShot) {
                    glyphs = working_glyphs(family, bindings, GameInput::Screenshot, key_name);
                }
            }
            else {
                std::vector<Field> fields;
                for (GameInput input : item.inputs) {
                    const std::vector<Field> reach = working(bindings, input);
                    if (!reach.empty()) {
                        fields.push_back(reach.front());
                    }
                }
                glyphs = group_glyphs(family, fields, key_name);
            }
            for (size_t i = 0; i < glyphs.size(); ++i) {
                const bool last = i + 1 == glyphs.size();
                out.push_back({ glyphs[i], last ? item.label : "", item.action });
            }
        }
        return out;
    }

    bool selftest(int& passed) {
        bool ok = true;
        const auto check = [&](bool cond, const std::string& what) {
            if (cond) ++passed;
            else { std::printf("  photo guide: %s\n", what.c_str()); ok = false; }
        };
        using K = FieldKind;
        const auto key = [](int code) { return Field{ K::Key, code, oot::input::HatDirection::Up }; };
        const auto button = [](int b) { return Field{ K::ControllerButton, b, oot::input::HatDirection::Up }; };
        const auto axis = [](int a, bool positive) {
            return Field{ positive ? K::ControllerAxisPositive : K::ControllerAxisNegative, a, oot::input::HatDirection::Up };
        };
        // The keyboard's defaults (input_bindings.cpp), by made-up scancodes and their SDL names.
        const KeyName names = [](int code) -> std::string {
            switch (code) {
                case 1: return "W"; case 2: return "A"; case 3: return "S"; case 4: return "D";
                case 5: return "I"; case 6: return "J"; case 7: return "K"; case 8: return "L";
                case 9: return "E"; case 10: return "R"; case 11: return "Up"; case 12: return "Down";
                case 13: return "Left Shift"; case 14: return "H"; case 15: return "F12"; case 16: return "C";
                case 17: return "F10"; case 18: return "Space"; default: return "?";
            }
        };
        const Lookup keyboard = [&](GameInput input) -> Slots {
            Slots s{};
            switch (input) {
                case GameInput::PhotoForward: s[0] = key(1); break;
                case GameInput::PhotoLeft: s[0] = key(2); break;
                case GameInput::PhotoBack: s[0] = key(3); break;
                case GameInput::PhotoRight: s[0] = key(4); break;
                case GameInput::PhotoTurnUp: s[0] = key(5); break;
                case GameInput::PhotoTurnLeft: s[0] = key(6); break;
                case GameInput::PhotoTurnDown: s[0] = key(7); break;
                case GameInput::PhotoTurnRight: s[0] = key(8); break;
                case GameInput::PhotoLower: s[0] = key(9); break;
                case GameInput::PhotoRaise: s[0] = key(10); break;
                case GameInput::PhotoCloser: s[0] = key(11); break;
                case GameInput::PhotoFurther: s[0] = key(12); break;
                case GameInput::PhotoReset: s[0] = key(13); break;
                case GameInput::PhotoHud: s[0] = key(14); break;
                case GameInput::Screenshot: s[0] = key(15); break;
                case GameInput::PhotoGuide: s[0] = key(16); break;
                case GameInput::PhotoMode: s[1] = key(17); break;   // the second slot is read too
                case GameInput::PhotoShot: s[0] = key(18); break;
                default: break;
            }
            return s;
        };
        const std::vector<Prompt> keys = rows(Family::Keyboard, keyboard, names, Here{});
        const auto word_of = [](const std::vector<Prompt>& list, const char* action, size_t nth = 0) -> std::string {
            size_t seen = 0;
            for (const Prompt& p : list) {
                if (p.action == action && seen++ == nth) return p.glyph.kind + ":" + p.glyph.key + p.glyph.shape;
            }
            return "missing";
        };
        check(word_of(keys, "photo-move") == "plate:W A S D", "the keys' move is one plate, W A S D: " + word_of(keys, "photo-move"));
        check(word_of(keys, "photo-turn") == "plate:I J K L", "the keys' turn is I J K L: " + word_of(keys, "photo-turn"));
        check(word_of(keys, "photo-height") == "plate:E R", "lower and raise read E R: " + word_of(keys, "photo-height"));
        check(word_of(keys, "photo-distance") == std::string("plate:") + ARROW_UP + " " + ARROW_DOWN, "closer and further are the arrows");
        check(word_of(keys, "photo-reset") == "plate:Shift", "Left Shift is Shift: " + word_of(keys, "photo-reset"));
        check(word_of(keys, "photo-leave") == "plate:F10", "a binding in the second slot counts");
        check(keys.size() == 9, "the keyboard's guide has every action once: " + std::to_string(keys.size()));
        bool labels = true;
        for (const Prompt& p : keys) labels = labels && !p.label.empty();
        check(labels, "a single glyph always carries its label");

        // The pad's defaults (controller_defaults), no screenshot or leave binding.
        const Lookup pad_defaults = [&](GameInput input) -> Slots {
            Slots s{};
            switch (input) {
                case GameInput::PhotoForward: s[0] = axis(pad::LeftY, false); break;
                case GameInput::PhotoBack: s[0] = axis(pad::LeftY, true); break;
                case GameInput::PhotoLeft: s[0] = axis(pad::LeftX, false); break;
                case GameInput::PhotoRight: s[0] = axis(pad::LeftX, true); break;
                case GameInput::PhotoTurnUp: s[0] = axis(pad::RightY, false); break;
                case GameInput::PhotoTurnDown: s[0] = axis(pad::RightY, true); break;
                case GameInput::PhotoTurnLeft: s[0] = axis(pad::RightX, false); break;
                case GameInput::PhotoTurnRight: s[0] = axis(pad::RightX, true); break;
                case GameInput::PhotoRaise: s[0] = button(pad::RightShoulder); break;
                case GameInput::PhotoLower: s[0] = button(pad::LeftShoulder); break;
                case GameInput::PhotoCloser: s[0] = button(pad::DUp); break;
                case GameInput::PhotoFurther: s[0] = button(pad::DDown); break;
                case GameInput::PhotoReset: s[0] = button(pad::X); break;
                case GameInput::PhotoHud: s[0] = button(pad::Y); break;
                case GameInput::PhotoGuide: s[0] = button(pad::B); break;
                case GameInput::PhotoShot: s[0] = button(pad::A); break;
                case GameInput::PhotoMode: s[0] = button(pad::LeftStick); break;
                default: break;
            }
            return s;
        };
        const std::vector<Prompt> xbox = rows(Family::Xbox, pad_defaults, names, Here{ true });
        check(word_of(xbox, "photo-move") == "medal:stick-l" && word_of(xbox, "photo-turn") == "medal:stick-r", "the sticks are drawn, moving");
        check(word_of(xbox, "photo-height", 0) == "plate:LB" && word_of(xbox, "photo-height", 1) == "plate:RB", "the shoulders are a pair, LB then RB");
        check(word_of(xbox, "photo-distance") == "medal:dpad-up-down", "the d-pad's two are one drawing, both arms lit: " + word_of(xbox, "photo-distance"));
        check(word_of(xbox, "photo-reset") == "face:X" && word_of(xbox, "photo-guide") == "face:B", "the face buttons are medallions");
        check(word_of(keys, "photo-shot") == "plate:Space", "the keys take a picture with Space: " + word_of(keys, "photo-shot"));
        check(word_of(xbox, "photo-shot") == "face:A", "a pad takes a picture with its bottom face button");
        check(word_of(xbox, "photo-leave") == "medal:stick-l-press", "a pad leaves with its left stick's press: " + word_of(xbox, "photo-leave"));
        check(xbox.size() == 10, "a pad's guide has every action: " + std::to_string(xbox.size()));
        const Lookup nothing_to_leave = [&](GameInput input) -> Slots {
            return input == GameInput::PhotoMode ? Slots{} : pad_defaults(input);
        };
        check(word_of(rows(Family::Xbox, nothing_to_leave, names, Here{}), "photo-leave") == "missing",
              "an action with no binding is left out");
        const Lookup screenshot_only = [&](GameInput input) -> Slots {
            Slots s{};
            if (input == GameInput::Screenshot) s[0] = key(15);
            return s;
        };
        check(word_of(rows(Family::Keyboard, screenshot_only, names, Here{}), "photo-shot") == "plate:F12",
              "with no picture control, the Screenshot control's key is shown");
        bool shown_hud = false;
        for (const Prompt& p : xbox) shown_hud = shown_hud || (p.action == "photo-hud" && p.label == "Show the HUD");
        check(shown_hud, "a hidden HUD's prompt offers to show it");
        const auto hud_label = [&](const Here& here) {
            for (const Prompt& p : rows(Family::Xbox, pad_defaults, names, here)) {
                if (p.action == "photo-hud") return p.label;
            }
            return std::string();
        };
        check(hud_label(Here{}) == "Dim the HUD" && hud_label(Here{ false, true }) == "Hide the HUD",
              "the HUD's prompt says the next of shown, dimmed, hidden");
        const std::vector<Prompt> ps = rows(Family::DualSense, pad_defaults, names, Here{});
        check(word_of(ps, "photo-reset") == "face:square" && word_of(ps, "photo-guide") == "face:circle", "a PlayStation pad's faces are its shapes");
        check(word_of(ps, "photo-height", 0) == "plate:L1", "a PlayStation pad's shoulder is L1");
        const std::vector<Prompt> nintendo = rows(Family::Nintendo, pad_defaults, names, Here{});
        check(word_of(nintendo, "photo-guide") == "face:A", "a Nintendo pad's east button reads A");
        check(word_of(nintendo, "photo-shot") == "face:B", "a Nintendo pad's bottom button reads B");
        check(word_of(ps, "photo-shot") == "face:cross", "a PlayStation pad takes a picture with cross");

        // Every other pad button has its drawing, for when someone binds it.
        const auto bound_to = [&](int b, GameInput where) -> Lookup {
            return [=](GameInput input) -> Slots {
                Slots s{};
                if (input == where) s[0] = button(b);
                return s;
            };
        };
        const auto shot_on = [&](Family f, int b) { return word_of(rows(f, bound_to(b, GameInput::PhotoShot), names, Here{}), "photo-shot"); };
        check(shot_on(Family::DualSense, pad::Back) == "art:sys-create" && shot_on(Family::DualShock, pad::Back) == "art:sys-share" &&
              shot_on(Family::Xbox, pad::Back) == "art:sys-view" && shot_on(Family::Nintendo, pad::Back) == "art:sys-minus",
              "the button left of center is each family's own, drawn");
        check(shot_on(Family::DualShock, pad::Start) == "art:sys-options" && shot_on(Family::Xbox, pad::Start) == "art:sys-menu" &&
              shot_on(Family::Nintendo, pad::Start) == "art:sys-plus", "the button right of center is each family's own, drawn");
        check(shot_on(Family::DualSense, pad::Guide) == "art:sys-ps" && shot_on(Family::Xbox, pad::Guide) == "art:sys-guide" &&
              shot_on(Family::Nintendo, pad::Guide) == "art:sys-home", "home is a house with the button's plain name");
        check(shot_on(Family::DualSense, pad::Misc1) == "art:sys-mute" && shot_on(Family::Xbox, pad::Misc1) == "art:sys-share-bar" &&
              shot_on(Family::Nintendo, pad::Misc1) == "art:sys-capture", "the extra button is Mute, Share or Capture");
        check(shot_on(Family::Xbox, pad::DLeft) == "medal:dpad-left" && shot_on(Family::Xbox, pad::RightStick) == "medal:stick-r-press",
              "a d-pad arm and a stick's press are drawn");
        const Lookup one_axis = [&](GameInput input) -> Slots {
            Slots s{};
            if (input == GameInput::PhotoShot) s[0] = axis(pad::RightY, false);
            return s;
        };
        check(word_of(rows(Family::Xbox, one_axis, names, Here{}), "photo-shot") == "medal:stick-r-up", "one direction of a stick is drawn with its chevron");
        const Lookup whole_dpad = [&](GameInput input) -> Slots {
            Slots s{};
            if (input == GameInput::PhotoForward) s[0] = button(pad::DUp);
            if (input == GameInput::PhotoLeft) s[0] = button(pad::DLeft);
            if (input == GameInput::PhotoBack) s[0] = button(pad::DDown);
            if (input == GameInput::PhotoRight) s[0] = button(pad::DRight);
            return s;
        };
        check(word_of(rows(Family::Xbox, whole_dpad, names, Here{}), "photo-move") == "medal:dpad-up-down-left-right",
              "a move on the d-pad lights its four arms in the drawings' order");

        // A rebound control is drawn as what is bound now: the menu on the touchpad shows the touchpad.
        const auto only = [&](GameInput where, Slots slots) -> Lookup {
            return [=](GameInput input) -> Slots { return input == where ? slots : Slots{}; };
        };
        Slots touch{};
        touch[0] = button(pad::Touchpad);
        std::vector<Glyph> gs = working_glyphs(Family::DualSense, only(GameInput::Menu, touch), GameInput::Menu, names);
        check(gs.size() == 1 && gs[0].kind == "pad", "a binding on the touchpad draws the touchpad");
        Slots second_only{};
        second_only[1] = key(17);
        gs = working_glyphs(Family::Keyboard, only(GameInput::Menu, second_only), GameInput::Menu, names);
        check(gs.size() == 1 && gs[0].key == "F10", "a binding in the second slot is drawn");
        check(working_glyphs(Family::Xbox, only(GameInput::Menu, Slots{}), GameInput::Menu, names).empty(), "nothing bound draws nothing");
        // Both slots shown when both work; one taken by a control acted on first is left out.
        Slots both{};
        both[0] = button(pad::Back);
        both[1] = button(pad::Touchpad);
        gs = working_glyphs(Family::DualSense, only(GameInput::Menu, both), GameInput::Menu, names);
        check(gs.size() == 2 && gs[0].shape == "sys-create" && gs[1].kind == "pad", "the menu on Create and the touchpad shows both");
        const Lookup shared = [&](GameInput input) -> Slots {
            Slots s{};
            if (input == GameInput::Screenshot) s[0] = key(18);                       // Space takes a screenshot first
            if (input == GameInput::PhotoShot) { s[0] = key(18); s[1] = key(16); }  // so only C reaches the picture
            return s;
        };
        gs = working_glyphs(Family::Keyboard, shared, GameInput::PhotoShot, names);
        check(gs.size() == 1 && gs[0].key == "C", "a key taken by a control acted on first is not shown");
        const std::vector<Prompt> two = rows(Family::Keyboard, [&](GameInput input) -> Slots {
            Slots s{};
            if (input == GameInput::PhotoHud) { s[0] = key(14); s[1] = key(13); }
            return s;
        }, names, Here{});
        check(word_of(two, "photo-hud", 0) == "plate:H" && word_of(two, "photo-hud", 1) == "plate:Shift",
              "a control with two working keys shows both, the label on the last");
        check(short_key("Keypad 8") == "Num 8" && short_key("F12") == "F12", "a key's name shortens only where it is long");
        return ok;
    }

}
