#pragma once

// A laid-out RmlUi document as the navigation engine's model (the menus plan, phase 94). The shell
// builds one whenever a document's layout changes; ui_navcheck builds one per document, size and
// interface size and checks the engine against the oracle on it.
//
// What the markup declares, and nothing else, decides what is in the model: no screen lists its
// rows in code.
//
//   an item    <button>, <input>, <select>, or any element with data-act="..." (an action),
//              data-value="..." (a value row, nav_values.h) or data-nav="item"; data-disabled or
//              the disabled attribute makes it unreachable
//   a group    data-nav="group"; data-nav-wrap makes it wrap; overflow-y auto or scroll makes it
//              a scroller (the wheel, scroll into view)
//   a layer    data-nav="layer" (a dialog, the keyboard): while shown it traps the focus. It is
//              hidden by its stylesheet and shown by the class is-open, and it names what Back
//              does there in data-back (data-back="close"); ui_navcheck fails a layer without one
//
// The focused element carries the class is-f, and its look must change with it (its own style or
// a descendant's): ui_navcheck fails an element that looks the same focused.
//
// Hidden elements (display none, visibility hidden, anywhere up the tree) are left out with
// everything inside them.

#include "ui/nav/nav.h"

#include <unordered_map>
#include <vector>

namespace Rml { class Element; class ElementDocument; }

namespace oot::ui::nav {

    struct RmlModel {
        Model model;
        std::vector<Rml::Element*> elements;              // node index -> element
        std::unordered_map<Rml::Element*, int> index;     // element -> node index
    };

    RmlModel build(Rml::ElementDocument* document);

}
