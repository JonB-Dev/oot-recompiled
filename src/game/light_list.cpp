#include "game/light_list.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace {

    // The console's address space as the runtime lays it out: a game address minus the base
    // indexes the RDRAM block, words in place and the bytes and halves of a word swapped
    // (recomp.h, MEM_B and MEM_H), which is what these three readers undo. The bound is the
    // 8MB the game itself can address; the runtime's block is larger, but nothing of the game's
    // lives above.
    constexpr uint32_t RAM_BASE = 0x80000000u;
    constexpr uint32_t RAM_SIZE = 0x00800000u;

    // The game's node pool holds 32 lights (LIGHTS_BUFFER_SIZE in z_lights.c); a walk longer
    // than that is a cycle or a corrupt link.
    constexpr uint32_t MAX_NODES = 32;

    // Offsets from include/light.h at the pinned decompilation: LightNode { info, prev, next },
    // LightInfo { u8 type; pad; LightParams }, LightPoint { s16 x, y, z; u8 color[3]; u8 drawGlow;
    // s16 radius }.
    constexpr uint32_t NODE_INFO = 0;
    constexpr uint32_t NODE_NEXT = 8;
    constexpr uint32_t NODE_SIZE = 12;
    constexpr uint32_t INFO_TYPE = 0;
    constexpr uint32_t INFO_X = 2;
    constexpr uint32_t INFO_Y = 4;
    constexpr uint32_t INFO_Z = 6;
    constexpr uint32_t INFO_COLOR = 8;
    constexpr uint32_t INFO_GLOW = 11;
    constexpr uint32_t INFO_RADIUS = 12;
    constexpr uint32_t INFO_SIZE = 14;
    constexpr uint32_t TYPE_POINT_NOGLOW = 0;
    constexpr uint32_t TYPE_POINT_GLOW = 2;

    std::atomic<uint32_t> g_context{ 0 };
    std::atomic<uint32_t> g_environment{ 0 };
    // play->envCtx.lightMode: LIGHT_MODE_TIME (0) where the place follows the time of day and has
    // a real sun, LIGHT_MODE_SETTINGS (1) where it holds a fixed indoor setting.
    std::atomic<uint32_t> g_light_mode{ 0 };
    // What the place's own surfaces give as light (patches/light_list.c): bit 0, its windows.
    std::atomic<uint32_t> g_scene_lights{ 0 };
    constexpr uint32_t SCENE_LIGHTS_WINDOWS = 1u << 0;

    // Offsets in the environment's live light settings (include/environment.h,
    // CurrentEnvLightSettings): the ambient, then the two directional lights as a direction of
    // three signed bytes and a color of three bytes each.
    constexpr uint32_t ENV_AMBIENT = 0;
    constexpr uint32_t ENV_DIR1 = 3;
    constexpr uint32_t ENV_COLOR1 = 6;
    constexpr uint32_t ENV_DIR2 = 9;
    constexpr uint32_t ENV_COLOR2 = 12;
    constexpr uint32_t ENV_SIZE = 15;

    bool in_ram(uint32_t address, uint32_t size) {
        return (address >= RAM_BASE) && (address - RAM_BASE + size <= RAM_SIZE);
    }

    uint32_t read_u32(const uint8_t* rdram, uint32_t address) {
        uint32_t value;
        std::memcpy(&value, rdram + (address - RAM_BASE), sizeof(value));
        return value;
    }

    int16_t read_s16(const uint8_t* rdram, uint32_t address) {
        int16_t value;
        std::memcpy(&value, rdram + ((address ^ 2u) - RAM_BASE), sizeof(value));
        return value;
    }

    uint8_t read_u8(const uint8_t* rdram, uint32_t address) {
        return rdram[(address ^ 3u) - RAM_BASE];
    }

} // namespace

namespace oot::light_list {

    void bind_context(uint32_t address) {
        g_context.store(address, std::memory_order_release);
    }

    void bind_environment(uint32_t address) {
        g_environment.store(address, std::memory_order_release);
    }

    void bind_light_mode(uint32_t address) {
        g_light_mode.store(address, std::memory_order_release);
    }

    void bind_scene_lights(uint32_t flags) {
        g_scene_lights.store(flags, std::memory_order_release);
    }

    bool windows_give_light() {
        return (g_scene_lights.load(std::memory_order_acquire) & SCENE_LIGHTS_WINDOWS) != 0;
    }

    bool read_sun_follows_time(const uint8_t* rdram, bool& out) {
        const uint32_t at = g_light_mode.load(std::memory_order_acquire);
        if ((rdram == nullptr) || (at == 0) || !in_ram(at, 1)) {
            return false;
        }
        out = (read_u8(rdram, at) == 0);   // LIGHT_MODE_TIME
        return true;
    }

    bool read_environment(const uint8_t* rdram, Environment& out) {
        const uint32_t base = g_environment.load(std::memory_order_acquire);
        if ((rdram == nullptr) || (base == 0) || !in_ram(base, ENV_SIZE)) {
            return false;
        }

        auto direction = [&](uint32_t at, float* dir) {
            float v[3];
            float length2 = 0.0f;
            for (uint32_t i = 0; i < 3; i++) {
                v[i] = float(int8_t(read_u8(rdram, at + i)));
                length2 += v[i] * v[i];
            }
            const float scale = (length2 > 0.0f) ? (1.0f / std::sqrt(length2)) : 0.0f;
            for (uint32_t i = 0; i < 3; i++) {
                dir[i] = v[i] * scale;
            }
        };
        auto color = [&](uint32_t at, float* col) {
            for (uint32_t i = 0; i < 3; i++) {
                col[i] = float(read_u8(rdram, at + i)) / 255.0f;
            }
        };

        color(base + ENV_AMBIENT, out.ambient);
        direction(base + ENV_DIR1, out.dir1);
        color(base + ENV_COLOR1, out.color1);
        direction(base + ENV_DIR2, out.dir2);
        color(base + ENV_COLOR2, out.color2);
        return true;
    }

    void read(const uint8_t* rdram, std::vector<Light>& out) {
        out.clear();
        const uint32_t context = g_context.load(std::memory_order_acquire);
        if ((rdram == nullptr) || (context == 0) || ((context & 3u) != 0) || !in_ram(context, 4)) {
            return;
        }

        uint32_t node = read_u32(rdram, context);
        for (uint32_t walked = 0; (node != 0) && (walked < MAX_NODES); walked++) {
            if (((node & 3u) != 0) || !in_ram(node, NODE_SIZE)) {
                return;
            }

            const uint32_t info = read_u32(rdram, node + NODE_INFO);
            if (((info & 1u) != 0) || !in_ram(info, INFO_SIZE)) {
                return;
            }

            const uint32_t type = read_u8(rdram, info + INFO_TYPE);
            if ((type == TYPE_POINT_NOGLOW) || (type == TYPE_POINT_GLOW)) {
                const int16_t radius = read_s16(rdram, info + INFO_RADIUS);
                if (radius > 0) {
                    Light light;
                    light.x = float(read_s16(rdram, info + INFO_X));
                    light.y = float(read_s16(rdram, info + INFO_Y));
                    light.z = float(read_s16(rdram, info + INFO_Z));
                    light.r = float(read_u8(rdram, info + INFO_COLOR + 0)) / 255.0f;
                    light.g = float(read_u8(rdram, info + INFO_COLOR + 1)) / 255.0f;
                    light.b = float(read_u8(rdram, info + INFO_COLOR + 2)) / 255.0f;
                    light.radius = float(radius);
                    light.glow = (type == TYPE_POINT_GLOW) && (read_u8(rdram, info + INFO_GLOW) != 0);
                    out.push_back(light);
                }
            }

            node = read_u32(rdram, node + NODE_NEXT);
        }
    }

} // namespace oot::light_list
