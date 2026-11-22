#pragma once

// THE PROMPTS (the menus plan, phase 109): which button of the device in use does what here, drawn
// in that device's own terms, and nothing else. Pure: the shell hands it the device of the last
// input and what the focused element can do; it hands back the footer's prompts. ui_tests checks
// every family and every action, and that a run of inputs from one device and then another
// changes the family each time (the user, 2026-10-12 21:37: "must detect in real time so if I
// switch mid menu navigation, it switched the UI to show the proper controls").

#include "ui/input/ui_input_core.h"

#include <string>
#include <vector>

namespace oot::ui::prompts {

    // How a device names its buttons. The mouse borrows the keyboard's (an empty footer reads as a
    // broken one, and a mouse is used beside a keyboard).
    enum class Family { Keyboard, Xbox, DualShock, DualSense, Nintendo };

    Family family_of(oot::ui::input::Device device);

    // What a prompt can stand for.
    enum class Action { Select, Back, Change, DialPrev, DialNext, Close, Type };

    // One button drawn: "face" (a medallion with a letter or a drawn shape), "plate" (a word on a
    // glossy plate) or "pad" (the touchpad's trapezoid); `tint` names the pastel the controllers
    // code it with (the stylesheet's class), empty for none.
    struct Glyph {
        std::string kind;
        std::string key;
        std::string shape;
        std::string tint;
    };

    Glyph glyph_for(Family family, Action action);

    // What the focused element and its place allow.
    struct Here {
        bool back = true;      // Back does something (not the launcher's first page)
        bool change = false;   // the focused element is a value: Left and Right change it
        bool close = false;    // the menu is open in the game, so its button closes it
        bool type_path = false;   // the ROM browser: the on-screen keyboard is a button away
        bool typing = false;   // the on-screen keyboard is up: keys type, the shoulders move the caret
        bool dial_pinned = false;   // the dial's pair is drawn at the dial's top corners (dial_pair), not here
    };

    struct Prompt {
        Glyph glyph;
        std::string label;     // empty for the first of a pair (LB, then RB "Change setting")
        std::string action;    // the element's id is "prompt-" + action
    };

    std::vector<Prompt> footer(Family family, const Here& here);

    // THE DIAL'S PAIR AT THE DIAL'S TOP CORNERS (the user, 2026-10-22: "those should actually kind of
    // be up in the top end, top left and top right of the menu itself, above the little rotating
    // medallions. That way they're more visible"): the button that turns the dial back, then the one
    // that turns it on, each a plate on every family, with no words (where they stand says it).
    std::vector<Prompt> dial_pair(Family family);

    bool selftest(int& passed);

}
