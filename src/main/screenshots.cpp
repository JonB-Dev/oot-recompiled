// Screenshots, and the before and after pair. See screenshots.h.
#include "main/screenshots.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#include "game/moments.h"
#include "game/render.h"
#include "main/places.h"
#include "main/recorder.h"
#include "main/shots.h"
#include "main/sweep.h"
#include "ui/ui_lighting.h"
#include "ui/ui_settings.h"

namespace oot::screenshots {

    namespace {

        enum class Phase {
            Idle,
            Shot, ShotWait,                 // the one screenshot
            Hold, Yours, YoursWait,         // the comparison: held, then the person's own picture
            Settle, Original, OriginalWait, // the original look, given time, then its picture
            Restore,                        // the person's look back, then the game released
        };

        // Presented frames between the hold and the first shot, so the frame shown is the held one.
        constexpr int HOLD_FRAMES = 4;
        // Presented frames for the original look to replace the person's: a resolution or an
        // antialiasing change rebuilds the renderer's targets and the pack is let go, and the
        // frame after all of that must be the settled one.
        constexpr int SETTLE_FRAMES = 60;
        // Presented frames with the person's look back before the game moves again, so the first
        // moving frame is theirs and not the original look's.
        constexpr int RESTORE_FRAMES = 6;
        // The frame is taken at full size: the recorder clamps a width past the frame's to the frame.
        constexpr int FULL_WIDTH = 16384;

        std::atomic<bool> g_requested{ false };
        Phase g_phase = Phase::Idle;
        int g_frames = 0;
        std::filesystem::path g_folder;
        std::string g_stem;
        std::filesystem::path g_pending;

        // The day's folder under the captures, and a stem no earlier file of this kind uses.
        bool make_names(bool comparison) {
            const std::time_t t = std::time(nullptr);
            std::tm now{};
            localtime_s(&now, &t);
            char day[16];
            char time_of_day[16];
            std::strftime(day, sizeof(day), "%Y-%m-%d", &now);
            std::strftime(time_of_day, sizeof(time_of_day), "%H-%M-%S", &now);
            g_folder = oot::places::captures() / day;
            std::error_code ec;
            std::filesystem::create_directories(g_folder, ec);
            if (ec) {
                std::fprintf(stderr, "[screenshot] refused: the folder %s could not be made\n", g_folder.string().c_str());
                return false;
            }

            const char* first = comparison ? "-compare-yours.png" : "-screenshot.png";
            g_stem = std::string(time_of_day) + "-oot";
            for (int n = 2; std::filesystem::exists(g_folder / (g_stem + first), ec); ++n) {
                g_stem = std::string(time_of_day) + "-" + std::to_string(n) + "-oot";
            }
            return true;
        }

        // Ask for the next presented frame; false while another snapshot is still waiting.
        bool take(const char* suffix) {
            g_pending = g_folder / (g_stem + suffix);
            // The game alone or the whole window, as the Screenshots show row says (2026-10-09).
            return oot::recorder::snapshot(g_pending, FULL_WIDTH, oot::ui::screenshots_game_alone());
        }

        // The snapshot writes a raw copy beside the PNG for the interface's thumbnails (recorder.cpp);
        // a full frame's is many megabytes nobody reads. Gone, as the sweep does.
        void taken() {
            std::error_code ec;
            std::filesystem::path raw = g_pending;
            raw.replace_extension(".rgba");
            std::filesystem::remove(raw, ec);
            std::fprintf(stderr, "[screenshot] %s\n", g_pending.string().c_str());
        }

        // The original look to the renderer, or the person's own back. The rows and the files are
        // never written: the display rows through the override, the lighting as the sweep puts a
        // value (the traced lighting row off over the person's own values, which takes every
        // traced pass, the dust, the shafts and the glow with it).
        void original_look(bool on) {
            oot::ui::set_original_look(on);
            if (on) {
                oot::ui::lighting::Lighting off = oot::ui::lighting::current();
                oot::ui::lighting::set_row(off, oot::ui::lighting::row_index("raytracing"), 0);
                oot::renderer::apply_lighting(oot::ui::lighting::values_of(off));
            }
            else {
                oot::renderer::apply_lighting(oot::ui::lighting::values());
            }
        }

    } // namespace

    void request() {
        g_requested.store(true, std::memory_order_release);
    }

    bool busy() {
        return g_phase != Phase::Idle;
    }

    void tick() {
        if (g_phase == Phase::Idle) {
            if (!g_requested.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            if (!oot::ui::screenshots_enabled()) {
                std::fprintf(stderr, "[screenshot] ignored: the Screenshots row is Off\n");
                return;
            }
            if (oot::sweep::running() || oot::recorder::snapshot_pending()) {
                std::fprintf(stderr, "[screenshot] ignored: a sweep or another capture is under way\n");
                return;
            }
            const bool comparison = oot::ui::comparison_shots_enabled();
            if (!make_names(comparison)) {
                return;
            }
            if (comparison) {
                oot::shots::set_hold(true);
                g_frames = 0;
                g_phase = Phase::Hold;
            }
            else {
                g_phase = Phase::Shot;
            }
            return;
        }

        // A press while one is under way is not queued.
        g_requested.store(false, std::memory_order_relaxed);

        switch (g_phase) {
            case Phase::Shot:
                if (take("-screenshot.png")) {
                    g_phase = Phase::ShotWait;
                }
                break;
            case Phase::ShotWait:
                if (!oot::recorder::snapshot_pending()) {
                    taken();
                    oot::moments::notice("Screenshot saved");
                    g_phase = Phase::Idle;
                }
                break;
            case Phase::Hold:
                if (++g_frames >= HOLD_FRAMES) {
                    g_phase = Phase::Yours;
                }
                break;
            case Phase::Yours:
                if (take("-compare-yours.png")) {
                    g_phase = Phase::YoursWait;
                }
                break;
            case Phase::YoursWait:
                if (!oot::recorder::snapshot_pending()) {
                    taken();
                    original_look(true);
                    g_frames = 0;
                    g_phase = Phase::Settle;
                }
                break;
            case Phase::Settle:
                if (++g_frames >= SETTLE_FRAMES) {
                    g_phase = Phase::Original;
                }
                break;
            case Phase::Original:
                if (take("-compare-original.png")) {
                    g_phase = Phase::OriginalWait;
                }
                break;
            case Phase::OriginalWait:
                if (!oot::recorder::snapshot_pending()) {
                    taken();
                    original_look(false);
                    g_frames = 0;
                    g_phase = Phase::Restore;
                }
                break;
            case Phase::Restore:
                if (++g_frames >= RESTORE_FRAMES) {
                    oot::shots::set_hold(false);
                    oot::moments::notice("Before and after saved");
                    g_phase = Phase::Idle;
                }
                break;
            case Phase::Idle:
                break;
        }
    }

} // namespace oot::screenshots
