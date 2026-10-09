// Deterministic captures for the harness. See shots.h.
#include "shots.h"

#include <atomic>
#include <cstdio>

#include "game/game_state.h"

namespace oot::shots {

    namespace {
        std::atomic<uint32_t> g_seed{ 0 };
        std::atomic<int32_t> g_freeze_entrance{ -1 };
        std::atomic<uint32_t> g_freeze_ticks{ 0 };
        std::atomic<bool> g_arrived{ false };
        std::atomic<uint32_t> g_ready_ticks{ 0 };
        std::atomic<uint32_t> g_stalled{ 0 };
        std::atomic<int32_t> g_clock{ -1 };
        std::atomic<bool> g_warp_seen{ false };
        std::atomic<bool> g_new_scene{ false };
        std::atomic<int32_t> g_frames_seen{ 0 };
        std::atomic<bool> g_frozen{ false };
        std::atomic<bool> g_held{ false };
        std::atomic<bool> g_photo{ false };
    }

    void set_seed(uint32_t seed) {
        g_seed.store(seed, std::memory_order_relaxed);
        if (seed != 0) {
            std::fprintf(stderr, "[shot] seed %u for every scene load\n", static_cast<unsigned>(seed));
        }
    }

    uint32_t seed() {
        return g_seed.load(std::memory_order_relaxed);
    }

    void set_freeze(int32_t entrance, uint32_t ticks) {
        g_freeze_entrance.store(entrance, std::memory_order_relaxed);
        g_freeze_ticks.store(ticks, std::memory_order_relaxed);
        g_arrived.store(false, std::memory_order_relaxed);
        g_frozen.store(false, std::memory_order_relaxed);
        std::fprintf(stderr, "[shot] will freeze %u updates after entrance 0x%04X\n",
                     static_cast<unsigned>(ticks), static_cast<unsigned>(entrance & 0xFFFF));
    }

    bool freeze_requested() {
        return g_freeze_entrance.load(std::memory_order_relaxed) >= 0;
    }

    bool frozen() {
        return g_frozen.load(std::memory_order_acquire) || g_held.load(std::memory_order_acquire) ||
               g_photo.load(std::memory_order_acquire);
    }

    void set_photo(bool on) {
        g_photo.store(on, std::memory_order_release);
        std::fprintf(stderr, "[shot] %s\n", on ? "held for photo mode" : "photo mode released");
    }

    bool photo() {
        return g_photo.load(std::memory_order_acquire);
    }

    void set_hold(bool on) {
        g_held.store(on, std::memory_order_release);
        std::fprintf(stderr, "[shot] %s\n", on ? "held for a comparison" : "released");
    }

    void set_clock(int32_t day_time) {
        g_clock.store(day_time, std::memory_order_relaxed);
        if (day_time >= 0) {
            std::fprintf(stderr, "[shot] clock 0x%04X on arrival\n", static_cast<unsigned>(day_time & 0xFFFF));
        }
    }

    Tick on_play_tick(int32_t entrance, int32_t mode, bool ready, int32_t gameplay_frames, int32_t state_frames) {
        if (g_frozen.load(std::memory_order_relaxed)) {
            return Tick::Run;
        }
        const int32_t target = g_freeze_entrance.load(std::memory_order_relaxed);
        if (target < 0 || mode != 0) {
            return Tick::Run;
        }

        // Armed only by the warp: the harness's warp has been taken, and a new scene has begun
        // since (the game state's own frame count started again). Without this a target that
        // is the file's own scene froze the scene the file opened in, and one that is the title
        // demo's froze the demo.
        if (!g_warp_seen.load(std::memory_order_relaxed)) {
            if (oot::game_state::warps_taken() == 0) {
                return Tick::Run;
            }
            g_warp_seen.store(true, std::memory_order_relaxed);
            g_frames_seen.store(state_frames, std::memory_order_relaxed);
            return Tick::Run;
        }
        if (!g_new_scene.load(std::memory_order_relaxed)) {
            const int32_t last = g_frames_seen.exchange(state_frames, std::memory_order_relaxed);
            if (state_frames >= last) {
                return Tick::Run;
            }
            g_new_scene.store(true, std::memory_order_relaxed);
        }
        if (entrance != target) {
            return Tick::Run;
        }

        if (!ready) {
            if (state_frames == 0) {
                // The first update of the scene runs whatever is in flight: the game draws
                // nothing before it, and holding it crashed the draw.
                return Tick::Run;
            }
            g_stalled.fetch_add(1, std::memory_order_relaxed);
            return Tick::Hold;
        }

        Tick tick = Tick::Run;
        if (!g_arrived.load(std::memory_order_relaxed)) {
            g_arrived.store(true, std::memory_order_relaxed);
            g_ready_ticks.store(0, std::memory_order_relaxed);
            const int32_t clock = g_clock.load(std::memory_order_relaxed);
            if (clock >= 0) {
                oot::game_state::set_pending_day_time(clock);
            }
            std::fprintf(stderr, "[shot] scene ready at entrance 0x%04X on update %d, %u updates held for loads\n",
                         static_cast<unsigned>(entrance & 0xFFFF), gameplay_frames,
                         static_cast<unsigned>(g_stalled.load(std::memory_order_relaxed)));
            tick = Tick::Arrive;
        }
        const uint32_t count = g_ready_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= g_freeze_ticks.load(std::memory_order_relaxed)) {
            g_frozen.store(true, std::memory_order_release);
            std::fprintf(stderr, "[shot] frozen at entrance 0x%04X after %u ready updates\n",
                         static_cast<unsigned>(entrance & 0xFFFF), static_cast<unsigned>(count));
        }
        return tick;
    }

} // namespace oot::shots
