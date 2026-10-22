// Value rows. See nav_values.h.

#include "ui/nav/nav_values.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace oot::ui::nav {

    Outcome step(Value& v, int delta) {
        Outcome out;
        if (v.count <= 0 || delta == 0) return out;
        const int last = v.count - 1;
        switch (v.kind) {
            case ValueKind::Toggle:
                v.index = v.index == 0 ? 1 : 0;
                out.changed = true;
                break;
            case ValueKind::Choice:
                v.index = ((v.index + delta) % v.count + v.count) % v.count;
                out.changed = true;
                break;
            case ValueKind::Stepper: {
                const int next = v.index + delta;
                if (next < 0 || next > last) {
                    out.refused = true;
                }
                else {
                    v.index = next;
                    out.changed = true;
                }
                break;
            }
            case ValueKind::Slider: {
                const int unit = v.fine ? 1 : std::max(1, v.step);
                const int next = std::clamp(v.index + delta * unit, 0, last);
                if (next == v.index) {
                    out.refused = true;
                }
                else {
                    v.index = next;
                    out.changed = true;
                }
                break;
            }
        }
        return out;
    }

    Outcome accept(Value& v) {
        Outcome out;
        switch (v.kind) {
            case ValueKind::Toggle:
                return step(v, 1);
            case ValueKind::Choice:
            case ValueKind::Stepper:
                // Forward, and from the last back to the first: select never dead-ends.
                if (v.count > 0) {
                    v.index = (v.index + 1) % v.count;
                    out.changed = true;
                }
                return out;
            case ValueKind::Slider:
                v.fine = !v.fine;
                out.mode = true;
                return out;
        }
        return out;
    }

    Outcome back(Value& v) {
        Outcome out;
        if (v.kind == ValueKind::Slider && v.fine) {
            v.fine = false;
            out.mode = true;
        }
        return out;
    }

    Outcome drag(Value& v, float at, float length) {
        Outcome out;
        if (v.kind != ValueKind::Slider || length <= 0.0f || v.count <= 1) return out;
        const float t = std::clamp(at / length, 0.0f, 1.0f);
        const int target = static_cast<int>(std::lround(t * static_cast<float>(v.count - 1)));
        if (target != v.index) {
            v.index = target;
            out.changed = true;
        }
        return out;
    }

    bool value_selftest(int& passed) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  values: %s\n", what); ok = false; }
        };

        // A stepper of six (render distance).
        Value s{ ValueKind::Stepper, 0, 6 };
        check(step(s, -1).refused && s.index == 0, "stepper: left at the start is refused and stays");
        check(step(s, 1).changed && s.index == 1, "stepper: right steps");
        s.index = 5;
        check(step(s, 1).refused && s.index == 5, "stepper: right at the end is refused and stays");
        check(accept(s).changed && s.index == 0, "stepper: select at the end wraps to the start");

        // A choice of three (window mode, camera axes).
        Value c{ ValueKind::Choice, 2, 3 };
        check(step(c, 1).changed && c.index == 0, "choice: right past the last wraps");
        check(step(c, -1).changed && c.index == 2, "choice: left past the first wraps");
        check(accept(c).changed && c.index == 0, "choice: select steps forward and wraps");

        // A toggle.
        Value t{ ValueKind::Toggle, 0, 2 };
        check(step(t, 1).changed && t.index == 1, "toggle: right flips on");
        check(step(t, 1).changed && t.index == 0, "toggle: right again flips off");
        check(step(t, -1).changed && t.index == 1, "toggle: left flips too");
        check(accept(t).changed && t.index == 0, "toggle: select flips");

        // A slider of 0 to 100 in fives, its finest unit a single point.
        Value v{ ValueKind::Slider, 50, 101, 5 };
        check(step(v, 1).changed && v.index == 55, "slider: right moves a step of five");
        check(accept(v).mode && v.fine, "slider: select enters fine adjustment");
        check(step(v, 1).changed && v.index == 56, "slider: fine moves by one");
        check(back(v).mode && !v.fine, "slider: back leaves fine adjustment");
        check(!back(v).mode, "slider: back outside fine adjustment is not consumed");
        v.index = 98;
        check(step(v, 1).changed && v.index == 100, "slider: a step stops at the end, not past it");
        check(step(v, 1).refused, "slider: right at the end is refused");
        check(drag(v, 25.0f, 100.0f).changed && v.index == 25, "slider: a drag a quarter along sets a quarter");
        check(drag(v, 500.0f, 100.0f).changed && v.index == 100, "slider: a drag past the end clamps");

        return ok;
    }

}
