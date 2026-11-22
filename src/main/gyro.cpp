#include "main/gyro.h"

#include "main/input.h"
#include "main/input_bindings.h"

#include <SDL.h>

#include "GamepadMotion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace oot::input::gyro {

    namespace {

        constexpr double PI = 3.14159265358979323846;
        constexpr double DEGREES_PER_RADIAN = 180.0 / PI;
        constexpr double UNITS_PER_DEGREE = 65536.0 / 360.0;
        // A reading more than this after the one before is the first after a gap (a pad asleep, a
        // stall): it starts the clock again rather than counting the gap as turning time.
        constexpr double LONGEST_STEP_SECONDS = 0.1;
        // A take this long after the one before is the first of a new aim (the game updates twenty
        // times a second, so an aim in progress takes every fiftieth of a second or so).
        constexpr double STALE_SECONDS = 0.25;

        // One pad's motion: the library's calibration, the last accelerometer reading, and the turn
        // summed since the game last took it.
        struct Tracker {
            GamepadMotion motion;
            float accel[3] = { 0.0f, 1.0f, 0.0f };   // in g; at rest, flat, gravity reads up
            uint64_t last_us = 0;
            double yaw_degrees = 0.0;
            double pitch_degrees = 0.0;

            Tracker() {
                motion.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::Stillness |
                                          GamepadMotionHelpers::CalibrationMode::SensorFusion);
            }

            // A gyroscope reading in SDL's units (radians a second, x pitch, y yaw, z roll) at
            // `at_us` microseconds.
            void turn(const float rate[3], uint64_t at_us) {
                const double step = (last_us == 0 || at_us <= last_us) ? 0.0 : double(at_us - last_us) / 1e6;
                last_us = at_us;
                if (step <= 0.0 || step > LONGEST_STEP_SECONDS) {
                    return;
                }
                motion.ProcessMotion(float(rate[0] * DEGREES_PER_RADIAN), float(rate[1] * DEGREES_PER_RADIAN),
                                     float(rate[2] * DEGREES_PER_RADIAN), accel[0], accel[1], accel[2], float(step));
                float pitch_rate = 0.0f;
                float yaw_rate = 0.0f;
                motion.GetPlayerSpaceGyro(pitch_rate, yaw_rate);
                pitch_degrees += pitch_rate * step;
                yaw_degrees += yaw_rate * step;
            }
        };

        bool is_stale(double seconds_since_last_take) {
            return seconds_since_last_take > STALE_SECONDS;
        }

        struct Settings {
            bool on = false;
            bool look = false;   // C up's looking turned too
            int sensitivity_percent = DEFAULT_GYRO_SENSITIVITY_PERCENT;
            int axes = 0;
        };

        // The summed turn as the aim takes it: player space yaw left and pitch up, scaled, turned
        // the way the axes row says. Pitch up tilts the aim up, which is a smaller tilt.
        Aim convert(double yaw_degrees, double pitch_degrees, const Settings& s) {
            if (!s.on) {
                return {};
            }
            const double scale = UNITS_PER_DEGREE * s.sensitivity_percent / 100.0;
            double yaw = yaw_degrees * scale;
            double pitch = -pitch_degrees * scale;
            if (s.axes & 1) yaw = -yaw;
            if (s.axes & 2) pitch = -pitch;
            return { int32_t(std::lround(std::clamp(yaw, -32767.0, 32767.0))),
                     int32_t(std::lround(std::clamp(pitch, -32767.0, 32767.0))) };
        }

        std::mutex g_mutex;
        // By SDL's instance id. Held by pointer: the library's state points into itself, so a
        // tracker is never copied or moved once made.
        std::map<int, std::unique_ptr<Tracker>> g_pads;
        std::chrono::steady_clock::time_point g_last_take{};

        // Looking around with C up turns with the pad only when its Gyro look row is on; aiming
        // turns whenever Gyro aiming is.
        Settings for_use(Settings s, bool look) {
            s.on = s.on && (!look || s.look);
            return s;
        }

        Settings settings_for(const std::string& guid) {
            Settings s;
            const std::shared_ptr<const Bindings> all = live();
            for (const Profile& p : all->profiles) {
                if (p.guid == guid) {
                    s.on = p.gyro;
                    s.look = p.gyro_look;
                    s.sensitivity_percent = p.gyro_sensitivity_percent;
                    s.axes = p.gyro_axes;
                }
            }
            return s;
        }

    } // namespace

    bool supported(SDL_GameController* controller) {
        return controller != nullptr && SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) &&
               SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL);
    }

    bool set_enabled(SDL_GameController* controller, int instance, bool on) {
        if (!supported(controller)) {
            return false;
        }
        const SDL_bool state = on ? SDL_TRUE : SDL_FALSE;
        if (SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_GYRO, state) != 0 ||
            SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_ACCEL, state) != 0) {
            std::fprintf(stderr, "[gyro] pad %d's sensors would not switch %s: %s\n", instance, on ? "on" : "off", SDL_GetError());
            return false;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if (on) {
            g_pads[instance] = std::make_unique<Tracker>();
        }
        else {
            g_pads.erase(instance);
        }
        std::fprintf(stderr, "[gyro] pad %d's sensors %s\n", instance, on ? "on" : "off");
        return true;
    }

    void close(int instance) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pads.erase(instance);
    }

    void on_sensor(const SDL_ControllerSensorEvent& event) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_pads.find(event.which);
        if (found == g_pads.end()) {
            return;
        }
        Tracker& t = *found->second;
        if (event.sensor == SDL_SENSOR_ACCEL) {
            for (int i = 0; i < 3; ++i) {
                t.accel[i] = event.data[i] / SDL_STANDARD_GRAVITY;
            }
        }
        else if (event.sensor == SDL_SENSOR_GYRO) {
            t.turn(event.data, event.timestamp_us);
        }
    }

    Aim take_aim(bool look) {
        devices::PadInfo pad;
        if (!devices::pad_info(0, pad) || !pad.gyro) {
            return {};
        }
        // A look that does not turn still takes the turn, so none of it waits to jump the aim later.
        const Settings s = for_use(settings_for(pad.guid), look);
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_mutex);
        const bool stale = is_stale(std::chrono::duration<double>(now - g_last_take).count());
        g_last_take = now;
        const auto found = g_pads.find(pad.instance);
        if (found == g_pads.end()) {
            return {};
        }
        Tracker& t = *found->second;
        const Aim aim = stale ? Aim{} : convert(t.yaw_degrees, t.pitch_degrees, s);
        t.yaw_degrees = 0.0;
        t.pitch_degrees = 0.0;
        return aim;
    }

    int selftest() {
        int failures = 0;
        const auto expect = [&failures](bool ok, const char* what) {
            if (!ok) {
                std::fprintf(stderr, "GYRO_SELFTEST_FAILED: %s\n", what);
                ++failures;
            }
        };

        // A pad held still and flat for two seconds, as it is before anyone aims, so the library has
        // settled on which way is down; then the turn, read every 5 ms, and what it summed.
        const auto held_then_turned = [](const float rate[3]) {
            const float still[3] = { 0.0f, 0.0f, 0.0f };
            Tracker t;
            uint64_t us = 1000;
            for (; us <= 2'000'000; us += 5000) {
                t.turn(still, us);
            }
            t.yaw_degrees = 0.0;
            t.pitch_degrees = 0.0;
            for (const uint64_t end = us + 1'000'000; us <= end; us += 5000) {
                t.turn(rate, us);
            }
            return std::pair{ t.yaw_degrees, t.pitch_degrees };
        };
        // Turned left at 90 degrees a second for one second.
        {
            const float left[3] = { 0.0f, float(90.0 / DEGREES_PER_RADIAN), 0.0f };
            const auto [yaw, pitch] = held_then_turned(left);
            if (std::fabs(yaw - 90.0) >= 3.0 || std::fabs(pitch) >= 1.0) {
                std::fprintf(stderr, "GYRO_SELFTEST: turned left, summed yaw %.2f and pitch %.2f\n", yaw, pitch);
            }
            expect(std::fabs(yaw - 90.0) < 3.0, "a pad turned left 90 degrees did not sum to 90 degrees of yaw");
            expect(std::fabs(pitch) < 1.0, "turning a flat pad left read as a tilt");
        }
        // Tilted up at 45 degrees a second for one second.
        {
            const float up[3] = { float(45.0 / DEGREES_PER_RADIAN), 0.0f, 0.0f };
            const auto [yaw, pitch] = held_then_turned(up);
            if (std::fabs(pitch - 45.0) >= 2.0) {
                std::fprintf(stderr, "GYRO_SELFTEST: tilted up, summed yaw %.2f and pitch %.2f\n", yaw, pitch);
            }
            expect(std::fabs(pitch - 45.0) < 2.0, "a pad tilted up 45 degrees did not sum to 45 degrees of pitch");
        }
        // A gap in the readings is not turning time.
        {
            Tracker t;
            const float left[3] = { 0.0f, float(90.0 / DEGREES_PER_RADIAN), 0.0f };
            t.turn(left, 1000);
            t.turn(left, 2'001'000);
            expect(t.yaw_degrees == 0.0, "two seconds without a reading counted as turning");
        }

        // The settings: off is nothing; 100% is the pad's own turn; left turns the aim left (its
        // heading up) and up tilts it up (its tilt down); each axis inverts on its own.
        {
            Settings s;
            expect(convert(10.0, 10.0, s).yaw == 0 && convert(10.0, 10.0, s).pitch == 0, "gyro aiming moved the aim while off");
            s.on = true;
            s.sensitivity_percent = 100;
            const Aim one = convert(10.0, 10.0, s);
            expect(std::abs(one.yaw - 1820) <= 1 && std::abs(one.pitch + 1820) <= 1,
                   "10 degrees at 100% was not 1820 units, left as plus and up as minus");
            s.sensitivity_percent = 200;
            expect(std::abs(convert(10.0, 0.0, s).yaw - 3641) <= 1, "200% did not double the turn");
            s.axes = 1;
            expect(convert(10.0, 10.0, s).yaw < 0 && convert(10.0, 10.0, s).pitch < 0, "inverting left and right touched up and down");
            s.axes = 2;
            expect(convert(10.0, 10.0, s).yaw > 0 && convert(10.0, 10.0, s).pitch > 0, "inverting up and down touched left and right");
            s.axes = 0;
            expect(convert(1e6, -1e6, s).yaw == 32767 && convert(1e6, -1e6, s).pitch == 32767, "a huge turn was not clamped");
        }

        // Gyro look: looking turns only with its row on, aiming either way, and nothing with Gyro
        // aiming off.
        {
            Settings s;
            s.on = true;
            s.sensitivity_percent = 100;
            expect(convert(10.0, 0.0, for_use(s, true)).yaw == 0, "looking turned with Gyro look off");
            expect(convert(10.0, 0.0, for_use(s, false)).yaw != 0, "aiming did not turn with Gyro look off");
            s.look = true;
            expect(convert(10.0, 0.0, for_use(s, true)).yaw != 0, "looking did not turn with Gyro look on");
            s.on = false;
            expect(convert(10.0, 0.0, for_use(s, true)).yaw == 0, "looking turned with Gyro aiming off");
        }

        // An aim in progress takes every frame and keeps its turn; the first take after a menu, a
        // pause or the start of an aim drops what was summed meanwhile.
        expect(!is_stale(0.05) && !is_stale(0.2), "a take a frame or a few after the last was stale");
        expect(is_stale(0.3) && is_stale(5.0), "a take after a menu or a pause kept the turn summed meanwhile");

        // A closed pad's readings go nowhere.
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pads[-77] = std::make_unique<Tracker>();
        }
        close(-77);
        SDL_ControllerSensorEvent reading{};
        reading.which = -77;
        reading.sensor = SDL_SENSOR_GYRO;
        on_sensor(reading);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            expect(g_pads.find(-77) == g_pads.end(), "a reading for a closed pad brought it back");
        }

        if (failures != 0) {
            return 1;
        }
        std::printf("GYRO_SELFTEST_OK\n");
        return 0;
    }

} // namespace oot::input::gyro
