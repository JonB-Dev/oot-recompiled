#pragma once

// The shell's data model (the menus plan, phase 96): what assets/ui/shell.rml draws around the
// page. The router (src/ui/shell/shell.h) decides; this carries its answer to the document: the
// dial's medallions and where each stands, the current section's name, the placement, whether the
// game is held, and the prompts the footer shows. Nothing here decides anything.

#include <string>
#include <vector>

namespace Rml { class Context; class ElementDocument; }

namespace oot::ui::shell_model {

    // A section on the dial. `slot` is its place relative to the current one: 0 the current, -1
    // and 1 beside it, -2 and 2 at the ends, 99 not on the dial at the moment (the rail shows every
    // section whatever its slot).
    struct Section {
        std::string id;      // "play", "settings", ...: the element's id is "sec-" + id
        std::string name;    // as the dial and the rail spell it
        std::string glyph;   // an icon under assets/icons/, without ".svg"
        int slot = 99;
        bool current = false;
    };

    // A prompt in the footer: the device's button and what it does here. `kind` is "face" (a
    // medallion with a letter or a drawn shape), "plate" (a word on a glossy plate) or "pad" (the
    // touchpad's trapezoid); `shape` names a drawn shape ("cross", "circle", "square", "triangle")
    // or is empty for a letter; `tint` is the button's color as its controllers code it.
    struct Prompt {
        std::string kind;
        std::string key;
        std::string shape;
        std::string tint;
        std::string label;
        std::string action;  // the element's id is "prompt-" + action; a click does what it says
    };

    // The footer's length, always: room for the longest set of prompts, the rest empty.
    // The footer's longest: the dial's pair, Select, Change, Back, Type a path, and Close with its
    // second working binding.
    constexpr size_t PROMPT_SLOTS = 8;
    // PHOTO MODE'S BUTTON GUIDE (2026-10-13, photo-guide.rml): its own list in this model, drawn
    // with the footer's own prompts. Fixed length too: nine actions, a pair among them, and each
    // single control showing up to its two working bindings (2026-10-13): fifteen at the most.
    constexpr size_t GUIDE_SLOTS = 16;

    struct State {
        std::string mode;        // "launcher" or "game"
        std::string placement;   // "launcher", "left", "right" or "center"
        std::string title;       // the current section's name
        std::string version;
        bool held = false;       // the game is held behind the menu
        std::vector<Section> sections;
        std::vector<Prompt> prompts;
    };

    bool create(Rml::Context* context);

    // Replaces what the document shows, marking only what changed.
    void set(const State& state);

    const State& get();

    // The button guide's prompts, marked only when they change; padded to GUIDE_SLOTS.
    void set_guide(const std::vector<Prompt>& prompts);

    void release();

    // THE FOOTER'S PROMPTS NEVER SHRINK (the user, 2026-10-13: "Those are already relatively small,
    // so maybe don't scale those down"): below 100% the shell's document takes the class that gives
    // the footer its sizes in px (tokens.rcss, .prompts-full-size), so they stay at their 100% size
    // while the rest of the menu shrinks. `dp` is the context's interface size.
    void hold_prompt_size(Rml::ElementDocument* doc, float dp);

}
