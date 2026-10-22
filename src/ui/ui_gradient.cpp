// Gradient ramps. See ui_gradient.h.

#include "ui_gradient.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace oot::ui::gradient {

    namespace {

        float smoothstep(float edge0, float edge1, float x) {
            if (edge1 <= edge0) {
                return x < edge0 ? 0.0f : 1.0f;
            }
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // RmlUi's mix_stop_colors, line for line: start at the first stop's color and blend toward
        // each next stop by a smoothstep across the gap between them.
        void color_at(const std::vector<Stop>& stops, float t, float out[4]) {
            for (int c = 0; c < 4; ++c) out[c] = stops[0].rgba[c];
            for (size_t i = 1; i < stops.size(); ++i) {
                const float k = smoothstep(stops[i - 1].position, stops[i].position, t);
                for (int c = 0; c < 4; ++c) out[c] = out[c] + (stops[i].rgba[c] - out[c]) * k;
            }
        }

        uint8_t byte_of(float v) {
            return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }

    }

    Ramp bake(const std::vector<Stop>& stops) {
        Ramp ramp;
        ramp.pixels.assign(RAMP_WIDTH * 4, 0);
        if (stops.empty()) {
            return ramp;
        }
        ramp.start = std::min(0.0f, stops.front().position);
        ramp.end = std::max(1.0f, stops.back().position);
        for (int i = 0; i < RAMP_WIDTH; ++i) {
            const float t = ramp.start + (ramp.end - ramp.start) * static_cast<float>(i) / (RAMP_WIDTH - 1);
            float rgba[4];
            color_at(stops, t, rgba);
            for (int c = 0; c < 4; ++c) ramp.pixels[i * 4 + c] = byte_of(rgba[c]);
        }
        return ramp;
    }

    int selftest() {
        bool ok = true;
        auto expect = [&](const char* what, int got, int want) {
            if (std::abs(got - want) > 1) {
                std::printf("  %s: %d, expected %d\n", what, got, want);
                ok = false;
            }
        };

        // Two stops, red to blue, opaque: the ends are the stops, the middle is half of each
        // (a smoothstep is one half at its midpoint).
        const Ramp two = bake({ { 0.0f, { 1, 0, 0, 1 } }, { 1.0f, { 0, 0, 1, 1 } } });
        expect("first texel red", two.pixels[0], 255);
        expect("last texel blue", two.pixels[(RAMP_WIDTH - 1) * 4 + 2], 255);
        const int mid = (RAMP_WIDTH / 2) * 4;
        expect("middle red is about half", two.pixels[mid + 0], 127);
        expect("middle blue is about half", two.pixels[mid + 2], 128);

        // The docked panel's scrim: transparent slate to 80% slate-deep, premultiplied, so the
        // color channels rise with the alpha and never exceed it.
        const float a = 0.8f;
        const Ramp scrim = bake({ { 0.0f, { 0, 0, 0, 0 } }, { 1.0f, { 0.047f * a, 0.071f * a, 0.082f * a, a } } });
        expect("scrim starts clear", scrim.pixels[3], 0);
        expect("scrim ends at 80%", scrim.pixels[(RAMP_WIDTH - 1) * 4 + 3], 204);
        for (int i = 0; i < RAMP_WIDTH; ++i) {
            if (scrim.pixels[i * 4 + 2] > scrim.pixels[i * 4 + 3]) {
                std::printf("  scrim texel %d is not premultiplied\n", i);
                ok = false;
                break;
            }
        }

        // A hard stop (the cut, phase 88): two stops at one position change color within a texel,
        // the way the cut's gold line starts exactly at the slant.
        const Ramp hard = bake({ { 0.5f, { 0, 1, 0, 1 } }, { 0.5f, { 1, 0, 0, 1 } } });
        const int before = static_cast<int>(0.5f * (RAMP_WIDTH - 1)) - 1;
        expect("before a hard stop is the first color", hard.pixels[before * 4 + 1], 255);
        expect("after a hard stop is the second", hard.pixels[(before + 2) * 4 + 0], 255);

        // A stop past the end widens the ramp rather than being cut off.
        const Ramp wide = bake({ { -0.5f, { 1, 1, 1, 1 } }, { 1.5f, { 0, 0, 0, 1 } } });
        if (wide.start != -0.5f || wide.end != 1.5f) {
            std::printf("  the ramp did not widen to its stops: %f to %f\n", wide.start, wide.end);
            ok = false;
        }

        std::printf("%s\n", ok ? "GRADIENT_OK" : "GRADIENT_FAIL");
        return ok ? 0 : 1;
    }

}
