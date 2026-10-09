#include "main/input_bindings.h"

#include "main/keyvalue.h"

#include <SDL_scancode.h>
#include <SDL_gamecontroller.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace oot::input {

    namespace {

        struct InputName {
            const char* key;
            const char* label;
            uint16_t bit;
        };

        // The N64 button bits, from ultra64's controller.h, in the enumeration's order.
        constexpr InputName NAMES[INPUT_COUNT] = {
            { "a",           "A",           0x8000 },
            { "b",           "B",           0x4000 },
            { "z",           "Z",           0x2000 },
            { "start",       "Start",       0x1000 },
            { "d_up",        "D-pad up",    0x0800 },
            { "d_down",      "D-pad down",  0x0400 },
            { "d_left",      "D-pad left",  0x0200 },
            { "d_right",     "D-pad right", 0x0100 },
            { "l",           "L",           0x0020 },
            { "r",           "R",           0x0010 },
            { "c_up",        "C up",        0x0008 },
            { "c_down",      "C down",      0x0004 },
            { "c_left",      "C left",      0x0002 },
            { "c_right",     "C right",     0x0001 },
            { "stick_up",    "Stick up",    0 },
            { "stick_down",  "Stick down",  0 },
            { "stick_left",  "Stick left",  0 },
            { "stick_right", "Stick right", 0 },
            { "menu",        "Menu",        0 },
            { "quick_save",  "Quick save",  0 },
            { "quick_load",  "Quick load",  0 },
            { "screenshot",  "Screenshot",  0 },
            { "record_video", "Record video", 0 },
            { "pause_video", "Pause video", 0 },
            { "photo_mode",  "Photo mode",  0 },
            { "photo_forward",    "Camera forward",  0 },
            { "photo_back",       "Camera back",     0 },
            { "photo_left",       "Camera left",     0 },
            { "photo_right",      "Camera right",    0 },
            { "photo_turn_up",    "Turn up",         0 },
            { "photo_turn_down",  "Turn down",       0 },
            { "photo_turn_left",  "Turn left",       0 },
            { "photo_turn_right", "Turn right",      0 },
            { "photo_raise",      "Camera up",       0 },
            { "photo_lower",      "Camera down",     0 },
            { "photo_closer",     "Closer",          0 },
            { "photo_further",    "Further",         0 },
            { "photo_reset",      "Put the camera back", 0 },
            { "photo_hud",        "Show or hide the HUD", 0 },
            { "hud_toggle",       "HUD",             0 },
        };

        constexpr const char* HAT_NAMES[static_cast<int>(HatDirection::Count)] = { "up", "down", "left", "right" };

        std::mutex g_live_mutex;
        std::shared_ptr<const Bindings> g_live = std::make_shared<const Bindings>();
        std::string g_path;

        int clamp_int(int value, int low, int high) {
            return std::max(low, std::min(high, value));
        }

        Field key_field(SDL_Scancode code) {
            Field f;
            f.kind = FieldKind::Key;
            f.index = static_cast<int>(code);
            return f;
        }

        Field button_field(SDL_GameControllerButton button) {
            Field f;
            f.kind = FieldKind::ControllerButton;
            f.index = static_cast<int>(button);
            return f;
        }

        Field axis_field(SDL_GameControllerAxis axis, bool positive) {
            Field f;
            f.kind = positive ? FieldKind::ControllerAxisPositive : FieldKind::ControllerAxisNegative;
            f.index = static_cast<int>(axis);
            return f;
        }

        // A profile's file keys are `profile.<guid>.<rest>`; the guid is the second dotted part.
        bool split_profile_key(const std::string& key, std::string& guid, std::string& rest) {
            const std::string prefix = "profile.";
            if (key.compare(0, prefix.size(), prefix) != 0) {
                return false;
            }
            const size_t guid_end = key.find('.', prefix.size());
            if (guid_end == std::string::npos) {
                return false;
            }
            guid = key.substr(prefix.size(), guid_end - prefix.size());
            rest = key.substr(guid_end + 1);
            return !guid.empty() && !rest.empty();
        }

        // `<input>.<slot>` for a binding line. Returns false for anything else.
        bool split_binding_key(const std::string& rest, GameInput& input, int& slot) {
            const size_t dot = rest.rfind('.');
            if (dot == std::string::npos) {
                return false;
            }
            if (!input_from_key(rest.substr(0, dot), input)) {
                return false;
            }
            slot = keyvalue::as_int(rest.substr(dot + 1), -1);
            return slot >= 0 && slot < BINDINGS_PER_INPUT;
        }

        void write_bindings(std::ofstream& file, const std::string& prefix, const InputBindings& bindings) {
            for (int i = 0; i < INPUT_COUNT; ++i) {
                for (int slot = 0; slot < BINDINGS_PER_INPUT; ++slot) {
                    file << prefix << NAMES[i].key << "." << slot << " = "
                         << field_to_string(bindings[static_cast<size_t>(i)][static_cast<size_t>(slot)]) << "\n";
                }
            }
        }

        void set_binding(InputBindings& bindings, GameInput input, int slot, const Field& field) {
            bindings[static_cast<size_t>(input)][static_cast<size_t>(slot)] = field;
        }

    } // namespace

    bool operator==(const Bindings& a, const Bindings& b) {
        if (a.keyboard != b.keyboard || a.profiles.size() != b.profiles.size()) {
            return false;
        }
        for (size_t i = 0; i < a.profiles.size(); ++i) {
            const Profile& p = a.profiles[i];
            const Profile& q = b.profiles[i];
            if (p.guid != q.guid || p.bindings != q.bindings || p.deadzone_percent != q.deadzone_percent ||
                p.rumble != q.rumble) {
                return false;
            }
        }
        return true;
    }

    const char* input_key(GameInput input) {
        return NAMES[static_cast<int>(input)].key;
    }

    const char* input_label(GameInput input) {
        return NAMES[static_cast<int>(input)].label;
    }

    uint16_t input_bit(GameInput input) {
        return NAMES[static_cast<int>(input)].bit;
    }

    bool input_from_key(const std::string& key, GameInput& out) {
        for (int i = 0; i < INPUT_COUNT; ++i) {
            if (key == NAMES[i].key) {
                out = static_cast<GameInput>(i);
                return true;
            }
        }
        return false;
    }

    // The keys the application has used since phase 20 (src/main/boot.cpp then, this table now),
    // and the ones tools/harness/drive.ps1 presses: Space A, Left Shift B, Q Z, Return Start,
    // the arrows the D-pad, E and R the shoulders, IJKL the C buttons, WASD the stick, F1 the
    // menu.
    void keyboard_defaults(InputBindings& out) {
        for (auto& slots : out) {
            for (auto& f : slots) {
                f = Field{};
            }
        }
        // The layout a person without a pad expects (the user's ask of 2026-09-19), which is
        // also the reference project's convention: the left hand on WASD with Space for A,
        // Left Shift for B, Q to target, E and R for the shoulders; the right hand on IJKL for
        // the C buttons and the arrows for the D-pad; Enter for Start. The harness's key table
        // (tools/harness/drive.ps1) mirrors this and changes with it.
        set_binding(out, GameInput::A,          0, key_field(SDL_SCANCODE_SPACE));
        set_binding(out, GameInput::B,          0, key_field(SDL_SCANCODE_LSHIFT));
        set_binding(out, GameInput::Z,          0, key_field(SDL_SCANCODE_Q));
        set_binding(out, GameInput::Start,      0, key_field(SDL_SCANCODE_RETURN));
        set_binding(out, GameInput::DUp,        0, key_field(SDL_SCANCODE_UP));
        set_binding(out, GameInput::DDown,      0, key_field(SDL_SCANCODE_DOWN));
        set_binding(out, GameInput::DLeft,      0, key_field(SDL_SCANCODE_LEFT));
        set_binding(out, GameInput::DRight,     0, key_field(SDL_SCANCODE_RIGHT));
        set_binding(out, GameInput::L,          0, key_field(SDL_SCANCODE_E));
        set_binding(out, GameInput::R,          0, key_field(SDL_SCANCODE_R));
        set_binding(out, GameInput::CUp,        0, key_field(SDL_SCANCODE_I));
        set_binding(out, GameInput::CDown,      0, key_field(SDL_SCANCODE_K));
        set_binding(out, GameInput::CLeft,      0, key_field(SDL_SCANCODE_J));
        set_binding(out, GameInput::CRight,     0, key_field(SDL_SCANCODE_L));
        set_binding(out, GameInput::StickUp,    0, key_field(SDL_SCANCODE_W));
        set_binding(out, GameInput::StickDown,  0, key_field(SDL_SCANCODE_S));
        set_binding(out, GameInput::StickLeft,  0, key_field(SDL_SCANCODE_A));
        set_binding(out, GameInput::StickRight, 0, key_field(SDL_SCANCODE_D));
        set_binding(out, GameInput::Menu,       0, key_field(SDL_SCANCODE_F1));
        // F6 and F9, because F5 is the updater's and F2 to F4 open the other documents; both
        // rebind like anything else, and neither has a pad default (the pad's buttons are the
        // game's; a person picks one on the controls screen if they want it).
        set_binding(out, GameInput::QuickSave,  0, key_field(SDL_SCANCODE_F6));
        set_binding(out, GameInput::QuickLoad,  0, key_field(SDL_SCANCODE_F9));
        // F12, the key screenshots are usually on; no pad default, for the same reason.
        set_binding(out, GameInput::Screenshot, 0, key_field(SDL_SCANCODE_F12));
        // F7 and F8, beside the quick keys and free of the documents'; no pad default either.
        set_binding(out, GameInput::RecordVideo, 0, key_field(SDL_SCANCODE_F7));
        set_binding(out, GameInput::PauseVideo, 0, key_field(SDL_SCANCODE_F8));
        // F10 for photo mode, free of every other; no pad default, like the rest of the program's.
        set_binding(out, GameInput::PhotoMode, 0, key_field(SDL_SCANCODE_F10));
        // Photo mode's camera on the keys the game's own moves are on, which a frozen game does
        // not read: WASD moves, IJKL turns, R and E up and down, the arrows closer and further,
        // Left Shift puts it back.
        set_binding(out, GameInput::PhotoForward,   0, key_field(SDL_SCANCODE_W));
        set_binding(out, GameInput::PhotoBack,      0, key_field(SDL_SCANCODE_S));
        set_binding(out, GameInput::PhotoLeft,      0, key_field(SDL_SCANCODE_A));
        set_binding(out, GameInput::PhotoRight,     0, key_field(SDL_SCANCODE_D));
        set_binding(out, GameInput::PhotoTurnUp,    0, key_field(SDL_SCANCODE_I));
        set_binding(out, GameInput::PhotoTurnDown,  0, key_field(SDL_SCANCODE_K));
        set_binding(out, GameInput::PhotoTurnLeft,  0, key_field(SDL_SCANCODE_J));
        set_binding(out, GameInput::PhotoTurnRight, 0, key_field(SDL_SCANCODE_L));
        set_binding(out, GameInput::PhotoRaise,     0, key_field(SDL_SCANCODE_R));
        set_binding(out, GameInput::PhotoLower,     0, key_field(SDL_SCANCODE_E));
        set_binding(out, GameInput::PhotoCloser,    0, key_field(SDL_SCANCODE_UP));
        set_binding(out, GameInput::PhotoFurther,   0, key_field(SDL_SCANCODE_DOWN));
        set_binding(out, GameInput::PhotoReset,     0, key_field(SDL_SCANCODE_LSHIFT));
        // H for the HUD, a key nothing else has.
        set_binding(out, GameInput::PhotoHud,       0, key_field(SDL_SCANCODE_H));
        // Tab for the HUD's fading, the key games use for a HUD or an overview.
        set_binding(out, GameInput::HudToggle,      0, key_field(SDL_SCANCODE_TAB));
    }

    // The standard layout of an Xbox or PlayStation pad as SDL names its buttons: A on the south
    // face button, B on the west, Z on the left trigger, the shoulders on the shoulders, Start on
    // start, the C buttons on the right stick with the north and east face buttons as second
    // bindings for C up and C right (the way the reference project lays them out, recomputed here
    // against SDL's own names), the D-pad on the D-pad, the stick on the left stick, the menu on
    // the back button.
    void controller_defaults(InputBindings& out) {
        for (auto& slots : out) {
            for (auto& f : slots) {
                f = Field{};
            }
        }
        set_binding(out, GameInput::A,          0, button_field(SDL_CONTROLLER_BUTTON_A));
        set_binding(out, GameInput::B,          0, button_field(SDL_CONTROLLER_BUTTON_X));
        set_binding(out, GameInput::Z,          0, axis_field(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true));
        set_binding(out, GameInput::Start,      0, button_field(SDL_CONTROLLER_BUTTON_START));
        set_binding(out, GameInput::DUp,        0, button_field(SDL_CONTROLLER_BUTTON_DPAD_UP));
        set_binding(out, GameInput::DDown,      0, button_field(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
        set_binding(out, GameInput::DLeft,      0, button_field(SDL_CONTROLLER_BUTTON_DPAD_LEFT));
        set_binding(out, GameInput::DRight,     0, button_field(SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
        set_binding(out, GameInput::L,          0, button_field(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
        set_binding(out, GameInput::R,          0, button_field(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
        set_binding(out, GameInput::CUp,        0, axis_field(SDL_CONTROLLER_AXIS_RIGHTY, false));
        set_binding(out, GameInput::CUp,        1, button_field(SDL_CONTROLLER_BUTTON_Y));
        set_binding(out, GameInput::CDown,      0, axis_field(SDL_CONTROLLER_AXIS_RIGHTY, true));
        // C DOWN AND C LEFT HAVE A BUTTON EACH AS WELL (upgrades phase 79, the free camera): with
        // the right stick turning the camera its C bindings are silent, and the two items on
        // these buttons would otherwise have no button at all. The right trigger and the left
        // bumper are the two the standard layout leaves free (L, on the left bumper too, does
        // nothing in this game's retail build). Rebinds like everything else.
        set_binding(out, GameInput::CDown,      1, axis_field(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true));
        set_binding(out, GameInput::CLeft,      0, axis_field(SDL_CONTROLLER_AXIS_RIGHTX, false));
        set_binding(out, GameInput::CLeft,      1, button_field(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
        set_binding(out, GameInput::CRight,     0, axis_field(SDL_CONTROLLER_AXIS_RIGHTX, true));
        set_binding(out, GameInput::CRight,     1, button_field(SDL_CONTROLLER_BUTTON_B));
        set_binding(out, GameInput::StickUp,    0, axis_field(SDL_CONTROLLER_AXIS_LEFTY, false));
        set_binding(out, GameInput::StickDown,  0, axis_field(SDL_CONTROLLER_AXIS_LEFTY, true));
        set_binding(out, GameInput::StickLeft,  0, axis_field(SDL_CONTROLLER_AXIS_LEFTX, false));
        set_binding(out, GameInput::StickRight, 0, axis_field(SDL_CONTROLLER_AXIS_LEFTX, true));
        set_binding(out, GameInput::Menu,       0, button_field(SDL_CONTROLLER_BUTTON_BACK));
        // THE TOUCHPAD CLICK OPENS THE MENU TOO, on a DualShock 4 or a DualSense (the user's ask
        // of 2026-09-23). It is a second binding rather than a replacement, so Back still works
        // on the pads that have one and this is simply the button a PlayStation player reaches
        // for; and being an ordinary binding, it rebinds like everything else. SDL reports the
        // click as its own button on both pads, so nothing here is PlayStation specific beyond
        // the fact that no other controller has one to press.
        set_binding(out, GameInput::Menu,       1, button_field(SDL_CONTROLLER_BUTTON_TOUCHPAD));
        // Photo mode's camera: the left stick moves it, the right stick turns it, the shoulders
        // raise and lower it (the user, 2026-10-09: "either do both two buttons or both one
        // buttons"), the D-pad's up and down bring it closer and further, B's button puts it back.
        set_binding(out, GameInput::PhotoForward,   0, axis_field(SDL_CONTROLLER_AXIS_LEFTY, false));
        set_binding(out, GameInput::PhotoBack,      0, axis_field(SDL_CONTROLLER_AXIS_LEFTY, true));
        set_binding(out, GameInput::PhotoLeft,      0, axis_field(SDL_CONTROLLER_AXIS_LEFTX, false));
        set_binding(out, GameInput::PhotoRight,     0, axis_field(SDL_CONTROLLER_AXIS_LEFTX, true));
        set_binding(out, GameInput::PhotoTurnUp,    0, axis_field(SDL_CONTROLLER_AXIS_RIGHTY, false));
        set_binding(out, GameInput::PhotoTurnDown,  0, axis_field(SDL_CONTROLLER_AXIS_RIGHTY, true));
        set_binding(out, GameInput::PhotoTurnLeft,  0, axis_field(SDL_CONTROLLER_AXIS_RIGHTX, false));
        set_binding(out, GameInput::PhotoTurnRight, 0, axis_field(SDL_CONTROLLER_AXIS_RIGHTX, true));
        set_binding(out, GameInput::PhotoRaise,     0, button_field(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
        set_binding(out, GameInput::PhotoLower,     0, button_field(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
        set_binding(out, GameInput::PhotoCloser,    0, button_field(SDL_CONTROLLER_BUTTON_DPAD_UP));
        set_binding(out, GameInput::PhotoFurther,   0, button_field(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
        set_binding(out, GameInput::PhotoReset,     0, button_field(SDL_CONTROLLER_BUTTON_X));
        // The top face button for the HUD: a frozen game reads none of the face buttons.
        set_binding(out, GameInput::PhotoHud,       0, button_field(SDL_CONTROLLER_BUTTON_Y));
        // The right stick's click, which the game's own layout leaves free.
        set_binding(out, GameInput::HudToggle,      0, button_field(SDL_CONTROLLER_BUTTON_RIGHTSTICK));
    }

    std::string field_to_string(const Field& field) {
        const std::string index = std::to_string(field.index);
        switch (field.kind) {
            case FieldKind::Key:                    return "key:" + index;
            case FieldKind::ControllerButton:       return "button:" + index;
            case FieldKind::ControllerAxisPositive: return "axis+:" + index;
            case FieldKind::ControllerAxisNegative: return "axis-:" + index;
            case FieldKind::JoystickButton:         return "jbutton:" + index;
            case FieldKind::JoystickAxisPositive:   return "jaxis+:" + index;
            case FieldKind::JoystickAxisNegative:   return "jaxis-:" + index;
            case FieldKind::JoystickHat:
                return "jhat:" + index + ":" + HAT_NAMES[static_cast<int>(field.hat)];
            default:                                return "none";
        }
    }

    bool field_from_string(const std::string& text, Field& out) {
        out = Field{};
        if (text == "none" || text.empty()) {
            return text == "none";
        }
        const size_t colon = text.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        const std::string kind = text.substr(0, colon);
        std::string rest = text.substr(colon + 1);

        FieldKind k = FieldKind::None;
        if (kind == "key")          k = FieldKind::Key;
        else if (kind == "button")  k = FieldKind::ControllerButton;
        else if (kind == "axis+")   k = FieldKind::ControllerAxisPositive;
        else if (kind == "axis-")   k = FieldKind::ControllerAxisNegative;
        else if (kind == "jbutton") k = FieldKind::JoystickButton;
        else if (kind == "jaxis+")  k = FieldKind::JoystickAxisPositive;
        else if (kind == "jaxis-")  k = FieldKind::JoystickAxisNegative;
        else if (kind == "jhat")    k = FieldKind::JoystickHat;
        else {
            return false;
        }

        HatDirection hat = HatDirection::Up;
        if (k == FieldKind::JoystickHat) {
            const size_t second = rest.find(':');
            if (second == std::string::npos) {
                return false;
            }
            const std::string direction = rest.substr(second + 1);
            rest = rest.substr(0, second);
            bool known = false;
            for (int i = 0; i < static_cast<int>(HatDirection::Count); ++i) {
                if (direction == HAT_NAMES[i]) {
                    hat = static_cast<HatDirection>(i);
                    known = true;
                }
            }
            if (!known) {
                return false;
            }
        }

        const int index = keyvalue::as_int(rest, -1);
        if (index < 0) {
            return false;
        }
        out.kind = k;
        out.index = clamp_int(index, 0, MAX_INDEX);
        out.hat = hat;
        return true;
    }

    Bindings load(const std::string& path) {
        Bindings result;
        keyboard_defaults(result.keyboard);

        std::ifstream file(path);
        if (!file) {
            return result;   // a missing file is the defaults, and not an error
        }

        std::string line;
        while (std::getline(file, line)) {
            std::string key, value;
            if (!keyvalue::parse_line(line, key, value)) {
                continue;
            }

            const std::string keyboard_prefix = "keyboard.";
            if (key.compare(0, keyboard_prefix.size(), keyboard_prefix) == 0) {
                GameInput input;
                int slot;
                if (split_binding_key(key.substr(keyboard_prefix.size()), input, slot)) {
                    Field f;
                    field_from_string(value, f);   // garbage reads as none, on purpose
                    set_binding(result.keyboard, input, slot, f);
                }
                continue;
            }

            std::string guid, rest;
            if (split_profile_key(key, guid, rest)) {
                Profile& profile = profile_for(result, guid);
                if (rest == "deadzone") {
                    profile.deadzone_percent = clamp_int(keyvalue::as_int(value, profile.deadzone_percent),
                                                         MIN_DEADZONE_PERCENT, MAX_DEADZONE_PERCENT);
                }
                else if (rest == "rumble") {
                    profile.rumble = keyvalue::as_int(value, profile.rumble ? 1 : 0) != 0;
                }
                else {
                    GameInput input;
                    int slot;
                    if (split_binding_key(rest, input, slot)) {
                        Field f;
                        field_from_string(value, f);
                        set_binding(profile.bindings, input, slot, f);
                    }
                }
                continue;
            }
            // An unknown key is ignored on purpose, as the settings file's are.
        }

        // A PROFILE FROM BEFORE THE FREE CAMERA (upgrades phase 79) gets the two buttons the
        // standard layout now gives C down and C left, in their EMPTY second slots only, and
        // only where the first slot is still the right stick direction the layout put there:
        // a person who rebound either row keeps exactly what they chose.
        for (Profile& profile : result.profiles) {
            auto give = [&](GameInput input, const Field& expected_first, const Field& second) {
                std::array<Field, BINDINGS_PER_INPUT>& slots = profile.bindings[static_cast<size_t>(input)];
                if ((slots[1].kind == FieldKind::None) && (slots[0] == expected_first)) {
                    slots[1] = second;
                }
            };
            give(GameInput::CDown, axis_field(SDL_CONTROLLER_AXIS_RIGHTY, true), axis_field(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true));
            give(GameInput::CLeft, axis_field(SDL_CONTROLLER_AXIS_RIGHTX, false), button_field(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
        }
        return result;
    }

    bool save(const std::string& path, const Bindings& bindings) {
        std::ofstream file(path, std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "[input] controls could not be written to %s\n", path.c_str());
            return false;
        }
        file << "# OoT: Recompiled controls. Edit by hand if you like; every value is clamped when it\n";
        file << "# is read, anything unrecognized is ignored, and a missing line is its default.\n";
        file << "# A binding is key:<scancode>, button:<n>, axis+:<n>, axis-:<n> (a pad SDL knows),\n";
        file << "# jbutton:<n>, jaxis+:<n>, jaxis-:<n>, jhat:<n>:<up|down|left|right> (a raw pad), or none.\n";
        write_bindings(file, "keyboard.", bindings.keyboard);
        for (const Profile& profile : bindings.profiles) {
            const std::string prefix = "profile." + profile.guid + ".";
            file << prefix << "deadzone = " << profile.deadzone_percent << "\n";
            file << prefix << "rumble = " << (profile.rumble ? 1 : 0) << "\n";
            write_bindings(file, prefix, profile.bindings);
        }
        return file.good();
    }

    void set_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(g_live_mutex);
        g_path = path;
    }

    const std::string& path() {
        return g_path;
    }

    bool save_live() {
        std::shared_ptr<const Bindings> current = live();
        if (g_path.empty()) {
            return false;
        }
        // One writer at a time: a capture saves from the pump and the controls document saves
        // from the render thread, and two writers to one file would leave neither's version.
        static std::mutex file_mutex;
        std::lock_guard<std::mutex> lock(file_mutex);
        return save(g_path, *current);
    }

    std::shared_ptr<const Bindings> live() {
        std::lock_guard<std::mutex> lock(g_live_mutex);
        return g_live;
    }

    void set_live(const Bindings& bindings) {
        std::shared_ptr<const Bindings> next = std::make_shared<const Bindings>(bindings);
        std::lock_guard<std::mutex> lock(g_live_mutex);
        g_live = next;
    }

    Profile& profile_for(Bindings& bindings, const std::string& guid) {
        for (Profile& p : bindings.profiles) {
            if (p.guid == guid) {
                return p;
            }
        }
        Profile fresh;
        fresh.guid = guid;
        controller_defaults(fresh.bindings);
        bindings.profiles.push_back(fresh);
        return bindings.profiles.back();
    }

    int selftest(const std::string& temp_path) {
        // A file with every kind of value, and garbage where a person could put it.
        {
            std::ofstream file(temp_path, std::ios::trunc);
            if (!file) {
                std::fprintf(stderr, "CONTROLS_SELFTEST_FAILED: cannot write %s\n", temp_path.c_str());
                return 1;
            }
            file << "# a comment\n"
                 << "keyboard.start.0 = key:19\n"
                 << "keyboard.start.1 = key:40\n"
                 << "keyboard.a.0 = banana\n"                   // garbage: none
                 << "keyboard.b.1 = key:abc\n"                  // garbage index: none
                 << "keyboard.z.0 = key:99999\n"                // clamped to 255
                 << "keyboard.c_up.5 = key:1\n"                 // no such slot: ignored
                 << "keyboard.nothing.0 = key:1\n"              // no such input: ignored
                 << "nonsense = 1\n"                            // unknown key: ignored
                 << "this line has no equals sign\n"
                 << "profile.0300abcd.a.0 = button:0\n"
                 << "profile.0300abcd.z.0 = axis+:4\n"
                 << "profile.0300abcd.c_left.1 = jhat:0:left\n"
                 << "profile.0300abcd.c_right.0 = jhat:0:sideways\n"   // garbage direction: none
                 << "profile.0300abcd.deadzone = 500\n"          // clamped to 90
                 << "profile.0300abcd.rumble = 7\n"              // any non-zero is on
                 << "profile.0300abcd.stick_up.0 = jaxis-:1\n"
                 << "profile.0300abcd.stick_up.1 = axis-:-5\n";  // negative index: none
        }

        const Bindings loaded = load(temp_path);
        InputBindings defaults;
        keyboard_defaults(defaults);

        auto fail = [](const char* what) {
            std::fprintf(stderr, "CONTROLS_SELFTEST_FAILED: %s\n", what);
            return 1;
        };
        auto kb = [&](GameInput input, int slot) -> const Field& {
            return loaded.keyboard[static_cast<size_t>(input)][static_cast<size_t>(slot)];
        };

        if (kb(GameInput::Start, 0) != key_field(static_cast<SDL_Scancode>(19))) return fail("start.0 not read");
        if (kb(GameInput::Start, 1) != key_field(SDL_SCANCODE_RETURN)) return fail("start.1 not read");
        if (kb(GameInput::A, 0).kind != FieldKind::None) return fail("garbage value did not read as none");
        if (kb(GameInput::B, 1).kind != FieldKind::None) return fail("garbage index did not read as none");
        if (kb(GameInput::Z, 0).index != MAX_INDEX) return fail("index not clamped to 255");
        if (kb(GameInput::CUp, 0) != defaults[static_cast<size_t>(GameInput::CUp)][0]) return fail("bad slot changed a default");
        if (kb(GameInput::CRight, 0) != defaults[static_cast<size_t>(GameInput::CRight)][0]) return fail("an unknown key changed a default");
        if (loaded.profiles.size() != 1 || loaded.profiles[0].guid != "0300abcd") return fail("profile not created");
        const Profile& p = loaded.profiles[0];
        auto pb = [&](GameInput input, int slot) -> const Field& {
            return p.bindings[static_cast<size_t>(input)][static_cast<size_t>(slot)];
        };
        if (pb(GameInput::A, 0) != button_field(SDL_CONTROLLER_BUTTON_A)) return fail("profile button not read");
        if (pb(GameInput::Z, 0) != axis_field(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true)) return fail("profile axis not read");
        if (pb(GameInput::CLeft, 1).kind != FieldKind::JoystickHat || pb(GameInput::CLeft, 1).hat != HatDirection::Left) return fail("hat not read");
        if (pb(GameInput::CRight, 0).kind != FieldKind::None) return fail("garbage hat direction did not read as none");
        if (p.deadzone_percent != MAX_DEADZONE_PERCENT) return fail("deadzone not clamped");
        if (!p.rumble) return fail("rumble not read");
        if (pb(GameInput::StickUp, 0).kind != FieldKind::JoystickAxisNegative || pb(GameInput::StickUp, 0).index != 1) return fail("joystick axis not read");
        if (pb(GameInput::StickUp, 1).kind != FieldKind::None) return fail("negative index did not read as none");
        // A profile fills the bindings it does not name from the controller defaults.
        InputBindings cdefaults;
        controller_defaults(cdefaults);
        if (pb(GameInput::Start, 0) != cdefaults[static_cast<size_t>(GameInput::Start)][0]) return fail("profile default missing");

        // The round trip: what is saved is what is loaded.
        if (!save(temp_path, loaded)) return fail("save failed");
        const Bindings again = load(temp_path);
        if (!(again == loaded)) return fail("round trip changed something");

        // Every field kind survives the spelling.
        for (int k = 0; k < static_cast<int>(FieldKind::Count); ++k) {
            Field f;
            f.kind = static_cast<FieldKind>(k);
            f.index = 7;
            f.hat = HatDirection::Down;
            Field back;
            const bool ok = field_from_string(field_to_string(f), back);
            if (f.kind == FieldKind::None) {
                if (!ok || back.kind != FieldKind::None) return fail("none did not round trip");
            }
            else if (!ok || back.kind != f.kind || back.index != 7 ||
                     (f.kind == FieldKind::JoystickHat && back.hat != HatDirection::Down)) {
                return fail("a field kind did not round trip");
            }
        }

        // A missing file is the defaults.
        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
        const Bindings missing = load(temp_path);
        if (missing.keyboard != defaults || !missing.profiles.empty()) return fail("a missing file was not the defaults");

        std::printf("CONTROLS_SELFTEST_OK\n");
        return 0;
    }

} // namespace oot::input
