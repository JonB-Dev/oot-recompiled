// The size a picture is taken at. See capture_size.h.
#include "main/capture_size.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace oot::capture {

    namespace {
        // The rows' heights after Match the window (0), in their order in the settings table.
        constexpr uint32_t HEIGHTS[] = { 0, 720, 1080, 1440, 2160 };
    }

    void size_at(int step, uint32_t window_width, uint32_t window_height, uint32_t max_width, uint32_t max_height,
                 uint32_t& width, uint32_t& height) {
        const int at = std::clamp(step, 0, static_cast<int>(std::size(HEIGHTS)) - 1);
        const double aspect = (window_height > 0) ? static_cast<double>(window_width) / static_cast<double>(window_height) : 16.0 / 9.0;
        double h = (at == 0) ? static_cast<double>(window_height) : static_cast<double>(HEIGHTS[at]);
        double w = h * aspect;
        if (w > max_width) {
            w = max_width;
            h = w / aspect;
        }
        if (h > max_height) {
            h = max_height;
            w = h * aspect;
        }
        width = std::max<uint32_t>(64u, static_cast<uint32_t>(std::lround(w)) & ~1u);
        height = std::max<uint32_t>(64u, static_cast<uint32_t>(std::lround(h)) & ~1u);
    }

} // namespace oot::capture
