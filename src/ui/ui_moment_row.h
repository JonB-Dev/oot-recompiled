#pragma once

#include <string>

// One row of the Moments list as markup: the picture, or the empty box in its place, then the
// words in the group a settings row keeps them in (ui/models/rows_model.cpp), so a narrow page
// stacks them the same way: the name, the slot's line under it, and where and when under that,
// beside the picture (the user, 2026-11-20). The shell builds the list with it and ui_navcheck
// lays out the same markup.
namespace oot::ui::moment_row {

    struct Parts {
        std::string id;        // the row's id after "mrow-"
        std::string picture;   // the thumbnail's src; empty draws the box without one
        std::string title;     // the moment's name, or the slot's when it holds none
        std::string slot;      // the line under it: the slot's name, why it failed, or Empty
        std::string where;     // where and when it was taken; empty when it holds no moment
    };

    // Every text and the picture's src are escaped here.
    std::string rml(const Parts& parts);

} // namespace oot::ui::moment_row
