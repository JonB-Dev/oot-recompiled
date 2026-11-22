#pragma once

#include <string>

// A row of words built by hand rather than by the rows template (ui/models/rows_model.cpp): its label
// and value in the group a settings row keeps them in, so a narrow page stacks them the same way, the
// value under the label (the user, 2026-11-22: the Debugging section's Warps rows stayed side by side
// in the docked menu). The shell builds the Warps list with it and ui_navcheck lays out the same markup.
namespace oot::ui::text_row {

    struct Parts {
        std::string id;          // the row's whole id
        std::string classes;     // added after "row", such as "here"
        std::string label;       // escaped here
        std::string value_rml;   // the value's markup as given: text the caller escaped, or step controls
    };

    std::string rml(const Parts& parts);

} // namespace oot::ui::text_row
