// The texture packs a person has put in their own folder, found by looking.
//
// The user, 2026-09-30: "add a folder to the 'Your files' to allow users a drag and drop of
// texture packs and it will allow cyclying through all the texture packs that they add there in a
// new setting on main launcher/in-game launcher menu", then, on what drag and drop means here:
// "when i say drag and drop, i mean, the user clicks open and moves their files to the folder".
// So there is no drop target and no import step. The folder is opened in the file manager, the
// person puts a pack in it, and this finds whatever is there the next time it looks.
//
// THIS FILE KNOWS NOTHING ABOUT THE RENDERER, on purpose. It answers what packs exist and where
// they are; `src/game/rt64_render_context.cpp` is the only place that holds the RT64 application
// and is where a chosen pack is actually handed over. Keeping the scan free of RT64 means the
// launcher can list packs before a renderer exists, which is exactly when the row is first shown.
//
// NOTHING HERE IS EVER COPIED, CONVERTED OR WRITTEN. A pack is the person's own material and
// stays their file in their folder: we read its name and hand the renderer its path. That is the
// same rule the ROM follows, and it is why a pack can never end up inside this repository.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace oot::packs {

    struct Pack {
        // What the row calls it: the pack's own display name when it declares one, else the name
        // of the folder or file the person dropped in.
        std::string name;
        // What the renderer is given. A directory holding rt64.json, or a zip.
        std::filesystem::path path;
    };

    // Look in the texture packs folder and answer what is there, sorted by name so the row's
    // order does not depend on the order the file system happens to hand them back.
    //
    // Cheap enough to call when a menu opens and nowhere near cheap enough to call per frame: it
    // touches the disk. `list()` returns the last scan.
    void rescan();

    // The packs found by the last rescan(), which is empty until one has run.
    const std::vector<Pack>& list();

    // The pack at a row value, where 0 is Off and 1 is the first pack. Empty path for Off or for
    // a value past the end, which is what a settings file naming a pack that has since been
    // deleted looks like.
    std::filesystem::path path_for(int value);

    // The row's label for a value, with the same rule. "Off" at 0.
    const char* label_for(int value);

    // How many options the row has: Off plus one per pack.
    int option_count();

    // Make the folder if it is not there, so Your files can show it and the file manager can open
    // it on the first run rather than reporting a missing path.
    void ensure_folder();

} // namespace oot::packs
