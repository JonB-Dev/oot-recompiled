// The one menu's router. See shell.h.

#include "ui/shell/shell.h"

#include <algorithm>
#include <cstdio>

namespace oot::ui::menu {

    Placement placement_for(Mode mode, int menu_side) {
        if (mode == Mode::Launcher) return Placement::Launcher;
        if (menu_side == 1) return Placement::DockedRight;
        if (menu_side == 2) return Placement::DockedLeft;
        return Placement::Centered;
    }

    Router::Router(Mode mode)
        : mode_(mode), open_(mode == Mode::Launcher),
          section_(mode == Mode::Launcher ? Section::Play : Section::Settings),
          last_(Section::Settings) {}

    void Router::set_mode(Mode mode) {
        mode_ = mode;
        pages_.clear();
        layers_.clear();
        if (mode == Mode::Launcher) {
            open_ = true;
            section_ = Section::Play;
        }
        else {
            // Play started the game: the menu goes, and opens next on Settings.
            open_ = false;
            section_ = Section::Settings;
            last_ = Section::Settings;
        }
    }

    std::vector<Section> Router::sections() const {
        std::vector<Section> out;
        if (mode_ == Mode::Launcher) out.push_back(Section::Play);
        for (Section s : { Section::Settings, Section::Graphics, Section::Gameplay, Section::Hud, Section::Controls,
                           Section::Capture, Section::Moments }) {
            out.push_back(s);
        }
        if (mode_ == Mode::Game && debugging_) out.push_back(Section::Debugging);
        out.push_back(Section::About);
        return out;
    }

    bool Router::available(Section section) const {
        const std::vector<Section> s = sections();
        return std::find(s.begin(), s.end(), section) != s.end();
    }

    void Router::go(Section section) {
        if (!available(section)) section = sections().front();
        section_ = section;
        pages_.clear();
        if (mode_ == Mode::Game) last_ = section;
    }

    void Router::open_on(Section section) {
        layers_.clear();
        go(section);
        open_ = true;
    }

    void Router::close() {
        layers_.clear();
        pages_.clear();
        if (mode_ == Mode::Game) open_ = false;
    }

    void Router::toggle() {
        if (mode_ == Mode::Launcher) return;   // the launcher is the program; nothing to toggle
        if (open_) close();
        else open_on(last_);
    }

    void Router::turn(int step) {
        if (!open_ || !layers_.empty()) return;   // a dialog holds the focus; the dial waits
        const std::vector<Section> s = sections();
        const auto at = std::find(s.begin(), s.end(), section_);
        const int i = at == s.end() ? 0 : static_cast<int>(at - s.begin());
        const int n = static_cast<int>(s.size());
        go(s[static_cast<size_t>(((i + step) % n + n) % n)]);
    }

    void Router::next() { turn(1); }
    void Router::prev() { turn(-1); }

    void Router::push(Page page) {
        if (page != Page::Root) pages_.push_back(page);
    }

    void Router::push_layer(Layer layer) {
        layers_.push_back(layer);
    }

    void Router::pop_layer() {
        if (!layers_.empty()) layers_.pop_back();
    }

    BackResult Router::back() {
        if (!layers_.empty()) { layers_.pop_back(); return BackResult::ClosedLayer; }
        if (!pages_.empty()) { pages_.pop_back(); return BackResult::PoppedPage; }
        if (mode_ == Mode::Game && open_) { close(); return BackResult::ClosedMenu; }
        return BackResult::Nothing;
    }

    void Router::unlock_debugging() {
        if (mode_ != Mode::Game) return;
        debugging_ = true;
        open_on(Section::Debugging);
    }

    // -------------------------------------------------------------------------------------------
    // The rules, checked
    // -------------------------------------------------------------------------------------------

    bool selftest(int& passed) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  menu: %s\n", what); ok = false; }
        };
        using S = Section;

        {
            Router r(Mode::Launcher);
            const std::vector<S> want = { S::Play, S::Settings, S::Graphics, S::Gameplay, S::Hud, S::Controls, S::Capture,
                                          S::Moments, S::About };
            check(r.sections() == want, "the launcher's dial: Play, Settings, Graphics, Gameplay, HUD, Controls, Capture, Moments, About");
            check(r.open() && r.section() == S::Play, "the launcher opens on Play, and is always open");
            r.prev();
            check(r.section() == S::About, "turning back from the first section wraps to the last");
            r.next();
            check(r.section() == S::Play, "and forward wraps to the first");
            check(r.back() == BackResult::Nothing && r.open(), "Back on the launcher's first page does nothing");
            r.push(Page::Browse);
            check(r.page() == Page::Browse && r.depth() == 1, "a page pushes");
            check(r.back() == BackResult::PoppedPage && r.page() == Page::Root, "Back pops it");
            r.close();
            check(r.open(), "the launcher cannot be closed");
            r.toggle();
            check(r.open(), "the menu key does nothing to the launcher");
            r.unlock_debugging();
            check(!r.available(S::Debugging), "no Debugging before Play");
        }
        {
            Router r(Mode::Launcher);
            r.set_mode(Mode::Game);
            check(!r.open(), "Play closes the menu");
            const std::vector<S> want = { S::Settings, S::Graphics, S::Gameplay, S::Hud, S::Controls, S::Capture, S::Moments,
                                          S::About };
            check(r.sections() == want, "the game's dial: Settings to About, no Play, no Debugging yet");
            r.toggle();
            check(r.open() && r.section() == S::Settings, "the menu key opens on Settings the first time");
            r.next();
            r.next();
            check(r.section() == S::Gameplay, "the shoulders turn the dial");
            check(r.back() == BackResult::ClosedMenu && !r.open(), "Back on a first page closes the menu in the game");
            r.toggle();
            check(r.section() == S::Gameplay, "it opens again where it was left");
            r.open_on(S::Controls);
            check(r.section() == S::Controls && r.depth() == 0, "F3 opens straight on Controls");
            r.open_on(S::About);
            check(r.section() == S::About, "F2 opens straight on About");
            r.open_on(S::Debugging);
            check(r.section() == S::Settings, "Debugging cannot be opened before its confirmation");
            r.unlock_debugging();
            check(r.section() == S::Debugging && r.available(S::Debugging), "after it, Debugging joins the dial and opens");
            check(r.sections().size() == 9 && r.sections()[7] == S::Debugging, "it sits before About");
            r.push(Page::Warps);
            r.push_layer(Layer::Ask);
            r.next();
            check(r.section() == S::Debugging, "the dial waits while a dialog is up");
            check(r.back() == BackResult::ClosedLayer, "Back closes the dialog first");
            check(r.back() == BackResult::PoppedPage && r.page() == Page::Root, "then the page");
            check(r.back() == BackResult::ClosedMenu, "then the menu");
            r.open_on(S::Settings);
            r.push(Page::Files);
            r.next();
            check(r.section() == S::Graphics && r.depth() == 0, "turning the dial leaves the pages behind");
            check(!r.holds() || r.open(), "the hold is never on with the menu closed");
            check(placement_for(Mode::Game, 0) == Placement::Centered && placement_for(Mode::Game, 1) == Placement::DockedRight &&
                  placement_for(Mode::Game, 2) == Placement::DockedLeft && placement_for(Mode::Launcher, 2) == Placement::Launcher,
                  "the placement follows the Menu placement row, and the launcher is its own");
        }
        {
            // The hold, across everything that can happen to an open menu (phase 97).
            Router r(Mode::Launcher);
            check(!r.holds(), "the launcher holds nothing: there is no game yet");
            r.set_mode(Mode::Game);
            check(!r.holds(), "Play leaves the game running");
            r.toggle();
            check(r.holds(), "opening the menu holds the game");
            r.next();
            check(r.holds(), "turning the dial keeps it held");
            r.push(Page::Files);
            r.push_layer(Layer::Ask);
            check(r.holds(), "a page and a dialog keep it held");
            r.back();
            r.back();
            check(r.holds(), "closing the dialog and the page keeps it held");
            r.back();
            check(!r.holds(), "closing the menu lets the game go");
            r.open_on(Section::About);
            r.close();
            check(!r.holds(), "and so does closing it from any section");
        }
        return ok;
    }

}
