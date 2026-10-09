#include "main/input.h"

#include "main/input_bindings.h"
#include "main/keyvalue.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace oot::input::devices {

    namespace {

        struct Pad {
            SDL_JoystickID instance = -1;
            SDL_GameController* controller = nullptr;   // null for a raw pad
            SDL_Joystick* joystick = nullptr;           // always set (a controller's underlying one)
            PadInfo info;
            bool is_virtual = false;
            bool rumble_on = false;
            uint32_t rumble_refreshed_at = 0;
            // Edges through the profile (take_pad_edges): what was active last tick, and what
            // has gone active since the pump last asked.
            uint64_t edge_prev = 0;
            uint64_t edge_pending = 0;
        };

        struct VirtualEvent {
            uint32_t at_ms = 0;
            enum class Kind { Button, Axis, Hat, Capture } kind = Kind::Button;
            int index = 0;      // the button, axis or hat; for Capture, the input
            int value = 0;      // the state; for Capture, the slot
        };

        std::mutex g_mutex;
        std::vector<Pad> g_pads;                        // slot order: connection order
        // Whether the free camera holds the right stick this frame (set from the game thread by
        // the patch, read by read_pad on the same thread; atomic for the documents' pump).
        std::atomic<bool> g_stick_held_by_camera{ false };
        std::unordered_set<std::string> g_database_guids;
        std::vector<VirtualEvent> g_script;
        size_t g_script_next = 0;
        uint32_t g_script_started_at = 0;
        SDL_Joystick* g_virtual = nullptr;

        // Bind by pressing. One capture at a time, on the main thread (the pump).
        struct Capture {
            CaptureStatus status = CaptureStatus::Idle;
            GameInput input = GameInput::A;
            int slot = 0;
            int pad_slot = -1;                          // -1 is the keyboard
            SDL_JoystickID pad_instance = -1;
            bool pad_is_controller = false;
            uint32_t started_at = 0;
            bool fresh = false;   // bound since the last tick: that tick reports no edges
        };
        Capture g_capture;
        // The capture is armed from the render thread (the controls document) and fed from the
        // pump, so it has a mutex of its own. Taken before g_mutex, never after it.
        std::mutex g_capture_mutex;

        constexpr int AXIS_AS_BUTTON_THRESHOLD = 16384;   // half deflection
        constexpr uint32_t RUMBLE_PULSE_MS = 500;
        constexpr uint32_t RUMBLE_REFRESH_MS = 200;
        constexpr uint32_t CAPTURE_TIMEOUT_MS = 5000;

        std::string guid_string(SDL_Joystick* joystick) {
            char text[64] = {};
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), text, sizeof(text));
            return std::string(text);
        }

        // The database's own GUIDs, so a pad's mapping can be attributed to it rather than to
        // SDL's built-in tables. Each line is `guid,name,mapping`; the guid is the first field.
        void read_database_guids(const std::string& path) {
            std::ifstream file(path);
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                const size_t comma = line.find(',');
                if (comma != std::string::npos && comma > 0) {
                    g_database_guids.insert(line.substr(0, comma));
                }
            }
        }

        void open_device(int device_index) {
            // Virtual is a property of the device index in SDL 2, asked before the open.
            const bool is_virtual = SDL_JoystickIsVirtual(device_index) == SDL_TRUE;
            SDL_Joystick* joystick = nullptr;
            SDL_GameController* controller = nullptr;
            if (SDL_IsGameController(device_index)) {
                controller = SDL_GameControllerOpen(device_index);
                if (controller != nullptr) {
                    joystick = SDL_GameControllerGetJoystick(controller);
                }
            }
            else {
                joystick = SDL_JoystickOpen(device_index);
            }
            if (joystick == nullptr) {
                std::fprintf(stderr, "[input] device %d could not be opened: %s\n", device_index, SDL_GetError());
                return;
            }

            Pad pad;
            pad.instance = SDL_JoystickInstanceID(joystick);
            pad.controller = controller;
            pad.joystick = joystick;
            pad.is_virtual = is_virtual;
            const char* name = (controller != nullptr) ? SDL_GameControllerName(controller) : SDL_JoystickName(joystick);
            pad.info.name = (name != nullptr) ? name : "unnamed";
            const std::string guid = guid_string(joystick);
            pad.info.guid = pad.is_virtual ? "virtual" : guid;
            if (controller == nullptr) {
                pad.info.mapping = "raw";
            }
            else if (pad.is_virtual) {
                pad.info.mapping = "virtual";
            }
            else {
                pad.info.mapping = (g_database_guids.count(guid) != 0) ? "database" : "built in";
            }
            pad.info.rumbles = (controller != nullptr) ? (SDL_GameControllerHasRumble(controller) == SDL_TRUE)
                                                       : (SDL_JoystickHasRumble(joystick) == SDL_TRUE);
            if (pad.is_virtual) {
                g_virtual = joystick;
            }

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const Pad& existing : g_pads) {
                    if (existing.instance == pad.instance) {
                        return;   // SDL announces a present device once per subsystem; open it once
                    }
                }
                // THE SCRIPTED PAD GOES FIRST, and this is the difference between the virtual
                // controller working and appearing to work. The game reads slot 0 and only slot 0
                // (boot.cpp asks read_pad(0) once a frame), while pads otherwise arrive in the
                // order SDL announces them, which puts whatever is plugged into the machine ahead
                // of a pad attached at startup. So on a machine with a real controller connected,
                // every scripted press landed in slot 1 and the game never saw any of it: the
                // trace said the event had been applied, the pad was listed, and Link did not
                // move. There is no ambiguity about intent here, because the virtual pad exists
                // only when --virtual-controller was passed.
                if (pad.is_virtual) {
                    g_pads.insert(g_pads.begin(), pad);
                }
                else {
                    g_pads.push_back(pad);
                }
            }

            // The pad's profile exists from now on, with the standard layout for a pad SDL
            // knows and nothing bound for a raw one until the user binds it: SDL's names mean
            // nothing to a raw pad, and showing them in the controls document as if they did
            // would be a lie. A profile the file already holds is left exactly as written.
            Bindings updated = *live();
            bool existed = false;
            for (const Profile& p : updated.profiles) {
                if (p.guid == pad.info.guid) {
                    existed = true;
                }
            }
            Profile& profile = profile_for(updated, pad.info.guid);
            if (!existed && controller == nullptr) {
                profile.bindings = InputBindings{};
            }
            set_live(updated);

            std::fprintf(stderr, "[input] controller added: %s guid %s mapping %s rumble %s\n",
                         pad.info.name.c_str(), pad.info.guid.c_str(), pad.info.mapping.c_str(),
                         pad.info.rumbles ? "yes" : "no");
        }

        void close_device(SDL_JoystickID instance) {
            Pad removed;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (size_t i = 0; i < g_pads.size(); ++i) {
                    if (g_pads[i].instance == instance) {
                        removed = g_pads[i];
                        g_pads.erase(g_pads.begin() + static_cast<long long>(i));
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                return;
            }
            if (removed.is_virtual) {
                g_virtual = nullptr;
            }
            if (removed.controller != nullptr) {
                SDL_GameControllerClose(removed.controller);
            }
            else if (removed.joystick != nullptr) {
                SDL_JoystickClose(removed.joystick);
            }
            std::fprintf(stderr, "[input] controller removed: %s\n", removed.info.name.c_str());
        }

        // The script: `<ms> button <index> <0|1>`, `<ms> axis <index> <-32768..32767>`,
        // `<ms> hat <index> <up|down|left|right|center>`, one per line, `#` comments.
        void read_script(const std::string& path) {
            std::ifstream file(path);
            if (!file) {
                std::fprintf(stderr, "[input] virtual controller script not readable: %s\n", path.c_str());
                return;
            }
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                std::string a, b, c, d;
                size_t pos = 0;
                auto next = [&](std::string& out) {
                    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
                    const size_t start = pos;
                    while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t' && line[pos] != '\r') ++pos;
                    out = line.substr(start, pos - start);
                };
                next(a); next(b); next(c); next(d);
                VirtualEvent ev;
                const int ms = keyvalue::as_int(a, -1);
                if (ms < 0) {
                    continue;
                }
                ev.at_ms = static_cast<uint32_t>(ms);
                if (b == "capture") {
                    // `<ms> capture <input> <slot>`: arm a capture on the virtual pad, so the
                    // harness can prove bind by pressing without a hand on a pad.
                    GameInput input;
                    const int slot = keyvalue::as_int(d, -1);
                    if (!input_from_key(c, input) || slot < 0 || slot >= BINDINGS_PER_INPUT) {
                        continue;
                    }
                    ev.kind = VirtualEvent::Kind::Capture;
                    ev.index = static_cast<int>(input);
                    ev.value = slot;
                    g_script.push_back(ev);
                    continue;
                }
                const int index = keyvalue::as_int(c, -1);
                if (index < 0) {
                    continue;
                }
                ev.index = index;
                if (b == "button") {
                    ev.kind = VirtualEvent::Kind::Button;
                    ev.value = keyvalue::as_int(d, 0) != 0 ? 1 : 0;
                }
                else if (b == "axis") {
                    ev.kind = VirtualEvent::Kind::Axis;
                    // as_int reads digits only; a leading minus is read here.
                    const bool negative = !d.empty() && d[0] == '-';
                    const int magnitude = keyvalue::as_int(negative ? d.substr(1) : d, 0);
                    ev.value = std::clamp(negative ? -magnitude : magnitude, -32768, 32767);
                }
                else if (b == "hat") {
                    ev.kind = VirtualEvent::Kind::Hat;
                    if (d == "up") ev.value = SDL_HAT_UP;
                    else if (d == "down") ev.value = SDL_HAT_DOWN;
                    else if (d == "left") ev.value = SDL_HAT_LEFT;
                    else if (d == "right") ev.value = SDL_HAT_RIGHT;
                    else ev.value = SDL_HAT_CENTERED;
                }
                else {
                    continue;
                }
                g_script.push_back(ev);
            }
            std::sort(g_script.begin(), g_script.end(),
                      [](const VirtualEvent& p, const VirtualEvent& q) { return p.at_ms < q.at_ms; });
            std::fprintf(stderr, "[input] virtual controller script: %zu events\n", g_script.size());
        }

        void apply_virtual_events() {
            if (g_virtual == nullptr || g_script_next >= g_script.size()) {
                return;
            }
            const uint32_t now = SDL_GetTicks() - g_script_started_at;
            while (g_script_next < g_script.size() && g_script[g_script_next].at_ms <= now) {
                const VirtualEvent& ev = g_script[g_script_next++];
                switch (ev.kind) {
                    case VirtualEvent::Kind::Button:
                        SDL_JoystickSetVirtualButton(g_virtual, ev.index, static_cast<Uint8>(ev.value));
                        break;
                    case VirtualEvent::Kind::Axis:
                        SDL_JoystickSetVirtualAxis(g_virtual, ev.index, static_cast<Sint16>(ev.value));
                        break;
                    case VirtualEvent::Kind::Hat:
                        SDL_JoystickSetVirtualHat(g_virtual, ev.index, static_cast<Uint8>(ev.value));
                        break;
                    case VirtualEvent::Kind::Capture: {
                        // The virtual pad is whichever slot holds it.
                        int slot = -1;
                        {
                            std::lock_guard<std::mutex> lock(g_mutex);
                            for (size_t i = 0; i < g_pads.size(); ++i) {
                                if (g_pads[i].is_virtual) {
                                    slot = static_cast<int>(i);
                                }
                            }
                        }
                        if (slot >= 0) {
                            start_capture(static_cast<GameInput>(ev.index), ev.value, slot);
                        }
                        break;
                    }
                }
                std::fprintf(stderr, "[input] virtual event at %u ms applied\n", ev.at_ms);
            }
        }

        // Whether a field is active on a pad, as a button.
        bool field_active(const Pad& pad, const Field& f) {
            switch (f.kind) {
                case FieldKind::ControllerButton:
                    return pad.controller != nullptr && f.index < SDL_CONTROLLER_BUTTON_MAX &&
                           SDL_GameControllerGetButton(pad.controller, static_cast<SDL_GameControllerButton>(f.index)) != 0;
                case FieldKind::ControllerAxisPositive:
                    return pad.controller != nullptr && f.index < SDL_CONTROLLER_AXIS_MAX &&
                           SDL_GameControllerGetAxis(pad.controller, static_cast<SDL_GameControllerAxis>(f.index)) >= AXIS_AS_BUTTON_THRESHOLD;
                case FieldKind::ControllerAxisNegative:
                    return pad.controller != nullptr && f.index < SDL_CONTROLLER_AXIS_MAX &&
                           SDL_GameControllerGetAxis(pad.controller, static_cast<SDL_GameControllerAxis>(f.index)) <= -AXIS_AS_BUTTON_THRESHOLD;
                case FieldKind::JoystickButton:
                    return f.index < SDL_JoystickNumButtons(pad.joystick) && SDL_JoystickGetButton(pad.joystick, f.index) != 0;
                case FieldKind::JoystickAxisPositive:
                    return f.index < SDL_JoystickNumAxes(pad.joystick) && SDL_JoystickGetAxis(pad.joystick, f.index) >= AXIS_AS_BUTTON_THRESHOLD;
                case FieldKind::JoystickAxisNegative:
                    return f.index < SDL_JoystickNumAxes(pad.joystick) && SDL_JoystickGetAxis(pad.joystick, f.index) <= -AXIS_AS_BUTTON_THRESHOLD;
                case FieldKind::JoystickHat: {
                    if (f.index >= SDL_JoystickNumHats(pad.joystick)) {
                        return false;
                    }
                    const Uint8 hat = SDL_JoystickGetHat(pad.joystick, f.index);
                    switch (f.hat) {
                        case HatDirection::Up:    return (hat & SDL_HAT_UP) != 0;
                        case HatDirection::Down:  return (hat & SDL_HAT_DOWN) != 0;
                        case HatDirection::Left:  return (hat & SDL_HAT_LEFT) != 0;
                        case HatDirection::Right: return (hat & SDL_HAT_RIGHT) != 0;
                        default:                  return false;
                    }
                }
                default:
                    return false;   // keys are the keyboard's business, in boot.cpp
            }
        }

        // A field's deflection for the stick, 0 to 1: an axis direction gives its fraction, a
        // button gives 1 when down.
        float field_deflection(const Pad& pad, const Field& f) {
            auto fraction = [](Sint16 v, bool positive) {
                const float raw = positive ? std::max(0.0f, static_cast<float>(v) / 32767.0f)
                                           : std::max(0.0f, static_cast<float>(-v) / 32768.0f);
                return std::min(1.0f, raw);
            };
            switch (f.kind) {
                case FieldKind::ControllerAxisPositive:
                case FieldKind::ControllerAxisNegative:
                    if (pad.controller == nullptr || f.index >= SDL_CONTROLLER_AXIS_MAX) return 0.0f;
                    return fraction(SDL_GameControllerGetAxis(pad.controller, static_cast<SDL_GameControllerAxis>(f.index)),
                                    f.kind == FieldKind::ControllerAxisPositive);
                case FieldKind::JoystickAxisPositive:
                case FieldKind::JoystickAxisNegative:
                    if (f.index >= SDL_JoystickNumAxes(pad.joystick)) return 0.0f;
                    return fraction(SDL_JoystickGetAxis(pad.joystick, f.index), f.kind == FieldKind::JoystickAxisPositive);
                default:
                    return field_active(pad, f) ? 1.0f : 0.0f;
            }
        }

        float input_deflection(const Pad& pad, const InputBindings& bindings, GameInput input) {
            float best = 0.0f;
            for (const Field& f : bindings[static_cast<size_t>(input)]) {
                best = std::max(best, field_deflection(pad, f));
            }
            return best;
        }

        // The deadzone, per axis, with the remaining travel stretched back to the full range so
        // the stick's edge still reaches the game's full deflection (the reference project's
        // shape, recomputed): under the deadzone is zero, and just past it is just past zero.
        float apply_deadzone(float v, float deadzone) {
            const float magnitude = std::fabs(v);
            if (magnitude <= deadzone) {
                return 0.0f;
            }
            const float scaled = (magnitude - deadzone) / (1.0f - deadzone);
            return std::copysign(std::min(1.0f, scaled), v);
        }

        const Profile* profile_of(const Pad& pad, const Bindings& bindings) {
            for (const Profile& p : bindings.profiles) {
                if (p.guid == pad.info.guid) {
                    return &p;
                }
            }
            return nullptr;
        }

        // The bindings a pad reads: its profile's, or the standard layout while it has none.
        const InputBindings& map_for(const Profile* profile) {
            static InputBindings defaults;
            static bool ready = false;
            if (!ready) {
                controller_defaults(defaults);
                ready = true;
            }
            return (profile != nullptr) ? profile->bindings : defaults;
        }

        // An input as a button: a stick direction past half its travel, anything else by its
        // fields. For the edges the documents navigate by.
        bool input_active(const Pad& pad, const InputBindings& map, GameInput input) {
            const int i = static_cast<int>(input);
            if (i >= static_cast<int>(GameInput::StickUp) && i <= static_cast<int>(GameInput::StickRight)) {
                return input_deflection(pad, map, input) >= 0.5f;
            }
            for (const Field& f : map[static_cast<size_t>(i)]) {
                if (field_active(pad, f)) {
                    return true;
                }
            }
            return false;
        }

        void stop_motor(const Pad& pad) {
            if (!pad.info.rumbles) {
                return;
            }
            if (pad.controller != nullptr) {
                SDL_GameControllerRumble(pad.controller, 0, 0, 0);
            }
            else {
                SDL_JoystickRumble(pad.joystick, 0, 0, 0);
            }
        }

        // SDL names the standard layout after an Xbox pad ("a", "leftshoulder", "lefttrigger").
        // The document and the hint say what is printed on the pad in the user's hand instead,
        // by the pad's family; a pad of no known family, and the virtual one, read as Xbox.
        enum class PadFamily { Xbox, PlayStation, Nintendo };

        PadFamily family_of(const Pad& pad) {
            if (pad.controller == nullptr) {
                return PadFamily::Xbox;
            }
            switch (SDL_GameControllerGetType(pad.controller)) {
                case SDL_CONTROLLER_TYPE_PS3:
                case SDL_CONTROLLER_TYPE_PS4:
                case SDL_CONTROLLER_TYPE_PS5:
                    return PadFamily::PlayStation;
                case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
                case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
                case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                    return PadFamily::Nintendo;
                default:
                    return PadFamily::Xbox;
            }
        }

        std::string button_name(int button, PadFamily family) {
            // In SDL_GameControllerButton order: A, B, X, Y, back, guide, start, the two stick
            // clicks, the two shoulders, the four directions, misc, the four paddles, touchpad.
            static const char* const xbox[] = {
                "A button", "B button", "X button", "Y button", "Back button", "Guide button", "Start button",
                "Left stick click", "Right stick click", "Left bumper", "Right bumper",
                "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Share button",
                "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad",
            };
            static const char* const playstation[] = {
                "Cross", "Circle", "Square", "Triangle", "Share", "PS button", "Options",
                "L3", "R3", "L1", "R1",
                "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Mute",
                "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad",
            };
            static const char* const nintendo[] = {
                "B button", "A button", "Y button", "X button", "Minus", "Home", "Plus",
                "Left stick click", "Right stick click", "L", "R",
                "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Capture",
                "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad",
            };
            constexpr int NAMED = static_cast<int>(sizeof(xbox) / sizeof(xbox[0]));
            if (button < 0 || button >= NAMED) {
                return "Button " + std::to_string(button);
            }
            switch (family) {
                case PadFamily::PlayStation: return playstation[button];
                case PadFamily::Nintendo:    return nintendo[button];
                default:                     return xbox[button];
            }
        }

        std::string axis_name(int axis, bool positive, PadFamily family) {
            switch (axis) {
                case SDL_CONTROLLER_AXIS_LEFTX:  return positive ? "Left stick right" : "Left stick left";
                case SDL_CONTROLLER_AXIS_LEFTY:  return positive ? "Left stick down" : "Left stick up";
                case SDL_CONTROLLER_AXIS_RIGHTX: return positive ? "Right stick right" : "Right stick left";
                case SDL_CONTROLLER_AXIS_RIGHTY: return positive ? "Right stick down" : "Right stick up";
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT: {
                    const char* name = family == PadFamily::PlayStation ? "L2" : family == PadFamily::Nintendo ? "ZL" : "Left trigger";
                    return positive ? std::string(name) : std::string(name) + " released";
                }
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: {
                    const char* name = family == PadFamily::PlayStation ? "R2" : family == PadFamily::Nintendo ? "ZR" : "Right trigger";
                    return positive ? std::string(name) : std::string(name) + " released";
                }
                default:
                    return "Axis " + std::to_string(axis) + (positive ? " +" : " -");
            }
        }

    } // namespace

    void init(const std::string& database_path, const std::string& virtual_script_path, bool virtual_raw) {
        const int added = SDL_GameControllerAddMappingsFromFile(database_path.c_str());
        if (added < 0) {
            std::fprintf(stderr, "[input] mapping database not loaded from %s: %s\n", database_path.c_str(), SDL_GetError());
        }
        else {
            read_database_guids(database_path);
            std::fprintf(stderr, "[input] mapping database: %d mappings from %s\n", added, database_path.c_str());
        }

        if (!virtual_script_path.empty()) {
            // SDL THROWS PAD INPUT AWAY WHILE THE WINDOW IS NOT FOCUSED, and that is the whole
            // reason a scripted pad appeared to do nothing on a locked machine. Setting a virtual
            // axis only stages a value; SDL copies it into the joystick's state on its next
            // update, and that copy runs through the same "should this event be ignored" check as
            // a real pad's, which answers yes whenever no window has keyboard focus. A locked
            // session never has one. So the trace could honestly say the event was applied while
            // the game read a centered stick and Link stood at the door.
            //
            // It is set ONLY on the harness path, not for everybody: a real pad quietly driving
            // the game from behind another window is somebody else's bug report, and nothing but
            // --virtual-controller reaches this branch.
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

            read_script(virtual_script_path);
            g_script_started_at = SDL_GetTicks();
            // SDL's standard layout has 21 buttons and 6 axes; one hat for the raw path. Attached
            // as a game controller SDL maps on its own, or as a joystick of unknown type, which
            // SDL leaves unmapped: the way a pad SDL does not know arrives.
            const int index = SDL_JoystickAttachVirtual(virtual_raw ? SDL_JOYSTICK_TYPE_UNKNOWN : SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                                        SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 1);
            if (index < 0) {
                std::fprintf(stderr, "[input] virtual controller could not be attached: %s\n", SDL_GetError());
            }
        }

        // Pads present before the pump ran its first event. SDL also queues an added event for
        // each, and open_device refuses a duplicate by instance id.
        const int count = SDL_NumJoysticks();
        for (int i = 0; i < count; ++i) {
            open_device(i);
        }
    }

    void handle_event(const SDL_Event& event) {
        switch (event.type) {
            case SDL_JOYDEVICEADDED:
                // A game controller announces itself as a joystick too; opening on the joystick
                // event covers both kinds once.
                open_device(event.jdevice.which);
                break;
            case SDL_JOYDEVICEREMOVED:
                close_device(event.jdevice.which);
                break;
            default:
                break;
        }
    }

    void tick() {
        apply_virtual_events();

        // The capture's timeout, and whether the pads' edges are to be believed this tick: not
        // while a capture is listening, and not in the tick that bound one, so the press that
        // binds a button never also navigates a document.
        bool suppress_edges = false;
        {
            std::lock_guard<std::mutex> capture_lock(g_capture_mutex);
            if (g_capture.status == CaptureStatus::Waiting && SDL_GetTicks() - g_capture.started_at > CAPTURE_TIMEOUT_MS) {
                g_capture.status = CaptureStatus::TimedOut;
                std::fprintf(stderr, "[input] capture timed out: %s slot %d\n", input_key(g_capture.input), g_capture.slot);
            }
            suppress_edges = g_capture.status == CaptureStatus::Waiting || g_capture.fresh;
            g_capture.fresh = false;
        }

        const uint32_t now = SDL_GetTicks();
        const std::shared_ptr<const Bindings> bindings = live();
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Pad& pad : g_pads) {
            const Profile* profile = profile_of(pad, *bindings);
            const InputBindings& map = map_for(profile);

            // Rumble is a pulse SDL stops on its own; while the game holds it on, keep pulsing.
            // A profile that has turned rumble off stops it here, mid-pulse if need be.
            const bool allowed = profile == nullptr || profile->rumble;
            if (pad.rumble_on && !allowed) {
                pad.rumble_on = false;
                stop_motor(pad);
            }
            if (pad.rumble_on && pad.info.rumbles && now - pad.rumble_refreshed_at >= RUMBLE_REFRESH_MS) {
                if (pad.controller != nullptr) {
                    SDL_GameControllerRumble(pad.controller, 0xFFFF, 0xFFFF, RUMBLE_PULSE_MS);
                }
                else {
                    SDL_JoystickRumble(pad.joystick, 0xFFFF, 0xFFFF, RUMBLE_PULSE_MS);
                }
                pad.rumble_refreshed_at = now;
            }

            // The edges, through the profile, for the documents and the menu button.
            uint64_t mask = 0;
            for (int i = 0; i < INPUT_COUNT; ++i) {
                if (input_active(pad, map, static_cast<GameInput>(i))) {
                    mask |= uint64_t{ 1 } << i;
                }
            }
            if (!suppress_edges) {
                pad.edge_pending |= mask & ~pad.edge_prev;
            }
            pad.edge_prev = mask;
        }
    }

    uint64_t take_pad_edges(int slot) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (slot < 0 || slot >= static_cast<int>(g_pads.size())) {
            return 0;
        }
        Pad& pad = g_pads[static_cast<size_t>(slot)];
        const uint64_t edges = pad.edge_pending;
        pad.edge_pending = 0;
        return edges;
    }

    int pad_count() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return static_cast<int>(g_pads.size());
    }

    bool pad_info(int slot, PadInfo& out) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (slot < 0 || slot >= static_cast<int>(g_pads.size())) {
            return false;
        }
        out = g_pads[static_cast<size_t>(slot)].info;
        return true;
    }

    bool read_pad(int slot, uint16_t* buttons, float* x, float* y) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (slot < 0 || slot >= static_cast<int>(g_pads.size())) {
            return false;
        }
        const Pad& pad = g_pads[static_cast<size_t>(slot)];

        const std::shared_ptr<const Bindings> bindings = live();
        const Profile* profile = profile_of(pad, *bindings);
        const InputBindings& map = map_for(profile);
        const float deadzone = static_cast<float>((profile != nullptr) ? profile->deadzone_percent : DEFAULT_DEADZONE_PERCENT) / 100.0f;

        // The C buttons on the right stick are silent while the free camera holds it
        // (set_right_stick_held_by_camera): a field on either right stick axis is skipped for
        // the four C inputs, and nothing else changes, so a C button on a face button still
        // works and the stick's other uses are untouched.
        const bool camera_holds_stick = g_stick_held_by_camera.load(std::memory_order_relaxed);
        auto right_stick_axis = [](const Field& f) {
            return ((f.kind == FieldKind::ControllerAxisPositive) || (f.kind == FieldKind::ControllerAxisNegative)) &&
                   ((f.index == SDL_CONTROLLER_AXIS_RIGHTX) || (f.index == SDL_CONTROLLER_AXIS_RIGHTY));
        };

        *buttons = 0;
        for (int i = 0; i < INPUT_COUNT; ++i) {
            const GameInput input = static_cast<GameInput>(i);
            const uint16_t bit = input_bit(input);
            if (bit == 0) {
                continue;
            }
            const bool c_button = (input == GameInput::CUp) || (input == GameInput::CDown) ||
                                  (input == GameInput::CLeft) || (input == GameInput::CRight);
            for (const Field& f : map[static_cast<size_t>(i)]) {
                if (camera_holds_stick && c_button && right_stick_axis(f)) {
                    continue;
                }
                if (field_active(pad, f)) {
                    *buttons |= bit;
                    break;
                }
            }
        }

        const float right = input_deflection(pad, map, GameInput::StickRight);
        const float left = input_deflection(pad, map, GameInput::StickLeft);
        const float up = input_deflection(pad, map, GameInput::StickUp);
        const float down = input_deflection(pad, map, GameInput::StickDown);
        *x = apply_deadzone(right - left, deadzone);
        *y = apply_deadzone(up - down, deadzone);
        return true;
    }

    float input_amount(GameInput input) {
        float amount = 0.0f;
        const std::shared_ptr<const Bindings> bindings = live();
        int keys_length = 0;
        const Uint8* keys = SDL_GetKeyboardState(&keys_length);
        if (keys != nullptr) {
            for (const Field& f : bindings->keyboard[static_cast<size_t>(input)]) {
                if (f.kind == FieldKind::Key && f.index >= 0 && f.index < keys_length && keys[f.index] != 0) {
                    amount = 1.0f;
                }
            }
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Pad& pad : g_pads) {
            const Profile* profile = profile_of(pad, *bindings);
            const InputBindings& map = map_for(profile);
            const float deadzone = static_cast<float>((profile != nullptr) ? profile->deadzone_percent : DEFAULT_DEADZONE_PERCENT) / 100.0f;
            amount = std::max(amount, apply_deadzone(input_deflection(pad, map, input), deadzone));
        }
        return amount;
    }

    bool read_right_stick(int slot, float* x, float* y) {
        *x = 0.0f;
        *y = 0.0f;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (slot < 0 || slot >= static_cast<int>(g_pads.size())) {
            return false;
        }
        const Pad& pad = g_pads[static_cast<size_t>(slot)];
        if (pad.controller == nullptr) {
            return false;   // a raw pad has no named right stick
        }
        const std::shared_ptr<const Bindings> bindings = live();
        const Profile* profile = profile_of(pad, *bindings);
        const float deadzone = static_cast<float>((profile != nullptr) ? profile->deadzone_percent : DEFAULT_DEADZONE_PERCENT) / 100.0f;
        const float raw_x = static_cast<float>(SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_RIGHTX)) / 32767.0f;
        const float raw_y = static_cast<float>(SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_RIGHTY)) / 32767.0f;
        *x = apply_deadzone(std::max(-1.0f, std::min(1.0f, raw_x)), deadzone);
        *y = apply_deadzone(std::max(-1.0f, std::min(1.0f, -raw_y)), deadzone);   // SDL's y grows downward
        return true;
    }

    void set_right_stick_held_by_camera(bool held) {
        g_stick_held_by_camera.store(held, std::memory_order_relaxed);
    }

    void set_rumble(int slot, bool on) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (slot < 0 || slot >= static_cast<int>(g_pads.size())) {
            return;
        }
        Pad& pad = g_pads[static_cast<size_t>(slot)];
        if (on) {
            // A profile with rumble off turns every request into a request to stop.
            const std::shared_ptr<const Bindings> bindings = live();
            const Profile* profile = profile_of(pad, *bindings);
            if (profile != nullptr && !profile->rumble) {
                on = false;
            }
        }
        if (pad.rumble_on == on) {
            return;
        }
        pad.rumble_on = on;
        pad.rumble_refreshed_at = 0;   // the next tick pulses at once
        if (!on) {
            stop_motor(pad);
        }
        std::fprintf(stderr, "[input] rumble %s\n", on ? "on" : "off");
    }

    bool pad_rumbles(int slot) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (slot < 0 || slot >= static_cast<int>(g_pads.size())) {
            return false;
        }
        const Pad& pad = g_pads[static_cast<size_t>(slot)];
        if (!pad.info.rumbles) {
            return false;
        }
        const std::shared_ptr<const Bindings> bindings = live();
        const Profile* profile = profile_of(pad, *bindings);
        return profile == nullptr || profile->rumble;
    }

    bool pad_has_motor(int slot) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (slot < 0 || slot >= static_cast<int>(g_pads.size())) {
            return false;
        }
        return g_pads[static_cast<size_t>(slot)].info.rumbles;
    }

    // ------------------------------------------------------------------------------------------
    // Bind by pressing
    // ------------------------------------------------------------------------------------------

    namespace {

        // Writes the captured field into the live bindings and the file.
        void finish_capture(const Field& field) {
            Bindings updated = *live();
            if (g_capture.pad_slot < 0) {
                updated.keyboard[static_cast<size_t>(g_capture.input)][static_cast<size_t>(g_capture.slot)] = field;
            }
            else {
                std::string guid;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (g_capture.pad_slot >= static_cast<int>(g_pads.size())) {
                        g_capture.status = CaptureStatus::Canceled;
                        return;
                    }
                    guid = g_pads[static_cast<size_t>(g_capture.pad_slot)].info.guid;
                }
                Profile& profile = profile_for(updated, guid);
                profile.bindings[static_cast<size_t>(g_capture.input)][static_cast<size_t>(g_capture.slot)] = field;
            }
            set_live(updated);
            const bool saved = save_live();
            g_capture.status = CaptureStatus::Bound;
            g_capture.fresh = true;
            std::fprintf(stderr, "[input] capture bound: %s slot %d = %s (%s)%s\n", input_key(g_capture.input),
                         g_capture.slot, field_to_string(field).c_str(), field_name(field, g_capture.pad_slot).c_str(),
                         saved ? "" : ", file not written");
        }

        // With g_capture_mutex held.
        void cancel_locked() {
            if (g_capture.status == CaptureStatus::Waiting) {
                g_capture.status = CaptureStatus::Canceled;
                std::fprintf(stderr, "[input] capture canceled\n");
            }
        }

    } // namespace

    void start_capture(GameInput input, int slot, int pad_slot) {
        Capture c;
        c.status = CaptureStatus::Waiting;
        c.input = input;
        c.slot = std::clamp(slot, 0, BINDINGS_PER_INPUT - 1);
        c.pad_slot = pad_slot;
        c.started_at = SDL_GetTicks();
        std::lock_guard<std::mutex> capture_lock(g_capture_mutex);
        if (pad_slot >= 0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (pad_slot >= static_cast<int>(g_pads.size())) {
                std::fprintf(stderr, "[input] capture refused: no pad in slot %d\n", pad_slot);
                return;
            }
            c.pad_instance = g_pads[static_cast<size_t>(pad_slot)].instance;
            c.pad_is_controller = g_pads[static_cast<size_t>(pad_slot)].controller != nullptr;
        }
        g_capture = c;
        std::fprintf(stderr, "[input] capture: %s slot %d on %s, waiting\n", input_key(input), c.slot,
                     pad_slot < 0 ? "keyboard" : "pad");
    }

    void cancel_capture() {
        std::lock_guard<std::mutex> capture_lock(g_capture_mutex);
        cancel_locked();
    }

    CaptureStatus capture_status() {
        std::lock_guard<std::mutex> capture_lock(g_capture_mutex);
        return g_capture.status;
    }

    bool capture_event(const SDL_Event& event) {
        std::lock_guard<std::mutex> capture_lock(g_capture_mutex);
        if (g_capture.status != CaptureStatus::Waiting) {
            return false;
        }
        Field f;
        if (g_capture.pad_slot < 0) {
            if (event.type != SDL_KEYDOWN || event.key.repeat != 0) {
                return false;
            }
            if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                cancel_locked();
                return true;
            }
            f.kind = FieldKind::Key;
            f.index = static_cast<int>(event.key.keysym.scancode);
            finish_capture(f);
            return true;
        }

        // A pad SDL maps binds by SDL's names and its raw events are ignored; a raw pad binds
        // by index. Either way the event has to come from the pad the capture was armed on.
        switch (event.type) {
            case SDL_CONTROLLERBUTTONDOWN:
                if (!g_capture.pad_is_controller || event.cbutton.which != g_capture.pad_instance) return false;
                f.kind = FieldKind::ControllerButton;
                f.index = event.cbutton.button;
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (!g_capture.pad_is_controller || event.caxis.which != g_capture.pad_instance) return false;
                if (event.caxis.value >= AXIS_AS_BUTTON_THRESHOLD) f.kind = FieldKind::ControllerAxisPositive;
                else if (event.caxis.value <= -AXIS_AS_BUTTON_THRESHOLD) f.kind = FieldKind::ControllerAxisNegative;
                else return false;
                f.index = event.caxis.axis;
                break;
            case SDL_JOYBUTTONDOWN:
                if (g_capture.pad_is_controller || event.jbutton.which != g_capture.pad_instance) return false;
                f.kind = FieldKind::JoystickButton;
                f.index = event.jbutton.button;
                break;
            case SDL_JOYAXISMOTION:
                if (g_capture.pad_is_controller || event.jaxis.which != g_capture.pad_instance) return false;
                if (event.jaxis.value >= AXIS_AS_BUTTON_THRESHOLD) f.kind = FieldKind::JoystickAxisPositive;
                else if (event.jaxis.value <= -AXIS_AS_BUTTON_THRESHOLD) f.kind = FieldKind::JoystickAxisNegative;
                else return false;
                f.index = event.jaxis.axis;
                break;
            case SDL_JOYHATMOTION:
                if (g_capture.pad_is_controller || event.jhat.which != g_capture.pad_instance) return false;
                if (event.jhat.value & SDL_HAT_UP) f.hat = HatDirection::Up;
                else if (event.jhat.value & SDL_HAT_DOWN) f.hat = HatDirection::Down;
                else if (event.jhat.value & SDL_HAT_LEFT) f.hat = HatDirection::Left;
                else if (event.jhat.value & SDL_HAT_RIGHT) f.hat = HatDirection::Right;
                else return false;
                f.kind = FieldKind::JoystickHat;
                f.index = event.jhat.hat;
                break;
            default:
                return false;
        }
        finish_capture(f);
        return true;
    }

    std::string field_name(const Field& field, int pad_slot) {
        PadFamily family = PadFamily::Xbox;
        const bool named_by_pad = field.kind == FieldKind::ControllerButton ||
                                  field.kind == FieldKind::ControllerAxisPositive ||
                                  field.kind == FieldKind::ControllerAxisNegative;
        if (pad_slot >= 0 && named_by_pad) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (pad_slot < static_cast<int>(g_pads.size())) {
                family = family_of(g_pads[static_cast<size_t>(pad_slot)]);
            }
        }
        switch (field.kind) {
            case FieldKind::Key: {
                const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(field.index));
                return (name != nullptr && name[0] != '\0') ? std::string(name) : "Key " + std::to_string(field.index);
            }
            case FieldKind::ControllerButton:
                return button_name(field.index, family);
            case FieldKind::ControllerAxisPositive:
            case FieldKind::ControllerAxisNegative:
                return axis_name(field.index, field.kind == FieldKind::ControllerAxisPositive, family);
            case FieldKind::JoystickButton:       return "Button " + std::to_string(field.index);
            case FieldKind::JoystickAxisPositive: return "Axis " + std::to_string(field.index) + " +";
            case FieldKind::JoystickAxisNegative: return "Axis " + std::to_string(field.index) + " -";
            case FieldKind::JoystickHat: {
                static const char* const dirs[] = { "up", "down", "left", "right" };
                return "Hat " + std::to_string(field.index) + " " + dirs[static_cast<int>(field.hat)];
            }
            default: return "unbound";
        }
    }

} // namespace oot::input::devices
