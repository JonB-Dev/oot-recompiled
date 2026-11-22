#pragma once

// GYRO AIMING (a player's suggestion, 2026-11-22: "gyroscope-based aiming with compatible
// controllers"). A pad with a gyroscope and an accelerometer has both switched on when it is opened;
// every sensor reading is calibrated and turned into player space yaw and pitch by
// GamepadMotionHelpers (lib/), and the turn is summed until the game takes it. The patched aim
// (patches/gyro_aim.c) takes it once a frame while the bow, the slingshot, the hookshot or the
// boomerang is up, and adds it to the aim, so the aim turns with the pad rather than at a speed.
//
// Per pad, in its profile (main/input_bindings.h): on or off, the sensitivity, the axes. The pad the
// game reads is the one in slot 0, as for the stick.

#include <cstdint>

typedef struct _SDL_GameController SDL_GameController;
struct SDL_ControllerSensorEvent;

namespace oot::input::gyro {

    // Main thread. Whether a pad has a gyroscope and an accelerometer. Its sensors are switched on
    // only while its Gyro aiming row is on (input.cpp, tick), so a pad without it sends no readings:
    // set_enabled switches them and starts or drops its sum, true when SDL did it. A pad closed: its
    // readings go with it.
    bool supported(SDL_GameController* controller);
    bool set_enabled(SDL_GameController* controller, int instance, bool on);
    void close(int instance);

    // Main thread: an SDL_CONTROLLERSENSORUPDATE.
    void on_sensor(const SDL_ControllerSensorEvent& event);

    // The turn to add to the aim, in the game's angle units (65536 to a full turn): yaw is added to
    // the aim's heading (positive turns it left), pitch to its tilt (positive tilts it down).
    struct Aim {
        int32_t yaw = 0;
        int32_t pitch = 0;
    };

    // Game thread, once a frame while aiming, or while looking around in first person with C up
    // (`look`): the turn of the game's pad since the last take, with its profile's settings
    // applied. Nothing when gyro aiming is off for it (or, looking, its Gyro look), when it has no
    // gyroscope, or when the last take was long enough ago (a menu, the first aiming frame) that
    // what was summed meanwhile is not an aim.
    Aim take_aim(bool look);

    // The turn summed from made-up readings, the settings applied to it, the stale rule, and a
    // closed pad's readings dropped. GYRO_SELFTEST_OK or 1. The build runs it after every link.
    int selftest();

} // namespace oot::input::gyro
