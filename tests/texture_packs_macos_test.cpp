#include "game/texture_packs.h"
#include "main/places.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void write(const fs::path& path, const std::string& value) {
    std::ofstream(path) << value;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path data(argv[1]);
    assert(oot::places::adopt_install_root(data));
    const auto root = oot::places::texture_packs();

    fs::create_directories(root / "folder");
    write(root / "folder/rt64.json", "{}");
    write(root / "folder/mod.json", "{\"display_name\":\"Named pack\"}");
    fs::create_directories(root / "outer/inner");
    write(root / "outer/inner/rt64.json", "{}");
    fs::create_directories(root / "malformed");
    write(root / "malformed/mod.json", "bad json");
    write(root / "archive.rtz", "placeholder");
    write(root / "archive2.ZIP", "placeholder");

    oot::packs::rescan();
    assert(oot::packs::option_count() == 5);
    bool found_folder = false, found_nested = false, found_rtz = false, found_zip = false;
    for (int i = 1; i < oot::packs::option_count(); ++i) {
        const auto path = oot::packs::path_for(i);
        found_folder |= path == root / "folder" && std::string(oot::packs::label_for(i)) == "Named pack";
        found_nested |= path == root / "outer/inner";
        found_rtz |= path == root / "archive.rtz";
        found_zip |= path == root / "archive2.ZIP";
    }
    assert(found_folder && found_nested && found_rtz && found_zip);
    assert(oot::packs::path_for(0).empty());
    assert(oot::packs::path_for(999).empty());

    fs::remove(root / "archive.rtz");
    oot::packs::rescan();
    assert(oot::packs::option_count() == 4);
    assert(fs::is_regular_file(root / "README.txt"));
}
