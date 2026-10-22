#include "ui/ui_warps.h"

#include <cstdio>

#include "game/game_state.h"
#include "game/warp_table.ntsc-1.0.h"

namespace {

    using oot::ui::warps::Level;

    Level g_level = Level::Regions;

    // Where we have walked to. -1 means not chosen, which is the state at the level above.
    int g_region = -1;
    int g_place = -1;

    // One focus per level, so climbing back out lands where you were rather than at the top.
    int g_focus[3] = { 0, 0, 0 };

    // The game's entrance index as of the last refresh. Negative when there is no game running or
    // memory is not bound, in which case nothing is marked rather than everything.
    int g_here = -1;

    int level_index() {
        switch (g_level) {
            case Level::Places:    return 1;
            case Level::Entrances: return 2;
            default:               return 0;
        }
    }

    const oot::warps::Region& region() {
        return oot::warps::REGIONS[g_region];
    }

    const oot::warps::Place& place() {
        return region().places[g_place];
    }

    bool region_valid() {
        return g_region >= 0 && g_region < oot::warps::REGION_COUNT;
    }

    bool place_valid() {
        return region_valid() && g_place >= 0 && g_place < region().count;
    }

    // Does any entrance of this place lead where the game currently is?
    bool place_holds_here(const oot::warps::Place& p) {
        if (g_here < 0) {
            return false;
        }
        for (int i = 0; i < p.count; ++i) {
            if (p.entrances[i].index == g_here) {
                return true;
            }
        }
        return false;
    }

} // namespace

namespace oot::ui::warps {

    Level level() {
        return g_level;
    }

    std::string trail() {
        if (!region_valid()) {
            return {};
        }
        std::string text = region().name;
        if (g_level == Level::Entrances && place_valid()) {
            text += " / ";
            text += place().name;
        }
        return text;
    }

    int row_count() {
        switch (g_level) {
            case Level::Places:    return region_valid() ? region().count : 0;
            case Level::Entrances: return place_valid() ? place().count : 0;
            default:               return oot::warps::REGION_COUNT;
        }
    }

    std::string row_label(int index) {
        if (index < 0 || index >= row_count()) {
            return {};
        }
        switch (g_level) {
            case Level::Places:    return region().places[index].name;
            case Level::Entrances: return place().entrances[index].label;
            default:               return oot::warps::REGIONS[index].name;
        }
    }

    std::string row_value(int index) {
        if (index < 0 || index >= row_count()) {
            return {};
        }

        char buffer[32];
        switch (g_level) {
            case Level::Places: {
                const oot::warps::Place& p = region().places[index];
                if (p.count == 1) {
                    std::snprintf(buffer, sizeof(buffer), "0x%03X", p.entrances[0].index);
                    return buffer;
                }
                std::snprintf(buffer, sizeof(buffer), "%d ways in", p.count);
                return buffer;
            }
            case Level::Entrances:
                std::snprintf(buffer, sizeof(buffer), "0x%03X", place().entrances[index].index);
                return buffer;
            default: {
                const int count = oot::warps::REGIONS[index].count;
                std::snprintf(buffer, sizeof(buffer), "%d place%s", count, count == 1 ? "" : "s");
                return buffer;
            }
        }
    }

    bool row_is_here(int index) {
        if (g_here < 0 || index < 0 || index >= row_count()) {
            return false;
        }
        switch (g_level) {
            case Level::Places:
                return place_holds_here(region().places[index]);
            case Level::Entrances:
                return place().entrances[index].index == g_here;
            default: {
                const oot::warps::Region& r = oot::warps::REGIONS[index];
                for (int i = 0; i < r.count; ++i) {
                    if (place_holds_here(r.places[i])) {
                        return true;
                    }
                }
                return false;
            }
        }
    }

    Action activate(int index) {
        if (index < 0 || index >= row_count()) {
            return Action::Nothing;
        }

        switch (g_level) {
            case Level::Regions:
                g_focus[0] = index;
                g_region = index;
                g_place = -1;
                g_level = Level::Places;
                g_focus[1] = 0;
                return Action::Descended;

            case Level::Places: {
                g_focus[1] = index;
                const oot::warps::Place& p = region().places[index];
                // One way in means one row, and a list of one is not worth opening.
                if (p.count == 1) {
                    oot::game_state::set_pending_warp(p.entrances[0].index);
                    std::fprintf(stderr, "[warp] %s / %s -> 0x%03X\n",
                                 region().name, p.name, p.entrances[0].index);
                    return Action::Warped;
                }
                g_place = index;
                g_level = Level::Entrances;
                g_focus[2] = 0;
                return Action::Descended;
            }

            case Level::Entrances: {
                g_focus[2] = index;
                const oot::warps::Entrance& e = place().entrances[index];
                oot::game_state::set_pending_warp(e.index);
                std::fprintf(stderr, "[warp] %s / %s / %s -> 0x%03X\n",
                             region().name, place().name, e.label, e.index);
                return Action::Warped;
            }
        }
        return Action::Nothing;
    }

    bool ascend() {
        switch (g_level) {
            case Level::Entrances:
                g_level = Level::Places;
                g_place = -1;
                return true;
            case Level::Places:
                g_level = Level::Regions;
                g_region = -1;
                return true;
            default:
                return false;
        }
    }

    int focus() {
        const int index = level_index();
        const int count = row_count();
        if (g_focus[index] >= count) {
            g_focus[index] = count > 0 ? count - 1 : 0;
        }
        if (g_focus[index] < 0) {
            g_focus[index] = 0;
        }
        return g_focus[index];
    }

    void set_focus(int index) {
        g_focus[level_index()] = index;
    }

    void reset() {
        g_level = Level::Regions;
        g_region = -1;
        g_place = -1;
        g_focus[0] = 0;
        g_focus[1] = 0;
        g_focus[2] = 0;
    }

    void refresh_here() {
        const oot::game_state::Snapshot now = oot::game_state::read();
        g_here = now.valid ? now.entrance_index : -1;
    }

} // namespace oot::ui::warps
