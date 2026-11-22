#include "game/warp_places.h"
#include "game/warp_table.ntsc-1.0.h"

#include <algorithm>
#include <vector>

namespace oot::warps {

    namespace {
        struct Row {
            int index;
            const Place* place;
            const Region* region;
        };

        // Every listed entrance once, by index, built on first use.
        const std::vector<Row>& rows() {
            static const std::vector<Row> sorted = [] {
                std::vector<Row> all;
                all.reserve(ENTRANCE_COUNT);
                for (int r = 0; r < REGION_COUNT; ++r) {
                    const Region& region = REGIONS[r];
                    for (int p = 0; p < region.count; ++p) {
                        const Place& place = region.places[p];
                        for (int e = 0; e < place.count; ++e) {
                            all.push_back({ place.entrances[e].index, &place, &region });
                        }
                    }
                }
                std::sort(all.begin(), all.end(), [](const Row& a, const Row& b) { return a.index < b.index; });
                return all;
            }();
            return sorted;
        }

        constexpr int LAYERS = 4;
    } // namespace

    Named place_of(int entrance) {
        const std::vector<Row>& all = rows();
        auto above = std::upper_bound(all.begin(), all.end(), entrance, [](int value, const Row& row) { return value < row.index; });
        if (above == all.begin()) {
            return {};
        }
        const Row& row = *(above - 1);
        if (entrance - row.index >= LAYERS) {
            return {};
        }
        return { row.place->name, row.region->name };
    }

} // namespace oot::warps
