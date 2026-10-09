// Photo mode. See photo.h.
#include "main/photo.h"

#include <atomic>
#include <cstdio>

#include "game/game_state.h"
#include "game/moments.h"
#include "main/input.h"
#include "main/input_bindings.h"
#include "main/shots.h"
#include "ui/ui_settings.h"
#include "ui/ui_shell.h"

namespace oot::photo {

    namespace {
        std::atomic<bool> g_picture_room{ false };
        std::atomic<bool> g_picture_said{ false };
        bool g_put_back_down = false;
        std::atomic<bool> g_hud_hidden{ false };

        float amount(oot::input::GameInput input) {
            return oot::input::devices::input_amount(input);
        }

        int32_t fraction(float value) {
            return static_cast<int32_t>(value * 4096.0f);
        }

        bool in_play() {
            const oot::game_state::Snapshot now = oot::game_state::read();
            return now.valid && now.game_mode == 0;
        }
    }

    void toggle() {
        if (oot::shots::photo()) {
            oot::shots::set_photo(false);
            g_hud_hidden.store(false, std::memory_order_relaxed);
            oot::moments::notice("Photo mode ended");
            return;
        }
        if (!in_play()) {
            std::fprintf(stderr, "[photo] ignored: the game is not in play\n");
            return;
        }
        g_picture_said.store(false, std::memory_order_relaxed);
        g_picture_room.store(false, std::memory_order_relaxed);
        oot::shots::set_photo(true);
        oot::moments::notice("Photo mode: the game is frozen");
    }

    void tick() {
        if (!oot::shots::photo()) {
            return;
        }
        if (!oot::ui::photo_mode_enabled() || !in_play()) {
            oot::shots::set_photo(false);
            g_hud_hidden.store(false, std::memory_order_relaxed);
            return;
        }
        if (g_picture_room.load(std::memory_order_relaxed) && !g_picture_said.exchange(true, std::memory_order_relaxed)) {
            oot::moments::notice("Photo mode: this room is a painted picture, so the camera stays where it is");
        }
    }

    int32_t camera_input(int which) {
        using GI = oot::input::GameInput;
        if (oot::ui::shell::capturing_input()) {
            g_put_back_down = false;
            return 0;
        }
        switch (which) {
            case MoveAcross: return fraction(amount(GI::PhotoRight) - amount(GI::PhotoLeft));
            case MoveAhead:  return fraction(amount(GI::PhotoForward) - amount(GI::PhotoBack));
            case TurnAcross: return fraction(amount(GI::PhotoTurnRight) - amount(GI::PhotoTurnLeft));
            case TurnUp:     return fraction(amount(GI::PhotoTurnUp) - amount(GI::PhotoTurnDown));
            case Rise:       return fraction(amount(GI::PhotoRaise) - amount(GI::PhotoLower));
            case Closer:     return fraction(amount(GI::PhotoCloser) - amount(GI::PhotoFurther));
            case PutBack: {
                const bool down = amount(GI::PhotoReset) >= 0.5f;
                const bool pressed = down && !g_put_back_down;
                g_put_back_down = down;
                return pressed ? 4096 : 0;
            }
            default:         return 0;
        }
    }

    void toggle_hud() {
        if (!oot::shots::photo()) {
            return;
        }
        const bool hidden = !g_hud_hidden.load(std::memory_order_relaxed);
        g_hud_hidden.store(hidden, std::memory_order_relaxed);
        oot::moments::notice(hidden ? "Photo mode: HUD hidden" : "Photo mode: HUD shown");
    }

    bool hud_hidden() {
        return oot::shots::photo() && g_hud_hidden.load(std::memory_order_relaxed);
    }

    void note_picture_room(bool on) {
        g_picture_room.store(on, std::memory_order_relaxed);
    }

    bool active() {
        return oot::shots::photo();
    }

} // namespace oot::photo
