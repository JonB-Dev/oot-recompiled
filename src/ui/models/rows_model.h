#pragma once

// THE ROWS AS DATA (the menus plan, phase 95). A row of a rows document (the settings, the
// launcher's list, lighting, the HUD, debugging) is an entry of its table and nothing else: the
// shell computes what each row says from the tables (ui_shell.cpp, row_views) and hands the list
// here; the documents draw it through one RmlUi data model, "rows", with an array per document.
// No row is ever written as markup in code.
//
// A document's rows container carries the model (settings.rml):
//
//     <div class="rows long" id="settings-rows" data-model="rows"></div>
//
// and the shell installs the one row template into it when the document loads (install below),
// so five documents share one template rather than five copies of it. The template is bound, not
// built: its markup is fixed, and every word in it comes from the model.
//
// The ids the shell's clicks and focus read (row-N, prev-N, next-N) come from `n`, the row's place
// in its screen's order, exactly as the string builders made them.

#include <string>
#include <vector>

namespace Rml { class Context; class Element; }

namespace oot::ui::rows {

    // How a row's value is drawn.
    enum Shape : int {
        Stepper = 1,   // the option between two steps, an end shown spent
        Check = 2,     // a box, ticked while on
        Action = 3,    // words shown only while focused or under the pointer
        Text = 4,      // plain words (the ROM row)
    };

    // One option of a short set, drawn beside the others (phase 100, the decision's segmented
    // choices): its words, and whether it is the one in force.
    struct Choice {
        std::string label;
        bool chosen = false;
    };

    struct Row {
        int n = 0;                // its place in the screen's row order: the row-, prev- and next- ids
        std::string label;
        std::string value;        // the option shown, an action's words, or plain text
        int shape = Stepper;
        bool low = false;         // a stepper at its first option
        bool high = false;        // a stepper at its last option
        bool on = false;          // a check that is ticked
        std::string note;         // what the chosen option means, under the row
        std::string section;      // a divider drawn before the row
        bool focused = false;
        // THE PLATE (the menus plan, phase 100, the decision's row): the glyph in its ring (an icon
        // under assets/icons/, without ".svg"); a short set of options drawn side by side, every
        // one of them, the one in force lit; or, for a longer set, a tick per option with the one
        // in force lit beside the option's words. Empty when the row is neither.
        std::string icon;
        std::vector<Choice> choices;
        std::vector<int> ticks;
    };

    // One list per rows document.
    enum class List { Launch, Settings, Lighting, Hud, Debug };

    // Creates the model in the context, before any rows document loads.
    bool create(Rml::Context* context);

    // Puts the row template, bound to `list`, into a rows document's container.
    void install(Rml::Element* container, List list);

    // Replaces a document's rows; the context draws them on its next update.
    void set(List list, std::vector<Row> rows);

    // Marks the focused row by `n` (-1 for none) without rebuilding the list.
    void focus(List list, int n);

    // Forgets the model (the context is going away).
    void release();

    // One row as one line of text, every field, for the rows dump (--ui-rows-dump).
    std::string dump_line(const Row& row);

    // The template against the real RmlUi, no window (ui_navcheck): a row of every shape, drawn,
    // its ids, classes and words as the string builders made them; a row taken away goes. Creates
    // the model in `context` if it is not there. Returns the number of failures, each printed.
    int selftest(Rml::Context* context, int& passed);

}
