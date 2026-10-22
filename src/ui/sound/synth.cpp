// The synthesizer for our menu sounds. See ui_sound.h.
//
// Each cue is a few short notes, each a sine with a little of its octave for body, a three
// millisecond rise so it never clicks, and an exponential fall. Rising notes for going in,
// falling for going out, one soft tick for a move, a low double buzz for "not here". Every number
// below is a compiled constant: nothing reaches the synthesizer from outside.

#include "ui/sound/ui_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace oot::ui::sound {

    namespace {

        struct Note {
            float start;      // seconds from the cue's start
            float length;     // seconds
            float pitch;      // hertz
            float octave;     // how much of the octave above is mixed in, 0 to 1
            float square;     // 0 a sine, 1 a soft square (the refusal's buzz)
        };

        struct Shape {
            float level;      // the cue's peak at a volume of 1
            std::vector<Note> notes;
        };

        const Shape& shape(Cue cue) {
            static const Shape SHAPES[CUE_COUNT] = {
                // Move: one soft, high tick.
                { 0.16f, { { 0.000f, 0.040f, 1568.0f, 0.15f, 0.0f } } },
                // Select: two notes rising a fifth.
                { 0.30f, { { 0.000f, 0.060f, 880.0f, 0.25f, 0.0f }, { 0.045f, 0.090f, 1318.5f, 0.25f, 0.0f } } },
                // Back: the same two falling.
                { 0.26f, { { 0.000f, 0.060f, 1318.5f, 0.20f, 0.0f }, { 0.045f, 0.090f, 880.0f, 0.20f, 0.0f } } },
                // Toggle: one bright note with its octave.
                { 0.24f, { { 0.000f, 0.070f, 1174.7f, 0.45f, 0.0f } } },
                // Refuse: a low double buzz.
                { 0.22f, { { 0.000f, 0.060f, 196.0f, 0.0f, 1.0f }, { 0.080f, 0.070f, 185.0f, 0.0f, 1.0f } } },
                // Open: three notes climbing.
                { 0.28f, { { 0.000f, 0.070f, 659.3f, 0.2f, 0.0f }, { 0.050f, 0.070f, 987.8f, 0.2f, 0.0f },
                           { 0.100f, 0.120f, 1318.5f, 0.2f, 0.0f } } },
                // Close: the three falling.
                { 0.26f, { { 0.000f, 0.070f, 1318.5f, 0.2f, 0.0f }, { 0.050f, 0.070f, 987.8f, 0.2f, 0.0f },
                           { 0.100f, 0.120f, 659.3f, 0.2f, 0.0f } } },
            };
            const int i = std::clamp(static_cast<int>(cue), 0, CUE_COUNT - 1);
            return SHAPES[i];
        }

        constexpr float TWO_PI = 6.28318530718f;
        constexpr float RISE = 0.003f;

    }

    std::vector<float> render(Cue cue, int rate, float volume) {
        std::vector<float> out;
        if (rate <= 0 || !(volume > 0.0f)) {
            return out;
        }
        volume = std::min(volume, 1.0f);
        const Shape& s = shape(cue);
        float seconds = 0.0f;
        for (const Note& n : s.notes) {
            seconds = std::max(seconds, n.start + n.length);
        }
        seconds = std::min(seconds, MAX_SECONDS);
        out.assign(static_cast<size_t>(seconds * static_cast<float>(rate)), 0.0f);
        for (const Note& n : s.notes) {
            const size_t first = static_cast<size_t>(n.start * static_cast<float>(rate));
            const size_t count = static_cast<size_t>(n.length * static_cast<float>(rate));
            for (size_t k = 0; k < count && first + k < out.size(); ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(rate);
                const float rise = std::min(1.0f, t / RISE);
                // Falls to about a fiftieth by the note's end, so it ends without a click.
                const float fall = std::exp(-4.0f * t / n.length);
                const float phase = TWO_PI * n.pitch * t;
                float wave = std::sin(phase) + n.octave * std::sin(2.0f * phase);
                if (n.square > 0.0f) {
                    // A soft square: the sine pushed toward its sign.
                    wave = (1.0f - n.square) * wave + n.square * std::tanh(4.0f * std::sin(phase));
                }
                out[first + k] += wave / (1.0f + n.octave) * rise * fall;
            }
        }
        // Normalized to the cue's level, so overlapping notes never exceed it.
        float peak = 0.0f;
        for (float v : out) {
            peak = std::max(peak, std::fabs(v));
        }
        if (peak > 0.0f) {
            const float scale = s.level * volume / peak;
            for (float& v : out) {
                v *= scale;
            }
        }
        return out;
    }

    bool selftest(int& passed) {
        bool ok = true;
        const auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  sound: %s\n", what); ok = false; }
        };
        const auto peak_of = [](const std::vector<float>& v) {
            float p = 0.0f;
            for (float x : v) p = std::max(p, std::fabs(x));
            return p;
        };
        bool bounded = true;
        bool audible = true;
        bool quiet_ends = true;
        bool scales = true;
        for (int i = 0; i < CUE_COUNT; ++i) {
            const Cue cue = static_cast<Cue>(i);
            const std::vector<float> full = render(cue, RATE, 1.0f);
            const float seconds = static_cast<float>(full.size()) / static_cast<float>(RATE);
            bounded = bounded && seconds > 0.02f && seconds <= MAX_SECONDS && peak_of(full) <= 0.5f;
            audible = audible && peak_of(full) >= 0.1f;
            quiet_ends = quiet_ends && !full.empty() && std::fabs(full.back()) < 0.02f;
            const std::vector<float> half = render(cue, RATE, 0.5f);
            scales = scales && std::fabs(peak_of(half) - 0.5f * peak_of(full)) < 0.001f;
        }
        check(bounded, "every sound lasts between 20 ms and a quarter second and peaks at most at half scale");
        check(audible, "every sound is loud enough to hear at full volume");
        check(quiet_ends, "every sound ends near silence, so it never clicks");
        check(scales, "the volume scales the peak");
        check(render(Cue::Select, RATE, 0.0f).empty() && render(Cue::Move, 0, 1.0f).empty(),
              "a volume of 0 is silence");
        check(render(Cue::Open, RATE, 1.0f) != render(Cue::Close, RATE, 1.0f), "opening and closing are different sounds");
        return ok;
    }

}
