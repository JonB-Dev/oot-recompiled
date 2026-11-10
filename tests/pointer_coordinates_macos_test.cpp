#include "ui/input/pointer_coordinates.h"
#include <cassert>
int main() {
    using oot::ui::input::drawable_coordinate;
    assert(drawable_coordinate(200, 1280, 1280) == 200);
    assert(drawable_coordinate(200, 1512, 3024) == 400);
    assert(drawable_coordinate(220, 949, 1898) == 440);
    assert(drawable_coordinate(200, 1280, 1920) == 300);
    // Display changes and temporary zero-sized windows must remain usable.
    assert(drawable_coordinate(200, 1280, 0) == 200);
    assert(drawable_coordinate(200, 0, 2560) == 200);
    assert(drawable_coordinate(-10, 1280, 2560) == -20);
}
