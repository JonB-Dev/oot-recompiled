#include "main/places.h"

#include <cassert>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    assert(!oot::places::adopt_install_root({}));
    assert(!oot::places::adopt_install_root("relative"));
    assert(oot::places::adopt_install_root(root));
    assert(oot::places::data() == root);
    assert(oot::places::saves() == root / "saves");
    assert(oot::places::texture_packs() == root / "texture-packs");
    assert(std::filesystem::is_directory(oot::places::saves()));
    assert(std::filesystem::is_directory(oot::places::texture_packs()));
    assert(oot::places::adopt_install_root(root));
    assert(!oot::places::default_install_root().empty());
}
