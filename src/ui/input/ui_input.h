#pragma once

// ONE INPUT STREAM FOR THE INTERFACE (the menus plan, phase 90): the SDL side. The pump hands
// every key edge and every mouse event here, and calls pump() once per pass; every connected pad
// is read (not only the first), a held direction repeats on the tokens' schedule, and the device
// that spoke last is kept by family for the prompts. The logic is in ui_input_core.h.
//
// Keys: the arrows and W A S D move, Enter and X select, Escape and C go back, Delete and
// Backspace clear, R restores a default, Q and Page Up and E and Page Down change section.
// Pads: by button position (main/input.h, ui_pad).

#include "ui/input/ui_input_core.h"

#include <SDL.h>

namespace oot::ui::input {

    // A key went down (true) or up (false). Repeats from the system are ignored by the caller:
    // the repeat schedule is ours.
    void key(SDL_Scancode code, bool down);

    // The window lost the keyboard: nothing stays held.
    void release_keys();

    // The mouse moved or clicked: it is the device in use.
    void mouse();

    // Once per pump, on the main thread: every pad read, the keyboard's held keys, the repeats
    // due, and the resulting actions handed to the shell while it is capturing input.
    void pump();

    // The family of the device that spoke last. Safe from any thread.
    Device last_device();

    // The pad in use, by SDL's instance id: the pad that pressed something last, or -1 when the
    // keyboard or the mouse spoke last. Safe from any thread.
    int last_pad_instance();

}
