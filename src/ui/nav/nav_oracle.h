#pragma once

// THE ORACLE (the menus plan, phase 94): the navigation rule of nav.h written a second time, apart
// from the engine and sharing none of its code, so that ui_navcheck can compare the two on every
// real document, size and direction. Two implementations that agree everywhere are the closest a
// headless check comes to proving the rule is what happens. It knows no memory: the check runs on
// a freshly opened document, where entering a group lands on the spatial target.

#include "ui/nav/nav.h"

namespace oot::ui::nav {

    int oracle_move(const Model& model, int from, Dir d);

}
