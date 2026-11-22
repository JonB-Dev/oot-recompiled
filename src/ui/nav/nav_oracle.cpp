// The oracle: the rule of nav.h, written again from its words. See nav_oracle.h.

#include "ui/nav/nav_oracle.h"

#include <vector>

namespace oot::ui::nav {

    namespace {

        // Which open layer is on top, walking every node once: a layer counts only if every
        // layer above it in the tree is open too.
        int top_layer(const Model& m) {
            int top = -1;
            for (size_t i = 0; i < m.nodes.size(); ++i) {
                if (m.nodes[i].kind != Kind::Layer) continue;
                bool open = true;
                for (int n = static_cast<int>(i); n >= 0; n = m.nodes[n].parent) {
                    if (m.nodes[n].kind == Kind::Layer && !m.nodes[n].open) open = false;
                }
                if (open) top = static_cast<int>(i);
            }
            return top;
        }

        int owning_layer(const Model& m, int i) {
            int n = m.nodes[i].parent;
            while (n >= 0 && m.nodes[n].kind != Kind::Layer) n = m.nodes[n].parent;
            return n;
        }

        bool usable(const Model& m, int i, int top) {
            const Node& n = m.nodes[i];
            if (n.kind != Kind::Item || !n.enabled || n.box.w <= 0 || n.box.h <= 0) return false;
            for (int p = n.parent; p >= 0; p = m.nodes[p].parent) {
                if (m.nodes[p].kind == Kind::Layer && !m.nodes[p].open) return false;
            }
            return owning_layer(m, i) == top;
        }

        struct Measure {
            int node;
            bool aligned;
            float cost;      // along, or along plus twice the gap across
            float overlap;   // when aligned
            int order;
        };

        // Lower is better: aligned first, then cost, then more overlap, then document order.
        bool better(const Measure& a, const Measure& b) {
            if (a.aligned != b.aligned) return a.aligned;
            if (a.cost != b.cost) return a.cost < b.cost;
            if (a.aligned && a.overlap != b.overlap) return a.overlap > b.overlap;
            return a.order < b.order;
        }

        // The candidate's measures from `s` in `d`, or false when it is not past that edge.
        bool measure(const Rect& s, const Rect& c, Dir d, Measure& out) {
            float gap_along, lo_s, hi_s, lo_c, hi_c;
            if (d == Dir::Right) {
                if (!(c.x >= s.x + s.w - 1.0f)) return false;
                gap_along = c.x - (s.x + s.w);
                lo_s = s.y; hi_s = s.y + s.h; lo_c = c.y; hi_c = c.y + c.h;
            }
            else if (d == Dir::Left) {
                if (!(c.x + c.w <= s.x + 1.0f)) return false;
                gap_along = s.x - (c.x + c.w);
                lo_s = s.y; hi_s = s.y + s.h; lo_c = c.y; hi_c = c.y + c.h;
            }
            else if (d == Dir::Down) {
                if (!(c.y >= s.y + s.h - 1.0f)) return false;
                gap_along = c.y - (s.y + s.h);
                lo_s = s.x; hi_s = s.x + s.w; lo_c = c.x; hi_c = c.x + c.w;
            }
            else {
                if (!(c.y + c.h <= s.y + 1.0f)) return false;
                gap_along = s.y - (c.y + c.h);
                lo_s = s.x; hi_s = s.x + s.w; lo_c = c.x; hi_c = c.x + c.w;
            }
            if (gap_along < 0.0f) gap_along = 0.0f;
            const float shared = (hi_s < hi_c ? hi_s : hi_c) - (lo_s > lo_c ? lo_s : lo_c);
            out.aligned = shared > 0.0f;
            out.overlap = out.aligned ? shared : 0.0f;
            out.cost = out.aligned ? gap_along : gap_along + 2.0f * (-shared);
            return true;
        }

        // `seen` holds each node's box as the current item sees it (rule 6).
        int choose(const Model& m, const std::vector<Rect>& seen, const Rect& s, Dir d, const std::vector<int>& pool) {
            bool have = false;
            Measure best{};
            for (int c : pool) {
                Measure x{};
                if (!measure(s, seen[c], d, x)) continue;
                x.node = c;
                x.order = m.nodes[c].order;
                if (!have || better(x, best)) { best = x; have = true; }
            }
            return have ? best.node : -1;
        }

        bool within(const Model& m, int i, int group) {
            for (int p = m.nodes[i].parent; p >= 0; p = m.nodes[p].parent) {
                if (p == group) return true;
            }
            return false;
        }

        // Rule 6's window: `c` through every scrolling list it sits in that `from` does not, or
        // an empty box when it does not show through one of them.
        Rect through_windows(const Model& m, int from, int c) {
            Rect r = m.nodes[c].box;
            for (int g = m.nodes[c].parent; g >= 0 && m.nodes[g].kind != Kind::Layer; g = m.nodes[g].parent) {
                if (m.nodes[g].kind != Kind::Group || !m.nodes[g].scrolls || within(m, from, g)) continue;
                const Rect& v = m.nodes[g].box;
                const float l = r.x > v.x ? r.x : v.x, t = r.y > v.y ? r.y : v.y;
                const float rt = (r.x + r.w) < (v.x + v.w) ? (r.x + r.w) : (v.x + v.w);
                const float b = (r.y + r.h) < (v.y + v.h) ? (r.y + r.h) : (v.y + v.h);
                if (rt <= l || b <= t) return Rect{};
                r = Rect{ l, t, rt - l, b - t };
            }
            return r;
        }

    }

    int oracle_move(const Model& model, int from, Dir d) {
        const int top = top_layer(model);
        std::vector<int> pool;
        std::vector<Rect> seen(model.nodes.size());
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            const int c = static_cast<int>(i);
            if (c == from || !usable(model, c, top)) continue;
            seen[i] = from >= 0 ? through_windows(model, from, c) : model.nodes[i].box;
            if (!seen[i].empty()) pool.push_back(c);
        }
        if (!usable(model, from, top)) {
            return pool.empty() ? -1 : pool.front();
        }
        const Rect& s = model.nodes[from].box;

        // Rule 6, inside a scrolling list: its own rows that way first.
        int scroller = -1;
        for (int g = model.nodes[from].parent; g >= 0 && model.nodes[g].kind != Kind::Layer; g = model.nodes[g].parent) {
            if (model.nodes[g].kind == Kind::Group && model.nodes[g].scrolls) { scroller = g; break; }
        }
        if (scroller >= 0) {
            std::vector<int> rows;
            for (int c : pool) if (within(model, c, scroller)) rows.push_back(c);
            const int row = choose(model, seen, s, d, rows);
            if (row >= 0) return row;
        }
        const int spatial = choose(model, seen, s, d, pool);
        if (spatial >= 0) return spatial;

        // Wrap: the nearest enclosing group that wraps carries the focus to its far end. Every
        // member is measured from the group's opposite edge (pressing down at the bottom lands
        // at the top): one sharing the current item's span across the axis first, then the one
        // nearest that edge, then the greater overlap, then document order; one not sharing it
        // is charged twice its gap across, as in a plain move.
        for (int g = model.nodes[from].parent; g >= 0 && model.nodes[g].kind != Kind::Layer; g = model.nodes[g].parent) {
            if (model.nodes[g].kind != Kind::Group || !model.nodes[g].wrap) continue;
            const Rect& gb = model.nodes[g].box;
            bool have = false;
            Measure best{};
            for (int c : pool) {
                if (!within(model, c, g)) continue;
                const Rect& r = seen[c];
                const bool vertical = d == Dir::Up || d == Dir::Down;
                float from_edge;
                if (d == Dir::Down) from_edge = r.y - gb.y;
                else if (d == Dir::Up) from_edge = (gb.y + gb.h) - (r.y + r.h);
                else if (d == Dir::Right) from_edge = r.x - gb.x;
                else from_edge = (gb.x + gb.w) - (r.x + r.w);
                const float lo_s = vertical ? s.x : s.y, hi_s = vertical ? s.x + s.w : s.y + s.h;
                const float lo_c = vertical ? r.x : r.y, hi_c = vertical ? r.x + r.w : r.y + r.h;
                const float shared = (hi_s < hi_c ? hi_s : hi_c) - (lo_s > lo_c ? lo_s : lo_c);
                Measure x{};
                x.node = c;
                x.order = model.nodes[c].order;
                x.aligned = shared > 0.0f;
                x.overlap = x.aligned ? shared : 0.0f;
                x.cost = x.aligned ? from_edge : from_edge + 2.0f * (-shared);
                if (!have || better(x, best)) { best = x; have = true; }
            }
            return have ? best.node : from;
        }
        return from;
    }

}
