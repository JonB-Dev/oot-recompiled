#pragma once

// PHOTO MODE'S BUTTON GUIDE (the user, 2026-10-13: "a row of buttons. You know, the small buttons
// kind of like in the footer of the launcher ... at the bottom of the screen when enabling the
// photo mode ... the basic maneuvering of the camera controls based on their obviously dynamically
// detected controller or keyboard setup ... it should show the proper controls to actually navigate
// and do things in that mode. And then they need a button to be able to hide that from view").
//
// Unlike the menu's footer, whose buttons are fixed, every one of these is rebindable, so each
// glyph is made from the binding itself: a key is a plate with its name, a pad's face button is its
// medallion, a stick is LS or RS. A move bound to four keys reads as one plate ("W A S D"). Pure,
// like prompts.h: the shell hands it the device's bindings and a way to name a key, and ui_tests
// checks the keyboard's and each pad family's defaults.

#include "main/input_bindings.h"
#include "ui/prompts/prompts.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace oot::ui::prompts::photo_guide {

    // SDL's standard layout by number (SDL_GameControllerButton, SDL_GameControllerAxis), so this
    // file needs no SDL. ui_shell.cpp checks them against SDL's own at compile time.
    namespace pad {
        enum Button : int {
            A = 0, B, X, Y, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
            DUp, DDown, DLeft, DRight, Misc1, Paddle1, Paddle2, Paddle3, Paddle4, Touchpad,
        };
        enum Axis : int { LeftX = 0, LeftY, RightX, RightY, TriggerLeft, TriggerRight };
    }

    // One device's two bindings for an input (the keyboard's, or one pad's profile).
    using Slots = std::array<oot::input::Field, oot::input::BINDINGS_PER_INPUT>;
    using Lookup = std::function<Slots(oot::input::GameInput)>;
    // A key's name as SDL gives it ("Left Shift", "Up"); shortened here for a plate.
    using KeyName = std::function<std::string(int scancode)>;

    struct Here {
        bool hud_hidden = false;   // the game's HUD is hidden by its binding: the prompt says Show
        bool hud_dimmed = false;   // dimmed: the next press hides it, so the prompt says Hide
    };

    // The guide, in the order a person reaches for them: move, turn, lower and raise, closer and
    // further, put back, the HUD, a picture, this guide, leave. An action with no binding on this
    // device is left out. An action drawn as several glyphs labels only the last, as the dial's
    // pair does.
    std::vector<Prompt> rows(Family family, const Lookup& bindings, const KeyName& key_name, const Here& here);

    // A key's name for a plate: the arrows as arrows, the long modifiers short.
    std::string short_key(const std::string& sdl_name);

    // THE BINDINGS IN FORCE THAT WORK, for any prompt naming a control a person can rebind (the
    // menu's Close as well as this guide; the user, 2026-10-13: "if the user changes the default
    // binding, that the menu/launcher/popovers/photo mode overlays all update to show what the
    // current binding is", and "show both of them but only in the event that both of them actually
    // work within the context"): each bound slot whose press reaches the control, in slot order, a
    // key or button counted once. Empty when none does.
    std::vector<oot::input::Field> working(const Lookup& bindings, oot::input::GameInput input);

    // Each of those drawn in the family's own terms, one glyph a binding.
    std::vector<Glyph> working_glyphs(Family family, const Lookup& bindings, oot::input::GameInput input,
                                      const KeyName& key_name);

    bool selftest(int& passed);

}
