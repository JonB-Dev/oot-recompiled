#include "main/probe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <share.h>
#endif

namespace {

    std::FILE* g_file = nullptr;
    std::atomic<bool> g_on{ false };

    // One VI is one tick. NTSC runs 60 of them a second, so a line a second is 60 ticks. The game
    // renders at 20, which is why the tick source is the VI and not the frame: the VI keeps
    // counting through a slow frame, and a slow frame is exactly when audio goes wrong.
    constexpr uint32_t TICKS_PER_LINE = 60;

    std::atomic<uint32_t> g_ticks{ 0 };
    std::atomic<uint64_t> g_line{ 0 };

    std::atomic<uint32_t> g_requested_hz{ 0 };
    std::atomic<uint32_t> g_opened_hz{ 0 };
    std::atomic<uint32_t> g_rate_changes{ 0 };

    std::atomic<uint32_t> g_buffers{ 0 };
    std::atomic<uint64_t> g_samples{ 0 };

    // Roughness. Real audio is smooth at the sample level: a 32 kHz stream of anything musical
    // moves a little between one sample and the next compared with how far it is from zero. Noise
    // does not. So sum|x[n] - x[n-1]| over sum|x[n]| separates "audio" from "whatever happened to
    // be in memory", which is the exact failure this project already shipped once, when
    // queue_samples asked the device to read twice the buffer and played the tail as a click
    // track. A tone sits well under 1. White noise lands around 1.5 and up.
    std::atomic<uint64_t> g_sum_abs{ 0 };
    std::atomic<uint64_t> g_sum_diff{ 0 };

    // The queue depth band, in frames. Reset each line.
    constexpr size_t DEPTH_UNSET = static_cast<size_t>(-1);
    std::atomic<size_t> g_depth_min{ DEPTH_UNSET };
    std::atomic<size_t> g_depth_max{ 0 };
    std::atomic<uint64_t> g_depth_sum{ 0 };
    std::atomic<uint32_t> g_depth_polls{ 0 };
    std::atomic<uint32_t> g_underruns{ 0 };

    // The run of consecutive empty polls, and the longest such run this line. Only the polling
    // side touches the running count, so it needs no more than relaxed ordering.
    std::atomic<uint32_t> g_zero_run{ 0 };
    std::atomic<uint32_t> g_zero_run_max{ 0 };

    // The graphics thread's spans, one set of counters per Span. Reset each line.
    constexpr int SPAN_COUNT = static_cast<int>(oot::probe::Span::Count);
    const char* const SPAN_NAMES[SPAN_COUNT] = { "displaylist", "screen", "submit", "present", "interface" };

    // A span over this is a hitch worth a line of its own. One display frame at sixty is 16.7 ms;
    // 12 ms of display list work is already most of one and is the level at which a person starts
    // seeing it. The present (Span::Screen) waits for the display by design, so it gets a much
    // higher bar: only a present that missed several refreshes is news.
    //
    // SUBMIT'S THRESHOLD IS THE ONE THAT NEEDS EXPLAINING. This game updates twenty times a
    // second in ordinary play (R_UPDATE_RATE 3, one game frame every three retraces), so display
    // lists arrive 50 ms apart and that is HEALTHY, not a hitch. The first version of this used
    // 40 ms and reported a hitch on literally every frame of every run, which buried the real
    // ones. 75 ms means a game frame and a half: a game frame that was actually missed.
    // PRESENT'S THRESHOLD is one and a half refreshes of a sixty hertz display, 25 ms. A frame
    // that took longer than that means the picture stood still for at least two refreshes, which
    // is what a person calls a stutter. It is deliberately not scaled to the display's real rate:
    // on a faster display the same 25 ms is an even worse stall, so the line is still right.
    // INTERFACE gets a low bar, 4 ms, because it is work this program chose to do on the thread
    // that presents. A quarter of a display frame spent there is already too much.
    constexpr uint64_t HITCH_US[SPAN_COUNT] = { 12000, 60000, 75000, 25000, 4000 };

    // At most this many hitch lines a second, so a run that hitches constantly still produces a
    // readable trace rather than a wall of text that hides the pattern.
    constexpr uint32_t HITCH_LINES_PER_LINE = 12;

    std::atomic<uint32_t> g_span_count[SPAN_COUNT];
    std::atomic<uint64_t> g_span_sum[SPAN_COUNT];
    std::atomic<uint64_t> g_span_max[SPAN_COUNT];
    std::atomic<uint32_t> g_span_over[SPAN_COUNT];
    std::atomic<uint32_t> g_hitch_lines{ 0 };

    void atomic_max64(std::atomic<uint64_t>& slot, uint64_t v) {
        uint64_t cur = slot.load(std::memory_order_relaxed);
        while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
        }
    }

    void atomic_min(std::atomic<size_t>& slot, size_t v) {
        size_t cur = slot.load(std::memory_order_relaxed);
        while (v < cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
        }
    }

    void atomic_max(std::atomic<size_t>& slot, size_t v) {
        size_t cur = slot.load(std::memory_order_relaxed);
        while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
        }
    }

} // namespace

namespace oot::probe {

    bool enable(const std::string& path) {
        if (g_file != nullptr) {
            return true;
        }
#ifdef _WIN32
        // _fsopen with _SH_DENYWR, not fopen. A plain fopen on Windows takes the file exclusively,
        // so the probe could not be read until the program exited, which defeats the point of a
        // live health line. Found by trying to read it mid-run and getting a permission error that
        // looks like a filesystem problem and is not one.
        g_file = _fsopen(path.c_str(), "w", _SH_DENYWR);
#else
        g_file = std::fopen(path.c_str(), "w");
#endif
        if (g_file == nullptr) {
            std::fprintf(stderr, "[probe] could not open %s for writing\n", path.c_str());
            return false;
        }

        // Unbuffered. A probe that loses its last second because the program died is useless in
        // the one case it was there for.
        std::setvbuf(g_file, nullptr, _IONBF, 0);
        std::fprintf(g_file,
                     "# line vis rate_hz rate_ok buffers samples q_min q_mean q_max q_polls "
                     "underruns zero_run_max roughness "
                     "dl_mean_us dl_max_us dl_over screen_mean_us screen_max_us screen_over "
                     "submit_mean_us submit_max_us submit_over "
                     "present_mean_us present_max_us present_over presents "
                     "ui_mean_us ui_max_us ui_over\n");
        g_on.store(true, std::memory_order_release);
        return true;
    }

    bool enabled() {
        return g_on.load(std::memory_order_acquire);
    }

    void note_device_rate(uint32_t requested_hz, uint32_t opened_hz) {
        g_requested_hz.store(requested_hz, std::memory_order_relaxed);
        g_opened_hz.store(opened_hz, std::memory_order_relaxed);
        g_rate_changes.fetch_add(1, std::memory_order_relaxed);
    }

    void note_audio_buffer(const int16_t* samples, size_t sample_count) {
        if (!enabled() || samples == nullptr || sample_count == 0) {
            return;
        }

        uint64_t abs_sum = 0;
        uint64_t diff_sum = 0;
        int32_t prev = samples[0];
        for (size_t i = 0; i < sample_count; ++i) {
            const int32_t s = samples[i];
            abs_sum += static_cast<uint64_t>(s < 0 ? -s : s);
            const int32_t d = s - prev;
            diff_sum += static_cast<uint64_t>(d < 0 ? -d : d);
            prev = s;
        }

        g_buffers.fetch_add(1, std::memory_order_relaxed);
        g_samples.fetch_add(sample_count, std::memory_order_relaxed);
        g_sum_abs.fetch_add(abs_sum, std::memory_order_relaxed);
        g_sum_diff.fetch_add(diff_sum, std::memory_order_relaxed);
    }

    void note_queue_depth(size_t frames_remaining) {
        if (!enabled()) {
            return;
        }
        g_depth_polls.fetch_add(1, std::memory_order_relaxed);
        g_depth_sum.fetch_add(frames_remaining, std::memory_order_relaxed);
        atomic_min(g_depth_min, frames_remaining);
        atomic_max(g_depth_max, frames_remaining);

        if (frames_remaining == 0) {
            g_underruns.fetch_add(1, std::memory_order_relaxed);
            const uint32_t run = g_zero_run.fetch_add(1, std::memory_order_relaxed) + 1;
            uint32_t worst = g_zero_run_max.load(std::memory_order_relaxed);
            while (run > worst &&
                   !g_zero_run_max.compare_exchange_weak(worst, run, std::memory_order_relaxed)) {
            }
        }
        else {
            g_zero_run.store(0, std::memory_order_relaxed);
        }
    }

    void note_span(Span which, uint64_t microseconds) {
        if (!enabled()) {
            return;
        }
        const int slot = static_cast<int>(which);
        if (slot < 0 || slot >= SPAN_COUNT) {
            return;
        }

        g_span_count[slot].fetch_add(1, std::memory_order_relaxed);
        g_span_sum[slot].fetch_add(microseconds, std::memory_order_relaxed);
        atomic_max64(g_span_max[slot], microseconds);

        if (microseconds >= HITCH_US[slot]) {
            g_span_over[slot].fetch_add(1, std::memory_order_relaxed);
            // Straight to stderr, not to the probe file, so a hitch sits in the trace beside the
            // `[cam]` and `[dl]` lines that say what the frame was doing when it happened.
            if (g_hitch_lines.fetch_add(1, std::memory_order_relaxed) < HITCH_LINES_PER_LINE) {
                std::fprintf(stderr, "[hitch] %s %llu us\n", SPAN_NAMES[slot],
                             static_cast<unsigned long long>(microseconds));
            }
        }
    }

    void tick() {
        if (!enabled()) {
            return;
        }
        if (g_ticks.fetch_add(1, std::memory_order_relaxed) + 1 < TICKS_PER_LINE) {
            return;
        }
        const uint32_t vis = g_ticks.exchange(0, std::memory_order_relaxed);

        const uint32_t requested = g_requested_hz.load(std::memory_order_relaxed);
        const uint32_t opened    = g_opened_hz.load(std::memory_order_relaxed);
        const uint32_t buffers   = g_buffers.exchange(0, std::memory_order_relaxed);
        const uint64_t samples   = g_samples.exchange(0, std::memory_order_relaxed);
        const uint64_t abs_sum   = g_sum_abs.exchange(0, std::memory_order_relaxed);
        const uint64_t diff_sum  = g_sum_diff.exchange(0, std::memory_order_relaxed);
        const size_t   dmin      = g_depth_min.exchange(DEPTH_UNSET, std::memory_order_relaxed);
        const size_t   dmax      = g_depth_max.exchange(0, std::memory_order_relaxed);
        const uint64_t dsum      = g_depth_sum.exchange(0, std::memory_order_relaxed);
        const uint32_t polls     = g_depth_polls.exchange(0, std::memory_order_relaxed);
        const uint32_t under     = g_underruns.exchange(0, std::memory_order_relaxed);
        const uint32_t zrun      = g_zero_run_max.exchange(0, std::memory_order_relaxed);
        const uint64_t dmean     = (polls == 0) ? 0 : dsum / polls;

        const double roughness = (abs_sum == 0)
                                     ? 0.0
                                     : static_cast<double>(diff_sum) / static_cast<double>(abs_sum);

        uint64_t span_mean[SPAN_COUNT];
        uint64_t span_max[SPAN_COUNT];
        uint32_t span_over[SPAN_COUNT];
        uint32_t span_n[SPAN_COUNT];
        for (int i = 0; i < SPAN_COUNT; ++i) {
            const uint32_t n = g_span_count[i].exchange(0, std::memory_order_relaxed);
            const uint64_t s = g_span_sum[i].exchange(0, std::memory_order_relaxed);
            span_n[i] = n;
            span_mean[i] = (n == 0) ? 0 : s / n;
            span_max[i]  = g_span_max[i].exchange(0, std::memory_order_relaxed);
            span_over[i] = g_span_over[i].exchange(0, std::memory_order_relaxed);
        }
        g_hitch_lines.store(0, std::memory_order_relaxed);

        std::fprintf(g_file,
                     "%llu %u %u %d %u %llu %lld %llu %llu %u %u %u %.4f "
                     "%llu %llu %u %llu %llu %u %llu %llu %u %llu %llu %u %u %llu %llu %u\n",
                     static_cast<unsigned long long>(g_line.fetch_add(1, std::memory_order_relaxed)),
                     vis,
                     opened,
                     (requested == opened) ? 1 : 0,
                     buffers,
                     static_cast<unsigned long long>(samples),
                     (dmin == DEPTH_UNSET) ? -1LL : static_cast<long long>(dmin),
                     static_cast<unsigned long long>(dmean),
                     static_cast<unsigned long long>(dmax),
                     polls,
                     under,
                     zrun,
                     roughness,
                     static_cast<unsigned long long>(span_mean[0]),
                     static_cast<unsigned long long>(span_max[0]),
                     span_over[0],
                     static_cast<unsigned long long>(span_mean[1]),
                     static_cast<unsigned long long>(span_max[1]),
                     span_over[1],
                     static_cast<unsigned long long>(span_mean[2]),
                     static_cast<unsigned long long>(span_max[2]),
                     span_over[2],
                     static_cast<unsigned long long>(span_mean[3]),
                     static_cast<unsigned long long>(span_max[3]),
                     span_over[3],
                     span_n[3],
                     static_cast<unsigned long long>(span_mean[4]),
                     static_cast<unsigned long long>(span_max[4]),
                     span_over[4]);
    }

    void close() {
        if (g_file != nullptr) {
            g_on.store(false, std::memory_order_release);
            std::fclose(g_file);
            g_file = nullptr;
        }
    }

} // namespace oot::probe
