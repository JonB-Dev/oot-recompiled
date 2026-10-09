// The video recorder. See video.h.
#include "main/video.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <limits>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "game/moments.h"
#include "main/microphone.h"
#include "main/places.h"
#include "ui/ui_settings.h"

using Microsoft::WRL::ComPtr;

namespace oot::video {

    namespace {

        constexpr uint32_t FPS = 60;
        constexpr uint32_t AUDIO_RATE = 48000;
        constexpr int64_t AUDIO_PER_FRAME = AUDIO_RATE / FPS;
        // Media Foundation counts time in hundreds of nanoseconds.
        constexpr int64_t TICKS_PER_SECOND = 10'000'000;

        // The Video size row's heights after "Match the window", which is 0 (ui_settings.cpp holds
        // the labels; this is the arithmetic only).
        constexpr uint32_t HEIGHTS[] = { 0, 720, 1080, 1440, 2160 };
        // The Video quality row as bits per pixel per frame, which makes the bit rate follow the
        // size: at 1080p sixty, 6, 12, 20 and 37 megabits a second. Standard is about what the
        // video sites ask for; Best is several times it, for a file that is edited again.
        constexpr double BITS_PER_PIXEL[] = { 0.05, 0.1, 0.16, 0.3 };
        constexpr uint32_t MIN_BITRATE = 1'000'000;
        constexpr uint32_t MAX_BITRATE = 200'000'000;
        // H.264's ceiling for the encoders Windows carries: 4096 across, 2304 down.
        constexpr uint32_t MAX_WIDTH = 4096;
        constexpr uint32_t MAX_HEIGHT = 2304;
        // A gap in the pictures is filled by repeating the last one, up to ten seconds of it. Past
        // that (the window minimized, say) the file simply holds the last picture longer.
        constexpr int64_t MAX_REPEATS = 600;

        // THE SOUND IS KEPT IN STEP WITH THE PICTURE BY THE SAME CLOCK. Each piece of the game's
        // sound is stamped with the clock's place when it arrives. A piece that arrives later than
        // the sound written so far by more than AUDIO_GAP (the game's sound stalled, a load) is
        // preceded by silence; one that arrives AUDIO_AHEAD early is dropped. And the picture
        // never runs more than AUDIO_SLACK ahead of the sound: the file's writer interleaves the
        // two and would wait on a sound that is not coming, so silence fills it.
        constexpr int64_t AUDIO_GAP = AUDIO_RATE * 6 / 100;
        constexpr int64_t AUDIO_AHEAD = AUDIO_RATE / 4;
        constexpr int64_t AUDIO_SLACK = AUDIO_RATE / 5;
        constexpr int64_t SILENCE_PIECE = AUDIO_RATE / 10;

        using Clock = std::chrono::steady_clock;

        struct Params {
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t bitrate = 0;
            bool sound = true;
            // The microphone (2026-10-09, main/microphone.h): mixed with the game's sound.
            bool mic = false;
            std::string mic_name;
            float mic_gain = 1.0f;
            // The Video mode row: written in pieces and made ordinary at the stop, or ordinary
            // from the start.
            bool reliable = true;
            // The Video shows row: the whole window rather than the game alone.
            bool whole_window = false;
            std::filesystem::path file;
        };

        enum class JobKind { Picture, Audio, Stop };

        struct Job {
            JobKind kind = JobKind::Stop;
            const uint8_t* bgra = nullptr;
            uint32_t pitch = 0;
            uint32_t source_width = 0;
            uint32_t source_height = 0;
            uint64_t index = 0;
            std::function<void()> done;
            std::vector<int16_t> audio;   // 48 kHz, stereo, interleaved
            int lane = 0;                 // 0 the game's sound, 1 the microphone's
            int64_t arrival = 0;          // the clock's place when it arrived, in 48 kHz frames
        };

        std::mutex g_mutex;
        std::condition_variable g_wake;
        std::deque<Job> g_jobs;
        State g_state = State::Idle;
        bool g_failed = false;
        bool g_accepting = false;
        Params g_params;
        std::thread g_thread;
        Clock::time_point g_start;
        Clock::time_point g_paused_at;
        Clock::duration g_paused_total{};
        uint64_t g_next_index = 0;
        // The resampler, on the game's audio thread, under the mutex.
        double g_phase = 0.0;
        int16_t g_prev[2] = { 0, 0 };

        std::atomic<bool> g_pictures_wanted{ false };
        std::atomic<bool> g_audio_wanted{ false };

        // How far into the recording, pauses left out. Under the mutex.
        Clock::duration media_time_locked(Clock::time_point now) {
            const Clock::time_point until = (g_state == State::Paused) ? g_paused_at : now;
            return (until - g_start) - g_paused_total;
        }

        int64_t nanoseconds(Clock::duration d) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        }

        // The recording's size: the Video size row's height at the window's shape, or the window's
        // own size, even in both directions (the picture's color is shared by pairs of pixels)
        // and inside the encoders' ceiling.
        void recording_size(uint32_t window_width, uint32_t window_height, uint32_t& width, uint32_t& height) {
            const int step = std::clamp(oot::ui::settings().video_size, 0, static_cast<int>(std::size(HEIGHTS)) - 1);
            const double aspect = (window_height > 0) ? static_cast<double>(window_width) / static_cast<double>(window_height) : 16.0 / 9.0;
            double h = (step == 0) ? static_cast<double>(window_height) : static_cast<double>(HEIGHTS[step]);
            double w = h * aspect;
            if (w > MAX_WIDTH) {
                w = MAX_WIDTH;
                h = w / aspect;
            }
            if (h > MAX_HEIGHT) {
                h = MAX_HEIGHT;
                w = h * aspect;
            }
            width = std::max<uint32_t>(64u, static_cast<uint32_t>(std::lround(w)) & ~1u);
            height = std::max<uint32_t>(64u, static_cast<uint32_t>(std::lround(h)) & ~1u);
        }

        uint32_t bitrate_for(uint32_t width, uint32_t height) {
            const int step = std::clamp(oot::ui::settings().video_quality, 0, static_cast<int>(std::size(BITS_PER_PIXEL)) - 1);
            const double bits = BITS_PER_PIXEL[step] * static_cast<double>(width) * static_cast<double>(height) * FPS;
            return static_cast<uint32_t>(std::clamp(bits, static_cast<double>(MIN_BITRATE), static_cast<double>(MAX_BITRATE)));
        }

        // The file, named by the moment the recording starts, in the person's Videos folder.
        std::filesystem::path make_file() {
            const std::filesystem::path folder = oot::places::videos();
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            if (ec) {
                std::fprintf(stderr, "[video] the folder %s could not be made\n", folder.string().c_str());
                return {};
            }
            const std::time_t t = std::time(nullptr);
            std::tm now{};
            localtime_s(&now, &t);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H-%M-%S", &now);
            std::filesystem::path file = folder / (std::string(stamp) + "-oot.mp4");
            for (int n = 2; std::filesystem::exists(file, ec); ++n) {
                file = folder / (std::string(stamp) + "-" + std::to_string(n) + "-oot.mp4");
            }
            return file;
        }

        // ------------------------------------------------------------------------------------------
        // The picture's color, from B, G, R to the encoders' NV12
        // ------------------------------------------------------------------------------------------

        // A few threads sharing one picture's rows. At 4K sixty a second the conversion is half a
        // billion pixels a second, which one thread cannot keep up with.
        class Rows {
        public:
            Rows() {
                const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
                const unsigned helpers = std::clamp(cores / 2u, 1u, 8u) - 1u;
                for (unsigned i = 0; i < helpers; ++i) {
                    threads_.emplace_back([this] { serve(); });
                }
            }

            ~Rows() {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    quit_ = true;
                }
                start_.notify_all();
                for (std::thread& t : threads_) {
                    t.join();
                }
            }

            Rows(const Rows&) = delete;
            Rows& operator=(const Rows&) = delete;

            // Calls job(first, end) over [0, count) in as many parts as there are threads, this
            // one included, and returns when every part is done.
            void run(uint32_t count, const std::function<void(uint32_t, uint32_t)>& job) {
                const uint32_t parts = static_cast<uint32_t>(threads_.size()) + 1u;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    job_ = &job;
                    count_ = count;
                    parts_ = parts;
                    next_part_ = 1;   // part 0 is this thread's
                    remaining_ = parts - 1u;
                    ++generation_;
                }
                start_.notify_all();
                job(0, count / parts);
                std::unique_lock<std::mutex> lock(mutex_);
                done_.wait(lock, [this] { return remaining_ == 0; });
                job_ = nullptr;
            }

        private:
            void serve() {
                uint64_t seen = 0;
                for (;;) {
                    uint32_t part = 0;
                    const std::function<void(uint32_t, uint32_t)>* job = nullptr;
                    uint32_t count = 0;
                    uint32_t parts = 0;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        start_.wait(lock, [&] { return quit_ || (generation_ != seen && next_part_ < parts_); });
                        if (quit_) {
                            return;
                        }
                        part = next_part_++;
                        if (next_part_ >= parts_) {
                            seen = generation_;
                        }
                        job = job_;
                        count = count_;
                        parts = parts_;
                    }
                    (*job)((count * part) / parts, (count * (part + 1u)) / parts);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        --remaining_;
                    }
                    done_.notify_all();
                }
            }

            std::vector<std::thread> threads_;
            std::mutex mutex_;
            std::condition_variable start_;
            std::condition_variable done_;
            const std::function<void(uint32_t, uint32_t)>* job_ = nullptr;
            uint32_t count_ = 0;
            uint32_t parts_ = 0;
            uint32_t next_part_ = 0;
            uint32_t remaining_ = 0;
            uint64_t generation_ = 0;
            bool quit_ = false;
        };

        // BT.709 at the video range (16 to 235, color 16 to 240), which is what the file says it
        // holds and what every player and video site assumes of high definition video. Integer
        // weights out of 256; the color of each pair of rows and columns is the mean of the four.
        inline uint8_t luma(int r, int g, int b) {
            return static_cast<uint8_t>(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
        }

        void convert_pairs(const uint8_t* bgra, uint32_t pitch, uint32_t width, uint8_t* y_plane, uint8_t* uv_plane,
                           uint32_t first, uint32_t end) {
            for (uint32_t pair = first; pair < end; ++pair) {
                const uint8_t* a = bgra + static_cast<size_t>(pair) * 2u * pitch;
                const uint8_t* b = a + pitch;
                uint8_t* ya = y_plane + static_cast<size_t>(pair) * 2u * width;
                uint8_t* yb = ya + width;
                uint8_t* uv = uv_plane + static_cast<size_t>(pair) * width;
                for (uint32_t x = 0; x < width; x += 2) {
                    const uint8_t* p0 = a + x * 4u;
                    const uint8_t* p1 = p0 + 4;
                    const uint8_t* p2 = b + x * 4u;
                    const uint8_t* p3 = p2 + 4;
                    ya[x] = luma(p0[2], p0[1], p0[0]);
                    ya[x + 1] = luma(p1[2], p1[1], p1[0]);
                    yb[x] = luma(p2[2], p2[1], p2[0]);
                    yb[x + 1] = luma(p3[2], p3[1], p3[0]);
                    const int r = p0[2] + p1[2] + p2[2] + p3[2];
                    const int g = p0[1] + p1[1] + p2[1] + p3[1];
                    const int bl = p0[0] + p1[0] + p2[0] + p3[0];
                    uv[x] = static_cast<uint8_t>(((-26 * r - 86 * g + 112 * bl + 512) >> 10) + 128);
                    uv[x + 1] = static_cast<uint8_t>(((112 * r - 102 * g - 10 * bl + 512) >> 10) + 128);
                }
            }
        }

        // THE WINDOW TO THE RECORDING'S SIZE (Video shows, the whole window), by straight lines
        // between the four nearest pixels. Each output column's two source columns and the share
        // between them are worked out once a picture, not once a pixel.
        struct Scale {
            uint32_t source_width = 0;
            uint32_t source_height = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint32_t> left;     // the source column to the left of each output column
            std::vector<uint32_t> right;    // and to its right
            std::vector<uint32_t> share;    // the right one's share, out of 256
        };

        Scale make_scale(uint32_t source_width, uint32_t source_height, uint32_t width, uint32_t height) {
            Scale s;
            s.source_width = std::max(source_width, 1u);
            s.source_height = std::max(source_height, 1u);
            s.width = width;
            s.height = height;
            s.left.resize(width);
            s.right.resize(width);
            s.share.resize(width);
            for (uint32_t x = 0; x < width; ++x) {
                const double at = std::max(0.0, (x + 0.5) * s.source_width / width - 0.5);
                const uint32_t left = std::min(static_cast<uint32_t>(at), s.source_width - 1u);
                s.left[x] = left;
                s.right[x] = std::min(left + 1u, s.source_width - 1u);
                s.share[x] = static_cast<uint32_t>((at - left) * 256.0);
            }
            return s;
        }

        // Output row `y` of the scaled picture, B, G, R, A, into `out`.
        void scale_row(const uint8_t* bgra, uint32_t pitch, const Scale& s, uint32_t y, uint8_t* out) {
            const double at = std::max(0.0, (y + 0.5) * s.source_height / s.height - 0.5);
            const uint32_t top = std::min(static_cast<uint32_t>(at), s.source_height - 1u);
            const uint32_t bottom = std::min(top + 1u, s.source_height - 1u);
            const uint32_t down = static_cast<uint32_t>((at - top) * 256.0);
            const uint8_t* a = bgra + static_cast<size_t>(top) * pitch;
            const uint8_t* b = bgra + static_cast<size_t>(bottom) * pitch;
            for (uint32_t x = 0; x < s.width; ++x) {
                const uint32_t l = s.left[x] * 4u;
                const uint32_t r = s.right[x] * 4u;
                const uint32_t across = s.share[x];
                for (uint32_t c = 0; c < 3; ++c) {
                    const uint32_t upper = a[l + c] * (256u - across) + a[r + c] * across;
                    const uint32_t lower = b[l + c] * (256u - across) + b[r + c] * across;
                    out[x * 4u + c] = static_cast<uint8_t>((upper * (256u - down) + lower * down + 32768u) >> 16);
                }
                out[x * 4u + 3u] = 255;
            }
        }

        // ------------------------------------------------------------------------------------------
        // The file
        // ------------------------------------------------------------------------------------------

        bool failed(HRESULT hr, const char* what, std::string& why) {
            if (SUCCEEDED(hr)) {
                return false;
            }
            char text[160];
            std::snprintf(text, sizeof(text), "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
            why = text;
            return true;
        }

        void set_video_color(IMFMediaType* type) {
            type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        }

        class Writer {
        public:
            bool open(const Params& p, std::string& why) {
                p_ = p;
                lanes_[0].on = p.sound;
                lanes_[1].on = p.mic;
                ComPtr<IMFAttributes> attributes;
                if (failed(MFCreateAttributes(&attributes, 2), "attributes", why)) {
                    return false;
                }
                // The graphics card's encoder where there is one, which is what keeps a 4K
                // recording from costing the game its frame rate.
                attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                // FRAGMENTED WHILE IT RECORDS (the user, 2026-10-09: "still play the file if it
                // crashes mid-recording"). An ordinary MP4 writes its index at the end, so a
                // recording that never reaches its stop does not play at all; a fragmented one
                // is complete every couple of seconds, and the stop turns it into an ordinary one
                // (finish_as_ordinary_mp4 below).
                // Compatible mode writes the ordinary MP4 directly, as the recorder first did.
                attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE,
                                    p.reliable ? MFTranscodeContainerType_FMPEG4 : MFTranscodeContainerType_MPEG4);
                if (failed(MFCreateSinkWriterFromURL(p.file.wstring().c_str(), nullptr, attributes.Get(), &writer_),
                           "the file could not be opened for writing", why)) {
                    return false;
                }

                // The picture: H.264, High profile, at the recording's size and sixty a second.
                ComPtr<IMFMediaType> out;
                MFCreateMediaType(&out);
                out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
                out->SetUINT32(MF_MT_AVG_BITRATE, p.bitrate);
                out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
                MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, p.width, p.height);
                MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, FPS, 1);
                MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                set_video_color(out.Get());
                if (failed(writer_->AddStream(out.Get(), &video_), "no H.264 encoder took the picture", why)) {
                    return false;
                }

                ComPtr<IMFMediaType> in;
                MFCreateMediaType(&in);
                in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                in->SetUINT32(MF_MT_DEFAULT_STRIDE, p.width);
                MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, p.width, p.height);
                MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, FPS, 1);
                MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                set_video_color(in.Get());

                // A key frame every two seconds, which is what the video sites ask for, and the
                // bit rate as an average the encoder may go above where the picture is busy. An
                // encoder that will not take these is given the picture with its own defaults.
                ComPtr<IMFAttributes> encoding;
                MFCreateAttributes(&encoding, 3);
                encoding->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
                encoding->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, p.bitrate);
                encoding->SetUINT32(CODECAPI_AVEncMPVGOPSize, FPS * 2);
                if (FAILED(writer_->SetInputMediaType(video_, in.Get(), encoding.Get())) &&
                    failed(writer_->SetInputMediaType(video_, in.Get(), nullptr), "the encoder would not take the picture", why)) {
                    return false;
                }

                // The sound: AAC at 48 kHz, 192 kilobits a second, from 16 bit stereo.
                if (p.sound || p.mic) {
                    ComPtr<IMFMediaType> aout;
                    MFCreateMediaType(&aout);
                    aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                    aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_RATE);
                    aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                    aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
                    if (failed(writer_->AddStream(aout.Get(), &audio_), "no AAC encoder took the sound", why)) {
                        return false;
                    }

                    ComPtr<IMFMediaType> ain;
                    MFCreateMediaType(&ain);
                    ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                    ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_RATE);
                    ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                    ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                    ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AUDIO_RATE * 4);
                    if (failed(writer_->SetInputMediaType(audio_, ain.Get(), nullptr), "the encoder would not take the sound", why)) {
                        return false;
                    }
                }

                return !failed(writer_->BeginWriting(), "the file could not begin", why);
            }

            // One picture into NV12, in a buffer of its own, `source_width` by `source_height`: the
            // recording's size when it is the game's picture drawn at that size, the window's when
            // it is the whole window (Video shows), which is then scaled to the recording's size
            // (the user, 2026-10-09: the size row must "still work regardless"). The caller's
            // memory is free when this returns.
            ComPtr<IMFMediaBuffer> convert(const uint8_t* bgra, uint32_t pitch, uint32_t source_width, uint32_t source_height) {
                const DWORD size = p_.width * p_.height * 3u / 2u;
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(MFCreateMemoryBuffer(size, &buffer))) {
                    return nullptr;
                }
                BYTE* bytes = nullptr;
                if (FAILED(buffer->Lock(&bytes, nullptr, nullptr))) {
                    return nullptr;
                }
                uint8_t* y_plane = bytes;
                uint8_t* uv_plane = bytes + static_cast<size_t>(p_.width) * p_.height;
                const uint32_t width = p_.width;
                if (source_width == p_.width && source_height == p_.height) {
                    rows_.run(p_.height / 2u, [&](uint32_t first, uint32_t end) {
                        convert_pairs(bgra, pitch, width, y_plane, uv_plane, first, end);
                    });
                }
                else {
                    const Scale scale = make_scale(source_width, source_height, p_.width, p_.height);
                    rows_.run(p_.height / 2u, [&](uint32_t first, uint32_t end) {
                        // Two rows at a time, each scaled into this part's own pair of rows,
                        // then converted as an unscaled pair would be.
                        std::vector<uint8_t> rows(static_cast<size_t>(width) * 8u);
                        for (uint32_t pair = first; pair < end; ++pair) {
                            scale_row(bgra, pitch, scale, pair * 2u, rows.data());
                            scale_row(bgra, pitch, scale, pair * 2u + 1u, rows.data() + static_cast<size_t>(width) * 4u);
                            convert_pairs(rows.data(), width * 4u, width, y_plane + static_cast<size_t>(pair) * 2u * width,
                                          uv_plane + static_cast<size_t>(pair) * width, 0, 1);
                        }
                    });
                }
                buffer->Unlock();
                buffer->SetCurrentLength(size);
                return buffer;
            }

            bool picture(IMFMediaBuffer* buffer, uint64_t index, std::string& why) {
                const int64_t at = static_cast<int64_t>(index);
                if (at < next_index_) {
                    return true;   // a picture for a place already written
                }
                // A gap is the last picture again, so the file stays sixty a second.
                if ((last_ != nullptr) && (at > next_index_)) {
                    const int64_t repeats = std::min(at - next_index_, MAX_REPEATS);
                    for (int64_t i = 0; i < repeats; ++i) {
                        if (!write_picture(last_.Get(), next_index_, why)) {
                            return false;
                        }
                        ++next_index_;
                    }
                }
                next_index_ = at;
                if (!write_picture(buffer, next_index_, why)) {
                    return false;
                }
                ++next_index_;
                last_ = buffer;
                return true;
            }

            // THE SOUND IS TWO LANES MIXED (2026-10-09): the game's and the microphone's. Each piece is
            // placed on its lane by when it arrived, by the rules above (more than AUDIO_GAP late is
            // preceded by silence, AUDIO_AHEAD early is dropped), and the mix is written as far as
            // every lane that is on has reached, or further where the picture or the stop needs
            // it, with silence for a lane that has not.
            bool sound(int lane, const std::vector<int16_t>& samples, int64_t arrival, std::string& why) {
                if (lane < 0 || lane >= LANES || !lanes_[lane].on || samples.empty()) {
                    return true;
                }
                Lane& l = lanes_[lane];
                if (l.cursor > arrival + AUDIO_AHEAD) {
                    return true;   // far ahead of the clock: dropped
                }
                if (l.cursor < arrival - AUDIO_GAP) {
                    l.cursor = arrival;
                }
                const int64_t frames = static_cast<int64_t>(samples.size() / 2);
                // Only what lands after the sound already written is kept.
                const int64_t skip = std::clamp<int64_t>(audio_written_ - l.cursor, 0, frames);
                const int64_t at = l.cursor + skip;
                if (l.data.empty()) {
                    l.start = at;
                }
                while (l.end() < at) {
                    l.data.push_back(0);
                    l.data.push_back(0);
                }
                l.data.insert(l.data.end(), samples.begin() + skip * 2, samples.end());
                l.cursor += frames;
                return mix_until(ready_until(), why);
            }

            // A lane that could not start (the microphone would not open) takes no part.
            void lane_off(int lane) {
                if (lane >= 0 && lane < LANES) {
                    lanes_[lane].on = false;
                    lanes_[lane].data.clear();
                }
            }

            bool finish(std::string& why) {
                if (has_sound() && !mix_until(next_index_ * AUDIO_PER_FRAME, why)) {
                    return false;
                }
                if (next_index_ == 0) {
                    why = "no picture reached the recorder";
                    return false;
                }
                return !failed(writer_->Finalize(), "the file could not be finished", why);
            }

            uint64_t pictures() const {
                return static_cast<uint64_t>(next_index_);
            }

        private:
            static int64_t ticks_at(int64_t count, int64_t rate) {
                return (count * TICKS_PER_SECOND) / rate;
            }

            bool write_picture(IMFMediaBuffer* buffer, int64_t index, std::string& why) {
                // The sound may not trail far behind, or the writer waits on it (above).
                if (has_sound() && !mix_until(index * AUDIO_PER_FRAME - AUDIO_SLACK, why)) {
                    return false;
                }
                ComPtr<IMFSample> sample;
                if (failed(MFCreateSample(&sample), "a picture sample", why)) {
                    return false;
                }
                sample->AddBuffer(buffer);
                const int64_t start = ticks_at(index, FPS);
                sample->SetSampleTime(start);
                sample->SetSampleDuration(ticks_at(index + 1, FPS) - start);
                return !failed(writer_->WriteSample(video_, sample.Get()), "a picture could not be written", why);
            }

            bool write_sound(const int16_t* samples, int64_t frames, std::string& why) {
                const DWORD bytes = static_cast<DWORD>(frames * 4);
                ComPtr<IMFMediaBuffer> buffer;
                if (failed(MFCreateMemoryBuffer(bytes, &buffer), "a sound buffer", why)) {
                    return false;
                }
                BYTE* data = nullptr;
                if (failed(buffer->Lock(&data, nullptr, nullptr), "a sound buffer", why)) {
                    return false;
                }
                if (samples != nullptr) {
                    std::memcpy(data, samples, bytes);
                }
                else {
                    std::memset(data, 0, bytes);
                }
                buffer->Unlock();
                buffer->SetCurrentLength(bytes);

                ComPtr<IMFSample> sample;
                if (failed(MFCreateSample(&sample), "a sound sample", why)) {
                    return false;
                }
                sample->AddBuffer(buffer.Get());
                const int64_t start = ticks_at(audio_written_, AUDIO_RATE);
                sample->SetSampleTime(start);
                sample->SetSampleDuration(ticks_at(audio_written_ + frames, AUDIO_RATE) - start);
                if (failed(writer_->WriteSample(audio_, sample.Get()), "the sound could not be written", why)) {
                    return false;
                }
                audio_written_ += frames;
                return true;
            }

            struct Lane {
                bool on = false;
                int64_t cursor = 0;          // where its next frame goes on the timeline
                int64_t start = 0;           // the timeline frame of data's first
                std::deque<int16_t> data;    // stereo, interleaved
                int64_t end() const {
                    return start + static_cast<int64_t>(data.size() / 2);
                }
            };
            static constexpr int LANES = 2;

            bool has_sound() const {
                return p_.sound || p_.mic;
            }

            // As far as every lane that is on has reached.
            int64_t ready_until() const {
                int64_t until = std::numeric_limits<int64_t>::max();
                bool any = false;
                for (const Lane& l : lanes_) {
                    if (l.on) {
                        until = std::min(until, l.end());
                        any = true;
                    }
                }
                return any ? until : audio_written_;
            }

            // The lanes summed, a piece at a time, up to `until`; a lane with nothing there is silent.
            bool mix_until(int64_t until, std::string& why) {
                std::vector<int16_t> block;
                while (audio_written_ < until) {
                    const int64_t frames = std::min(until - audio_written_, SILENCE_PIECE);
                    block.assign(static_cast<size_t>(frames) * 2, 0);
                    for (Lane& l : lanes_) {
                        if (!l.on) {
                            continue;
                        }
                        for (int64_t f = 0; f < frames; ++f) {
                            const int64_t at = audio_written_ + f;
                            if (at < l.start || at >= l.end()) {
                                continue;
                            }
                            const size_t from = static_cast<size_t>(at - l.start) * 2;
                            for (size_t c = 0; c < 2; ++c) {
                                const int32_t mixed = block[static_cast<size_t>(f) * 2 + c] + l.data[from + c];
                                block[static_cast<size_t>(f) * 2 + c] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
                            }
                        }
                    }
                    if (!write_sound(block.data(), frames, why)) {
                        return false;
                    }
                    for (Lane& l : lanes_) {
                        while (!l.data.empty() && l.start < audio_written_) {
                            l.data.pop_front();
                            l.data.pop_front();
                            ++l.start;
                        }
                        if (l.data.empty() && l.start < audio_written_) {
                            l.start = audio_written_;
                        }
                    }
                }
                return true;
            }

            Params p_;
            ComPtr<IMFSinkWriter> writer_;
            DWORD video_ = 0;
            DWORD audio_ = 0;
            ComPtr<IMFMediaBuffer> last_;
            int64_t next_index_ = 0;
            int64_t audio_written_ = 0;
            Lane lanes_[LANES];
            Rows rows_;
        };

        // THE STOP MAKES IT AN ORDINARY MP4 (2026-10-09). The fragmented file the recording wrote
        // is read back and every sample copied as it is into an ordinary MP4 beside it, no decoding
        // and no encoding, which then takes its name. Some editors are fussy about a fragmented
        // file and every one reads an ordinary one. If anything here fails the fragmented file is
        // left as it is: it plays, and it is the recording.
        bool finish_as_ordinary_mp4(const std::filesystem::path& file, std::string& why) {
            const std::filesystem::path temp = file.string() + ".finishing";
            bool ok = false;
            {
                ComPtr<IMFSourceReader> reader;
                if (failed(MFCreateSourceReaderFromURL(file.wstring().c_str(), nullptr, &reader), "the recording could not be read back", why)) {
                    return false;
                }
                ComPtr<IMFAttributes> attributes;
                MFCreateAttributes(&attributes, 1);
                attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
                ComPtr<IMFSinkWriter> writer;
                if (failed(MFCreateSinkWriterFromURL(temp.wstring().c_str(), nullptr, attributes.Get(), &writer),
                           "the ordinary MP4 could not be opened", why)) {
                    return false;
                }

                // Each stream as it was written: the compressed picture and the compressed sound.
                std::vector<DWORD> streams;
                for (DWORD s = 0;; ++s) {
                    ComPtr<IMFMediaType> type;
                    const HRESULT hr = reader->GetNativeMediaType(s, 0, &type);
                    if (hr == MF_E_INVALIDSTREAMNUMBER) {
                        break;
                    }
                    DWORD out = 0;
                    if (failed(hr, "a stream of the recording", why) ||
                        failed(reader->SetStreamSelection(s, TRUE), "a stream of the recording", why) ||
                        failed(reader->SetCurrentMediaType(s, nullptr, type.Get()), "a stream of the recording", why) ||
                        failed(writer->AddStream(type.Get(), &out), "a stream of the ordinary MP4", why) ||
                        failed(writer->SetInputMediaType(out, type.Get(), nullptr), "a stream of the ordinary MP4", why)) {
                        return false;
                    }
                    streams.push_back(out);
                }
                if (streams.empty()) {
                    why = "the recording holds no stream";
                    return false;
                }
                if (failed(writer->BeginWriting(), "the ordinary MP4 could not begin", why)) {
                    return false;
                }

                size_t ended = 0;
                while (ended < streams.size()) {
                    DWORD stream = 0;
                    DWORD flags = 0;
                    LONGLONG time = 0;
                    ComPtr<IMFSample> sample;
                    if (failed(reader->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &stream, &flags, &time, &sample),
                               "the recording could not be read back", why)) {
                        return false;
                    }
                    if ((flags & MF_SOURCE_READERF_ERROR) != 0 || stream >= streams.size()) {
                        why = "the recording could not be read back";
                        return false;
                    }
                    if (sample != nullptr &&
                        failed(writer->WriteSample(streams[stream], sample.Get()), "a sample could not be copied", why)) {
                        return false;
                    }
                    if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
                        writer->SendStreamTick(streams[stream], time);
                    }
                    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                        ++ended;
                    }
                }
                ok = !failed(writer->Finalize(), "the ordinary MP4 could not be finished", why);
            }

            std::error_code ec;
            if (ok) {
                std::filesystem::rename(temp, file, ec);
                if (ec) {
                    why = "the ordinary MP4 could not take the recording's name";
                    ok = false;
                }
            }
            if (!ok) {
                std::filesystem::remove(temp, ec);
            }
            return ok;
        }

        // Hands every picture still queued back to the renderer. Under no lock: `done` takes the
        // renderer's own.
        void hand_back(std::deque<Job>& jobs) {
            for (Job& job : jobs) {
                if (job.kind == JobKind::Picture && job.done) {
                    job.done();
                }
            }
            jobs.clear();
        }

        void stop_locked(const char* note) {
            g_state = State::Saving;
            g_accepting = false;
            g_pictures_wanted.store(false, std::memory_order_relaxed);
            g_audio_wanted.store(false, std::memory_order_relaxed);
            Job stop;
            stop.kind = JobKind::Stop;
            g_jobs.push_back(std::move(stop));
            g_wake.notify_one();
            if (note != nullptr) {
                oot::moments::notice(note);
            }
        }

        void encoder_main(Params p) {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
            std::string why = started ? "" : "Media Foundation would not start";
            bool ok = false;
            uint64_t pictures = 0;
            {
                Writer writer;
                ok = started && writer.open(p, why);
                if (!ok) {
                    std::fprintf(stderr, "[video] could not start: %s\n", why.c_str());
                }
                // The microphone is listened to for this recording only, and closed when it ends.
                if (ok && p.mic && !oot::microphone::open(p.mic_name, p.mic_gain)) {
                    writer.lane_off(1);
                    oot::moments::notice("The microphone could not be opened; the video has the game's sound alone");
                }

                for (;;) {
                    Job job;
                    {
                        std::unique_lock<std::mutex> lock(g_mutex);
                        g_wake.wait(lock, [] { return !g_jobs.empty(); });
                        job = std::move(g_jobs.front());
                        g_jobs.pop_front();
                    }
                    if (job.kind == JobKind::Stop) {
                        break;
                    }

                    if (job.kind == JobKind::Picture) {
                        ComPtr<IMFMediaBuffer> nv12;
                        if (ok) {
                            nv12 = writer.convert(job.bgra, job.pitch, job.source_width, job.source_height);
                        }
                        if (job.done) {
                            job.done();
                        }
                        if (ok) {
                            ok = (nv12 != nullptr) ? writer.picture(nv12.Get(), job.index, why) : false;
                            if (nv12 == nullptr) {
                                why = "a picture buffer could not be made";
                            }
                        }
                    }
                    else if (ok) {
                        ok = writer.sound(job.lane, job.audio, job.arrival, why);
                    }

                    // A failure ends the recording at once, rather than leaving a plate that says
                    // it is still going.
                    if (!ok) {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        if ((g_state == State::Recording) || (g_state == State::Paused)) {
                            std::fprintf(stderr, "[video] stopped: %s\n", why.c_str());
                            stop_locked(nullptr);
                        }
                    }
                }

                // Closed before the file is finished, so nothing more arrives for it. Never under the
                // lock: the capture thread takes it to hand its sound over.
                oot::microphone::close();
                if (ok) {
                    ok = writer.finish(why);
                }
                pictures = writer.pictures();
            }

            std::deque<Job> left;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                left.swap(g_jobs);
            }
            hand_back(left);

            // Whatever reached the file is kept: a recording that failed part way still plays up
            // to where it failed. Only an empty file goes.
            std::error_code ec;
            const bool has_content = std::filesystem::is_regular_file(p.file, ec) && (std::filesystem::file_size(p.file, ec) > 0);
            if (!has_content) {
                std::filesystem::remove(p.file, ec);
            }
            if (started && has_content && p.reliable) {
                std::string finish_why;
                const auto finish_start = Clock::now();
                if (finish_as_ordinary_mp4(p.file, finish_why)) {
                    std::fprintf(stderr, "[video] made an ordinary MP4 in %lld ms\n",
                                 static_cast<long long>(nanoseconds(Clock::now() - finish_start) / 1'000'000));
                }
                else {
                    std::fprintf(stderr, "[video] kept as it was recorded (still plays): %s\n", finish_why.c_str());
                }
            }

            if (started) {
                MFShutdown();
            }
            if (SUCCEEDED(com)) {
                CoUninitialize();
            }

            if (!ok) {
                std::fprintf(stderr, "[video] not finished cleanly: %s\n", why.c_str());
            }
            else {
                std::fprintf(stderr, "[video] saved %s: %llu pictures, %ux%u, %u bits a second\n", p.file.string().c_str(),
                             static_cast<unsigned long long>(pictures), p.width, p.height, p.bitrate);
            }

            std::lock_guard<std::mutex> lock(g_mutex);
            g_state = State::Idle;
            g_failed = !ok;
            oot::moments::notice(ok ? "Video saved to your Videos folder"
                                    : (has_content ? "The video stopped early; what was recorded is in your Videos folder"
                                                   : "The video could not be saved; the log says why"));
        }

    } // namespace

    Status status() {
        std::lock_guard<std::mutex> lock(g_mutex);
        Status s;
        s.state = g_state;
        s.failed = g_failed;
        if ((g_state == State::Recording) || (g_state == State::Paused)) {
            s.seconds = static_cast<double>(nanoseconds(media_time_locked(Clock::now()))) / 1e9;
        }
        return s;
    }

    void toggle_record() {
        std::unique_lock<std::mutex> lock(g_mutex);
        switch (g_state) {
            case State::Idle:
                // The last recording's thread has finished (it went Idle on its way out) and is
                // let go here, so the next one can start.
                if (g_thread.joinable()) {
                    lock.unlock();
                    g_thread.join();
                    lock.lock();
                    if (g_state != State::Idle) {
                        return;
                    }
                }
                g_state = State::Starting;
                g_failed = false;
                g_pictures_wanted.store(true, std::memory_order_relaxed);
                std::fprintf(stderr, "[video] asked to start; begins at the next presented frame\n");
                break;
            case State::Starting:
                g_state = State::Idle;
                g_pictures_wanted.store(false, std::memory_order_relaxed);
                break;
            case State::Recording:
            case State::Paused:
                stop_locked("Saving the video");
                break;
            case State::Saving:
                std::fprintf(stderr, "[video] still saving the last recording; ignored\n");
                break;
        }
    }

    void toggle_pause() {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Clock::time_point now = Clock::now();
        if (g_state == State::Recording) {
            g_state = State::Paused;
            g_paused_at = now;
            g_pictures_wanted.store(false, std::memory_order_relaxed);
            g_audio_wanted.store(false, std::memory_order_relaxed);
            oot::moments::notice("Video paused");
        }
        else if (g_state == State::Paused) {
            g_paused_total += now - g_paused_at;
            g_state = State::Recording;
            g_pictures_wanted.store(true, std::memory_order_relaxed);
            g_audio_wanted.store(g_params.sound, std::memory_order_relaxed);
            oot::moments::notice("Video recording");
        }
    }

    bool wants_pictures() {
        return g_pictures_wanted.load(std::memory_order_relaxed);
    }

    bool records_window() {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Before the first picture the row decides; from it on, the recording's own choice.
        return (g_state == State::Starting) ? (oot::ui::settings().video_source != 0) : g_params.whole_window;
    }

    bool wants_picture(uint32_t window_width, uint32_t window_height, uint32_t& width, uint32_t& height, uint64_t& index) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Clock::time_point now = Clock::now();
        if (g_state == State::Starting) {
            Params p;
            recording_size(window_width, window_height, p.width, p.height);
            p.bitrate = bitrate_for(p.width, p.height);
            p.sound = (oot::ui::settings().video_sound == 0);
            p.reliable = (oot::ui::settings().video_mode == 0);
            p.mic = (oot::ui::settings().microphone != 0);
            p.mic_name = oot::microphone::device_name(oot::ui::settings().mic_device);
            p.mic_gain = oot::ui::mic_gain();
            p.whole_window = (oot::ui::settings().video_source != 0);
            p.file = make_file();
            if (p.file.empty()) {
                g_state = State::Idle;
                g_failed = true;
                g_pictures_wanted.store(false, std::memory_order_relaxed);
                oot::moments::notice("The video could not be started; the log says why");
                return false;
            }
            if (g_thread.joinable()) {
                g_thread.join();   // a thread that went Idle has only to return
            }
            g_params = p;
            g_jobs.clear();
            g_start = now;
            g_paused_total = Clock::duration{};
            g_next_index = 0;
            g_phase = 0.0;
            g_prev[0] = 0;
            g_prev[1] = 0;
            g_accepting = true;
            g_state = State::Recording;
            g_audio_wanted.store(p.sound, std::memory_order_relaxed);
            g_thread = std::thread(encoder_main, p);
            std::fprintf(stderr, "[video] recording %s (%s) at %ux%u, sixty a second, %u bits a second, sound %s\n",
                         p.file.string().c_str(), p.reliable ? "reliable" : "compatible", p.width, p.height, p.bitrate,
                         p.sound ? "on" : "off");
            oot::moments::notice("Recording video");
        }
        if (g_state != State::Recording) {
            return false;
        }
        const int64_t ns = nanoseconds(media_time_locked(now));
        const uint64_t place = static_cast<uint64_t>((ns * FPS + 500'000'000) / 1'000'000'000);
        if (place < g_next_index) {
            return false;
        }
        index = place;
        g_next_index = place + 1;
        width = g_params.width;
        height = g_params.height;
        return true;
    }

    void submit_picture(const uint8_t* bgra, uint32_t pitch, uint32_t width, uint32_t height, uint64_t index,
                        std::function<void()> done) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_accepting) {
                Job job;
                job.kind = JobKind::Picture;
                job.bgra = bgra;
                job.pitch = pitch;
                job.source_width = width;
                job.source_height = height;
                job.index = index;
                job.done = std::move(done);
                g_jobs.push_back(std::move(job));
                g_wake.notify_one();
                return;
            }
        }
        if (done) {
            done();
        }
    }

    bool wants_audio() {
        return g_audio_wanted.load(std::memory_order_relaxed);
    }

    void submit_audio(const int16_t* stereo, std::size_t frames, uint32_t rate) {
        if (!wants_audio() || (stereo == nullptr) || (frames == 0) || (rate == 0)) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if ((g_state != State::Recording) || !g_accepting || !g_params.sound) {
            return;
        }

        // To 48 kHz, by a straight line between neighbors. The piece before ends where this one
        // starts: y[0] is the last frame of the last piece, y[1..frames] are this one's, and the
        // phase is where the next output frame lies past y[0].
        const double step = static_cast<double>(rate) / AUDIO_RATE;
        const double end = static_cast<double>(frames);
        Job job;
        job.kind = JobKind::Audio;
        job.arrival = (nanoseconds(media_time_locked(Clock::now())) * AUDIO_RATE) / 1'000'000'000;
        job.audio.reserve((static_cast<size_t>(end / step) + 4u) * 2u);
        auto sample = [&](size_t i, int channel) -> double {
            return static_cast<double>((i == 0) ? g_prev[channel] : stereo[(i - 1) * 2 + static_cast<size_t>(channel)]);
        };
        double position = g_phase;
        while (position < end) {
            const size_t i = static_cast<size_t>(position);
            const double share = position - static_cast<double>(i);
            for (int channel = 0; channel < 2; ++channel) {
                const double a = sample(i, channel);
                const double b = sample(i + 1, channel);
                job.audio.push_back(static_cast<int16_t>(std::lround(a + (b - a) * share)));
            }
            position += step;
        }
        g_phase = position - end;
        g_prev[0] = stereo[(frames - 1) * 2];
        g_prev[1] = stereo[(frames - 1) * 2 + 1];
        if (!job.audio.empty()) {
            g_jobs.push_back(std::move(job));
            g_wake.notify_one();
        }
    }

    namespace {
        // The command line's schedule: start, then pause and resume if given, then stop.
        uint32_t g_schedule[4] = {};
        int g_schedule_count = 0;
        int g_schedule_next = 0;
    }

    void request_at_play_ticks(const uint32_t* ticks, int count) {
        g_schedule_count = (count == 2 || count == 4) ? count : 0;
        for (int i = 0; i < g_schedule_count; ++i) {
            g_schedule[i] = ticks[i];
        }
        g_schedule_next = 0;
        if (g_schedule_count == 0) {
            std::fprintf(stderr, "--video needs <start>,<stop> or <start>,<pause>,<resume>,<stop>.\n");
        }
    }

    void tick(uint32_t play_ticks) {
        if (g_schedule_next >= g_schedule_count || play_ticks < g_schedule[g_schedule_next]) {
            return;
        }
        const bool pause_step = (g_schedule_count == 4) && (g_schedule_next == 1 || g_schedule_next == 2);
        std::fprintf(stderr, "[video] the command line's step %d at play tick %u\n", g_schedule_next, play_ticks);
        if (pause_step) {
            toggle_pause();
        }
        else {
            toggle_record();
        }
        ++g_schedule_next;
    }

    void submit_mic(const int16_t* stereo, std::size_t frames) {
        if ((stereo == nullptr) || (frames == 0)) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if ((g_state != State::Recording) || !g_accepting || !g_params.mic) {
            return;
        }
        Job job;
        job.kind = JobKind::Audio;
        job.lane = 1;
        job.arrival = (nanoseconds(media_time_locked(Clock::now())) * AUDIO_RATE) / 1'000'000'000;
        job.audio.assign(stereo, stereo + frames * 2);
        g_jobs.push_back(std::move(job));
        g_wake.notify_one();
    }

    void renderer_gone() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state == State::Starting) {
            g_state = State::Idle;
            g_pictures_wanted.store(false, std::memory_order_relaxed);
        }
        else if ((g_state == State::Recording) || (g_state == State::Paused)) {
            stop_locked("Saving the video");
        }
    }

    void shutdown() {
        renderer_gone();
        if (g_thread.joinable()) {
            g_thread.join();
        }
    }

} // namespace oot::video
