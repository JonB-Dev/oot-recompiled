#pragma once

// About's credits (the menus plan, phase 106), read from assets/licenses/MANIFEST.txt: every
// bundled file we did not draw ourselves, grouped by where it came from and its license, so a
// face or an icon set added to the manifest is credited without a line here. Pure text in and
// out, so ui_tests checks that every entry the manifest lists is credited.

#include <string>
#include <vector>

namespace oot::ui::credits {

    struct Credit {
        std::string source;    // where it came from (a repository, a family)
        std::string license;   // its license, as the manifest names it
        std::vector<std::string> files;   // the bundled files, under assets/
    };

    // The manifest's lines (tab separated: path, source, version, license and its text, hash,
    // note) as credits. Our own drawn files (licensed as this program) are not credits.
    std::vector<Credit> from_manifest(const std::string& text);

    // One line a credit for About: the family, the license, how many files.
    std::string describe(const Credit& credit);

    bool selftest(const std::string& manifest_text, int& passed);

}
