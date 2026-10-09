#include "ui/ui_controls.h"

#include "main/input.h"
#include "ui/ui_settings.h"

// For the right stick's axis numbers, which is all this needs of SDL.
#include <SDL_gamecontroller.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace oot::ui::controls {

    namespace {

        using namespace oot::input;

        struct Device {
            std::string name;
            std::string guid;      // empty for the keyboard
            bool raw = false;
            int pad_slot = -1;     // -1 for the keyboard
        };

        std::vector<Device> g_devices;
        int g_device = 0;
        std::vector<Row> g_rows;

        bool is_pad(int index) {
            return index > 0 && index < static_cast<int>(g_devices.size());
        }

        // The four the free camera can take the stick out from under.
        bool c_button(GameInput input) {
            return input == GameInput::CUp || input == GameInput::CDown ||
                   input == GameInput::CLeft || input == GameInput::CRight;
        }

        // A field on either axis of the right stick, which is the stick the free camera holds.
        // A C button on a face button or a trigger is unaffected and stays live throughout.
        bool right_stick_field(const Field& f) {
            const bool axis = f.kind == FieldKind::ControllerAxisPositive ||
                              f.kind == FieldKind::ControllerAxisNegative;
            return axis && (f.index == SDL_CONTROLLER_AXIS_RIGHTX || f.index == SDL_CONTROLLER_AXIS_RIGHTY);
        }

        void rebuild_rows() {
            g_rows.clear();
            g_rows.push_back(Row{ RowKind::Device, GameInput::A });
            // The game's and the program's controls first, then Photo mode's section (2026-10-09),
            // whatever their order in the enumeration, which appending keeps.
            for (int pass = 0; pass < 2; ++pass) {
                for (int i = 0; i < INPUT_COUNT; ++i) {
                    const GameInput input = static_cast<GameInput>(i);
                    if (is_photo_input(input) == (pass == 1)) {
                        g_rows.push_back(Row{ RowKind::Binding, input });
                    }
                }
            }
            if (is_pad(g_device)) {
                g_rows.push_back(Row{ RowKind::Deadzone, GameInput::A });
                g_rows.push_back(Row{ RowKind::Rumble, GameInput::A });
            }
            g_rows.push_back(Row{ RowKind::Reset, GameInput::A });
        }

        // What the selected device reads: the keyboard's table, or the pad's profile. A pad
        // whose profile has not been written yet shows the standard layout, and a raw pad
        // shows nothing bound, which is what the device layer gives each on first sight.
        InputBindings current_bindings(const Bindings& all, int* deadzone, bool* rumble) {
            *deadzone = DEFAULT_DEADZONE_PERCENT;
            *rumble = true;
            if (!is_pad(g_device)) {
                return all.keyboard;
            }
            const Device& d = g_devices[static_cast<size_t>(g_device)];
            for (const Profile& p : all.profiles) {
                if (p.guid == d.guid) {
                    *deadzone = p.deadzone_percent;
                    *rumble = p.rumble;
                    return p.bindings;
                }
            }
            InputBindings fresh{};
            if (!d.raw) {
                controller_defaults(fresh);
            }
            return fresh;
        }

        // Write through to the live set and the file: the keyboard's table, or the pad's
        // profile, created when it is missing. Each pointer that is null leaves that part alone.
        void store(const InputBindings* bindings, const int* deadzone, const bool* rumble) {
            Bindings updated = *live();
            if (!is_pad(g_device)) {
                if (bindings != nullptr) {
                    updated.keyboard = *bindings;
                }
            }
            else {
                Profile& p = profile_for(updated, g_devices[static_cast<size_t>(g_device)].guid);
                if (bindings != nullptr) p.bindings = *bindings;
                if (deadzone != nullptr) p.deadzone_percent = *deadzone;
                if (rumble != nullptr) p.rumble = *rumble;
            }
            set_live(updated);
            if (!save_live()) {
                std::fprintf(stderr, "[ui] controls: the bindings file could not be written; the change is live until exit\n");
            }
        }

        bool valid(int index) {
            return index >= 0 && index < static_cast<int>(g_rows.size());
        }

        int clamp_slot(int slot) {
            return std::clamp(slot, 0, BINDINGS_PER_INPUT - 1);
        }

        int pad_slot_of_selected() {
            return is_pad(g_device) ? g_devices[static_cast<size_t>(g_device)].pad_slot : -1;
        }

    } // namespace

    void refresh() {
        const std::string keep = (g_device >= 0 && g_device < static_cast<int>(g_devices.size()))
                                     ? g_devices[static_cast<size_t>(g_device)].guid
                                     : std::string();
        g_devices.clear();
        g_devices.push_back(Device{ "Keyboard", "", false, -1 });
        const int pads = devices::pad_count();
        for (int slot = 0; slot < pads; ++slot) {
            devices::PadInfo info;
            if (devices::pad_info(slot, info)) {
                g_devices.push_back(Device{ info.name, info.guid, info.mapping == "raw", slot });
            }
        }
        g_device = 0;
        if (!keep.empty()) {
            for (size_t i = 0; i < g_devices.size(); ++i) {
                if (g_devices[i].guid == keep) {
                    g_device = static_cast<int>(i);
                }
            }
        }
        rebuild_rows();
    }

    int device_count() {
        return static_cast<int>(g_devices.size());
    }

    int device() {
        return g_device;
    }

    std::string device_name(int index) {
        if (index < 0 || index >= static_cast<int>(g_devices.size())) {
            return "";
        }
        return g_devices[static_cast<size_t>(index)].name;
    }

    bool device_is_pad(int index) {
        return is_pad(index);
    }

    bool device_through_layer(int index) {
        if (!is_pad(index)) {
            return false;
        }
        const std::string& guid = g_devices[static_cast<size_t>(index)].guid;
        return guid.size() == 32 && guid.compare(8, 4, "5e04") == 0 && guid.compare(16, 4, "8e02") == 0;
    }

    bool device_is_raw(int index) {
        return is_pad(index) && g_devices[static_cast<size_t>(index)].raw;
    }

    int row_count() {
        return static_cast<int>(g_rows.size());
    }

    Row row(int index) {
        return valid(index) ? g_rows[static_cast<size_t>(index)] : Row{};
    }

    std::string row_label(int index) {
        if (!valid(index)) {
            return "";
        }
        const Row& r = g_rows[static_cast<size_t>(index)];
        switch (r.kind) {
            case RowKind::Device:   return "Device";
            case RowKind::Binding:  return input_label(r.input);
            case RowKind::Deadzone: return "Stick deadzone";
            case RowKind::Rumble:   return "Rumble";
            case RowKind::Reset:    return "Reset to defaults";
        }
        return "";
    }

    std::string row_value(int index, int slot) {
        if (!valid(index)) {
            return "";
        }
        const Row& r = g_rows[static_cast<size_t>(index)];
        const std::shared_ptr<const Bindings> all = live();
        int deadzone = DEFAULT_DEADZONE_PERCENT;
        bool rumble = true;
        const InputBindings bindings = current_bindings(*all, &deadzone, &rumble);

        switch (r.kind) {
            case RowKind::Device:
                return device_name(g_device);
            case RowKind::Binding: {
                const Field& f = bindings[static_cast<size_t>(r.input)][static_cast<size_t>(clamp_slot(slot))];
                return devices::field_name(f, pad_slot_of_selected());
            }
            case RowKind::Deadzone:
                return std::to_string(deadzone) + "%";
            case RowKind::Rumble: {
                if (is_pad(g_device) && !devices::pad_has_motor(pad_slot_of_selected())) {
                    return rumble ? "on, no motor" : "off";
                }
                return rumble ? "on" : "off";
            }
            case RowKind::Reset:
                return "";
        }
        return "";
    }

    bool slot_idle(int index, int slot) {
        if (!valid(index) || !is_pad(g_device)) {
            return false;
        }
        const Row& r = g_rows[static_cast<size_t>(index)];
        if (r.kind != RowKind::Binding || !c_button(r.input)) {
            return false;
        }
        // Bit 1 is the Free camera row. The aiming bits say how it behaves, not whether it is on.
        if ((settings().free_camera != 1)) {
            return false;
        }
        const std::shared_ptr<const Bindings> all = live();
        int deadzone = DEFAULT_DEADZONE_PERCENT;
        bool rumble = true;
        const InputBindings bindings = current_bindings(*all, &deadzone, &rumble);
        return right_stick_field(bindings[static_cast<size_t>(r.input)][static_cast<size_t>(clamp_slot(slot))]);
    }

    std::string row_badge(int index) {
        if (!valid(index)) {
            return "";
        }
        const Row& r = g_rows[static_cast<size_t>(index)];
        if (r.kind != RowKind::Binding) {
            return "";
        }
        switch (r.input) {
            case GameInput::Screenshot:
                return screenshots_enabled() ? "" : "Needs Screenshots on";
            case GameInput::RecordVideo:
            case GameInput::PauseVideo:
                return video_recording_enabled() ? "" : "Needs Video recording on";
            case GameInput::HudToggle:
                return (settings().hud_fade != 0) ? "" : "Needs HUD fades set";
            default:
                // Photo mode's rows say it on their section's divider (section_badge).
                return "";
        }
    }

    std::string section_before(int index) {
        if (!valid(index)) {
            return "";
        }
        const Row& r = g_rows[static_cast<size_t>(index)];
        return (r.kind == RowKind::Binding && r.input == GameInput::PhotoMode) ? "Photo mode" : "";
    }

    std::string section_badge(int index) {
        if (section_before(index).empty()) {
            return "";
        }
        return photo_mode_enabled() ? "" : "Needs Photo mode on";
    }

    std::string idle_note() {
        for (int i = 0; i < row_count(); ++i) {
            for (int s = 0; s < BINDINGS_PER_INPUT; ++s) {
                if (slot_idle(i, s)) {
                    return "The right stick belongs to free aim, so the C buttons on it are not used "
                           "while it is on. They come back when free aim is off; nothing needs rebinding.";
                }
            }
        }
        return "";
    }

    bool nudge(int index, int direction) {
        if (!valid(index) || direction == 0) {
            return false;
        }
        const Row& r = g_rows[static_cast<size_t>(index)];
        const std::shared_ptr<const Bindings> all = live();
        int deadzone = DEFAULT_DEADZONE_PERCENT;
        bool rumble = true;
        (void)current_bindings(*all, &deadzone, &rumble);

        switch (r.kind) {
            case RowKind::Device: {
                const int count = static_cast<int>(g_devices.size());
                if (count < 2) {
                    return false;
                }
                g_device = (g_device + (direction > 0 ? 1 : count - 1)) % count;
                rebuild_rows();
                return true;
            }
            case RowKind::Deadzone: {
                const int next = std::clamp(deadzone + (direction > 0 ? 5 : -5), MIN_DEADZONE_PERCENT, MAX_DEADZONE_PERCENT);
                if (next == deadzone) {
                    return false;
                }
                store(nullptr, &next, nullptr);
                return true;
            }
            case RowKind::Rumble: {
                const bool next = !rumble;
                store(nullptr, nullptr, &next);
                return true;
            }
            default:
                return false;
        }
    }

    bool begin_capture(int index, int slot) {
        if (!valid(index) || g_rows[static_cast<size_t>(index)].kind != RowKind::Binding) {
            return false;
        }
        devices::start_capture(g_rows[static_cast<size_t>(index)].input, clamp_slot(slot), pad_slot_of_selected());
        return true;
    }

    std::string capture_title(int index, int slot) {
        if (!valid(index)) {
            return "";
        }
        std::string title = "Rebind ";
        title += input_label(g_rows[static_cast<size_t>(index)].input);
        if (clamp_slot(slot) == 1) {
            title += ", second binding";
        }
        return title;
    }

    bool clear(int index, int slot) {
        if (!valid(index) || g_rows[static_cast<size_t>(index)].kind != RowKind::Binding) {
            return false;
        }
        const GameInput input = g_rows[static_cast<size_t>(index)].input;
        const int s = clamp_slot(slot);
        const std::shared_ptr<const Bindings> all = live();
        int deadzone = DEFAULT_DEADZONE_PERCENT;
        bool rumble = true;
        InputBindings bindings = current_bindings(*all, &deadzone, &rumble);

        if (!is_pad(g_device) && input == GameInput::Menu) {
            // The other slot has to hold something, or the menu could never be opened again
            // from the keyboard and the only way back would be editing the file.
            const Field& other = bindings[static_cast<size_t>(input)][static_cast<size_t>(1 - s)];
            if (other.kind == FieldKind::None) {
                return false;
            }
        }
        bindings[static_cast<size_t>(input)][static_cast<size_t>(s)] = Field{};
        store(&bindings, nullptr, nullptr);
        return true;
    }

    void reset_defaults() {
        InputBindings defaults{};
        int deadzone = DEFAULT_DEADZONE_PERCENT;
        bool rumble = true;
        if (!is_pad(g_device)) {
            keyboard_defaults(defaults);
            store(&defaults, nullptr, nullptr);
            return;
        }
        if (!g_devices[static_cast<size_t>(g_device)].raw) {
            controller_defaults(defaults);
        }
        store(&defaults, &deadzone, &rumble);
    }

    void menu_shortcut(std::string& lead, std::string& note) {
        const std::shared_ptr<const Bindings> all = live();
        std::string keys;
        for (const Field& f : all->keyboard[static_cast<size_t>(GameInput::Menu)]) {
            if (f.kind == FieldKind::None) {
                continue;
            }
            if (!keys.empty()) {
                keys += " or ";
            }
            keys += devices::field_name(f, -1);
        }
        if (keys.empty()) {
            keys = "No key";   // the model keeps one; this is what shows if it ever did not
        }
        lead = keys + " opens the menu";
        note = "Any time in play";

        // EVERY WAY THE PAD CAN OPEN IT, not just the first one found. A DualShock or DualSense
        // has two by default now, Back and the touchpad click (the user's ask of 2026-09-23), and
        // naming only one of them tells half the story to the person most likely to press the
        // other. Listed in the order they are bound, joined so the sentence still reads.
        devices::PadInfo info;
        if (devices::pad_count() > 0 && devices::pad_info(0, info)) {
            for (const Profile& p : all->profiles) {
                if (p.guid != info.guid) {
                    continue;
                }
                std::string pad;
                for (const Field& f : p.bindings[static_cast<size_t>(GameInput::Menu)]) {
                    if (f.kind == FieldKind::None) {
                        continue;
                    }
                    pad += (pad.empty() ? "" : " or ") + devices::field_name(f, 0);
                }
                if (!pad.empty()) {
                    note += ", or " + pad + " on the pad";
                }
                return;
            }
        }
    }

} // namespace oot::ui::controls
