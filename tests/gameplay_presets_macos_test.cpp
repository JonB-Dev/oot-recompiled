#include "ui/macos_gameplay_presets.h"
#include <cassert>
namespace oot::renderer { void apply_lighting(const LightingValues&) {} }
int main() {
    using namespace oot::ui;
    Settings s;
    s.volume_percent = 37; s.texture_pack = 2; s.texture_detail = 1;
    lighting::Lighting l;
    for (int quality = 0; quality < 3; ++quality) {
        assert(macos::picture_quality(quality, s));
        assert(macos::picture_choice(s) == quality);
        assert(s.resolution == (quality == 0 ? 4 : 2));
        assert(s.antialiasing == (quality == 2 ? 3 : 1));
        assert(s.volume_percent == 37 && s.texture_pack == 2 && s.texture_detail == 1);
        assert(s.frame_rate == 0 && s.downsampling == 0);
        assert(s.window_mode == 0 && s.aspect == 0);
    }
    assert(!macos::picture_quality(3, s));
    s.resolution = 6;
    assert(macos::picture_choice(s) == -1);
    assert(!macos::texture_quality(2, 0, 0, s)); // No pack: never pretend it is enabled.
    assert(s.texture_pack == 2);
    assert(macos::texture_quality(0, 2, 2, s));
    assert(macos::texture_choice(s) == 0);
    assert(macos::texture_quality(1, 2, 2, s));
    assert(s.texture_pack == 2 && s.texture_detail == 1 && s.resolution == 6);
    assert(macos::texture_quality(2, 1, 2, s)); // Removed remembered pack: use the available one.
    assert(s.texture_pack == 1 && s.texture_detail == 2);
    assert(!macos::texture_quality(-1, 2, 1, s));
    for (int effect = 0; effect < 4; ++effect) {
        assert(macos::lighting_quality(effect, l));
        assert(macos::lighting_choice(l) == effect);
        assert(lighting::values_of(l).level == (effect < 2 ? effect : 2));
        if (effect >= 2) assert(lighting::values_of(l).occlusion_samples == (effect == 3 ? 4 : 2));
        const auto saved = l;
        assert(macos::lighting_quality(effect, l));
        assert(l == saved);
    }
    const auto saved = l;
    assert(!macos::lighting_quality(4, l));
    assert(l == saved);
    l.glow = 2;
    assert(macos::lighting_choice(l) == -1);
}
