#pragma once
#include "ui/ui_settings.h"
#include "ui/ui_lighting.h"

namespace oot::ui::macos {
// Each surface control owns one group. Other choices and unrelated preferences survive.
inline bool picture_quality(int quality, Settings& s) {
    if (quality < 0 || quality > 2) return false;
    s.resolution = quality == 0 ? 4 : 2;
    s.downsampling = 0;
    s.antialiasing = quality == 2 ? 3 : 1;
    s.frame_rate = 0;
    s.render_distance = 0;
    return true;
}
inline int picture_choice(const Settings& s) {
    if (s.downsampling || s.frame_rate || s.render_distance) return -1;
    if (s.resolution == 4 && s.antialiasing == 1) return 0;
    if (s.resolution == 2 && s.antialiasing == 1) return 1;
    if (s.resolution == 2 && s.antialiasing == 3) return 2;
    return -1;
}
inline bool texture_quality(int quality, int pack_count, int remembered_pack, Settings& s) {
    if (quality < 0 || quality > 2 || (quality && pack_count < 1)) return false;
    if (!quality) { s.texture_pack = 0; return true; }
    if (s.texture_pack < 1 || s.texture_pack > pack_count)
        s.texture_pack = remembered_pack >= 1 && remembered_pack <= pack_count ? remembered_pack : 1;
    s.texture_detail = quality == 1 ? 1 : 2;
    return true;
}
inline int texture_choice(const Settings& s) {
    return s.texture_pack == 0 ? 0 : (s.texture_detail == 1 ? 1 : (s.texture_detail == 2 ? 2 : -1));
}
inline bool lighting_quality(int quality, lighting::Lighting& l) {
    if (quality < 0 || quality > 3) return false;
    l = lighting::Lighting{};
    l.raytracing = quality != 0;
    l.shadows = 1;
    l.occlusion = quality >= 2 ? 1 : 0;
    l.occlusion_quality = quality == 3 ? 0 : 3;
    l.lights = quality >= 2;
    // Keep expensive, unqualified level-3 effects in the advanced controls.
    l.bounce = l.reflections = l.glow = l.haze = 0;
    l.indoor_dust = l.outdoor_dust = l.window_shafts = 0;
    return true;
}
inline int lighting_choice(const lighting::Lighting& l) {
    if (!l.raytracing) return 0;
    for (int quality = 1; quality < 4; ++quality) {
        lighting::Lighting candidate;
        lighting_quality(quality, candidate);
        if (l == candidate) return quality;
    }
    return -1;
}
}
