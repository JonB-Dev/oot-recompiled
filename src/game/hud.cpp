// The HUD's fading. See hud.h.
#include "game/hud.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include "game/moments.h"
#include "game/render.h"
#include "ui/ui_settings.h"

namespace oot::hud {

    namespace {

        using Clock = std::chrono::steady_clock;

        // The rows' steps as values.
        constexpr float OPACITY[] = { 0.20f, 0.35f, 0.50f, 0.65f, 0.80f };
        constexpr float COLOR[] = { 1.0f, 0.75f, 0.5f, 0.25f, 0.0f };
        constexpr float DELAY_SECONDS[] = { 1.0f, 2.0f, 3.0f, 5.0f, 8.0f, 12.0f };
        // Seconds for a whole fade, out or in; 0 is at once.
        constexpr float FADE_SECONDS[] = { 0.0f, 0.15f, 0.4f, 1.0f };

        enum Mode : int { Never = 0, WhenIdle = 1, Always = 2, ByButton = 3 };

        std::mutex g_mutex;
        Clock::time_point g_last_active = Clock::now();
        Clock::time_point g_last_note = Clock::now();
        bool g_noted = false;
        // The button's own state: in Always, the HUD shown; in By button, the HUD faded.
        bool g_button_on = false;
        std::atomic<bool> g_button_pressed{ false };
        float g_opacity[PartCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        float g_color[PartCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

        int step(int value, int count) {
            return std::clamp(value, 0, count - 1);
        }

        bool part_fades(const oot::ui::Settings& s, int part) {
            switch (part) {
                case Hearts:  return s.hud_fade_hearts == 0;
                case Magic:   return s.hud_fade_magic == 0;
                case Rupees:  return s.hud_fade_rupees == 0;
                case Buttons: return s.hud_fade_buttons == 0;
                case Map:     return s.hud_fade_map == 0;
                default:      return false;
            }
        }

        // The activity that brings the HUD back: each box ticked under Comes back for, its own.
        uint32_t wakers(const oot::ui::Settings& s) {
            uint32_t bits = 0;
            bits |= (s.hud_wake_target != 0) ? Targeting : 0u;
            bits |= (s.hud_wake_c != 0) ? CButtons : 0u;
            bits |= (s.hud_wake_talk != 0) ? Talking : 0u;
            bits |= (s.hud_wake_health != 0) ? HealthMagic : 0u;
            bits |= (s.hud_wake_items != 0) ? RupeesItems : 0u;
            bits |= (s.hud_wake_pause != 0) ? Paused : 0u;
            bits |= (s.hud_wake_any != 0) ? AnyButton : 0u;
            return bits;
        }

        float approach(float current, float target, float step_size) {
            if (current < target) {
                return std::min(target, current + step_size);
            }
            return std::max(target, current - step_size);
        }

    } // namespace

    void press_button() {
        g_button_pressed.store(true, std::memory_order_relaxed);
    }

    void note(uint32_t activity) {
        const oot::ui::Settings& s = oot::ui::settings();
        std::lock_guard<std::mutex> lock(g_mutex);
        const Clock::time_point now = Clock::now();
        const float dt = g_noted ? std::chrono::duration<float>(now - g_last_note).count() : 0.0f;
        g_last_note = now;
        g_noted = true;

        const int mode = step(s.hud_fade, 4);
        const bool pressed = g_button_pressed.exchange(false, std::memory_order_relaxed);
        if (pressed) {
            if (mode == WhenIdle) {
                g_last_active = now;   // a look at it, then the delay again
            }
            else if (mode == Always || mode == ByButton) {
                g_button_on = !g_button_on;
                const bool faded = (mode == Always) ? !g_button_on : g_button_on;
                oot::moments::notice(faded ? "HUD faded" : "HUD shown");
            }
        }
        if (mode != Always && mode != ByButton) {
            g_button_on = false;
        }

        if (activity & wakers(s)) {
            g_last_active = now;
        }

        bool faded = false;
        switch (mode) {
            case WhenIdle:
                faded = std::chrono::duration<float>(now - g_last_active).count() >= DELAY_SECONDS[step(s.hud_delay, 6)];
                break;
            case Always:
                faded = !g_button_on;
                break;
            case ByButton:
                faded = g_button_on;
                break;
            default:
                faded = false;
                break;
        }

        const bool hidden = step(s.hud_fade_to, 2) == 1;
        const float faded_opacity = hidden ? 0.0f : OPACITY[step(s.hud_opacity, 5)];
        const float faded_color = hidden ? 1.0f : COLOR[step(s.hud_color, 5)];
        const float seconds = FADE_SECONDS[step(s.hud_speed, 4)];
        const float move = (seconds <= 0.0f) ? 1.0f : (dt / seconds);

        for (int p = 0; p < PartCount; ++p) {
            bool part_faded = faded && part_fades(s, p);
            // LOW HEALTH KEEPS THE HEARTS UP, and only the hearts, while its box is ticked, unless
            // the button alone decides (By button).
            if (p == Hearts && (activity & HealthLow) && s.hud_wake_low != 0 && mode != ByButton) {
                part_faded = false;
            }
            const float target_opacity = part_faded ? faded_opacity : 1.0f;
            const float target_color = part_faded ? faded_color : 1.0f;
            g_opacity[p] = approach(g_opacity[p], target_opacity, move);
            g_color[p] = approach(g_color[p], target_color, move);
        }
    }

    uint32_t layout(int p) {
        constexpr uint32_t SIZE_PERCENT[] = { 50, 60, 70, 80, 90, 100, 115, 130, 150, 175, 200 };
        const oot::ui::Settings& s = oot::ui::settings();
        int size = s.hud_size;
        if (s.hud_sizes != 0) {
            switch (p) {
                case Hearts:  size = s.hud_size_hearts; break;
                case Magic:   size = s.hud_size_magic; break;
                case Rupees:  size = s.hud_size_rupees; break;
                case Buttons: size = s.hud_size_buttons; break;
                case Map:     size = s.hud_size_map; break;
                default:      break;
            }
        }
        return SIZE_PERCENT[step(size, 11)];
    }

    uint32_t place() {
        constexpr uint32_t PERCENT[] = { 0, 1, 2, 3, 4, 5, 6, 8, 10, 15, 20, 25 };
        const oot::ui::Settings& s = oot::ui::settings();
        if (s.hud_ratio != 3) {
            return 0;
        }
        return (1u << 31) | PERCENT[step(s.hud_left, 12)] | (PERCENT[step(s.hud_right, 12)] << 8) |
               (PERCENT[step(s.hud_top, 12)] << 16) | (PERCENT[step(s.hud_bottom, 12)] << 24);
    }

    uint32_t frame_q16() {
        return static_cast<uint32_t>(std::lround(oot::renderer::frame_aspect() / (4.0 / 3.0) * 65536.0));
    }

    uint32_t part(int p) {
        if (p < 0 || p >= PartCount) {
            return 256u | (256u << 16);
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        const uint32_t opacity = static_cast<uint32_t>(std::lround(std::clamp(g_opacity[p], 0.0f, 1.0f) * 256.0f));
        const uint32_t color = static_cast<uint32_t>(std::lround(std::clamp(g_color[p], 0.0f, 1.0f) * 256.0f));
        return opacity | (color << 16);
    }

} // namespace oot::hud
