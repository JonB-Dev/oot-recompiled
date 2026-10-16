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

    int32_t g_band = -1;
    int32_t g_peak = 0;
    int32_t g_song = -1;
    std::chrono::steady_clock::time_point g_last_summary{};

    void line(const char* why, int32_t total, int32_t largest_free) {
        std::fprintf(stderr, "[actors] %s: %d of the game's %d (peak %d), largest free block %d bytes%s\n", why, total, ACTOR_LIMIT, g_peak,
                     largest_free, (total > ACTOR_LIMIT) ? ", SPAWNS REFUSED" : "");
    }

} // namespace

namespace oot::actor_room {

    void note(int32_t total, int32_t largest_free, int32_t song) {
        if (total > g_peak) {
            g_peak = total;
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
