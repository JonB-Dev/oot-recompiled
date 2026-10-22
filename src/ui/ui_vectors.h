#pragma once

// THE INTERFACE'S VECTORS (the menus plan, phase 86). RmlUi's SVG plugin is built into its core
// against lib/lunasvg: <svg src="..."> elements and the svg() decorator draw the bundled line
// icons (assets/icons/) and the medallion's layers (assets/ui/medallion/) at any interface size.
// Only files under the install's assets/ are ever named by a document (tools/check_tokens.py
// refuses anything else), never a path from settings, a save or a person.

#include <string>

namespace oot::ui::vectors {

    // Rasterizes every SVG under assets/icons/ and assets/ui/medallion/ with lunasvg at 1x and 2x,
    // no RmlUi and no window, and fails on any that will not parse or draws nothing. Prints
    // SVG_OK <n> or SVG_FAIL and returns the exit code.
    int selftest(const std::string& assets_dir);

}
