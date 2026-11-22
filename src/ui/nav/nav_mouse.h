#pragma once

// THE MOUSE (the menus plan, phase 93), first class beside the pads and the keys.
//
//   Pointing focuses: the item under the pointer takes the focus, so a pad or the keys pick up
//   exactly where the mouse left off.
//   A click is a press and a release on the SAME item; a press that slides off before the release
//   does nothing, the way a button on any desktop behaves.
//   The wheel scrolls the innermost scrolling group under the pointer.
//   Under an open dialog nothing can be pointed at or clicked: a press outside the dialog is
//   swallowed, never passed to what is beneath it.
//   The mouse's back button is Back (the shell's).
//
// The focus ring belongs to the pads and the keys: it shows while one of them is the last device
// and hides while the mouse is, which shows its own hover instead (pointer_mode).

#include "ui/input/ui_input_core.h"
#include "ui/nav/nav.h"

namespace oot::ui::nav {

    // The reachable item under the point, or -1. Where items nest or overlap, the later in
    // document order (drawn on top) wins.
    int hit(const Model& model, float x, float y);

    enum class Click { None, Activate, Swallowed };

    struct Pointer {
        int pressed = -1;   // the item the press landed on
    };

    // A button went down at (x, y). Swallowed when a dialog is open and the point is outside it.
    Click press(const Model& model, Pointer& pointer, float x, float y);

    // The button came up at (x, y). Activate, with `target` set, when it came up on the item it
    // went down on.
    Click release(const Model& model, Pointer& pointer, float x, float y, int& target);

    // The innermost scrolling group under the point in the live layer, or -1.
    int wheel_target(const Model& model, float x, float y);

    // True while the mouse is the device in use: hover shows, the focus ring does not.
    inline bool pointer_mode(input::Device device) { return device == input::Device::Mouse; }

    bool mouse_selftest(int& passed);

}
