#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace oot::ui::input {
// SDL mouse positions are window points; the Metal/RmlUi canvas uses drawable pixels.
inline int drawable_coordinate(int position, int window_extent, int drawable_extent) {
    if (window_extent <= 0 || drawable_extent <= 0) return position;
    const auto pixels = static_cast<int64_t>(position) * drawable_extent / window_extent;
    return static_cast<int>(std::clamp<int64_t>(pixels, std::numeric_limits<int>::min(),
                                               std::numeric_limits<int>::max()));
}
}
