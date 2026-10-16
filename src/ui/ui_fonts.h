#pragma once

// THE INTERFACE'S THREE FACES (the menus plan, phase 84): the website's own, bundled under the
// SIL Open Font License, with the license texts in assets/licenses/ and their hashes in
// assets/licenses/MANIFEST.txt.
//
//   Big Shoulders Display   anything announced (titles, the section names)    800, 900
//   Bricolage Grotesque     anything explained (labels, notes, buttons)       400, 600
//   JetBrains Mono          anything measured (values, versions, captions)    400, 700
//
// The mono's bold is 700, not the website's 600: the face ships no 600 instance, and RmlUi reads
// only a variable font's named instances, so 600 would quietly become whichever is nearest.
//
// RCSS custom properties take one family each (tokens.rcss says why), so the tokens name these
// three families, and the fallback, used only when a bundled file is missing, registers system
// faces UNDER THESE NAMES rather than under their own.

#include <string>

namespace oot::ui::fonts {

    // Registers the faces with RmlUi. Call after Rml::Initialise and before any document loads.
    void load(const std::string& assets_dir);

    // Reads each bundled face with FreeType, without RmlUi and without a window: its family name
    // and the weights the tokens use must be named instances. A missing file reports the system
    // fallback instead. Prints FONTS_OK or FONTS_FAIL and returns the exit code.
    int selftest(const std::string& assets_dir);

}
