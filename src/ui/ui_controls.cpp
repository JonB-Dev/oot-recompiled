#include "ui/ui_controls.h"

#include "main/input.h"
#include "ui/ui_settings.h"
#include "ui/input/ui_input.h"

// For the right stick's axis numbers, which is all this needs of SDL.
#include <SDL_gamecontroller.h>

#include <algorithm>
#include <cstdio>
#include <functional>
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

        // The stick deadzone row's step, and the gyro sensitivity row's, in percent.
        constexpr int DEADZONE_STEP = 5;
        constexpr int GYRO_SENSITIVITY_STEP = 25;

        // The Gyro axes row's choices, in the aiming rows' words (ui_settings.cpp, aimaxes).
        constexpr const char* GYRO_AXES[GYRO_AXES_COUNT] = {
            "As the controller turns", "Invert left and right", "Invert up and down", "Invert both" };

        bool is_pad(int index) {
            return index > 0 && index < static_cast<int>(g_devices.size());
        }

        int pad_slot_of(int index) {
            return is_pad(index) ? g_devices[static_cast<size_t>(index)].pad_slot : -1;
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

        InputBindings current_bindings(const Bindings& all, Profile& own);

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
                // Gyro aiming, for a pad with a gyroscope; Gyro look, its sensitivity and axes while it is on.
                if (devices::pad_has_gyro(pad_slot_of(g_device))) {
                    g_rows.push_back(Row{ RowKind::GyroAim, GameInput::A });
                    Profile own;
                    (void)current_bindings(*live(), own);
                    if (own.gyro) {
                        g_rows.push_back(Row{ RowKind::GyroLook, GameInput::A });
                        g_rows.push_back(Row{ RowKind::GyroSensitivity, GameInput::A });
                        g_rows.push_back(Row{ RowKind::GyroAxes, GameInput::A });
                    }
                }
                g_rows.push_back(Row{ RowKind::Rumble, GameInput::A });
            }
            g_rows.push_back(Row{ RowKind::Reset, GameInput::A });
        }

        // What the selected device reads: the keyboard's table, or the pad's profile, whose own
        // settings (deadzone, rumble, gyro) are copied to `own`; the keyboard's are the defaults.
        // A pad whose profile has not been written yet shows the standard layout, and a raw pad
        // shows nothing bound, which is what the device layer gives each on first sight.
        InputBindings current_bindings(const Bindings& all, Profile& own) {
            own = Profile{};
            if (!is_pad(g_device)) {
                return all.keyboard;
            }
            const Device& d = g_devices[static_cast<size_t>(g_device)];
            for (const Profile& p : all.profiles) {
                if (p.guid == d.guid) {
                    own = p;
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
        // profile, created when it is missing. `bindings`, when given, replaces the device's;
        // `change`, when given, edits a pad's own settings.
        void store(const InputBindings* bindings, const std::function<void(Profile&)>& change = {}) {
            Bindings updated = *live();
            if (!is_pad(g_device)) {
                if (bindings != nullptr) {
                    updated.keyboard = *bindings;
                }
            }
            else {
                Profile& p = profile_for(updated, g_devices[static_cast<size_t>(g_device)].guid);
                if (bindings != nullptr) p.bindings = *bindings;
                if (change) change(p);
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
            return pad_slot_of(g_device);
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
        bool kept = keep.empty();
        if (!keep.empty()) {
            for (size_t i = 0; i < g_devices.size(); ++i) {
                if (g_devices[i].guid == keep) {
                    g_device = static_cast<int>(i);
                    kept = true;
                }
            }
        }
        rebuild_rows();
        // The selected pad is gone: the device in use, if it is still here, else the keyboard.
        if (!kept) {
            select_in_use(oot::ui::input::last_pad_instance());
        }
    }

    void select_in_use(int pad_instance) {
        int chosen = 0;
        if (pad_instance >= 0) {
            for (size_t i = 1; i < g_devices.size(); ++i) {
                devices::UiPad pad;
                if (devices::ui_pad(g_devices[i].pad_slot, pad) && pad.instance == pad_instance) {
                    chosen = static_cast<int>(i);
                    break;
                }
            }
        }
        if (chosen != g_device) {
            g_device = chosen;
            rebuild_rows();
        }
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
            case RowKind::GyroAim:  return "Gyro aiming";
            case RowKind::GyroLook: return "Gyro look";
            case RowKind::GyroSensitivity: return "Gyro sensitivity";
            case RowKind::GyroAxes: return "Gyro axes";
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
        Profile own;
        const InputBindings bindings = current_bindings(*live(), own);

        switch (r.kind) {
            case RowKind::Device:
                return device_name(g_device);
            case RowKind::Binding: {
                const Field& f = bindings[static_cast<size_t>(r.input)][static_cast<size_t>(clamp_slot(slot))];
                return devices::field_name(f, pad_slot_of_selected());
            }
            case RowKind::Deadzone:
                return std::to_string(own.deadzone_percent) + "%";
            case RowKind::GyroAim:
                return own.gyro ? "on" : "off";
            case RowKind::GyroLook:
                return own.gyro_look ? "on" : "off";
            case RowKind::GyroSensitivity:
                return std::to_string(own.gyro_sensitivity_percent) + "%";
            case RowKind::GyroAxes:
                return GYRO_AXES[std::clamp(own.gyro_axes, 0, GYRO_AXES_COUNT - 1)];
            case RowKind::Rumble: {
                if (is_pad(g_device) && !devices::pad_has_motor(pad_slot_of_selected())) {
                    return own.rumble ? "on, no motor" : "off";
                }
                return own.rumble ? "on" : "off";
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
        Profile own;
        const InputBindings bindings = current_bindings(*live(), own);
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
        const auto needs = [](GameInput input) -> std::string {
            switch (input) {
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
        };
        const std::string need = needs(r.input);
        if (!need.empty()) {
            return need;
        }
        // A KEY OR BUTTON TWO CONTROLS SHARE, read at the same time (input_bindings.h, clashes): one
        // press does both. A new binding is moved rather than shared; this shows the ones from before.
        Profile own;
        const InputBindings map = current_bindings(*live(), own);
        for (int s = 0; s < BINDINGS_PER_INPUT; ++s) {
            const Field& f = map[static_cast<size_t>(r.input)][static_cast<size_t>(s)];
            for (const BindingSlot& c : clashes(map, r.input, s, f)) {
                if (c.input != r.input) {
                    return std::string("Shared with ") + input_label(c.input);
                }
            }
        }
        return "";
    }

    std::string bound_line(int index, int slot) {
        std::string line = "Bound to " + row_value(index, slot);
        const std::vector<BindingSlot> moved = devices::capture_moved();
        for (size_t i = 0; i < moved.size(); ++i) {
            line += (i == 0) ? ", taken off " : " and ";
            line += input_label(moved[i].input);
        }
        return line;
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
        Profile own;
        (void)current_bindings(*live(), own);
        // A stepped value moved by its step and kept in range; false when it was already at the end.
        const auto step = [direction](int value, int by, int low, int high, int& next) {
            next = std::clamp(value + (direction > 0 ? by : -by), low, high);
            return next != value;
        };
        int next = 0;

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
            case RowKind::Deadzone:
                if (!step(own.deadzone_percent, DEADZONE_STEP, MIN_DEADZONE_PERCENT, MAX_DEADZONE_PERCENT, next)) {
                    return false;
                }
                store(nullptr, [next](Profile& p) { p.deadzone_percent = next; });
                return true;
            case RowKind::GyroAim:
                store(nullptr, [on = !own.gyro](Profile& p) { p.gyro = on; });
                // Gyro look, its sensitivity and axes come and go with it.
                rebuild_rows();
                return true;
            case RowKind::GyroLook:
                store(nullptr, [on = !own.gyro_look](Profile& p) { p.gyro_look = on; });
                return true;
            case RowKind::GyroSensitivity:
                if (!step(own.gyro_sensitivity_percent, GYRO_SENSITIVITY_STEP, MIN_GYRO_SENSITIVITY_PERCENT,
                          MAX_GYRO_SENSITIVITY_PERCENT, next)) {
                    return false;
                }
                store(nullptr, [next](Profile& p) { p.gyro_sensitivity_percent = next; });
                return true;
            case RowKind::GyroAxes:
                if (!step(own.gyro_axes, 1, 0, GYRO_AXES_COUNT - 1, next)) {
                    return false;
                }
                store(nullptr, [next](Profile& p) { p.gyro_axes = next; });
                return true;
            case RowKind::Rumble:
                store(nullptr, [on = !own.rumble](Profile& p) { p.rumble = on; });
                return true;
            default:
                return false;
        }
    }

    rows::Row device_view(int n, const std::string& name, int device, int count) {
        rows::Row row;
        row.n = n;
        row.label = "Device";
        row.value = name;
        rows::draw_options(row, count, device, [](int) -> const char* { return nullptr; });
        return row;
    }

    rows::Row deadzone_view(int n, int percent) {
        rows::Row row;
        row.n = n;
        row.label = "Stick deadzone";
        row.value = std::to_string(percent) + "%";
        row.low = percent <= MIN_DEADZONE_PERCENT;
        row.high = percent >= MAX_DEADZONE_PERCENT;
        rows::draw_options(row, (MAX_DEADZONE_PERCENT - MIN_DEADZONE_PERCENT) / DEADZONE_STEP + 1,
                           (percent - MIN_DEADZONE_PERCENT) / DEADZONE_STEP, [](int) -> const char* { return nullptr; });
        return row;
    }

    rows::Row gyro_view(int n, bool on) {
        rows::Row row;
        row.n = n;
        row.label = "Gyro aiming";
        row.value = on ? "On" : "Off";
        rows::draw_options(row, 2, on ? 1 : 0, [](int i) -> const char* { return i == 0 ? "Off" : "On"; });
        row.note = "Turn the controller to aim the bow, the slingshot, the hookshot and the boomerang";
        return row;
    }

    rows::Row gyro_look_view(int n, bool on) {
        rows::Row row;
        row.n = n;
        row.label = "Gyro look";
        row.value = on ? "On" : "Off";
        rows::draw_options(row, 2, on ? 1 : 0, [](int i) -> const char* { return i == 0 ? "Off" : "On"; });
        row.note = "Turn the controller to look around in first person with C up, as well as to aim";
        return row;
    }

    rows::Row gyro_sensitivity_view(int n, int percent) {
        rows::Row row;
        row.n = n;
        row.label = "Gyro sensitivity";
        row.value = std::to_string(percent) + "%";
        row.low = percent <= MIN_GYRO_SENSITIVITY_PERCENT;
        row.high = percent >= MAX_GYRO_SENSITIVITY_PERCENT;
        rows::draw_options(row, (MAX_GYRO_SENSITIVITY_PERCENT - MIN_GYRO_SENSITIVITY_PERCENT) / GYRO_SENSITIVITY_STEP + 1,
                           (percent - MIN_GYRO_SENSITIVITY_PERCENT) / GYRO_SENSITIVITY_STEP, [](int) -> const char* { return nullptr; });
        row.note = "At 100% the aim turns as far as the controller does";
        return row;
    }

    rows::Row gyro_axes_view(int n, int axes) {
        rows::Row row;
        row.n = n;
        row.label = "Gyro axes";
        const int chosen = std::clamp(axes, 0, GYRO_AXES_COUNT - 1);
        row.value = GYRO_AXES[chosen];
        rows::draw_options(row, GYRO_AXES_COUNT, chosen, [](int i) -> const char* { return GYRO_AXES[i]; });
        return row;
    }

    rows::Row rumble_view(int n, bool on, bool motor) {
        rows::Row row;
        row.n = n;
        row.label = "Rumble";
        row.value = on ? "On" : "Off";
        rows::draw_options(row, 2, on ? 1 : 0, [](int i) -> const char* { return i == 0 ? "Off" : "On"; });
        if (!motor) {
            row.note = "This controller reports no rumble motor";
        }
        return row;
    }

    rows::Row reset_view(int n) {
        rows::Row row;
        row.n = n;
        row.label = "Reset to defaults";
        row.value = "Puts this device's bindings back as they came";
        row.shape = rows::Action;
        return row;
    }

    namespace {
        // The template's rows for the device shown: the device above the bindings (top), or what
        // follows them (the foot).
        std::vector<rows::Row> views(bool top) {
            Profile own;
            (void)current_bindings(*live(), own);
            const bool motor = !is_pad(g_device) || devices::pad_has_motor(pad_slot_of_selected());
            std::vector<rows::Row> out;
            for (int i = 0; i < row_count(); ++i) {
                switch (g_rows[static_cast<size_t>(i)].kind) {
                    case RowKind::Device:
                        if (top) out.push_back(device_view(i, device_name(g_device), g_device, device_count()));
                        break;
                    case RowKind::Deadzone:
                        if (!top) out.push_back(deadzone_view(i, own.deadzone_percent));
                        break;
                    case RowKind::GyroAim:
                        if (!top) out.push_back(gyro_view(i, own.gyro));
                        break;
                    case RowKind::GyroLook:
                        if (!top) out.push_back(gyro_look_view(i, own.gyro_look));
                        break;
                    case RowKind::GyroSensitivity:
                        if (!top) out.push_back(gyro_sensitivity_view(i, own.gyro_sensitivity_percent));
                        break;
                    case RowKind::GyroAxes:
                        if (!top) out.push_back(gyro_axes_view(i, own.gyro_axes));
                        break;
                    case RowKind::Rumble:
                        if (!top) out.push_back(rumble_view(i, own.rumble, motor));
                        break;
                    case RowKind::Reset:
                        if (!top) out.push_back(reset_view(i));
                        break;
                    case RowKind::Binding:
                        break;
                }
            }
            return out;
        }
    }

    std::vector<rows::Row> top_views() {
        return views(true);
    }

    std::vector<rows::Row> foot_views() {
        return views(false);
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
        Profile own;
        InputBindings bindings = current_bindings(*live(), own);

        if (!is_pad(g_device) && input == GameInput::Menu) {
            // The other slot has to hold something, or the menu could never be opened again
            // from the keyboard and the only way back would be editing the file.
            const Field& other = bindings[static_cast<size_t>(input)][static_cast<size_t>(1 - s)];
            if (other.kind == FieldKind::None) {
                return false;
            }
        }
        bindings[static_cast<size_t>(input)][static_cast<size_t>(s)] = Field{};
        store(&bindings);
        return true;
    }

    void reset_defaults() {
        InputBindings defaults{};
        if (!is_pad(g_device)) {
            keyboard_defaults(defaults);
            store(&defaults);
            return;
        }
        if (!g_devices[static_cast<size_t>(g_device)].raw) {
            controller_defaults(defaults);
        }
        // The pad's own settings go back as they came too: the deadzone, rumble and gyro aiming.
        store(&defaults, [](Profile& p) { p = Profile{ p.guid, p.bindings }; });
        rebuild_rows();
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
