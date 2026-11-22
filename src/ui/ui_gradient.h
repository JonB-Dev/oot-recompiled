#pragma once

// GRADIENTS (the menus plan, phase 87): RmlUi's linear-gradient and radial-gradient decorators,
// for the scrim where a docked panel meets the picture and the gold lit edge.
//
// The renderer bakes each gradient's color stops into a small ramp texture once, when the
// decorator compiles, and a pixel shader (PSGradient in shaders/ui.hlsl) finds each pixel's place
// along the gradient from its own coordinates and samples the ramp. The colors between stops blend
// exactly as RmlUi's own reference renderer blends them: a smoothstep from one stop to the next,
// in premultiplied color. That math lives here, apart from the GPU, so it can be tested headless.

#include <cstdint>
#include <vector>

namespace oot::ui::gradient {

    struct Stop {
        float position;   // along the gradient, where 0 is its start and 1 its end
        float rgba[4];    // premultiplied, 0 to 1
    };

    // 1024 texels, so a hard stop (the cut's gold edge, phase 88) stays within a pixel across a
    // panel the height of a 1080 line window.
    constexpr int RAMP_WIDTH = 1024;

    struct Ramp {
        std::vector<uint8_t> pixels;   // RAMP_WIDTH by 1, RGBA8, premultiplied
        float start = 0.0f;            // the gradient position of the first texel
        float end = 1.0f;              // and of the last
    };

    // The ramp over [min(0, first stop), max(1, last stop)], so a stop outside the gradient's own
    // line still lands on a texel.
    Ramp bake(const std::vector<Stop>& stops);

    // The ramp's math against known answers. Prints GRADIENT_OK or GRADIENT_FAIL; the exit code.
    int selftest();

}
