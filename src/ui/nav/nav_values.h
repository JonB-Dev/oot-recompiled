#pragma once

// VALUE ROWS (the menus plan, phase 92): what left, right, select, a click and the wheel do on a
// row that holds a value, the same on every row of every menu.
//
//   stepper  an ordered scale (render distance, resolution, a volume): left and right step it and
//            STOP at either end, refusing the step past it so the end can be heard; select steps
//            forward and wraps to the start after the last.
//   choice   a set with no order (window mode, the camera's axes): left and right step it and
//            wrap round; select steps forward.
//   toggle   two states: left, right and select all flip it.
//   slider   a continuous range in steps (a percentage): left and right move by a step and stop
//            at the ends; select enters FINE adjustment, where they move by a fifth of a step,
//            until select or back leaves it; a drag sets it from where the pointer is.
//
// A value row captures the horizontal axis: left and right change its value and never move the
// focus off it (nav.h, adjusts). Up and down still move. Rows that must be confirmed before a
// change (the recorder's) are the caller's: the outcome says what would change; the caller asks.

namespace oot::ui::nav {

    enum class ValueKind { Stepper, Choice, Toggle, Slider };

    struct Value {
        ValueKind kind = ValueKind::Stepper;
        int index = 0;        // the option, or the slider's position in its finest units
        int count = 2;        // the options; a slider's span in its finest units, plus one
        int step = 1;         // a slider's coarse step in finest units (fine is one unit)
        bool fine = false;    // a slider in fine adjustment
    };

    struct Outcome {
        bool changed = false;   // the value moved
        bool refused = false;   // a step past an end was refused (the refusal sound plays)
        bool mode = false;      // a slider entered or left fine adjustment
    };

    // Left (-1) or right (+1).
    Outcome step(Value& v, int delta);

    // Select pressed on the row.
    Outcome accept(Value& v);

    // Back pressed on the row: leaves fine adjustment if in it (and is then consumed).
    Outcome back(Value& v);

    // A click on the row's left or right arrow is a step; on its body, select.
    // The wheel over a focused value: up is +1, down -1, as a step.
    // A drag along a slider of `length` pixels, the pointer at `at` from its start.
    Outcome drag(Value& v, float at, float length);

    bool value_selftest(int& passed);

}
