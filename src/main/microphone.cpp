// The microphone in a video. See microphone.h.
#include "main/microphone.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#include "main/video.h"

namespace oot::microphone {

    namespace {
        std::mutex g_list_mutex;
        std::vector<std::string> g_labels = { "System default" };
        std::atomic<bool> g_listed{ false };

        // The first ask reads the list, so the launcher's settings have it too.
        void list_once() {
            if (!g_listed.load(std::memory_order_acquire)) {
                refresh();
            }
        }

        SDL_AudioDeviceID g_device = 0;
        std::atomic<float> g_gain{ 1.0f };

        // SDL's capture thread: the sound as it arrives, scaled, to the video.
        void SDLCALL arrived(void* /*user*/, Uint8* stream, int length) {
            const size_t frames = static_cast<size_t>(length) / (sizeof(int16_t) * 2);
            if (frames == 0) {
                return;
            }
            int16_t* samples = reinterpret_cast<int16_t*>(stream);
            const float gain = g_gain.load(std::memory_order_relaxed);
            if (gain < 0.999f || gain > 1.001f) {
                for (size_t i = 0; i < frames * 2; ++i) {
                    const float scaled = static_cast<float>(samples[i]) * gain;
                    samples[i] = static_cast<int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
                }
            }
            oot::video::submit_mic(samples, frames);
        }
    }

    void refresh() {
        // Before the system's audio is up there is no list to read, and it is read again later.
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
            return;
        }
        std::vector<std::string> labels = { "System default" };
        const int count = SDL_GetNumAudioDevices(1);
        for (int i = 0; i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(i, 1);
            if (name != nullptr && name[0] != 0) {
                labels.emplace_back(name);
            }
        }
        std::lock_guard<std::mutex> lock(g_list_mutex);
        g_labels = std::move(labels);
        g_listed.store(true, std::memory_order_release);
    }

    int option_count() {
        list_once();
        std::lock_guard<std::mutex> lock(g_list_mutex);
        return static_cast<int>(g_labels.size());
    }

    const char* label_for(int index) {
        list_once();
        std::lock_guard<std::mutex> lock(g_list_mutex);
        if (index < 0 || index >= static_cast<int>(g_labels.size())) {
            return "System default";
        }
        return g_labels[static_cast<size_t>(index)].c_str();
    }

    std::string device_name(int index) {
        std::lock_guard<std::mutex> lock(g_list_mutex);
        if (index <= 0 || index >= static_cast<int>(g_labels.size())) {
            return {};
        }
        return g_labels[static_cast<size_t>(index)];
    }

    bool open(const std::string& name, float gain) {
        close();
        g_gain.store(gain, std::memory_order_relaxed);
        SDL_AudioSpec want{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = arrived;
        SDL_AudioSpec have{};
        // No changes allowed: SDL converts whatever the device gives to 48 kHz 16 bit stereo, which
        // is what the video's sound is.
        g_device = SDL_OpenAudioDevice(name.empty() ? nullptr : name.c_str(), 1, &want, &have, 0);
        if (g_device == 0) {
            std::fprintf(stderr, "[microphone] could not open %s: %s\n", name.empty() ? "the system default" : name.c_str(),
                         SDL_GetError());
            return false;
        }
        SDL_PauseAudioDevice(g_device, 0);
        std::fprintf(stderr, "[microphone] listening to %s for the video\n", name.empty() ? "the system default" : name.c_str());
        return true;
    }

    void close() {
        if (g_device != 0) {
            SDL_CloseAudioDevice(g_device);
            g_device = 0;
            std::fprintf(stderr, "[microphone] closed\n");
        }
    }

} // namespace oot::microphone
