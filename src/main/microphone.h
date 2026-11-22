#pragma once

#include <string>

// THE MICROPHONE IN A VIDEO (the user, 2026-10-09: the recording request's "in-game sound and
// overlay talking", then "did you not add the voice recording and device detection/selection for
// selecting the mic to use?" and "the recording doesn't need deferment as it can still be used with
// the current video recording implementation without needing external connections").
//
// The system's capture devices, listed for the Microphone device row, and the chosen one opened
// ONLY while a video is recording with the Microphone row On, its sound handed to the video's
// encoder (main/video.h, submit_mic) to be mixed with the game's. Nothing is kept anywhere else,
// nothing is sent anywhere, and the device is closed when the video stops: a person's microphone is
// never open while they are only playing. .scaffold/security/app-layer.md has the posture.
namespace oot::microphone {

    // Read the system's capture devices again. The settings screen calls this as it draws the
    // device row, so a microphone plugged in since shows up.
    void refresh();

    // The device row's choices: 0 the system's default, then each device by its own name.
    int option_count();
    const char* label_for(int index);

    // The name to open for a row value: empty for the system's default, or a device that has gone.
    std::string device_name(int index);

    // Open the device by name (empty: the system's default) at 48 kHz stereo, its sound scaled by
    // `gain` and handed to the video's encoder; and close it. From the encoder's thread.
    bool open(const std::string& name, float gain);
    void close();

} // namespace oot::microphone
