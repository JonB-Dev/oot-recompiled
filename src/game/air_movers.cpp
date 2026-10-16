#include "game/air_movers.h"

#include <atomic>
#include <cstring>

namespace {

    // The console's address space as the runtime lays it out (see light_list.cpp): a game address
    // minus the base indexes the RDRAM block, whole words in place.
    //
    // UP TO THE PATCH RAM'S CEILING, NOT THE CONSOLE'S 8MB (2026-10-08). The table is a patch's
    // own static, so it lives in the patch RAM above the console's memory (0x80813B94 in the build
    // that found this, patches/patches.ld: 0x80801000 to 0x81000000), and a check that stopped at
    // 8MB refused it every frame: the dust was never pushed by anything. The runtime's block
    // reaches the patch RAM's ceiling, since the patches themselves read and write there.
    constexpr uint32_t RAM_BASE = 0x80000000u;
    constexpr uint32_t RAM_SIZE = 0x01000000u;

    // The table patches/air_movers.c writes: a count, then eight words a body (its foot, its
    // radius and height, its motion this update).
    constexpr uint32_t MAX_MOVERS = 8;
    constexpr uint32_t FIELDS = 8;
    constexpr uint32_t TABLE_BYTES = (1 + MAX_MOVERS * FIELDS) * 4;

    std::atomic<uint32_t> g_table{ 0 };

    bool in_ram(uint32_t address, uint32_t size) {
        return (address >= RAM_BASE) && ((address & 3u) == 0) && (address - RAM_BASE + size <= RAM_SIZE);
    }

    int32_t read_s32(const uint8_t* rdram, uint32_t address) {
        int32_t value;
        std::memcpy(&value, rdram + (address - RAM_BASE), sizeof(value));
        return value;
    }

} // namespace

namespace oot::air_movers {

    void bind(uint32_t address) {
        g_table.store(address, std::memory_order_release);
    }

    void read(const uint8_t* rdram, std::vector<Mover>& out) {
        out.clear();
        const uint32_t table = g_table.load(std::memory_order_acquire);
        if ((rdram == nullptr) || (table == 0) || !in_ram(table, TABLE_BYTES)) {
            return;
        }

        const int32_t count = read_s32(rdram, table);
        if ((count <= 0) || (count > int32_t(MAX_MOVERS))) {
            return;
        }

        for (int32_t i = 0; i < count; i++) {
            const uint32_t entry = table + 4u + uint32_t(i) * FIELDS * 4u;
            Mover mover;
            mover.x = float(read_s32(rdram, entry + 0)) / 4.0f;
            mover.y = float(read_s32(rdram, entry + 4)) / 4.0f;
            mover.z = float(read_s32(rdram, entry + 8)) / 4.0f;
            mover.radius = float(read_s32(rdram, entry + 12)) / 4.0f;
            mover.height = float(read_s32(rdram, entry + 16)) / 4.0f;
            mover.dx = float(read_s32(rdram, entry + 20)) / 64.0f;
            mover.dy = float(read_s32(rdram, entry + 24)) / 64.0f;
            mover.dz = float(read_s32(rdram, entry + 28)) / 64.0f;
            // A body the patch would never write (it caps both) is a table read mid-write.
            if ((mover.radius <= 0.0f) || (mover.radius > 60.0f) || (mover.height <= 0.0f) || (mover.height > 150.0f)) {
                continue;
            }
            out.push_back(mover);
        }
    }

} // namespace oot::air_movers
