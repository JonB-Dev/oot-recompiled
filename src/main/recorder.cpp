#include "main/recorder.h"

#include "game/camera_cuts.h"

#include "game/game_state.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "main/places.h"
#include "game/rt_state.h"
#include "ui/ui_lighting.h"
#include "ui/ui_settings.h"

#include "build_info.h"

// miniz writes the PNG. It is already in this link twice over (RT64 compiles its own copy, and
// librecomp links the runtime's), so using it adds no dependency and .scaffold/dependencies.md
// needs no new row. Its header comes in through RT64's contrib include path.
#include <miniz/miniz.h>

namespace {

    using Clock = std::chrono::steady_clock;

    // Five seconds to a minute, in fives. The user's own words: "any incriment of 5 seconds every
    // 5 seconds up to 1 min".
    constexpr int LENGTH_STEP = 5;
    constexpr int LENGTH_MAX = 60;
    constexpr int LENGTH_COUNT = LENGTH_MAX / LENGTH_STEP;

    // The deflate effort miniz spends per frame, on its 0 to 10 scale.
    //
    // ONE, AND THE REASON IS THROUGHPUT, NOT LAZINESS. The encoders have to keep up with a live
    // frame rate or the present thread starts waiting, and waiting changes the very timing this
    // feature exists to measure. Level 1 on a 1080p frame is tens of milliseconds; the levels
    // above it cost several times that for a file perhaps a fifth smaller. Disk is cheap and a
    // perturbed measurement is not.
    constexpr mz_uint PNG_LEVEL = 1;

    // How many frames may sit waiting to be encoded before the present thread has to wait for
    // room. At 1080p a frame is about 8 MB here, so this is roughly half a gigabyte: enough to
    // ride out a slow patch without the program's memory use becoming the new problem.
    constexpr int QUEUE_CAP = 64;

    struct Frame {
        std::vector<uint8_t> bgra;   // tightly packed, width * 4 per row
        int width = 0;
        int height = 0;
        int number = 0;              // 1 for the first frame of the recording
        uint64_t presented_us = 0;   // since the recording started
    };

    std::mutex g_mutex;
    std::condition_variable g_has_work;   // an encoder has something to do
    std::condition_variable g_has_room;   // the present thread may queue again
    std::deque<Frame> g_queue;

    std::vector<std::thread> g_threads;
    bool g_threads_running = false;
    bool g_stopping = false;

    // Frames taken off the queue and not yet written. THE QUEUE BEING EMPTY IS NOT THE SAME AS
    // THE WORK BEING DONE, which is what the first version assumed: an encoder holds a frame for
    // tens of milliseconds after popping it, so the finisher could see an empty queue and write
    // the notes while two frames were still being deflated. It reported 299 where 301 landed.
    std::atomic<int> g_in_flight{ 0 };

    // Read by the overlay from another thread, so they are atomic rather than guarded: the
    // overlay only ever displays them, and a count that is one frame stale on screen is fine.
    std::atomic<oot::recorder::State> g_state{ oot::recorder::State::Idle };
    std::atomic<int> g_seconds_total{ 0 };
    std::atomic<int> g_frames_taken{ 0 };
    std::atomic<int> g_frames_written{ 0 };
    std::atomic<int> g_stalls{ 0 };
    std::atomic<bool> g_failed{ false };

    // Written when a recording starts and read by the encoders. Fixed for the whole recording, so
    // no lock: the encoders are started before it is set for the first time and the value only
    // changes while the queue is empty.
    std::filesystem::path g_folder;
    std::string g_stem;

    // When the recording started, as steady clock microseconds. An atomic integer rather than a
    // time_point because the overlay reads it from the render thread while toggle() may be writing
    // it from the same thread's command drain: a torn read of a two word type is a real hazard for
    // the sake of a number that is only ever displayed.
    std::atomic<int64_t> g_started_us{ 0 };

    int64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   Clock::now().time_since_epoch()).count();
    }

    // What the game was doing on each captured frame, sampled beside the picture.
    struct FrameState {
        int32_t day_time = -1;
        int32_t night_flag = -1;
        int32_t entrance = -1;
        int32_t next_day_time = -1;
        // R_UPDATE_RATE: how many video retraces the game means each of its own updates to last.
        // Three in ordinary play, which is its twenty a second. THE REASON IT IS HERE: in the
        // capture of the clock racing, the clock moved on SIX CONSECUTIVE DRAWN FRAMES, and a
        // game updating three times a second slower than the picture can only move it on every
        // third. So the game was updating once per drawn frame through that window, and this is
        // the number that says so outright instead of by inference.
        int32_t update_rate = -1;
        uint32_t ticks = 0;
        uint32_t vis = 0;
        uint32_t cuts = 0;
        // Frames the game dropped for an overflowed display list pool, cumulative. THE COLUMN
        // THAT NAMED THE FAULT: a run of pictures with updates and no submissions is the
        // teleporting, and this is the count that says so directly (game_state.h).
        uint32_t dropped = 0;
    };
    std::vector<FrameState> g_frame_states;

    // The picture's size, for the notes file. Written by the present thread, read by the finisher.
    std::atomic<int> g_last_width{ 0 };
    std::atomic<int> g_last_height{ 0 };

    // The thread that waits for the encoders to catch up and then writes the notes. Held rather
    // than detached so shutdown can join it: a detached thread still touching these statics while
    // the process tears them down is the kind of exit crash that only happens on somebody else's
    // machine.
    std::thread g_finisher;

    // The per frame timing, collected while recording and written out at the end. Guarded by
    // g_mutex with the queue, because the encoders never touch it; only the present thread appends
    // and only the finishing path reads.
    std::vector<uint64_t> g_frame_times_us;

    void say(const char* what) {
        std::fprintf(stderr, "[record] %s\n", what);
    }

    // Local time, because the folder and the file name are for a person looking for the recording
    // they just made. localtime_s is the one that is safe here and is what Windows offers.
    std::tm local_now() {
        const std::time_t now = std::time(nullptr);
        std::tm out{};
        localtime_s(&out, &now);
        return out;
    }

    // BGRA as the swap chain hands it over, to the RGB a PNG wants. Three channels rather than
    // four on purpose: the alpha of a presented frame is meaningless, and dropping it takes a
    // quarter off both the file and the time spent deflating it.
    std::vector<uint8_t> to_rgb(const Frame& frame) {
        const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
        std::vector<uint8_t> rgb(pixels * 3);
        const uint8_t* src = frame.bgra.data();
        uint8_t* dst = rgb.data();
        for (size_t i = 0; i < pixels; ++i) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst += 3;
            src += 4;
        }
        return rgb;
    }

    void write_one(const Frame& frame) {
        const std::vector<uint8_t> rgb = to_rgb(frame);

        size_t length = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(
            rgb.data(), frame.width, frame.height, 3, &length, PNG_LEVEL, MZ_FALSE);
        if (png == nullptr || length == 0) {
            g_failed.store(true, std::memory_order_relaxed);
            say("a frame could not be encoded");
            return;
        }

        // The user's own naming, kept exactly: <time>-oot-gameplay-debug.f<N>.png, the frame
        // number rising through the sequence and NOT zero padded, which is how they wrote it.
        // Windows sorts numerically in Explorer and in every picture viewer worth using, so f2
        // still lands between f1 and f10.
        const std::filesystem::path path =
            g_folder / (g_stem + ".f" + std::to_string(frame.number) + ".png");

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(png), static_cast<std::streamsize>(length));
        const bool ok = out.good();
        out.close();
        mz_free(png);

        if (!ok) {
            g_failed.store(true, std::memory_order_relaxed);
            say("a frame could not be written; is the disk full?");
            return;
        }
        g_frames_written.fetch_add(1, std::memory_order_relaxed);
    }

    // The notes beside the pictures, and the half of this feature that turns "see the problem"
    // into "measure the problem". A folder of images says a hitch happened; this says which frames
    // it happened on and how long each one actually took.
    void write_notes(int width, int height) {
        std::vector<uint64_t> times;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            times = g_frame_times_us;
        }

        const std::filesystem::path path = g_folder / (g_stem + ".frames.csv");
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            say("the notes file could not be written");
            return;
        }

        out << "# OoT: Recompiled frame recording\n";
        out << "# picture," << width << "x" << height << "\n";
        out << "# asked for," << g_seconds_total.load(std::memory_order_relaxed) << " seconds\n";
        out << "# frames," << times.size() << "\n";
        out << "# stalls," << g_stalls.load(std::memory_order_relaxed)
            << "   (times the recorder had to hold the picture up to keep a frame;"
               " these frames' timings are the recorder's fault, not the game's)\n";
        // MICROSECONDS SINCE THE FIRST FRAME, not since the machine started. The clock behind
        // these is a steady one whose zero is arbitrary, so the raw numbers were eleven digits of
        // noise with the useful part in the last four. Frame 1 is time 0 and everything is
        // measured from it, which is what somebody counting the length of a hitch actually wants.
        out << "frame,microseconds,delta_us,day_time,clock,night,entrance,next_day_time,"
               "update_rate,ticks,updates_here,vis_here,cuts,cut_here,dropped,dropped_here\n";
        const std::vector<FrameState> states = g_frame_states;
        const uint64_t origin = times.empty() ? 0 : times.front();
        uint64_t previous = 0;
        for (size_t i = 0; i < times.size(); ++i) {
            const uint64_t at = times[i] - origin;
            const uint64_t delta = (i == 0) ? 0 : at - previous;
            out << (i + 1) << "," << at << "," << delta;
            if (i < states.size()) {
                const FrameState& s = states[i];
                // The clock as a person reads it, beside the raw value, because "0x8000" is not
                // what anybody means when they say the time changed.
                const int minutes = (s.day_time < 0) ? -1
                                  : static_cast<int>(static_cast<int64_t>(s.day_time) * 1440 / 65536);
                char clock[16] = "?";
                if (minutes >= 0) {
                    std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
                }
                const bool cut_here = (i > 0 && i < states.size()
                                       && s.cuts > states[i - 1].cuts);
                out << "," << s.day_time << "," << clock << "," << s.night_flag
                    << "," << s.entrance << "," << s.next_day_time
                    << "," << s.update_rate
                    << "," << s.ticks
                    << "," << ((i > 0) ? (s.ticks - states[i - 1].ticks) : 0)
                    << "," << ((i > 0) ? (s.vis - states[i - 1].vis) : 0)
                    << "," << s.cuts << "," << (cut_here ? 1 : 0)
                    << "," << s.dropped
                    << "," << ((i > 0) ? (s.dropped - states[i - 1].dropped) : 0);
            }
            out << "\n";
            previous = at;
        }
    }

    void encoder_loop() {
        for (;;) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_has_work.wait(lock, [] { return g_stopping || !g_queue.empty(); });
                if (g_queue.empty()) {
                    if (g_stopping) {
                        return;
                    }
                    continue;
                }
                frame = std::move(g_queue.front());
                g_queue.pop_front();
                g_in_flight.fetch_add(1, std::memory_order_relaxed);
            }
            g_has_room.notify_one();
            write_one(frame);
            g_in_flight.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void start_threads() {
        if (g_threads_running) {
            return;
        }
        // One fewer than the machine has, and at least one. The present thread and the game thread
        // both still need to run; taking every core would slow the thing being recorded.
        unsigned int count = std::thread::hardware_concurrency();
        count = (count > 2) ? count - 1 : 1;
        g_stopping = false;
        for (unsigned int i = 0; i < count; ++i) {
            g_threads.emplace_back(encoder_loop);
        }
        g_threads_running = true;
        std::fprintf(stderr, "[record] %u encoder threads\n", count);
    }

    // Nothing queued AND nothing being written. Both halves are needed; see g_in_flight.
    bool all_written() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_queue.empty() && g_in_flight.load(std::memory_order_relaxed) == 0;
    }

    int queue_size() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return static_cast<int>(g_queue.size());
    }

    void finish() {
        // The frames already queued still get written; that is what Draining is.
        g_state.store(oot::recorder::State::Draining, std::memory_order_relaxed);

        // A previous finisher has always run to completion by now: a new recording cannot start
        // while one is draining (toggle refuses), and the only other caller is the frame that ran
        // the clock out. So this join is free rather than a wait on the present thread.
        if (g_finisher.joinable()) {
            g_finisher.join();
        }
        g_finisher = std::thread([] {
            while (!all_written()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            write_notes(g_last_width.load(std::memory_order_relaxed),
                        g_last_height.load(std::memory_order_relaxed));
            std::fprintf(stderr, "[record] finished: %d frames, %d stalls, in %s\n",
                         g_frames_written.load(std::memory_order_relaxed),
                         g_stalls.load(std::memory_order_relaxed),
                         g_folder.string().c_str());
            g_state.store(oot::recorder::State::Idle, std::memory_order_relaxed);
        });
    }

} // namespace

namespace oot::recorder {

    int length_seconds(int index) {
        if (index < 0) {
            index = 0;
        }
        if (index >= LENGTH_COUNT) {
            index = LENGTH_COUNT - 1;
        }
        return (index + 1) * LENGTH_STEP;
    }

    Status status() {
        Status s;
        s.state = g_state.load(std::memory_order_relaxed);
        s.seconds_total = g_seconds_total.load(std::memory_order_relaxed);
        s.frames_taken = g_frames_taken.load(std::memory_order_relaxed);
        s.frames_written = g_frames_written.load(std::memory_order_relaxed);
        s.stalls = g_stalls.load(std::memory_order_relaxed);
        s.failed = g_failed.load(std::memory_order_relaxed);
        s.queue_depth = queue_size();
        s.seconds_elapsed = (s.state == State::Recording)
                                ? static_cast<double>(now_us() - g_started_us.load(std::memory_order_relaxed)) / 1e6
                                : 0.0;
        return s;
    }

    bool recording() {
        return g_state.load(std::memory_order_relaxed) == State::Recording;
    }

    // The command line's recording (phase 57): armed by --record, fired by the renderer's
    // context once a frame when the play tick count is reached. Plain atomics; the request is
    // written once at boot and read by one thread.
    namespace {
        std::atomic<bool> g_request_armed{ false };
        std::atomic<int> g_request_seconds{ 0 };
        std::atomic<uint32_t> g_request_ticks{ 0 };
    }

    void request_at_play_ticks(int seconds, uint32_t ticks) {
        if (seconds <= 0) {
            std::fprintf(stderr, "[recorder] --record needs a positive number of seconds\n");
            return;
        }

        g_request_seconds.store(seconds, std::memory_order_relaxed);
        g_request_ticks.store(ticks, std::memory_order_relaxed);
        g_request_armed.store(true, std::memory_order_release);
        std::fprintf(stderr, "[recorder] will record %d s from play tick %u\n", seconds, ticks);
    }

    void tick(uint32_t play_ticks) {
        if (!g_request_armed.load(std::memory_order_acquire)) {
            return;
        }

        if (play_ticks < g_request_ticks.load(std::memory_order_relaxed)) {
            return;
        }

        g_request_armed.store(false, std::memory_order_release);
        if (g_state.load(std::memory_order_relaxed) == State::Idle) {
            std::fprintf(stderr, "[recorder] play tick %u reached, recording\n", play_ticks);
            toggle(g_request_seconds.load(std::memory_order_relaxed));
        }
    }

    void toggle(int seconds) {
        const State state = g_state.load(std::memory_order_relaxed);

        if (state == State::Recording) {
            // Stopping early keeps everything taken so far. The person who clicked has seen what
            // they wanted to see and does not want the other forty seconds.
            //
            // Nothing in flight is lost by stopping here. The draw hook hands over the frame that
            // was already captured BEFORE it drains the commands that can land this click, so by
            // the time this runs there is nothing waiting.
            say("stopped early");
            finish();
            return;
        }
        if (state == State::Draining) {
            // Still writing the last one. Starting another now would interleave two recordings in
            // one folder, so this press is ignored rather than making a mess.
            say("still finishing the last recording");
            return;
        }

        const std::tm now = local_now();
        char day[16];
        char time_of_day[16];
        std::strftime(day, sizeof(day), "%Y-%m-%d", &now);
        std::strftime(time_of_day, sizeof(time_of_day), "%H-%M-%S", &now);

        g_folder = oot::places::captures() / day;
        g_stem = std::string(time_of_day) + "-oot-gameplay-debug";

        std::error_code ec;
        std::filesystem::create_directories(g_folder, ec);
        if (ec) {
            g_failed.store(true, std::memory_order_relaxed);
            say("the folder for the recording could not be made");
            return;
        }

        // THE SETTINGS IN FORCE, beside the frames, before the first one lands (the user,
        // 2026-09-24: "a timestamped log along with the screenshots so that if I record
        // anything it tells the EXACT settings I was using"). Every row of the settings and of
        // the lighting menu by key, number and word, the lighting level actually in force (the
        // command line can override the menu for a run), the version, the mode and where the
        // game was. Written first so a recording that stops early still has it.
        {
            const std::filesystem::path path = g_folder / (g_stem + ".settings.txt");
            std::ofstream out(path, std::ios::trunc);
            if (out) {
                char stamp[32];
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &now);
                const oot::game_state::Snapshot game = oot::game_state::read();
                char entrance[16];
                std::snprintf(entrance, sizeof(entrance), "0x%04X", static_cast<unsigned>(game.entrance_index) & 0xFFFFu);
                char clock[16];
                std::snprintf(clock, sizeof(clock), "0x%04X", static_cast<unsigned>(game.day_time));
                out << "# OoT: Recompiled frame recording: the settings in force when it started\n";
                out << "recorded = " << stamp << " (local time)\n";
                out << "version = " << oot::build_info::version << "\n";
                out << "mode = " << oot::places::mode_word() << "\n";
                out << "entrance = " << entrance << "\n";
                out << "age = " << (game.link_age == 1 ? "child" : "adult") << "\n";
                out << "day_time = " << clock << "\n";
                out << "health = " << game.health << " of " << game.health_capacity << "\n";
                out << "lighting_level_in_force = " << oot::rt_state::level()
                    << "  (the command line's --rt-level wins over the menu when given)\n";
                out << "\n[settings]\n";
                const oot::ui::Settings& s = oot::ui::settings();
                for (int row = 0; row < oot::ui::row_count(); ++row) {
                    const int value = oot::ui::get_row(s, row);
                    const char* word = oot::ui::option_label(row, value);
                    out << oot::ui::row_label(row) << " = " << value;
                    if (word != nullptr) {
                        out << " (" << word << ")";
                    } else {
                        out << " (" << value << "%)";
                    }
                    out << "\n";
                }
                out << "\n[lighting]\n";
                const oot::ui::lighting::Lighting& l = oot::ui::lighting::current();
                for (int row = 0; row < oot::ui::lighting::row_count(); ++row) {
                    const int value = oot::ui::lighting::get_row(l, row);
                    out << oot::ui::lighting::row_key(row) << " = " << value << " ("
                        << oot::ui::lighting::option_label(row, value) << ")\n";
                }
            } else {
                say("the settings log could not be written");
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_frame_times_us.clear();
            g_frame_states.clear();
        }
        g_frames_taken.store(0, std::memory_order_relaxed);
        g_frames_written.store(0, std::memory_order_relaxed);
        g_stalls.store(0, std::memory_order_relaxed);
        g_failed.store(false, std::memory_order_relaxed);
        g_seconds_total.store(seconds, std::memory_order_relaxed);

        start_threads();
        g_started_us.store(now_us(), std::memory_order_relaxed);
        g_state.store(State::Recording, std::memory_order_relaxed);
        std::fprintf(stderr, "[record] started: %d seconds into %s\n", seconds,
                     g_folder.string().c_str());
    }

    // The one off thumbnail (phase 75). A path waiting means the next frame is wanted.
    std::mutex g_snapshot_mutex;
    std::filesystem::path g_snapshot_path;
    int g_snapshot_width = 0;
    std::atomic<bool> g_snapshot_pending{ false };
    std::atomic<bool> g_snapshot_game_alone{ false };

    bool snapshot(const std::filesystem::path& png, int width, bool game_alone) {
        if (g_snapshot_pending.load(std::memory_order_acquire)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_snapshot_mutex);
            g_snapshot_path = png;
            g_snapshot_width = width > 0 ? width : 320;
        }
        g_snapshot_game_alone.store(game_alone, std::memory_order_relaxed);
        g_snapshot_pending.store(true, std::memory_order_release);
        return true;
    }

    bool snapshot_wants_game_alone() {
        return g_snapshot_pending.load(std::memory_order_acquire) && g_snapshot_game_alone.load(std::memory_order_relaxed);
    }

    bool snapshot_pending() {
        return g_snapshot_pending.load(std::memory_order_acquire);
    }

    // Scale by dropping pixels (nearest), convert to RGB, encode, write. Render thread.
    void take_snapshot(const uint8_t* bgra, int width, int height, std::size_t pitch) {
        std::filesystem::path path;
        int out_w = 0;
        {
            std::lock_guard<std::mutex> lock(g_snapshot_mutex);
            path = g_snapshot_path;
            out_w = g_snapshot_width;
        }
        if (out_w > width) {
            out_w = width;
        }
        const int out_h = (height * out_w) / width;
        if (out_w <= 0 || out_h <= 0) {
            return;
        }
        std::vector<uint8_t> rgb(static_cast<size_t>(out_w) * out_h * 3);
        for (int y = 0; y < out_h; ++y) {
            const int sy = (y * height) / out_h;
            const uint8_t* row = bgra + static_cast<size_t>(sy) * pitch;
            uint8_t* dst = rgb.data() + static_cast<size_t>(y) * out_w * 3;
            for (int x = 0; x < out_w; ++x) {
                const uint8_t* px = row + static_cast<size_t>((x * width) / out_w) * 4;
                dst[0] = px[2];
                dst[1] = px[1];
                dst[2] = px[0];
                dst += 3;
            }
        }
        size_t length = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), out_w, out_h, 3, &length, 6, MZ_FALSE);
        if (png == nullptr || length == 0) {
            say("the thumbnail could not be encoded");
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(png), static_cast<std::streamsize>(length));
        mz_free(png);
        if (!out.good()) {
            say("the thumbnail could not be written");
        }

        // AND THE SAME PICTURE RAW, beside the PNG, for the interface: its texture loader draws no
        // bitmap format (ui_render.cpp, LoadTexture), and a PNG decoder is a dependency this project
        // does not take for a 320 pixel thumbnail. The raw file is ours: "RGBA", width, height, the
        // pixels; the PNG stays for people.
        std::filesystem::path raw = path;
        raw.replace_extension(".rgba");
        std::ofstream rawout(raw, std::ios::binary | std::ios::trunc);
        const char magic[4] = { 'R', 'G', 'B', 'A' };
        const uint32_t dims[2] = { static_cast<uint32_t>(out_w), static_cast<uint32_t>(out_h) };
        rawout.write(magic, 4);
        rawout.write(reinterpret_cast<const char*>(dims), sizeof(dims));
        std::vector<uint8_t> rgba(static_cast<size_t>(out_w) * out_h * 4);
        for (size_t i = 0; i < static_cast<size_t>(out_w) * out_h; ++i) {
            rgba[i * 4 + 0] = rgb[i * 3 + 0];
            rgba[i * 4 + 1] = rgb[i * 3 + 1];
            rgba[i * 4 + 2] = rgb[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
        rawout.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    }

    // The swap chain is wanted for a recording, or for a snapshot of the whole window; a
    // snapshot of the game alone is taken through the picture hook instead.
    bool wants_frame() {
        return g_state.load(std::memory_order_relaxed) == State::Recording ||
               (g_snapshot_pending.load(std::memory_order_acquire) && !g_snapshot_game_alone.load(std::memory_order_relaxed));
    }

    void submit_snapshot(const uint8_t* bgra, int width, int height, std::size_t pitch) {
        if (bgra == nullptr || width <= 0 || height <= 0 || !g_snapshot_pending.load(std::memory_order_acquire)) {
            return;
        }
        take_snapshot(bgra, width, height, pitch);
        g_snapshot_pending.store(false, std::memory_order_release);
    }

    bool time_is_up() {
        if (g_state.load(std::memory_order_relaxed) != State::Recording) {
            return false;
        }
        const int64_t since_start = now_us() - g_started_us.load(std::memory_order_relaxed);
        return since_start >= static_cast<int64_t>(g_seconds_total.load(std::memory_order_relaxed)) * 1000000;
    }

    void stop() {
        if (g_state.load(std::memory_order_relaxed) != State::Recording) {
            return;
        }
        finish();
    }

    void submit(const uint8_t* bgra, int width, int height, std::size_t pitch,
                uint64_t presented_us) {
        if (bgra == nullptr || width <= 0 || height <= 0) {
            return;
        }
        // The thumbnail first, whether or not a recording runs: it wanted exactly one frame and
        // this is it.
        if (g_snapshot_pending.load(std::memory_order_acquire) && !g_snapshot_game_alone.load(std::memory_order_relaxed)) {
            take_snapshot(bgra, width, height, pitch);
            g_snapshot_pending.store(false, std::memory_order_release);
        }
        // NO TIME CHECK HERE. See time_is_up in the header: the caller hands over the frame that
        // was already in flight before it asks whether to stop, so a frame that arrives here is
        // one that was captured while recording and is always kept.
        if (g_state.load(std::memory_order_relaxed) != State::Recording) {
            return;
        }

        g_last_width.store(width, std::memory_order_relaxed);
        g_last_height.store(height, std::memory_order_relaxed);

        Frame frame;
        frame.width = width;
        frame.height = height;
        frame.number = g_frames_taken.fetch_add(1, std::memory_order_relaxed) + 1;
        frame.presented_us = presented_us;

        // Copied out row by row because the readback buffer's rows are aligned wider than the
        // picture. The caller's memory is reused for the next frame the moment this returns, so
        // there is no holding a pointer to it.
        const size_t row = static_cast<size_t>(width) * 4;
        frame.bgra.resize(row * height);
        for (int y = 0; y < height; ++y) {
            std::memcpy(frame.bgra.data() + static_cast<size_t>(y) * row, bgra + static_cast<size_t>(y) * pitch, row);
        }

        {
            std::unique_lock<std::mutex> lock(g_mutex);
            // THE WAIT, RATHER THAN A DROP. See the header: losing a frame would quietly corrupt
            // the one measurement this feature exists to make. Holding the picture up is visible
            // both on screen and in the notes file, so it can be allowed for.
            if (g_queue.size() >= static_cast<size_t>(QUEUE_CAP)) {
                g_stalls.fetch_add(1, std::memory_order_relaxed);
                g_has_room.wait(lock, [] { return g_queue.size() < static_cast<size_t>(QUEUE_CAP); });
            }
            g_frame_times_us.push_back(presented_us);
            // THE STATE THAT FRAME WAS SHOWING. Sampled here, with the picture, so the row and
            // the image with the same number describe the same instant.
            {
                FrameState fs;
                const oot::game_state::Snapshot snap = oot::game_state::read();
                if (snap.valid) {
                    fs.day_time = snap.day_time;
                    fs.night_flag = snap.night_flag;
                    fs.entrance = snap.entrance_index;
                    fs.next_day_time = snap.next_day_time;
                    fs.update_rate = snap.update_rate;
                }
                fs.cuts = oot::camera_cuts::counts().cuts;
                fs.ticks = oot::game_state::play_ticks();
                fs.vis = oot::game_state::vi_count();
                fs.dropped = oot::game_state::dropped_frames();
                g_frame_states.push_back(fs);
            }
            g_queue.push_back(std::move(frame));
        }
        g_has_work.notify_one();
    }

    void shutdown() {
        if (!g_threads_running) {
            return;
        }
        g_state.store(State::Idle, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_stopping = true;
        }
        g_has_work.notify_all();
        for (std::thread& t : g_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        g_threads.clear();
        g_threads_running = false;
        if (g_finisher.joinable()) {
            g_finisher.join();
        }
    }

} // namespace oot::recorder
