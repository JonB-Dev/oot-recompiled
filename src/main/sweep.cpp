#include "main/sweep.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "game/render.h"
#include "main/places.h"
#include "main/recorder.h"
#include "ui/ui_lighting.h"

namespace oot::sweep {

    namespace {

        struct Step {
            int row;
            int value;
        };

        enum class Phase { Idle, Apply, Settle, Snapshot, Waiting };

        // Presented frames a step is given before its frame is taken: the history blends the new
        // sample in at the smoothing control's share, and at Heavy (a sixteenth a frame) forty
        // frames carry it past ninety percent of the way.
        constexpr int SETTLE_FRAMES = 40;
        // The frame is taken at full size: the recorder clamps a width past the frame's to the frame.
        constexpr int FULL_WIDTH = 16384;

        Phase g_phase = Phase::Idle;
        std::vector<Step> g_steps;
        size_t g_index = 0;
        int g_frames = 0;
        std::filesystem::path g_folder;
        std::string g_stem;
        std::ofstream g_log;
        std::filesystem::path g_last_frame;   // the step's PNG, whose raw sibling is removed once taken
        oot::ui::lighting::Lighting g_original;

        // A file name part from a row's option word: letters and digits, the rest a dash.
        std::string slug(const char* text) {
            std::string out;
            for (const char* p = text; *p != '\0'; ++p) {
                const char c = *p;
                const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                out += keep ? c : '-';
            }
            while (!out.empty() && out.back() == '-') {
                out.pop_back();
            }
            return out;
        }

        std::string step_file(size_t index, const Step& step) {
            char number[8];
            std::snprintf(number, sizeof(number), "%03u", static_cast<unsigned>(index));
            return g_stem + ".s" + number + "-" + oot::ui::lighting::row_key(step.row) + "-" +
                   slug(oot::ui::lighting::option_label(step.row, step.value)) + ".png";
        }

        void finish(const char* why) {
            // The person's own values back, whatever the last step left in the renderer.
            oot::renderer::apply_lighting(oot::ui::lighting::values());
            if (g_log.is_open()) {
                g_log << "\n" << why << "\n";
                g_log.close();
            }
            std::fprintf(stderr, "[sweep] %s: %zu of %zu frames in %s\n", why, g_index, g_steps.size(), g_folder.string().c_str());
            g_phase = Phase::Idle;
            g_steps.clear();
            g_index = 0;
        }

    } // namespace

    bool start() {
        if (g_phase != Phase::Idle) {
            std::fprintf(stderr, "[sweep] refused: a sweep is running\n");
            return false;
        }

        // Every option of every row, in the menu's order. The Inspect views and the Experiment
        // variants are rows like the rest and are walked too: the views show one pass at a time
        // and the variants each undo one piece of the math, which is what the person is hunting.
        g_original = oot::ui::lighting::current();
        g_steps.clear();
        for (int row = 0; row < oot::ui::lighting::row_count(); ++row) {
            const int count = oot::ui::lighting::option_count(row);
            for (int value = 0; value < count; ++value) {
                g_steps.push_back(Step{ row, value });
            }
        }
        if (g_steps.empty()) {
            std::fprintf(stderr, "[sweep] refused: no rows to walk\n");
            return false;
        }

        const std::time_t t = std::time(nullptr);
        std::tm now{};
        localtime_s(&now, &t);
        char day[16];
        char time_of_day[16];
        std::strftime(day, sizeof(day), "%Y-%m-%d", &now);
        std::strftime(time_of_day, sizeof(time_of_day), "%H-%M-%S", &now);
        g_folder = oot::places::captures() / day;
        g_stem = std::string(time_of_day) + "-oot-lighting-sweep";
        std::error_code ec;
        std::filesystem::create_directories(g_folder, ec);
        if (ec) {
            std::fprintf(stderr, "[sweep] refused: the folder %s could not be made\n", g_folder.string().c_str());
            g_steps.clear();
            return false;
        }

        g_log.open(g_folder / (g_stem + ".sweep.txt"), std::ios::out | std::ios::trunc);
        if (g_log.is_open()) {
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &now);
            g_log << "# OoT: Recompiled lighting sweep: every option of every row, one frame each\n";
            g_log << "started = " << stamp << " (local time)\n";
            g_log << "settle_frames = " << SETTLE_FRAMES << " presented frames before each capture\n";
            g_log << "\n[the person's own rows, put back at the end]\n";
            for (int row = 0; row < oot::ui::lighting::row_count(); ++row) {
                const int value = oot::ui::lighting::get_row(g_original, row);
                g_log << oot::ui::lighting::row_key(row) << " = " << value << " (" << oot::ui::lighting::option_label(row, value) << ")\n";
            }
            g_log << "\n[steps]\n";
        }

        g_index = 0;
        g_frames = 0;
        g_phase = Phase::Apply;
        std::fprintf(stderr, "[sweep] started: %zu steps into %s\n", g_steps.size(), (g_folder / g_stem).string().c_str());
        return true;
    }

    void tick() {
        if (g_phase == Phase::Idle) {
            return;
        }
        if (g_index >= g_steps.size()) {
            finish("done");
            return;
        }
        const Step& step = g_steps[g_index];
        switch (g_phase) {
            case Phase::Apply: {
                // The step's option over the person's own values, to the renderer alone.
                oot::ui::lighting::Lighting trial = g_original;
                oot::ui::lighting::set_row(trial, step.row, step.value);
                oot::renderer::apply_lighting(oot::ui::lighting::values_of(trial));
                g_frames = 0;
                g_phase = Phase::Settle;
                break;
            }
            case Phase::Settle:
                if (++g_frames >= SETTLE_FRAMES) {
                    g_phase = Phase::Snapshot;
                }
                break;
            case Phase::Snapshot: {
                const std::string file = step_file(g_index, step);
                g_last_frame = g_folder / file;
                if (oot::recorder::snapshot(g_last_frame, FULL_WIDTH)) {
                    if (g_log.is_open()) {
                        g_log << g_index << "  " << oot::ui::lighting::row_label(step.row) << " = "
                              << oot::ui::lighting::option_label(step.row, step.value) << "  ->  " << file << "\n";
                        g_log.flush();
                    }
                    std::fprintf(stderr, "[sweep] %zu/%zu %s = %s -> %s\n", g_index + 1, g_steps.size(),
                                 oot::ui::lighting::row_label(step.row), oot::ui::lighting::option_label(step.row, step.value), file.c_str());
                    g_phase = Phase::Waiting;
                }
                // Else a snapshot is still waiting (a moment's thumbnail, say): try next frame.
                break;
            }
            case Phase::Waiting:
                if (!oot::recorder::snapshot_pending()) {
                    // The snapshot writes a raw copy beside the PNG for the interface's thumbnails
                    // (recorder.cpp); a full frame's is eight megabytes nobody reads. Gone.
                    std::error_code ec;
                    std::filesystem::path raw = g_last_frame;
                    raw.replace_extension(".rgba");
                    std::filesystem::remove(raw, ec);
                    ++g_index;
                    g_phase = Phase::Apply;
                }
                break;
            case Phase::Idle:
                break;
        }
    }

    bool running() {
        return g_phase != Phase::Idle;
    }

    std::string status() {
        if (g_phase == Phase::Idle || g_index >= g_steps.size()) {
            return std::string();
        }
        const Step& step = g_steps[g_index];
        return "Sweep " + std::to_string(g_index + 1) + " of " + std::to_string(g_steps.size()) + ": " +
               oot::ui::lighting::row_label(step.row) + " = " + oot::ui::lighting::option_label(step.row, step.value);
    }

} // namespace oot::sweep
