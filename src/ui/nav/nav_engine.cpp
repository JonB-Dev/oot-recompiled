// The navigation engine. See nav.h for the rule.

#include "ui/nav/nav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace oot::ui::nav {

    namespace {

        constexpr float EDGE = 1.0f;   // a pixel of tolerance where two boxes share an edge

        bool inside(const Model& m, int node, int ancestor) {
            for (int n = m.nodes[node].parent; n >= 0; n = m.nodes[n].parent) {
                if (n == ancestor) return true;
            }
            return ancestor < 0;
        }

        int layer_of(const Model& m, int node) {
            for (int n = m.nodes[node].parent; n >= 0; n = m.nodes[n].parent) {
                if (m.nodes[n].kind == Kind::Layer) return n;
            }
            return -1;
        }

        bool shown(const Model& m, int node) {
            for (int n = m.nodes[node].parent; n >= 0; n = m.nodes[n].parent) {
                if (m.nodes[n].kind == Kind::Layer && !m.nodes[n].open) return false;
            }
            return true;
        }

        bool beyond(const Rect& s, const Rect& c, Dir d) {
            switch (d) {
                case Dir::Right: return c.x >= s.right() - EDGE;
                case Dir::Left:  return c.right() <= s.x + EDGE;
                case Dir::Down:  return c.y >= s.bottom() - EDGE;
                case Dir::Up:    return c.bottom() <= s.y + EDGE;
            }
            return false;
        }

        // Along the axis: the gap between the facing edges. Across: the overlap (positive) or the
        // gap (as a negative overlap) between the two boxes' spans.
        float along(const Rect& s, const Rect& c, Dir d) {
            switch (d) {
                case Dir::Right: return std::max(0.0f, c.x - s.right());
                case Dir::Left:  return std::max(0.0f, s.x - c.right());
                case Dir::Down:  return std::max(0.0f, c.y - s.bottom());
                case Dir::Up:    return std::max(0.0f, s.y - c.bottom());
            }
            return 0.0f;
        }

        float across(const Rect& s, const Rect& c, Dir d) {
            const bool horizontal = d == Dir::Left || d == Dir::Right;
            const float a0 = horizontal ? s.y : s.x, a1 = horizontal ? s.bottom() : s.right();
            const float b0 = horizontal ? c.y : c.x, b1 = horizontal ? c.bottom() : c.right();
            return std::min(a1, b1) - std::max(a0, b0);
        }

        // A candidate as it is seen from the current item: its box, cut to the view of every
        // scrolling list it sits in that the current item does not.
        struct Seen {
            int node;
            Rect box;
        };

        // The best of `candidates` from `s` in `d`, by the rule; -1 when none qualifies.
        int best(const Model& m, const Rect& s, Dir d, const std::vector<Seen>& candidates) {
            int pick = -1;
            std::tuple<int, float, float, int> key{};
            for (const Seen& c : candidates) {
                const Rect& r = c.box;
                if (!beyond(s, r, d)) continue;
                const float a = along(s, r, d);
                const float x = across(s, r, d);
                const std::tuple<int, float, float, int> k = (x > 0.0f)
                    ? std::make_tuple(0, a, -x, m.nodes[c.node].order)
                    : std::make_tuple(1, a + 2.0f * (-x), 0.0f, m.nodes[c.node].order);
                if (pick < 0 || k < key) {
                    pick = c.node;
                    key = k;
                }
            }
            return pick;
        }

        std::vector<int> live_items(const Model& m) {
            std::vector<int> out;
            for (int i = 0; i < static_cast<int>(m.nodes.size()); ++i) {
                if (reachable(m, i)) out.push_back(i);
            }
            return out;
        }

        Rect intersect(const Rect& a, const Rect& b) {
            const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
            const float x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
            return { x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
        }

        // Every live item but `from`, as `from` sees it. A row scrolled out of a list's view is
        // laid out beyond the list, often under whatever sits below it, so from outside the list
        // only what shows of it counts; from inside, every row counts (moving to it scrolls).
        std::vector<Seen> seen_from(const Model& m, int from) {
            std::vector<Seen> out;
            for (int c : live_items(m)) {
                if (c == from) continue;
                Rect r = m.nodes[c].box;
                for (int g = m.nodes[c].parent; g >= 0 && !r.empty(); g = m.nodes[g].parent) {
                    if (m.nodes[g].kind == Kind::Layer) break;
                    if (m.nodes[g].kind == Kind::Group && m.nodes[g].scrolls && !inside(m, from, g)) {
                        r = intersect(r, m.nodes[g].box);
                    }
                }
                if (!r.empty()) out.push_back({ c, r });
            }
            return out;
        }

        // The innermost scrolling list that holds `node`, or -1.
        int scroller_of(const Model& m, int node) {
            for (int g = m.nodes[node].parent; g >= 0; g = m.nodes[g].parent) {
                if (m.nodes[g].kind == Kind::Layer) return -1;
                if (m.nodes[g].kind == Kind::Group && m.nodes[g].scrolls) return g;
            }
            return -1;
        }

        // The outermost group that holds `target` and not `from`: the group being entered.
        int entered_group(const Model& m, int from, int target) {
            int entered = -1;
            for (int n = m.nodes[target].parent; n >= 0; n = m.nodes[n].parent) {
                if (m.nodes[n].kind == Kind::Layer) break;
                if (m.nodes[n].kind == Kind::Group && !inside(m, from, n)) entered = n;
            }
            return entered;
        }

    }

    int live_layer(const Model& model) {
        int live = -1;
        for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
            const Node& n = model.nodes[i];
            if (n.kind == Kind::Layer && n.open && shown(model, i)) live = i;
        }
        return live;
    }

    bool reachable(const Model& model, int item) {
        if (item < 0 || item >= static_cast<int>(model.nodes.size())) return false;
        const Node& n = model.nodes[item];
        if (n.kind != Kind::Item || !n.enabled || n.box.empty() || !shown(model, item)) return false;
        return layer_of(model, item) == live_layer(model);
    }

    bool adjusts(const Model& model, int item, Dir d) {
        return item >= 0 && item < static_cast<int>(model.nodes.size()) && model.nodes[item].value &&
               (d == Dir::Left || d == Dir::Right);
    }

    int first_focus(const Model& model, const Memory& memory) {
        const int layer = live_layer(model);
        // The remembered item of the layer's outermost group, when there is one.
        for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
            const Node& n = model.nodes[i];
            if (n.kind != Kind::Group || layer_of(model, i) != layer) continue;
            if (n.parent >= 0 && model.nodes[n.parent].kind == Kind::Group) continue;
            auto r = memory.last.find(i);
            if (r != memory.last.end() && reachable(model, r->second)) return r->second;
        }
        const std::vector<int> items = live_items(model);
        return items.empty() ? -1 : items.front();
    }

    int move(const Model& model, const Memory& memory, int from, Dir d) {
        if (!reachable(model, from)) {
            return first_focus(model, memory);
        }
        const std::vector<Seen> candidates = seen_from(model, from);
        const Rect& s = model.nodes[from].box;

        // In a scrolling list, its own rows that way come first: the focus leaves the list only
        // past its last row, never for something that happens to sit nearer than a hidden row.
        int target = -1;
        const int scroller = scroller_of(model, from);
        if (scroller >= 0) {
            std::vector<Seen> own;
            for (const Seen& c : candidates) {
                if (inside(model, c.node, scroller)) own.push_back(c);
            }
            target = best(model, s, d, own);
        }
        if (target < 0) target = best(model, s, d, candidates);
        if (target >= 0) {
            const int group = entered_group(model, from, target);
            if (group >= 0) {
                auto r = memory.last.find(group);
                if (r != memory.last.end() && reachable(model, r->second) && inside(model, r->second, group)) {
                    return r->second;
                }
            }
            return target;
        }

        // Nothing that way: wrap within the innermost group that wraps, from its far end.
        for (int g = model.nodes[from].parent; g >= 0; g = model.nodes[g].parent) {
            if (model.nodes[g].kind == Kind::Layer) break;
            if (model.nodes[g].kind != Kind::Group || !model.nodes[g].wrap) continue;
            std::vector<Seen> members;
            for (const Seen& c : candidates) {
                if (inside(model, c.node, g)) members.push_back(c);
            }
            if (members.empty()) break;
            // A box just past the group's far edge on the opposite side, the width (or height)
            // of `from`, looks back in `d` across the whole group. The far edge is the CONTENT's,
            // not the box's: a scrolled list has rows laid out past its view, and the last row
            // is the far end even while it is scrolled away (found by the oracle, phase 94).
            Rect probe = s;
            Rect gb = model.nodes[g].box;
            for (const Seen& c : members) {
                const Rect& r = c.box;
                const float x0 = std::min(gb.x, r.x), y0 = std::min(gb.y, r.y);
                const float x1 = std::max(gb.right(), r.right()), y1 = std::max(gb.bottom(), r.bottom());
                gb = { x0, y0, x1 - x0, y1 - y0 };
            }
            switch (d) {
                case Dir::Down:  probe.y = gb.y - s.h - 2 * EDGE; break;
                case Dir::Up:    probe.y = gb.bottom() + 2 * EDGE; break;
                case Dir::Right: probe.x = gb.x - s.w - 2 * EDGE; break;
                case Dir::Left:  probe.x = gb.right() + 2 * EDGE; break;
            }
            const int wrapped = best(model, probe, d, members);
            return wrapped >= 0 ? wrapped : from;
        }
        return from;
    }

    void remember(const Model& model, Memory& memory, int item) {
        if (item < 0 || item >= static_cast<int>(model.nodes.size())) return;
        for (int n = model.nodes[item].parent; n >= 0; n = model.nodes[n].parent) {
            if (model.nodes[n].kind == Kind::Layer) break;
            if (model.nodes[n].kind == Kind::Group) memory.last[n] = item;
        }
    }

    float scroll_to_show(float item_start, float item_length, float view_start, float view_length,
                         float scroll, float margin) {
        const float top = item_start - margin;
        const float bottom = item_start + item_length + margin;
        if (top < view_start) return scroll - (view_start - top);
        if (bottom > view_start + view_length) {
            // An item taller than the view shows its start rather than its end.
            if (item_length + 2 * margin > view_length) return scroll - (view_start - top);
            return scroll + (bottom - (view_start + view_length));
        }
        return scroll;
    }

    // -------------------------------------------------------------------------------------------
    // The engine against synthetic layouts
    // -------------------------------------------------------------------------------------------

    bool selftest(int& passed) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  nav: %s\n", what); ok = false; }
        };
        auto item = [](Model& m, int parent, float x, float y, float w, float h) {
            Node n; n.kind = Kind::Item; n.parent = parent; n.box = { x, y, w, h };
            return m.add(n);
        };
        auto group = [](Model& m, int parent, float x, float y, float w, float h, bool wrap = false) {
            Node n; n.kind = Kind::Group; n.parent = parent; n.box = { x, y, w, h }; n.wrap = wrap;
            return m.add(n);
        };

        // A column of five rows.
        {
            Model m; Memory mem;
            const int g = group(m, -1, 0, 0, 400, 250);
            int r[5];
            for (int i = 0; i < 5; ++i) r[i] = item(m, g, 0, i * 50.0f, 400, 48);
            check(move(m, mem, r[0], Dir::Down) == r[1], "column: down goes to the next row");
            check(move(m, mem, r[3], Dir::Up) == r[2], "column: up goes to the row above");
            check(move(m, mem, r[4], Dir::Down) == r[4], "column: down from the last row stays without wrap");
            check(move(m, mem, r[2], Dir::Left) == r[2], "column: left in a single column stays");
            check(first_focus(m, mem) == r[0], "column: the first focus is the first row");
        }

        // The same column wrapping.
        {
            Model m; Memory mem;
            const int g = group(m, -1, 0, 0, 400, 250, true);
            int r[5];
            for (int i = 0; i < 5; ++i) r[i] = item(m, g, 0, i * 50.0f, 400, 48);
            check(move(m, mem, r[4], Dir::Down) == r[0], "wrap: down from the last row reaches the first");
            check(move(m, mem, r[0], Dir::Up) == r[4], "wrap: up from the first row reaches the last");
        }

        // A wrapping list that scrolls: ten rows, five in view, scrolled down by two, so rows
        // laid out above and below the view. The far end is the list's, not the view's.
        {
            Model m; Memory mem;
            Node v; v.kind = Kind::Group; v.box = { 0, 0, 400, 250 }; v.wrap = true; v.scrolls = true;
            const int g = m.add(v);
            int r[10];
            for (int i = 0; i < 10; ++i) r[i] = item(m, g, 0, (i - 2) * 50.0f, 400, 48);
            check(move(m, mem, r[0], Dir::Up) == r[9], "scrolled wrap: up from the first row reaches the last, out of view");
            check(move(m, mem, r[9], Dir::Down) == r[0], "scrolled wrap: down from the last row reaches the first, out of view");
        }

        // A scrolling list over an action bar: eight rows, four in view, the rest laid out under
        // the bar. From the bar, up reaches what shows, and nothing lies under the bar; from the
        // last row in view, down is the next row, not the bar.
        {
            Model m; Memory mem;
            Node v; v.kind = Kind::Group; v.box = { 0, 0, 400, 200 }; v.scrolls = true;
            const int g = m.add(v);
            int r[8];
            for (int i = 0; i < 8; ++i) r[i] = item(m, g, 0, i * 50.0f, 400, 48);
            const int bar = item(m, -1, 0, 220, 160, 40);
            check(move(m, mem, bar, Dir::Down) == bar, "window: nothing under the bar, though hidden rows are laid out there");
            check(move(m, mem, bar, Dir::Up) == r[3], "window: up from the bar reaches the last row in view");
            check(move(m, mem, r[3], Dir::Down) == r[4], "window: down from the last row in view is the next row");
            // Scrolled to the end: the last row sits at the bottom of the view, the bar below it.
            for (int i = 0; i < 8; ++i) m.nodes[r[i]].box.y -= 200.0f;
            check(move(m, mem, r[7], Dir::Down) == bar, "window: down from the last row leaves the list for the bar");
            check(move(m, mem, bar, Dir::Up) == r[7], "window: up from the bar returns to the last row");
        }

        // A row of three buttons.
        {
            Model m; Memory mem;
            const int a = item(m, -1, 0, 0, 100, 40), b = item(m, -1, 120, 0, 100, 40), c = item(m, -1, 240, 0, 100, 40);
            check(move(m, mem, a, Dir::Right) == b, "row: right goes to the next button");
            check(move(m, mem, c, Dir::Left) == b, "row: left goes back");
            check(move(m, mem, b, Dir::Up) == b, "row: up from a lone row stays");
        }

        // The Moments case: a list of slots above a two by two grid of buttons.
        {
            Model m; Memory mem;
            const int slots = group(m, -1, 0, 0, 400, 200);
            int s[4];
            for (int i = 0; i < 4; ++i) s[i] = item(m, slots, 0, i * 50.0f, 400, 48);
            const int grid = group(m, -1, 0, 220, 400, 100);
            const int del = item(m, grid, 0, 220, 190, 44), over = item(m, grid, 210, 220, 190, 44);
            const int load = item(m, grid, 0, 276, 190, 44), cancel = item(m, grid, 210, 276, 190, 44);
            check(move(m, mem, load, Dir::Up) == del, "moments: up from Load reaches Delete");
            check(move(m, mem, cancel, Dir::Up) == over, "moments: up from Cancel reaches Save over");
            check(move(m, mem, load, Dir::Right) == cancel, "moments: right from Load reaches Cancel");
            check(move(m, mem, del, Dir::Down) == load, "moments: down from Delete reaches Load");
            remember(m, mem, s[2]);
            check(move(m, mem, over, Dir::Up) == s[2], "moments: up from the grid returns to the slot left, not the nearest");
            remember(m, mem, cancel);
            check(move(m, mem, s[3], Dir::Down) == cancel, "moments: down from the slots returns to the button left");
        }

        // A list beside a panel: across and back returns to the row left.
        {
            Model m; Memory mem;
            const int list = group(m, -1, 0, 0, 300, 400);
            int r[8];
            for (int i = 0; i < 8; ++i) r[i] = item(m, list, 0, i * 50.0f, 300, 48);
            const int panel = group(m, -1, 340, 0, 300, 400);
            const int p1 = item(m, panel, 340, 0, 300, 48), p2 = item(m, panel, 340, 300, 300, 48);
            remember(m, mem, r[5]);
            check(move(m, mem, r[5], Dir::Right) == p2, "beside: right from a row reaches the panel's aligned button");
            remember(m, mem, p2);
            check(move(m, mem, p2, Dir::Left) == r[5], "beside: left returns to the row left");
            check(move(m, mem, p1, Dir::Down) == p2, "beside: down within the panel");
        }

        // Uneven rows: two narrow over one wide.
        {
            Model m; Memory mem;
            const int a = item(m, -1, 0, 0, 190, 40), b = item(m, -1, 210, 0, 190, 40), w = item(m, -1, 0, 60, 400, 40);
            check(move(m, mem, b, Dir::Down) == w, "uneven: down from the right narrow reaches the wide row");
            check(move(m, mem, w, Dir::Up) == a, "uneven: up from the wide row takes the more overlapped (then first)");
        }

        // An aligned candidate beats a nearer one that is not aligned.
        {
            Model m; Memory mem;
            const int s = item(m, -1, 0, 0, 100, 40);
            const int near_off = item(m, -1, 200, 50, 100, 40);   // closer along, not aligned
            const int far_on = item(m, -1, 0, 200, 100, 40);      // farther, aligned
            check(move(m, mem, s, Dir::Down) == far_on, "aligned beats nearer but unaligned");
            (void)near_off;
        }

        // A dialog over the page traps the focus.
        {
            Model m; Memory mem;
            const int page = item(m, -1, 0, 0, 400, 40);
            Node layer; layer.kind = Kind::Layer; layer.box = { 100, 100, 200, 100 };
            const int dlg = m.add(layer);
            const int yes = item(m, dlg, 110, 150, 80, 40), no = item(m, dlg, 210, 150, 80, 40);
            check(!reachable(m, page), "dialog: the page beneath cannot be reached");
            check(move(m, mem, page, Dir::Down) == yes, "dialog: a move from beneath lands in the dialog");
            check(move(m, mem, yes, Dir::Right) == no, "dialog: right between its buttons");
            check(move(m, mem, no, Dir::Up) == no, "dialog: nothing above inside it, so the focus stays");
            m.nodes[dlg].open = false;
            check(reachable(m, page), "dialog: closed, the page is reachable again");
        }

        // A scrolled list: rows outside the view are still candidates, and come into view.
        {
            check(scroll_to_show(500, 50, 0, 400, 0, 8) == 158.0f, "scroll: a row below the view scrolls up into it with the margin");
            check(scroll_to_show(100, 50, 0, 400, 0, 8) == 0.0f, "scroll: a row in view leaves the scroll alone");
            check(scroll_to_show(-60, 50, 0, 400, 200, 8) == 132.0f, "scroll: a row above the view scrolls down to it");
        }

        // A value row captures left and right, not up and down.
        {
            Model m;
            const int row = item(m, -1, 0, 0, 400, 48);
            m.nodes[row].value = true;
            check(adjusts(m, row, Dir::Left) && adjusts(m, row, Dir::Right), "value: a value row takes left and right");
            check(!adjusts(m, row, Dir::Up) && !adjusts(m, row, Dir::Down), "value: up and down still move off it");
        }

        // Disabled items are passed over.
        {
            Model m; Memory mem;
            const int a = item(m, -1, 0, 0, 100, 40), b = item(m, -1, 0, 50, 100, 40), c = item(m, -1, 0, 100, 100, 40);
            m.nodes[b].enabled = false;
            check(move(m, mem, a, Dir::Down) == c, "disabled: a disabled row is passed over");
        }

        return ok;
    }

}
