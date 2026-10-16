#pragma once

// THE NAVIGATION ENGINE (the menus plan, phase 91). Where a direction goes is decided by where
// things ARE: the boxes RmlUi has laid out, never a list of rows written per screen. A screen that
// needs navigation code of its own is a defect in this engine, not a place to write a function
// (the user, 2026-10-12: "a way it can never regress and just works", "its essentially just a list
// with a css override").
//
// This file is the pure model, no RmlUi, so it is tested headless (ui_tests) and checked against
// an independent oracle on every real document (ui_navcheck, phase 94). The adapter that builds
// a Model from a document is in nav_rml.
//
// THE RULE, in words, which the oracle implements separately:
//   1. Only items in the LIVE layer are candidates: the topmost open layer (a dialog, the
//      keyboard) traps the focus; beneath it nothing can be reached.
//   2. A candidate lies in the direction pressed when its box is wholly past the current item's
//      edge on that side (a pixel of tolerance for shared edges).
//   3. Among those, one that OVERLAPS the current item across the axis (a row below in the same
//      column, a button beside in the same row) always beats one that does not. Then the nearest
//      along the axis, then the greater overlap, then document order. A candidate that does not
//      overlap is scored by its distance along the axis plus twice its gap across it.
//   4. Entering a GROUP from outside it lands on the item last focused in that group, if it still
//      exists, rather than on whichever item happens to be nearest: coming back to a list returns
//      to the row you left.
//   5. Nothing lies that way: a group marked to wrap carries the focus to its far end (aligned
//      across the axis where it can be); otherwise the focus stays. The far end is the group's
//      content, which in a scrolling list runs past its view.
//   6. A SCROLLING list (a group whose content scrolls) is a window onto its rows. From outside
//      it, only what shows through the window is a candidate, cut to the window: a row scrolled
//      away is laid out beyond the list, often under whatever sits below it, and must not be
//      reachable through that. From inside it, its own rows that way come first, shown or not
//      (moving to one scrolls it into view); the focus leaves the list only when it has no row
//      that way. (Both found by the navigation check's oracle, phase 94.)

#include <map>
#include <vector>

namespace oot::ui::nav {

    struct Rect {
        float x = 0, y = 0, w = 0, h = 0;
        float right() const { return x + w; }
        float bottom() const { return y + h; }
        float cx() const { return x + w * 0.5f; }
        float cy() const { return y + h * 0.5f; }
        bool empty() const { return w <= 0.0f || h <= 0.0f; }
    };

    enum class Dir { Up, Down, Left, Right };
    enum class Kind { Item, Group, Layer };

    struct Node {
        Kind kind = Kind::Item;
        int parent = -1;       // index of the enclosing group or layer; -1 for the document itself
        Rect box;              // in the document's coordinates, as laid out (scrolling included)
        bool wrap = false;     // a group: the focus wraps from one end to the other
        bool scrolls = false;  // a group: its content scrolls (the wheel, scroll into view)
        bool value = false;    // an item holding a value: left and right change it (phase 92)
        bool enabled = true;
        bool open = true;      // a layer: shown
        int order = 0;         // document order, the last tie-break
    };

    struct Model {
        std::vector<Node> nodes;
        int add(Node n) {
            n.order = static_cast<int>(nodes.size());
            nodes.push_back(n);
            return n.order;
        }
    };

    // What each group last held, so re-entering it returns there. One per document.
    struct Memory {
        std::map<int, int> last;   // group node -> item node
    };

    // The topmost open layer: the last open layer in document order, or -1 (the document).
    int live_layer(const Model& model);

    // True when `item` can be focused now: an enabled item, with a box, inside the live layer.
    bool reachable(const Model& model, int item);

    // True when pressing `d` on `item` changes its value rather than moving the focus: a value
    // row (nav_values.h) captures left and right; up and down still move.
    bool adjusts(const Model& model, int item, Dir d);

    // Where `from` goes in direction `d`, or `from` itself when nothing lies that way. If `from`
    // is not reachable (a dialog opened over it), the live layer's first focus.
    int move(const Model& model, const Memory& memory, int from, Dir d);

    // Record that `item` has the focus: every group it sits in remembers it.
    void remember(const Model& model, Memory& memory, int item);

    // Where the focus starts in the live layer: its remembered item if one, else the first
    // reachable item in document order, else -1.
    int first_focus(const Model& model, const Memory& memory);

    // The scroll offset that brings `item` into a scrolled view, leaving `margin` around it: the
    // view's own offset unchanged when the item already shows. Along one axis (start and length).
    float scroll_to_show(float item_start, float item_length, float view_start, float view_length,
                         float scroll, float margin);

    // The engine against synthetic layouts (ui_tests).
    bool selftest(int& passed);

}
