#pragma once

#include <string>

#include "main/input_bindings.h"

// The controls document's model: the rows it shows for the device it is showing, and the
// actions the shell applies to them. It reads and writes the bindings through
// main/input_bindings.h and asks the device layer (main/input.h) about pads and captures, and
// holds no RmlUi state of its own, so the document's behavior can be read without a renderer.
// Called on the render thread by the shell; every write goes through the model's live set,
// which is swapped whole, and the file, which one writer holds at a time.
namespace oot::ui::controls {

    enum class RowKind { Device, Binding, Deadzone, Rumble, Reset };

    struct Row {
        RowKind kind = RowKind::Binding;
        oot::input::GameInput input = oot::input::GameInput::A;
    };

    // Re-read the pads and rebuild the rows. Called when the document opens and whenever a pad
    // comes or goes. The selected device is kept when it is still there.
    void refresh();

    // The keyboard, then every connected pad by slot.
    int device_count();
    int device();
    std::string device_name(int index);
    bool device_is_pad(int index);
    bool device_is_raw(int index);
    // True when the pad identifies as an Xbox 360 controller (vendor 045e, product 028e), which
    // on this machine is almost always a real pad behind a layer (DS4Windows, Steam Input) that
    // presents a virtual Xbox 360 pad and hides the buttons the real one has and that one does
    // not: the touchpad, the PS button unless mapped. Read from SDL's GUID, whose bytes 4 to 5
    // are the vendor and 8 to 9 the product, little endian, as hex digits 8 to 11 and 16 to 19.
    bool device_through_layer(int index);

    int row_count();
    Row row(int index);
    std::string row_label(int index);

    // A binding row's slot (0 or 1) by its human name, "unbound" when empty; the other rows'
    // value: the device's name, the deadzone as a percentage, on or off, nothing for reset.
    std::string row_value(int index, int slot);

    // IS THIS BINDING BEING IGNORED RIGHT NOW, and why.
    //
    // The four C buttons sit on the right stick by default, and the free camera takes the right
    // stick while it is on, so those four are silent whenever it has the stick (the device layer
    // skips them; `set_right_stick_held_by_camera`). Nothing on screen said so, and a binding
    // that is plainly listed and does nothing reads as a broken program.
    //
    // It is SAID rather than PREVENTED, on the user's instruction (2026-09-27: "rather than
    // disabling just put a notice that they wouldn't be used. But this way users can still set
    // it", "So when they disable free cam they don't need to rebind"). Refusing the binding
    // would mean anybody turning free aim off had to go and set the four C buttons up again,
    // which is a worse trade than a line of text.
    bool slot_idle(int index, int slot);

    // One line for whatever is currently being ignored, empty when nothing is. Shown under the
    // rows, so the reason sits next to the thing it explains.
    std::string idle_note();

    // A BADGE BESIDE A BINDING THAT DOES NOTHING YET (the user, 2026-10-09: "a badge next to the photo
    // mode and video mode settings ... notifying users that those settings won't work until photo
    // mode or video recording is turned on, and then dynamically hide that once those modes are
    // on"). The program's own controls that a settings row switches on (Screenshot, Record video,
    // Pause video, Photo mode) say which row, while that row is Off; empty otherwise, and for every
    // other row. Read whenever the rows are drawn, so turning the row On takes it away.
    std::string row_badge(int index);

    // A SECTION OF ITS OWN (the user, 2026-10-09: "a dedicated section within the controls panel,
    // like after a divider, because those would explicitly ... be controlled within photo mode"):
    // the name of the section row `index` begins (Photo mode, at its toggle, with its camera's
    // rows after it), empty for every other row. Its badge stands on the divider, not on each row:
    // the setting it needs while that is Off, empty once it is On.
    std::string section_before(int index);
    std::string section_badge(int index);

    // Left and right. The device row cycles, the deadzone steps by five, rumble toggles, and
    // the change is saved. Returns true when something changed.
    bool nudge(int index, int direction);

    // Enter on a binding row arms the device layer's capture for that slot: the next press on
    // this device binds it, and the device layer saves. False for the other rows.
    bool begin_capture(int index, int slot);
    std::string capture_title(int index, int slot);

    // Delete on a binding row. The menu input keeps its last keyboard binding, so the
    // application can always be reached; false says the clear was refused for that reason.
    bool clear(int index, int slot);

    // Enter on the reset row: the device's defaults (the keyboard's keys, the standard pad
    // layout, nothing at all for a pad SDL has no mapping for), saved.
    void reset_defaults();

    // The startup hint's two lines, from the live bindings each time: the keyboard's menu key
    // by name, and the first pad's menu button when a pad is connected.
    void menu_shortcut(std::string& lead, std::string& note);

} // namespace oot::ui::controls
