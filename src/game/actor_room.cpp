#include "game/actor_room.h"

#include <chrono>
#include <cstdio>

namespace {

    // The game refuses a spawn once its scene holds more than this (z_actor.c, ACTOR_NUMBER_MAX).
    constexpr int32_t ACTOR_LIMIT = 200;
    // Below this a scene is ordinary and the count is not worth a line of its own.
    constexpr int32_t WORTH_NOTING = 150;
    constexpr int32_t BAND = 10;
    constexpr std::chrono::seconds SUMMARY_EVERY{ 10 };
    // Under this the largest free block may not hold an arrow, a hook or a song's effect with its
    // code (2026-10-21: the user's Hyrule Field went to 96 bytes with 163 actors, and the bow drew
    // no arrow and the longshot's hook never came).
    constexpr int32_t MEMORY_SHORT = 16 * 1024;

    int32_t g_band = -1;
    int32_t g_peak = 0;
    int32_t g_song = -1;
    int32_t g_heap = -1;
    int32_t g_hookshot = 0;
    bool g_short = false;
    std::chrono::steady_clock::time_point g_last_summary{};

    void line(const char* why, int32_t total, int32_t largest_free) {
        const char* refused = (total > ACTOR_LIMIT) ? ", SPAWNS REFUSED" : (largest_free < MEMORY_SHORT) ? ", MEMORY SHORT, SPAWNS MAY BE REFUSED" : "";
        std::fprintf(stderr, "[actors] %s: %d of the game's %d (peak %d), largest free block %d bytes%s\n", why, total, ACTOR_LIMIT, g_peak,
                     largest_free, refused);
    }

} // namespace

namespace oot::actor_room {

    void note(int32_t total, int32_t largest_free, int32_t song, int32_t heap_size, int32_t hookshot) {
        if (total > g_peak) {
            g_peak = total;
        }

        // Each scene makes its heap afresh; its size says whether the bigger heap is in effect. Free
        // plus allocated leaves out the blocks' own headers, so the sum drifts by bytes with every
        // spawn; only a jump that size cannot explain is a new heap (2026-10-21, the user's test log).
        constexpr int32_t NEW_HEAP = 64 * 1024;
        if ((g_heap < 0) || (heap_size > g_heap + NEW_HEAP) || (heap_size < g_heap - NEW_HEAP)) {
            std::fprintf(stderr, "[actors] the actor heap is %d KB, largest free block %d bytes\n", heap_size / 1024, largest_free);
            g_heap = heap_size;
        }

        // Memory short is worth a line the moment it starts, whatever the count; it ends only once
        // twice as much is free again, so a heap hovering at the line does not fill the log.
        if (largest_free < MEMORY_SHORT) {
            if (!g_short) {
                line("memory", total, largest_free);
            }
            g_short = true;
        } else if (largest_free >= 2 * MEMORY_SHORT) {
            g_short = false;
        }

        // The hookshot guard (patches/actor_heap.c): a hook put back, or one the game still has no room for.
        if (hookshot != g_hookshot) {
            const int32_t put_back = hookshot & 0xFFFF;
            if (put_back != (g_hookshot & 0xFFFF)) {
                std::fprintf(stderr, "[actors] the hookshot was in hand with no hook; the hook was made again (%d so far)\n", put_back);
            }
            if ((hookshot & 0x10000) && !(g_hookshot & 0x10000)) {
                std::fprintf(stderr, "[actors] the hookshot is in hand with no hook and no room to make one; largest free block %d bytes\n",
                             largest_free);
            }
            g_hookshot = hookshot;
        }

        // A band of ten crossed, either way, once the scene is past the ordinary.
        const int32_t band = (total >= WORTH_NOTING) ? (total / BAND) : -1;
        if (band != g_band) {
            if ((band >= 0) || (g_band >= 0)) {
                line("now", total, largest_free);
            }
            g_band = band;
        }

        const auto now = std::chrono::steady_clock::now();
        if ((total >= WORTH_NOTING) && ((now - g_last_summary) >= SUMMARY_EVERY)) {
            g_last_summary = now;
            line("still", total, largest_free);
        }

        if (song != g_song) {
            // Bits from patches/actor_note.c: 1 stranded, 2 the effect alive, 4 to 16 the time
            // change's state, 32 and up the pose's mode.
            std::fprintf(stderr, "[song] stranded %d, effect %s, time change %d, mode %d; %d actors, largest free block %d bytes\n",
                         song & 1, (song & 2) ? "alive" : "none", (song >> 2) & 7, (song >> 5) & 0x3F, total, largest_free);
            g_song = song;
        }
    }

} // namespace oot::actor_room
