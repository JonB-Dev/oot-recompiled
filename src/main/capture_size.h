#pragma once

// THE SIZE A PICTURE IS TAKEN AT (2026-10-14, the user: screenshots and photo mode "at any given
// resolution, even if the user screen is ... capped at like 1080p ... like it does for the video").
// The Video size and Screenshot size rows offer the same choices, Match the window, 720p, 1080p,
// 1440p and 4K, and both mean the same thing: that height at the window's own shape. The picture is
// drawn again at that size from what the Resolution row renders, so it can be larger than the
// window and the screen it is on (ui_render.cpp, the picture hook).
#include <cstdint>

namespace oot::capture {

    // The size for a row's choice (0 is Match the window) at the window's shape: even both ways,
    // at least 64, and shrunk with its shape kept until neither side passes the ceiling given.
    void size_at(int step, uint32_t window_width, uint32_t window_height, uint32_t max_width, uint32_t max_height,
                 uint32_t& width, uint32_t& height);

} // namespace oot::capture
