// The device layer: the pads SDL sees, read through the binding model.
//
// A pad SDL recognizes (its own tables, or the community database in assets/input/) is opened as
// a game controller and its buttons and axes are named the way SDL names them; a pad it does not
// recognize is opened as a raw joystick and offers its buttons, axes and hats by index. Either
// way the pad's bindings come from its profile in the model (main/input_bindings.h), keyed by
// the GUID SDL reports, created from the standard layout the first time the pad is seen.
//
// Everything here runs on the main thread (SDL's event pump, `update_gfx` in boot.cpp) except
// `read_pad` and `set_rumble`, which the runtime calls on the game thread; the pad list is
// behind a mutex for that reason and every call into SDL from the game thread is one SDL
// documents as safe from any thread (state reads and rumble).
//
// A virtual joystick, attached on `--virtual-controller <script>`, is the hardware-free way the
// harness exercises this layer: the script is a list of timed button, axis and hat events the
// layer applies from inside the process. Its profile is keyed "virtual" rather than by GUID so
// the harness can write bindings for it before the game starts.

#pragma once

#include <cstdint>
#include <string>

#include "main/input_bindings.h"

union SDL_Event;

namespace oot::input::devices {

    struct PadInfo {
        std::string name;
        std::string guid;        // as the model keys it: SDL's 32 hex digits, or "virtual"
        std::string mapping;     // "built in", "database", "virtual" or "raw"
        bool rumbles = false;
    };

    // Loads the mapping database (a count in the trace), attaches the virtual joystick when a
    // script path is given (as a game controller SDL maps, or as a raw joystick when `virtual_raw`
    // is set, which is how a pad SDL does not know is tested), and opens every pad already
    // present. Called once, after SDL is up and the bindings are loaded.
    void init(const std::string& database_path, const std::string& virtual_script_path, bool virtual_raw);

    // Bind by pressing. `start_capture` arms a capture for one input's slot on the keyboard
    // (`pad_slot` of -1) or on a pad; the first qualifying event within five seconds becomes the
    // binding, the live bindings are updated and the file saved. Escape cancels a keyboard
    // capture. An axis binds one direction once it passes half its travel; a hat binds one
    // direction. On a pad SDL maps, buttons and axes bind by SDL's names; on a raw pad by index.
    enum class CaptureStatus { Idle, Waiting, Bound, Canceled, TimedOut };
    void start_capture(GameInput input, int slot, int pad_slot);
    void cancel_capture();
    CaptureStatus capture_status();

    // The pump hands every key and pad event here first; true means a capture consumed it.
    bool capture_event(const SDL_Event& event);

    // A field's name for a person: "P", "Return", "A button", "Left trigger", "Button 6",
    // "Hat 0 up", or "unbound". Given a pad slot, a pad's buttons are named as they are
    // printed on that pad (Cross and L1 on a PlayStation pad, B and ZL on a Nintendo one)
    // rather than after SDL's standard layout, which names everything as an Xbox pad.
    std::string field_name(const Field& field, int pad_slot = -1);

    // The inputs that went from released to pressed on the pad since the last call, as a mask
    // over GameInput (bit 1 << input), found through the pad's profile once per tick. The pump
    // turns these into the documents' navigation and the menu toggle, the way it does key
    // edges; get_input reads state, and would see a held button every frame. Nothing is
    // reported while a capture is listening, nor in the tick that bound one, so the press
    // that binds a button never also navigates.
    uint64_t take_pad_edges(int slot);

    // Device added and removed events from the pump. Anything else is ignored.
    void handle_event(const SDL_Event& event);

    // Once per pump: the virtual script's due events, and the rumble refresh.
    void tick();

    int pad_count();
    bool pad_info(int slot, PadInfo& out);

    // The pad in `slot` (0 is the first connected) read through its profile: the button word
    // and the stick after the deadzone, in the game's sense (y up). False when no pad is there.
    bool read_pad(int slot, uint16_t* buttons, float* x, float* y);

    // THE FREE CAMERA'S STICK (upgrades phase 79, patches/free_camera.c). The right stick of the
    // pad in `slot`, raw from SDL's standard layout rather than through the bindings, past the
    // profile's deadzone, right and up positive; false, and zero, without a pad SDL maps.
    bool read_right_stick(int slot, float* x, float* y);

    // How far an input is pressed, 0 to 1, the most of any device: a key bound to it on the
    // keyboard, and on every pad its profile's binding, a stick direction past that pad's
    // deadzone. For inputs that are not the game's controller (photo mode's camera, main/photo.h),
    // which read their own bindings rather than the controller the game sees.
    float input_amount(GameInput input);

    // While the camera holds the right stick, the C button bindings that sit on it are silent
    // in read_pad, so a turn of the camera is not also an item. The patch says so once a frame;
    // it says no with the ocarina out, when the notes need the stick.
    void set_right_stick_held_by_camera(bool held);

    // Rumble as the game sees it: a motor the pad has AND a profile that allows it, so a pad
    // whose rumble is turned off in the controls document reports no Rumble Pak and is never
    // asked. `pad_has_motor` is the hardware alone, for the document's own row.
    void set_rumble(int slot, bool on);
    bool pad_rumbles(int slot);
    bool pad_has_motor(int slot);

} // namespace oot::input::devices
