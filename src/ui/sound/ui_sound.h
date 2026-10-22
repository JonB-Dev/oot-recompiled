#pragma once

// THE MENU'S SOUNDS (the menus plan, phases 111 and 112). Our own small set, synthesized here at
// run time from compiled constants and played on a playback device of our own: no files, no
// decoder, nothing of the game's. In the game the menus ask the game to play its own system sounds
// instead (phase 112), and our set plays wherever the game cannot.
//
// The rows: Menu sounds (Off, On) and Menu sound volume, scaled by the program's Volume as well,
// so a person who turned the sound down hears the menus quietly too. A device that will not open
// leaves the menus silent with one log line and never blocks.
//
// render() and selftest() are pure (synth.cpp), so ui_tests checks each sound's length and peak
// without a device; the device and play() are ui_sound.cpp.

#include <vector>

namespace oot::ui::sound {

    enum class Cue { Move, Select, Back, Toggle, Refuse, Open, Close };
    constexpr int CUE_COUNT = 7;

    // The sample rate the set is rendered and played at.
    constexpr int RATE = 48000;

    // A cue rendered as mono samples at `rate`, its peak `volume` times the cue's own level (the
    // loudest cue at a volume of 1 peaks at half of full scale). Empty at a volume of 0.
    std::vector<float> render(Cue cue, int rate, float volume);
    // The longest any cue may last, for the tests and for the device's voice list.
    constexpr float MAX_SECONDS = 0.25f;

    // Opens our device; once, from the UI's start. A failure logs once and leaves play() silent.
    void start();
    void stop();
    // Plays a cue at the rows' settings: nothing when Menu sounds is Off or the volume is 0. While
    // the game is taking them (phase 112, game_queue.h), the cue goes to the game instead.
    void play(Cue cue);

    bool selftest(int& passed);

}
