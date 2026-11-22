// The mouse. See nav_mouse.h.

#include "ui/nav/nav_mouse.h"

#include <cstdio>

namespace oot::ui::nav {

    namespace {

        bool contains(const Rect& r, float x, float y) {
            return x >= r.x && x < r.right() && y >= r.y && y < r.bottom();
        }

        bool under(const Model& m, int node, int ancestor) {
            for (int n = m.nodes[node].parent; n >= 0; n = m.nodes[n].parent) {
                if (n == ancestor) return true;
            }
            return ancestor < 0;
        }

    }

    int hit(const Model& model, float x, float y) {
        int found = -1;
        for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
            if (reachable(model, i) && contains(model.nodes[i].box, x, y)) {
                found = i;   // later in document order is on top
            }
        }
        return found;
    }

    Click press(const Model& model, Pointer& pointer, float x, float y) {
        pointer.pressed = -1;
        const int layer = live_layer(model);
        if (layer >= 0 && !contains(model.nodes[layer].box, x, y)) {
            return Click::Swallowed;
        }
        pointer.pressed = hit(model, x, y);
        return Click::None;   // a press never acts alone; the release on the same item does
    }

    Click release(const Model& model, Pointer& pointer, float x, float y, int& target) {
        target = -1;
        const int pressed = pointer.pressed;
        pointer.pressed = -1;
        const int layer = live_layer(model);
        if (layer >= 0 && !contains(model.nodes[layer].box, x, y)) {
            return Click::Swallowed;
        }
        if (pressed >= 0 && hit(model, x, y) == pressed) {
            target = pressed;
            return Click::Activate;
        }
        return Click::None;
    }

    int wheel_target(const Model& model, float x, float y) {
        const int layer = live_layer(model);
        int found = -1;
        for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
            const Node& n = model.nodes[i];
            if (n.kind == Kind::Group && n.scrolls && contains(n.box, x, y) && under(model, i, layer)) {
                found = i;   // nested scrollers come later in document order: the innermost wins
            }
        }
        return found;
    }

    bool mouse_selftest(int& passed) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  mouse: %s\n", what); ok = false; }
        };
        auto item = [](Model& m, int parent, float x, float y, float w, float h) {
            Node n; n.kind = Kind::Item; n.parent = parent; n.box = { x, y, w, h };
            return m.add(n);
        };

        Model m;
        Node list; list.kind = Kind::Group; list.box = { 0, 0, 400, 300 }; list.scrolls = true;
        const int g = m.add(list);
        const int a = item(m, g, 0, 0, 400, 48), b = item(m, g, 0, 50, 400, 48);
        const int button = item(m, -1, 0, 320, 120, 44);

        check(hit(m, 10, 10) == a && hit(m, 10, 60) == b && hit(m, 10, 330) == button, "hit: the item under the pointer");
        check(hit(m, 10, 49) == -1, "hit: the gap between rows is nothing");

        Pointer p; int target = -1;
        press(m, p, 10, 330);
        check(release(m, p, 20, 340, target) == Click::Activate && target == button, "click: press and release on one button activates it");
        press(m, p, 10, 330);
        check(release(m, p, 10, 60, target) == Click::None && target == -1, "click: a press that slides off does nothing");

        check(wheel_target(m, 10, 10) == g, "wheel: over the list scrolls the list");
        check(wheel_target(m, 10, 330) == -1, "wheel: over the button scrolls nothing");

        // A dialog: the page beneath takes no clicks.
        Node dlg; dlg.kind = Kind::Layer; dlg.box = { 100, 100, 200, 100 };
        const int layer = m.add(dlg);
        const int ok_button = item(m, layer, 120, 140, 80, 40);
        check(press(m, p, 10, 330) == Click::Swallowed, "dialog: a press outside it is swallowed");
        check(hit(m, 10, 330) == -1, "dialog: what is beneath cannot be pointed at");
        press(m, p, 130, 150);
        check(release(m, p, 130, 150, target) == Click::Activate && target == ok_button, "dialog: its own button takes a click");

        check(pointer_mode(input::Device::Mouse) && !pointer_mode(input::Device::DualSense) &&
              !pointer_mode(input::Device::Keyboard), "modes: the ring hides for the mouse, shows for pads and keys");
        return ok;
    }

}
