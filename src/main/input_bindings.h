// The binding model: what the game's controls are, what a device input is, and the file that
// maps one to the other.
//
// The game has fourteen buttons (ultra64's controller.h gives each a bit), a stick with four
// directions, and the application adds one input of its own, the menu toggle. Each of those can
// be reached by up to two bindings per device class: two on the keyboard, and two on a pad. A
// pad's bindings live in a profile keyed by the GUID SDL reports for it, so two different pads
// keep their own, and a pad SDL has no mapping for is bound by its raw buttons, axes and hats.
//
// The file is `controls.txt` beside `settings.txt`, in the same grammar (`key = value`, comments,
// unknown keys ignored, every value clamped), with dotted keys rather than sections, because the
// grammar has no sections and one parser serves both files (main/keyvalue.h):
//
//     keyboard.start.0 = key:40
//     profile.<guid>.a.0 = button:0
//     profile.<guid>.deadzone = 20
//
// A binding's value names the device kind and an index: `key:<scancode>`, `button:<n>`,
// `axis+:<n>`, `axis-:<n>` (SDL's game controller indices), `jbutton:<n>`, `jaxis+:<n>`,
// `jaxis-:<n>`, `jhat:<n>:<up|down|left|right>` (a raw joystick), or `none`. Garbage reads as
// `none`; an index is clamped to 0 to 255 here and to the device's own count by the device layer.
//
// Nothing here touches SDL. The device layer (main/input.cpp) reads the live bindings and asks
// the devices; the controls document (ui/ui_controls.cpp) reads and writes them through this
// header only.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oot::input {

    // The order here IS the order in the file and on the controls document.
    enum class GameInput : int {
        A, B, Z, Start,
        DUp, DDown, DLeft, DRight,
        L, R,
        CUp, CDown, CLeft, CRight,
        StickUp, StickDown, StickLeft, StickRight,
        Menu,
        // The application's own, like Menu: no controller bit. Quick save writes slot 1 of the
        // saved moments, quick load resumes it (phase 77, .scaffold/states/).
        QuickSave, QuickLoad,
        // 2026-10-09: one screenshot, or a comparison pair (main/screenshots.h). Appended last, so
        // every earlier input keeps its number and its bit in the pad's edges.
        Screenshot,
        // 2026-10-09: the video recorder (main/video.h): start and stop, and pause and resume, as
        // two controls, so a montage is made in one file. Appended for the same reason.
        RecordVideo, PauseVideo,
        // 2026-10-09: photo mode (main/photo.h), on and off.
        PhotoMode,
        // PHOTO MODE'S CAMERA, bound apart from the game's controls (the user, 2026-10-09: "a
        // dedicated section within the controls panel ... those controls can be different than the
        // ones that are used elsewhere"), on every device the way every other row is. No
        // controller bit: photo mode reads them itself (main/photo.h, camera_input). The
        // controls screen shows Photo mode and these under a divider of their own.
        PhotoForward, PhotoBack, PhotoLeft, PhotoRight,
        PhotoTurnUp, PhotoTurnDown, PhotoTurnLeft, PhotoTurnRight,
        PhotoRaise, PhotoLower, PhotoCloser, PhotoFurther, PhotoReset,
        // The game's HUD shown or hidden while photo mode holds the game (2026-10-09).
        PhotoHud,
        // The HUD's fading (game/hud.h): brings the HUD back, or fades it, as HUD fades says. The
        // game's own, so the controls screen lists it before the Photo mode section.
        HudToggle,
        Count
    };
    constexpr int INPUT_COUNT = static_cast<int>(GameInput::Count);
    // More than 32 since photo mode's camera (2026-10-09): the pads' edge masks are 64 bit.
    static_assert(INPUT_COUNT <= 64, "the pad edge masks hold one bit per input");

    // Photo mode's own rows, the toggle and its camera, which the controls screen sets apart.
    constexpr bool is_photo_input(GameInput input) {
        return static_cast<int>(input) >= static_cast<int>(GameInput::PhotoMode) &&
               static_cast<int>(input) <= static_cast<int>(GameInput::PhotoHud);
    }
    constexpr int BINDINGS_PER_INPUT = 2;

    enum class FieldKind : int {
        None,
        Key,                       // an SDL scancode
        ControllerButton,          // SDL_GameControllerButton
        ControllerAxisPositive,    // SDL_GameControllerAxis, the positive direction
        ControllerAxisNegative,    // the negative direction
        JoystickButton,            // a raw pad's button
        JoystickAxisPositive,      // a raw pad's axis, positive
        JoystickAxisNegative,      // negative
        JoystickHat,               // a raw pad's hat, one direction
        Count
    };

    enum class HatDirection : int { Up, Down, Left, Right, Count };

    constexpr int MAX_INDEX = 255;
    constexpr int MIN_DEADZONE_PERCENT = 0;
    constexpr int MAX_DEADZONE_PERCENT = 90;
    constexpr int DEFAULT_DEADZONE_PERCENT = 20;

    struct Field {
        FieldKind kind = FieldKind::None;
        int index = 0;
        HatDirection hat = HatDirection::Up;

        bool operator==(const Field& other) const {
            return kind == other.kind && index == other.index && hat == other.hat;
        }
        bool operator!=(const Field& other) const { return !(*this == other); }
    };

    using InputBindings = std::array<std::array<Field, BINDINGS_PER_INPUT>, INPUT_COUNT>;

    struct Profile {
        std::string guid;                  // as SDL prints it, 32 hex digits
        InputBindings bindings{};
        int deadzone_percent = DEFAULT_DEADZONE_PERCENT;
        bool rumble = true;
    };

    struct Bindings {
        InputBindings keyboard{};
        std::vector<Profile> profiles;
    };

    bool operator==(const Bindings& a, const Bindings& b);

    // Names: the file's key for an input ("start"), the label the document shows ("Start"), and
    // the button bit from ultra64's controller.h (zero for the stick's directions and the menu).
    const char* input_key(GameInput input);
    const char* input_label(GameInput input);
    uint16_t input_bit(GameInput input);
    bool input_from_key(const std::string& key, GameInput& out);

    // The defaults. The keyboard's are the keys the application has used since phase 20 and
    // the harness presses; the controller's are the standard layout of an Xbox or PlayStation
    // pad as SDL names it (phase 48 puts them to use).
    void keyboard_defaults(InputBindings& out);
    void controller_defaults(InputBindings& out);

    // A field in and out of the file's spelling. Garbage reads as `none` and returns false.
    std::string field_to_string(const Field& field);
    bool field_from_string(const std::string& text, Field& out);

    // The file. `load` never fails: a missing file is the defaults, and a present one is the
    // defaults with every line it names applied and clamped. `save` writes every keyboard binding
    // and every profile in the fixed order, and says why when it cannot. `set_path` remembers
    // where the file lives so a capture or the document can `save_live` without being told.
    Bindings load(const std::string& path);
    bool save(const std::string& path, const Bindings& bindings);
    void set_path(const std::string& path);
    const std::string& path();
    bool save_live();

    // The live copy the device layer reads. Set at startup from the file, by the device layer
    // when a pad first connects (its profile), and by the controls document. Readers on the game
    // thread take a shared pointer, so a swap from the main thread never races a read.
    std::shared_ptr<const Bindings> live();
    void set_live(const Bindings& bindings);

    // The profile for a pad, created with the controller defaults on first sight.
    Profile& profile_for(Bindings& bindings, const std::string& guid);

    // --controls-selftest: a file full of every kind of value including garbage is written at
    // `temp_path`, read back, checked against the clamps, saved and read again. Returns 0 on
    // success and prints the first failure otherwise.
    int selftest(const std::string& temp_path);

} // namespace oot::input
