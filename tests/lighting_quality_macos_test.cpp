#include "ui/ui_lighting.h"
#include <cassert>
#include <filesystem>
#include <fstream>

namespace { oot::renderer::LightingValues applied; }
namespace oot::renderer {
void apply_lighting(const LightingValues& values) { applied = values; }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    namespace lighting = oot::ui::lighting;
    const auto root = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(root);
    const auto path = root / "lighting.txt";
    const int samples[] = {4, 8, 16, 2};
    const auto baseline = lighting::values_of(lighting::defaults());
    for (int quality = 0; quality < 4; ++quality) {
        { std::ofstream file(path); file << "occlusionquality = " << quality << '\n'; }
        lighting::load(path.string());
        assert(applied.occlusion_samples == samples[quality]);
        assert(applied.shadow_strength == baseline.shadow_strength);
        assert(applied.light_strength == baseline.light_strength);
        assert(applied.occlusion_strength == baseline.occlusion_strength);
        assert(applied.history_blend == baseline.history_blend);
        assert(lighting::apply(lighting::current()));
        lighting::load(path.string());
        assert(lighting::current().occlusion_quality == quality);
        assert(applied.occlusion_samples == samples[quality]);
    }
    for (const auto *invalid : {"-1", "999", "garbage"}) {
        { std::ofstream file(path); file << "occlusionquality = " << invalid << '\n'; }
        lighting::load(path.string());
        assert(applied.occlusion_samples == (std::string(invalid) == "999" ? 2 : 4));
    }
    std::filesystem::remove(path);
    lighting::load(path.string());
    assert(applied.occlusion_samples == 4);
}
