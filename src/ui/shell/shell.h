#pragma once

// THE ONE MENU'S ROUTER (the menus plan, phase 96): which section the dial is on, the pages
// pushed inside it, the layers (dialogs) over it, and what Back does. One menu serves the
// launcher and the game; only the dial's sections and Back's last step differ between them.
// Pure logic, no RmlUi, so ui_tests checks every rule below; the shell's document draws what
// this says.
//
//   THE DIAL turns with the shoulders, Q and E, or a click on a medallion, and wraps: past the last
//   section is the first. Its medallions are for the pointer and the shoulders only; the
//   directions move within the page, never onto the dial (nav_rml.h, data-nav="pointer").
//
//   BACK closes the top layer, else pops a page, else closes the menu in the game. Before Play,
//   Back on a section's first page does nothing: the launcher is the program, and Quit is a
//   button (the plan's phase 96 exit).
//
//   DEBUGGING is a section only in the game and only after its confirmation (the user,
//   2026-09-26: "one debugging menu item ... behind a warning"); once confirmed it stays on the
//   dial until the program closes.

#include <vector>

namespace oot::ui::menu {

    // THE SETTINGS SPLIT (the user, 2026-10-13: "right now the settings menu is huge"): Settings
    // keeps what does not change the game itself; Graphics the picture, the textures and, as a
    // page, the lighting; Gameplay how the game behaves; Capture its Photo and Video pages.
    enum class Section { Play, Settings, Graphics, Gameplay, Hud, Controls, Capture, Moments, Debugging, About };

    // A page inside a section. The section's own first page is Root.
    enum class Page { Root, Browse, Files, Warps, Lighting, Photo, Video };

    // A layer over the page: a dialog, the on-screen keyboard.
    enum class Layer { Ask, Keyboard };

    enum class Mode { Launcher, Game };

    // Where the menu sits: docked to an edge (the Medallion direction, mirrored on the right),
    // centered (the Ledger with medallions), or the launcher's own window.
    enum class Placement { DockedLeft, DockedRight, Centered, Launcher };

    // The Menu placement setting: 0 over the picture, 1 a panel on the right, 2 on the left.
    Placement placement_for(Mode mode, int menu_side);

    enum class BackResult { ClosedLayer, PoppedPage, ClosedMenu, Nothing };

    class Router {
    public:
        explicit Router(Mode mode = Mode::Launcher);

        Mode mode() const { return mode_; }
        void set_mode(Mode mode);

        // The menu is up. Before Play it always is.
        bool open() const { return open_; }

        // THE MENU HOLDS THE GAME (phase 97): in the game, while it is open, whatever section,
        // page or dialog is up; never before Play, when there is no game to hold.
        bool holds() const { return mode_ == Mode::Game && open_; }

        Section section() const { return section_; }
        Page page() const { return pages_.empty() ? Page::Root : pages_.back(); }
        int depth() const { return static_cast<int>(pages_.size()); }
        const std::vector<Layer>& layers() const { return layers_; }

        // The dial's sections, in order, for the mode and the debugging gate.
        std::vector<Section> sections() const;
        bool available(Section section) const;

        // Opens the menu on `section` (its first page); an unavailable section opens the first.
        void open_on(Section section);
        // Closes the menu in the game, every page and layer with it. Before Play it stays open.
        void close();
        // The menu key: opens on the last section visited, or closes.
        void toggle();

        // Turns the dial one section either way, wrapping, to that section's first page.
        void next();
        void prev();

        void push(Page page);
        void push_layer(Layer layer);
        void pop_layer();

        BackResult back();

        // After the confirmation: Debugging joins the dial (in the game) and opens.
        void unlock_debugging();
        bool debugging_unlocked() const { return debugging_; }

    private:
        void go(Section section);
        void turn(int step);

        Mode mode_;
        bool open_;
        Section section_;
        Section last_;
        std::vector<Page> pages_;
        std::vector<Layer> layers_;
        bool debugging_ = false;
    };

    // CLOSE GAME? (the owner, 2026-11-22): what the window's close button asks in play. With Save on
    // quit on nothing is lost; with it off the detail is a red notice.
    struct CloseQuestion {
        const char* detail;
        bool danger;
    };
    CloseQuestion close_question(bool save_on_quit);

    // One click of the close button arrives twice (a window close and a quit), so a close while the
    // question is up counts as a second press, and closes, only this long after it was asked.
    constexpr double CLOSE_AGAIN_SECONDS = 0.5;
    constexpr bool second_close_counts(double since_asked) { return since_asked >= CLOSE_AGAIN_SECONDS; }

    // The router against every rule above, and the close question (ui_tests).
    bool selftest(int& passed);

}
