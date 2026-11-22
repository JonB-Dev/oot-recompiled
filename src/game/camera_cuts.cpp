#include "game/camera_cuts.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    constexpr uint32_t RAM_BASE = 0x80000000u;
    constexpr uint32_t RAM_SIZE = 0x00800000u;

    /* WHAT COUNTS AS A CUT, and both of these were too tight, which is what made ordinary
       movement skip. A cut stops the renderer interpolating for that frame, so a false cut
       delivers three display frames of motion in one: the skip that was reported on running side
       to side.

       The eye test is RELATIVE now. The old one allowed the eye to be 300 units from where last
       frame's velocity predicted, which tests ACCELERATION rather than continuity: any change of
       speed or direction puts the eye off its own prediction, and changing direction is exactly
       what running side to side is. A cut is the eye moving much further this update than it has
       recently been moving, with a floor so a camera at rest jumping a short way still counts. */
    constexpr float EYE_STEP_FLOOR = 900.0f;    // never a cut below this, whatever the prediction said
    constexpr float EYE_STEP_FACTOR = 6.0f;     // or this many times how far it has lately been moving
    /* And the view may turn this much in one game update: cos 55 degrees. It was cos 25, which is
       a cap of 500 degrees a second at twenty updates, and this camera swings past that whenever
       you reverse while running. A real cut is not a fast turn, it is a different place. */
    constexpr float COS_TURN_LIMIT = 0.5736f;

    struct Vec3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    Vec3 g_prev_eye;
    Vec3 g_prev_at;
    Vec3 g_eye_velocity;
    // How far the eye has lately been moving each update, smoothed, so the step test can ask
    // whether THIS step is out of character rather than compare against a fixed number.
    float g_eye_step_avg = 0.0f;
    int32_t g_prev_entrance = -1;
    bool g_have_prev = false;
    bool g_on = false;

    uint32_t g_calls = 0;
    uint32_t g_smooth = 0;
    uint32_t g_cuts = 0;

    bool plausible(uint32_t address) {
        return address >= RAM_BASE && address + 12 <= RAM_BASE + RAM_SIZE && (address & 3u) == 0;
    }

    // Words are stored natively at rdram + offset, so a float is its word's bits.
    float read_float(const uint8_t* rdram, uint32_t address) {
        uint32_t bits;
        std::memcpy(&bits, rdram + (address - RAM_BASE), sizeof(bits));
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    Vec3 read_vec3(const uint8_t* rdram, uint32_t address) {
        return Vec3{ read_float(rdram, address), read_float(rdram, address + 4), read_float(rdram, address + 8) };
    }

    Vec3 sub(const Vec3& a, const Vec3& b) {
        return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
    }

    float dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    float length(const Vec3& a) {
        return std::sqrt(dot(a, a));
    }

} // namespace

namespace oot::camera_cuts {

    void enable(bool on) {
        g_on = on;
    }

    Counts counts() {
        Counts c;
        c.calls = g_calls;
        c.smooth = g_smooth;
        c.cuts = g_cuts;
        return c;
    }

    bool smooth(const uint8_t* rdram, uint32_t eye_address, uint32_t at_address, int32_t entrance,
                uint32_t patch_calls) {
        ++g_calls;
        if (rdram == nullptr || !plausible(eye_address) || !plausible(at_address)) {
            ++g_cuts;
            return false;
        }

        const Vec3 eye = read_vec3(rdram, eye_address);
        const Vec3 at = read_vec3(rdram, at_address);

        bool is_smooth = false;
        float eye_error = 0.0f;
        float cos_turn = 0.0f;

        // Remembered before the state below moves on, so the diagnostic can say WHICH kind of cut
        // this was rather than recomputing it from state that has already been updated.
        const bool new_place = (!g_have_prev || entrance != g_prev_entrance);

        if (new_place) {
            // A new place, or nothing to compare with: a cut, and the tracking starts over.
            g_prev_entrance = entrance;
            g_eye_velocity = Vec3{};
            g_eye_step_avg = 0.0f;
        } else {
            // How far the eye actually moved this update, against how far it has lately been
            // moving. Reported as eye_error so the diagnostic line keeps its meaning.
            const float step = length(sub(eye, g_prev_eye));
            eye_error = step;
            const float step_limit = (g_eye_step_avg * EYE_STEP_FACTOR > EYE_STEP_FLOOR)
                                   ? g_eye_step_avg * EYE_STEP_FACTOR : EYE_STEP_FLOOR;

            // How far the view direction turned.
            const Vec3 dir = sub(at, eye);
            const Vec3 prev_dir = sub(g_prev_at, g_prev_eye);
            const float lengths = length(dir) * length(prev_dir);
            cos_turn = (lengths > 1e-6f) ? dot(dir, prev_dir) / lengths : 0.0f;

            is_smooth = (step <= step_limit) && (cos_turn >= COS_TURN_LIMIT);

            // This frame's velocity, for anything that still wants it, and the smoothed step. A
            // cut forgets both, so the frame after a cut is not judged against the jump.
            g_eye_velocity = is_smooth ? sub(eye, g_prev_eye) : Vec3{};
            g_eye_step_avg = is_smooth ? (g_eye_step_avg * 0.75f + step * 0.25f) : 0.0f;
        }

        g_prev_eye = eye;
        g_prev_at = at;
        g_have_prev = true;

        if (is_smooth) ++g_smooth; else ++g_cuts;

        // EVERY CUT SAYS SO, added 2026-09-20 for the stutter reports. A cut stops the renderer
        // drawing anything between game frames, so a run of them IS a drop from the display's
        // rate to the game's twenty, which is what "the Z targeting camera adjusts at 20fps" and
        // "when I roll and am panning the camera I get a frame drop of 1-2 frames" both describe.
        // The periodic line below cannot see a run of three: it samples one frame in sixty.
        // Rate limited so a genuinely cut-heavy stretch (a cutscene) stays readable.
        if (g_on && !is_smooth) {
            if (g_cuts <= 400 || (g_cuts % 20) == 0) {
                std::fprintf(stderr, "[cam] CUT at frame %u: eye step %.1f (avg %.1f), view turn cos %.4f (limit %.4f)%s\n",
                             g_calls, eye_error, g_eye_step_avg, cos_turn, COS_TURN_LIMIT,
                             new_place ? " [new place, nothing to compare with]" : "");
            }
        }

        if (g_on && (g_calls % 60) == 0) {
            // DIAGNOSTIC for the phase 41 flare crash: the play state's sun position, derived
            // from the view's address (the play view sits at play + 0xB8 and its eye at 0x28 of
            // the view; the environment context at play + 0x10A24 and its sunPos at 0x04). Only
            // meaningful when the view IS the play state's, which the pan and the cutscene
            // calls are; the pause menu's own view gives a nonsense reading.
            const uint32_t play = eye_address - 0xB8u - 0x28u;
            const uint32_t sun = play + 0x10A24u + 0x04u;
            float sx = 0.0f, sy = 0.0f, sz = 0.0f;
            if (plausible(sun)) {
                sx = read_float(rdram, sun);
                sy = read_float(rdram, sun + 4);
                sz = read_float(rdram, sun + 8);
            }
            std::fprintf(stderr,
                         "[cam] frame %u (patch counter %u): eye error %.1f, view turn cos %.4f -> %s (%u smooth, %u cut so far); eye %.1f %.1f %.1f sun offset %.1f %.1f %.1f\n",
                         g_calls, patch_calls, eye_error, cos_turn, is_smooth ? "smooth" : "CUT", g_smooth, g_cuts,
                         eye.x, eye.y, eye.z, sx, sy, sz);
        }
        return is_smooth;
    }

} // namespace oot::camera_cuts
