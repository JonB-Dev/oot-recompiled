// Starting the game.
//
// Everything above this file prepares; this is where the recompiled code actually runs. The runtime
// wants a Configuration full of callbacks: it owns the threads, the scheduler and the main loop,
// and calls back into us for the things only this program knows, which are the renderer, the
// window, audio output, controller input, and which RSP microcode a task wants.
//
// Scope for the first boot: enough of each to get a picture on screen. Audio is wired to SDL's
// queue, input to a single keyboard-and-gamepad mapping, and the RSP dispatcher knows the two
// microcodes this game uses. The settings UI and the proper input binding arrive at Phase 24.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/ultra64.h"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "librecomp/overlays.hpp"

#include "game/game.h"
#include "game/render.h"
#include "main/boot.h"
#include "game/texture_packs.h"
#include "main/places.h"
#include "main/recorder.h"
#include "main/screenshots.h"
#include "main/updates.h"
#include "game/moments.h"
#include "main/input.h"
#include "main/input_bindings.h"
#include "main/launch.h"
#include "main/probe.h"
#include "main/shots.h"
#include "main/video.h"
#include "main/photo.h"
#include "game/hud.h"
#include "game/actor_registry.h"
#include "game/camera_cuts.h"
#include "game/dl_check.h"
#include "game/game_state.h"
#include "ui/ui_render.h"
#include "ui/ui_settings.h"
#include "ui/ui_lighting.h"
#include "ui/ui_shell.h"

// The two recompiled microcodes, generated into src/rsp.
//
// NOT extern "C". RSPRecomp emits C++ with ordinary C++ linkage, so declaring these as C gives them
// a different mangled name and the build fails at link with "undefined symbol: aspMain" while the
// definition is sitting in a file the build plainly compiled.
RspExitReason aspMain(uint8_t* rdram, uint32_t ucode_addr);
RspExitReason njpgdspMain(uint8_t* rdram, uint32_t ucode_addr);

namespace {

    SDL_Window* g_window = nullptr;
    SDL_AudioDeviceID g_audio_device = 0;
    std::atomic<uint32_t> g_sample_rate{ 32000 };
    // The window's client size, kept by the thread that pumps its messages so the game thread
    // can read it without touching the window (post-parity phase 43, the wide transitions).
    // --virtual-controller <script>: the device layer attaches a virtual pad and drives it from
    // this file. Empty in a normal run.
    std::string g_virtual_script;
    // --virtual-raw: the virtual pad is attached as a joystick SDL has no mapping for.
    bool g_virtual_raw = false;
    // --play: straight into the game with no launcher, which is how the harness runs and how
    // anyone who never wants the launcher can run.
    bool g_play_now = false;
    bool g_open_files = false;

    std::atomic<int> g_window_width{ 0 };
    std::atomic<int> g_window_height{ 0 };

    // ------------------------------------------------------------------------------------------
    // RSP
    //
    // The game submits a task naming the microcode it wants by the address of its text. Matching on
    // that address is how the runtime knows which of our translated microcodes to run.
    //
    // The two addresses are this game's own, from app-type.md and confirmed against the symbol
    // table: aspMain's text at 0x800E2FC0 and njpgdsp's at 0x800E6BC0. They are ROM-relative in the
    // task, so the comparison is on the low bits the task carries.
    // ------------------------------------------------------------------------------------------

    // MATCH ON THE MICROCODE ADDRESS, NOT THE TASK TYPE.
    //
    // The type number is ambiguous in this game, and that cost real time. `sCIC6105Task` in
    // src/boot/cic6105.c declares type 4, which is the same number as M_NJPEGTASK, so a dispatcher
    // that switches on the type matches the anti-piracy task to the JPEG microcode and runs it over
    // unrelated data. The runtime's own header warns about this: it passes the whole OSTask
    // "in case the task_type number is not enough information to distinguish out the exact
    // microcode function."
    //
    // The addresses are this game's own, from the symbol table rather than from the reference
    // project: aspMainTextStart is 0x800E2FC0 and njpgdspMainTextStart is 0x800E6BC0. A task
    // carries the ucode pointer as a game address, so the comparison masks to the low 24 bits,
    // which is enough to tell these two apart and survives a K0 or physical spelling.
    constexpr uint32_t ASP_UCODE_ADDR     = 0x0E2FC0;
    constexpr uint32_t NJPGDSP_UCODE_ADDR = 0x0E6BC0;

    RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
        const uint32_t ucode = static_cast<uint32_t>(task->t.ucode) & 0xFFFFFF;

        if (ucode == ASP_UCODE_ADDR) {
            return aspMain;
        }
        if (ucode == NJPGDSP_UCODE_ADDR) {
            return njpgdspMain;
        }

        // Anything else is a microcode this build did not recompile. Say which, and say it in terms
        // that point at the cause: the address is what identifies it, the type alone is not.
        std::fprintf(stderr,
                     "[rsp] unrecognized microcode at 0x%06X (task type %u). This build recompiled\n"
                     "      aspMain at 0x%06X and njpgdspMain at 0x%06X and nothing else.\n",
                     ucode, static_cast<unsigned>(task->t.type), ASP_UCODE_ADDR, NJPGDSP_UCODE_ADDR);
        return nullptr;
    }

    // ------------------------------------------------------------------------------------------
    // Audio, through SDL's queue. The runtime hands us signed 16 bit stereo frames.
    // ------------------------------------------------------------------------------------------

    // The video recorder's two controls answer while its row is On, and ALSO while a video is
    // going whatever the row says, so a recording can always be stopped (2026-10-09).
    bool video_controls_live() {
        return oot::ui::video_recording_enabled() || (oot::video::status().state != oot::video::State::Idle);
    }

    void queue_samples(int16_t* audio_data, size_t sample_count) {
        if (g_audio_device == 0) {
            return;
        }

        // sample_count is a count of int16 VALUES, not of stereo frames. The runtime computes it as
        // `byte_count / sizeof(int16_t)` in ultramodern/src/audio.cpp, so the byte count is simply
        // sample_count * 2.
        //
        // Multiplying by another 2 for "stereo" asks SDL to read twice the buffer, and the second
        // half is whatever happened to follow it in memory. That sounds exactly like what it is:
        // continuous clicking, with the real audio buried under it.
        oot::probe::note_audio_buffer(audio_data, sample_count);

        // THE CHANNELS ARRIVE THE WRONG WAY ROUND, and nothing about this buffer says so.
        //
        // The runtime stores the console's memory with sixteen bit values at `offset ^ 2`
        // (N64Recomp's MEM_H), which is how a big endian console's halfwords land in a little
        // endian word. Thirty two bit values are unaffected, which is why every other native
        // reader in this project (the camera's Vec3f, for one) can memcpy a float and be right.
        // A sixteen bit one cannot. Stereo frames are interleaved halfwords, so the left and
        // right of every frame sit either side of that xor and a straight read hands SDL the
        // pair reversed: the game's whole sound stage mirrored, left for right, all the way
        // through. It plays perfectly and is wrong, which is why it survived to 0.1.0 and was
        // found while measuring the popping rather than by listening. The reference project
        // corrects the same thing in the same place, for the same reason.
        //
        // The swap and the volume ride in one pass, because both want every sample and a second
        // pass over the buffer would be waste. At full volume (the usual case) the multiply is
        // skipped and the pass is the swap alone.
        const float gain = oot::ui::volume_gain();
        const bool scale = gain < 0.999f;
        // THE VIDEO'S SOUND IS THE GAME'S OWN LEVEL (2026-10-09, main/video.h): a speaker turned
        // down must not make a quiet video. While a video takes the sound, the swap runs alone,
        // the recorder copies the buffer, and the volume is a second pass.
        const bool record = oot::video::wants_audio();
        const bool gain_in_swap = scale && !record;
        for (size_t i = 0; i + 1 < sample_count; i += 2) {
            const int16_t left = audio_data[i + 1];
            const int16_t right = audio_data[i];
            if (gain_in_swap) {
                audio_data[i + 0] = static_cast<int16_t>(static_cast<float>(left) * gain);
                audio_data[i + 1] = static_cast<int16_t>(static_cast<float>(right) * gain);
            }
            else {
                audio_data[i + 0] = left;
                audio_data[i + 1] = right;
            }
        }
        if (record) {
            oot::video::submit_audio(audio_data, sample_count / 2, g_sample_rate.load(std::memory_order_relaxed));
            if (scale) {
                for (size_t i = 0; i < sample_count; ++i) {
                    audio_data[i] = static_cast<int16_t>(static_cast<float>(audio_data[i]) * gain);
                }
            }
        }

        SDL_QueueAudio(g_audio_device, audio_data, static_cast<Uint32>(sample_count * sizeof(int16_t)));
    }

    size_t get_frames_remaining() {
        if (g_audio_device == 0) {
            return 0;
        }
        // Bytes to frames: two channels of int16 per frame.
        const size_t frames = SDL_GetQueuedAudioSize(g_audio_device) / (sizeof(int16_t) * 2);
        oot::probe::note_queue_depth(frames);

        // UNDER-REPORT BY ONE RETRACE, so the game keeps that much more ahead of the device.
        //
        // This number is what the game's audio thread reads as "still to play", and it generates
        // to fill the gap. Measured on 0.1.0, the queue sawtooths between about two retraces'
        // worth and nearly nothing (mean 620 frames, minimum 16), so the trough is where a late
        // frame turns into silence. Reporting one retrace less moves the whole sawtooth up by
        // that much without changing its shape: the cost is one retrace of extra latency,
        // about 17 ms, which is below what a person can hear against the picture.
        //
        // The runtime already does the same thing by half a retrace and says so in its own
        // comment ("If there's ever any audio popping, check here first"), where the SDL2 value
        // is given as a whole one and the half is Godot's. We cannot change that line without
        // forking a vendored library, so the other half is added here.
        const size_t retrace = g_sample_rate / 60;
        return (frames > retrace) ? (frames - retrace) : 0;
    }

    void set_frequency(uint32_t freq) {
        // The game changes sample rate at load boundaries. Reopen the device rather than resample.
        if (g_audio_device != 0) {
            SDL_CloseAudioDevice(g_audio_device);
            g_audio_device = 0;
        }

        SDL_AudioSpec want{};
        want.freq = static_cast<int>(freq);
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        // THE DEVICE'S PERIOD, and it was the popping. SDL pulls exactly this many frames at a
        // time from the queue and plays silence for whatever is missing. At 1024 frames and this
        // game's 32 kHz that is a 32 ms bite, while the game keeps about 19 ms queued on average
        // (measured: mean depth 620 frames, dipping to 16), so SDL was regularly asked for more
        // than had been written and filled the gap with nothing. That is the pop, and it lands
        // exactly when a frame runs late because that is when the queue is thinnest, which is
        // why both reports describe the two together. 256 frames is 8 ms, comfortably inside
        // what the game keeps ahead; the reference project uses the same number for the same
        // stated reason.
        want.samples = 256;
        want.callback = nullptr;   // queue driven, not callback driven

        SDL_AudioSpec have{};
        g_audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (g_audio_device == 0) {
            std::fprintf(stderr, "[audio] could not open a device at %u Hz: %s\n", freq, SDL_GetError());
            return;
        }

        // SDL is asked for an exact rate (allowed_changes is 0), so `have.freq` differing from
        // `freq` would be a pitch error of exactly that ratio. Recorded rather than assumed.
        oot::probe::note_device_rate(freq, static_cast<uint32_t>(have.freq));

        g_sample_rate = freq;
        SDL_PauseAudioDevice(g_audio_device, 0);
    }

    // ------------------------------------------------------------------------------------------
    // Input. One controller, on the keyboard, through the binding model (main/input_bindings.h):
    // every game input is looked up in the live keyboard bindings, whose defaults are the keys
    // this function hard coded from phase 20 to phase 46 (X A, C B, Z Z, Return Start, the
    // arrows the D-pad, Q and E the shoulders, IJKL the C buttons, WASD the stick), so nothing
    // the harness presses changed when the table moved. Pads arrive in phase 48 through the
    // same model.
    //
    // THE FIRST VERSION OF THE TABLE HAD TWO KEYS DOING TWO JOBS. `A` was both the L shoulder
    // and analog left, and `S` was both R and analog down, so walking left pressed L and walking
    // backward pressed R. It looked fine in the title screen, where neither is read, and would
    // have looked like a game bug the moment anyone moved. Shoulders are on Q and E since.
    // ------------------------------------------------------------------------------------------

    void poll_input() {
        // SDL events are pumped on the main thread in main.cpp. Nothing to do per poll.
    }

    // Whether any of an input's keyboard bindings is a key that is down.
    bool key_bound_down(const Uint8* keys, const oot::input::Bindings& bindings, oot::input::GameInput input) {
        const auto& slots = bindings.keyboard[static_cast<size_t>(input)];
        for (const oot::input::Field& f : slots) {
            if (f.kind == oot::input::FieldKind::Key && f.index >= 0 && f.index < SDL_NUM_SCANCODES &&
                keys[f.index]) {
                return true;
            }
        }
        return false;
    }

    bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;

        if (controller_num != 0) {
            return false;   // one controller for now
        }

        // While a document is open the game gets a neutral controller. Without this, moving down
        // a settings list also walks Link across the room, and pressing A both changes a value
        // and swings a sword. Returning true with everything zero is deliberate: the controller
        // is still connected, it is just not being touched.
        if (oot::ui::shell::capturing_input()) {
            return true;
        }

        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        if (keys == nullptr) {
            return true;
        }

        using oot::input::GameInput;
        const std::shared_ptr<const oot::input::Bindings> bindings = oot::input::live();
        for (int i = 0; i < oot::input::INPUT_COUNT; ++i) {
            const GameInput input = static_cast<GameInput>(i);
            const uint16_t bit = oot::input::input_bit(input);
            if (bit != 0 && key_bound_down(keys, *bindings, input)) {
                *buttons |= bit;
            }
        }

        // The stick on keys, at full deflection. A keyboard has no middle position, so every
        // movement is a run.
        if (key_bound_down(keys, *bindings, GameInput::StickUp))    *y += 1.0f;
        if (key_bound_down(keys, *bindings, GameInput::StickDown))  *y -= 1.0f;
        if (key_bound_down(keys, *bindings, GameInput::StickRight)) *x += 1.0f;
        if (key_bound_down(keys, *bindings, GameInput::StickLeft))  *x -= 1.0f;

        // The first pad drives the same player. Its buttons join the keyboard's, and its stick
        // takes over whenever it is deflected past its deadzone, since a pad's range is the one
        // that carries walking as well as running.
        uint16_t pad_buttons = 0;
        float pad_x = 0.0f;
        float pad_y = 0.0f;
        if (oot::input::devices::read_pad(0, &pad_buttons, &pad_x, &pad_y)) {
            *buttons |= pad_buttons;
            if (pad_x != 0.0f || pad_y != 0.0f) {
                *x = pad_x;
                *y = pad_y;
            }
        }
        *x = std::max(-1.0f, std::min(1.0f, *x));
        *y = std::max(-1.0f, std::min(1.0f, *y));

        return true;
    }

    void set_rumble(int controller_num, bool on) {
        // The runtime asks for the console port; the first pad is port 0.
        oot::input::devices::set_rumble(controller_num, on);
    }

    ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
        ultramodern::input::connected_device_info_t info{};
        info.connected_device =
            (controller_num == 0) ? ultramodern::input::Device::Controller : ultramodern::input::Device::None;
        // A Rumble Pak is reported when the first pad can rumble, so the game asks for it.
        info.connected_pak = (controller_num == 0 && oot::input::devices::pad_rumbles(0))
                                 ? ultramodern::input::Pak::RumblePak
                                 : ultramodern::input::Pak::None;
        return info;
    }

    // ------------------------------------------------------------------------------------------
    // Graphics plumbing. The window already exists; these just hand it over.
    // ------------------------------------------------------------------------------------------

    void* create_gfx() {
        return nullptr;   // no gfx-side state of our own
    }

    ultramodern::renderer::WindowHandle create_window(void*) {
        SDL_SysWMinfo wmInfo{};
        SDL_VERSION(&wmInfo.version);
        SDL_GetWindowWMInfo(g_window, &wmInfo);

        ultramodern::renderer::WindowHandle handle{};
        handle.window = wmInfo.info.win.window;
        handle.thread_id = GetCurrentThreadId();
        return handle;
    }

    // Keys the interface owns. F1, F2 and F3 open its screens; everything else here only does
    // anything while a screen is open, so the game's controls are untouched the rest of the time.
    void handle_key(SDL_Scancode code) {
        // THE KEYBOARD'S OWN MENU BINDING, which used to go nowhere. F1 below was the only key
        // that opened the interface, so a Menu rebound on the controls screen (the user,
        // 2026-09-23, to Left Ctrl: "it even said it in the overlay but f1 is all that works
        // still") was captured, saved, shown in its row and never acted on. The pad's binding has
        // always come through handle_pad_edges; this is the same door for a key. F1 stays as well,
        // because the hint names it.
        if (const auto live = oot::input::live()) {
            using GI = oot::input::GameInput;
            const auto bound = [&](GI input) {
                for (const oot::input::Field& f : live->keyboard[static_cast<size_t>(input)]) {
                    if (f.kind == oot::input::FieldKind::Key && f.index == static_cast<int>(code)) {
                        return true;
                    }
                }
                return false;
            };
            if (bound(GI::Menu)) {
                oot::ui::shell::post_toggle_settings();
                return;
            }
            // The quick keys (phases 77 and 78): save takes the first empty slot and never
            // overwrites (the user, 2026-09-23); load resumes the newest moment.
            if (bound(GI::QuickSave)) {
                oot::moments::quick_save();
                return;
            }
            if (bound(GI::QuickLoad)) {
                oot::moments::quick_load();
                return;
            }
            // The Screenshot binding (2026-10-09): one shot, or the comparison pair.
            if (bound(GI::Screenshot)) {
                oot::screenshots::request();
                return;
            }
            // The video recorder's two (2026-10-09), while its row is On.
            if (video_controls_live() && bound(GI::RecordVideo)) {
                oot::video::toggle_record();
                return;
            }
            if (video_controls_live() && bound(GI::PauseVideo)) {
                oot::video::toggle_pause();
                return;
            }
            // Photo mode (2026-10-09), while its row is On; and always to end it.
            if ((oot::ui::photo_mode_enabled() || oot::photo::active()) && bound(GI::PhotoMode)) {
                oot::photo::toggle();
                return;
            }
            // The game's HUD, while photo mode holds the game (2026-10-09).
            if (oot::photo::active() && bound(GI::PhotoHud)) {
                oot::photo::toggle_hud();
                return;
            }
            // The HUD's fading (game/hud.h), in play.
            if (!oot::photo::active() && bound(GI::HudToggle)) {
                oot::hud::press_button();
                return;
            }
        }
        switch (code) {
            case SDL_SCANCODE_F1:
                oot::ui::shell::post_toggle_settings();
                return;
            case SDL_SCANCODE_F2:
                oot::ui::shell::post_open(oot::ui::shell::Screen::About);
                return;
            case SDL_SCANCODE_F3:
                oot::ui::shell::post_toggle(oot::ui::shell::Screen::Controls);
                return;
            // THE UPDATE, AND IT IS ALWAYS THE PERSON'S OWN ACTION. Nothing downloads on its own
            // and nothing installs on its own: the launcher's foot says a version is available and
            // this is the key that says yes. Pressed again once it has been fetched and verified,
            // it runs the installer and closes this copy. A press with nothing to do is ignored,
            // so the key never interrupts anybody.
            case SDL_SCANCODE_F5:
                if (oot::updates::state() == oot::updates::State::Newer) {
                    oot::updates::start_download();
                }
                else if (oot::updates::state() == oot::updates::State::Ready) {
                    if (oot::updates::install()) {
                        oot::launch::request_quit();
                    }
                }
                return;
            default:
                break;
        }

        if (!oot::ui::shell::capturing_input()) {
            return;
        }

        using Nav = oot::ui::shell::Nav;
        switch (code) {
            case SDL_SCANCODE_UP:
            case SDL_SCANCODE_W:      oot::ui::shell::post_nav(Nav::Up); break;
            case SDL_SCANCODE_DOWN:
            case SDL_SCANCODE_S:      oot::ui::shell::post_nav(Nav::Down); break;
            case SDL_SCANCODE_LEFT:
            case SDL_SCANCODE_A:      oot::ui::shell::post_nav(Nav::Left); break;
            case SDL_SCANCODE_RIGHT:
            case SDL_SCANCODE_D:      oot::ui::shell::post_nav(Nav::Right); break;
            case SDL_SCANCODE_X:
            case SDL_SCANCODE_RETURN: oot::ui::shell::post_nav(Nav::Accept); break;
            case SDL_SCANCODE_C:
            case SDL_SCANCODE_ESCAPE: oot::ui::shell::post_nav(Nav::Back); break;
            case SDL_SCANCODE_DELETE:
            case SDL_SCANCODE_BACKSPACE: oot::ui::shell::post_nav(Nav::Clear); break;
            default: break;
        }
    }

    // A pad's edges, through its profile (phase 50): the menu button toggles the interface the
    // way F1 does, and while a document is open the directions, A and B navigate it. The game
    // reads pad STATE in get_input; an edge here is a press since the last pump, which is what
    // a toggle and a menu step need, for the same reason the keys above are edges.
    void handle_pad_edges(uint64_t edges) {
        if (edges == 0) {
            return;
        }
        oot::ui::shell::post_activity();

        using GI = oot::input::GameInput;
        const auto has = [edges](GI input) { return (edges & (uint64_t{ 1 } << static_cast<int>(input))) != 0; };
        if (has(GI::Menu)) {
            oot::ui::shell::post_toggle_settings();
            return;
        }
        if (has(GI::QuickSave)) {
            oot::moments::quick_save();
            return;
        }
        if (has(GI::QuickLoad)) {
            oot::moments::quick_load();
            return;
        }
        if (has(GI::Screenshot)) {
            oot::screenshots::request();
            return;
        }
        if (video_controls_live() && has(GI::RecordVideo)) {
            oot::video::toggle_record();
            return;
        }
        if (video_controls_live() && has(GI::PauseVideo)) {
            oot::video::toggle_pause();
            return;
        }
        if ((oot::ui::photo_mode_enabled() || oot::photo::active()) && has(GI::PhotoMode)) {
            oot::photo::toggle();
            return;
        }
        if (oot::photo::active() && has(GI::PhotoHud)) {
            oot::photo::toggle_hud();
            return;
        }
        if (!oot::photo::active() && has(GI::HudToggle)) {
            oot::hud::press_button();
            return;
        }
        if (!oot::ui::shell::capturing_input()) {
            return;
        }

        using Nav = oot::ui::shell::Nav;
        if (has(GI::DUp) || has(GI::StickUp))       oot::ui::shell::post_nav(Nav::Up);
        if (has(GI::DDown) || has(GI::StickDown))   oot::ui::shell::post_nav(Nav::Down);
        if (has(GI::DLeft) || has(GI::StickLeft))   oot::ui::shell::post_nav(Nav::Left);
        if (has(GI::DRight) || has(GI::StickRight)) oot::ui::shell::post_nav(Nav::Right);
        if (has(GI::A))                              oot::ui::shell::post_nav(Nav::Accept);
        if (has(GI::B))                              oot::ui::shell::post_nav(Nav::Back);
    }

    // THIS IS THE MESSAGE PUMP, and it is the only one. The comment that used to sit here said
    // the pump lived on the main thread in main.cpp, which was not true anywhere: main.cpp calls
    // boot::run and never gets the thread back, and there was no loop in it to pump from.
    //
    // `recomp::start` runs this on the calling thread, which IS the main thread and the thread
    // that created the window:
    //
    //     while (!exited) {
    //         ultramodern::sleep_milliseconds(1);
    //         if (gfx_callbacks.update_gfx != nullptr) { gfx_callbacks.update_gfx(gfx_data); }
    //     }
    //
    // so this callback is the slot the runtime leaves for exactly this, and leaving it empty is
    // what produced the symptom the user reported: the game audible and animating, the window
    // grayed out and refusing every key. Windows marks a window Not Responding when its owning
    // thread stops servicing its queue, and the game and render threads are elsewhere and do not
    // care. Nothing about it looks like an input bug, which is why it survived a whole phase.
    //
    // `poll_input` cannot do this instead. The runtime calls that one from osContStartReadData,
    // on the game thread, and a window's messages have to be pumped by the thread that owns it.
    void update_gfx(void*) {
        // A quit that waited for its moment: once the capture has landed (or given up), go.
        static bool s_quit_sent = false;
        if (!s_quit_sent && oot::moments::quit_pending() && oot::moments::quit_ready()) {
            s_quit_sent = true;
            ultramodern::quit();
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    if (!oot::moments::on_quit_requested(oot::launch::game_started())) {
                        ultramodern::quit();
                    }
                    break;

                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                        ev.window.windowID == SDL_GetWindowID(g_window)) {
                        // Save on quit (phase 78b): the close button keeps a moment first when
                        // the row asks; the pump below quits once it has landed.
                        if (!oot::moments::on_quit_requested(oot::launch::game_started())) {
                            ultramodern::quit();
                        }
                    }
                    else if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        g_window_width.store(ev.window.data1, std::memory_order_relaxed);
                        g_window_height.store(ev.window.data2, std::memory_order_relaxed);
                    }
                    break;

                case SDL_MOUSEMOTION:
                    // The mouse reaches the documents (the launcher, a menu over the game) and
                    // the game window's own header, which shows when the pointer is at the top.
                    // The game itself never reads it.
                    oot::ui::shell::post_mouse_move(ev.motion.x, ev.motion.y);
                    break;

                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    // SDL numbers left, middle, right as 1, 2, 3; RmlUi as 0, 2, 1.
                    const int button = (ev.button.button == SDL_BUTTON_LEFT) ? 0
                                     : (ev.button.button == SDL_BUTTON_RIGHT) ? 1 : 2;
                    oot::ui::shell::post_mouse_button(button, ev.type == SDL_MOUSEBUTTONDOWN);
                    break;
                }

                case SDL_MOUSEWHEEL:
                    // SDL's y grows away from the user; RmlUi's delta grows down the page.
                    oot::ui::shell::post_mouse_wheel(-static_cast<float>(ev.wheel.y));
                    break;

                case SDL_KEYDOWN:
                    // The ROM browser's path field (2026-09-19): while it has the focus the
                    // keys are its own. Enter takes the path, Escape lets go of the field, and
                    // the editing keys go to the field with their modifiers (repeats included,
                    // so a held Backspace keeps deleting).
                    if (oot::ui::shell::text_entry_active()) {
                        const SDL_Scancode code = ev.key.keysym.scancode;
                        if (code == SDL_SCANCODE_RETURN || code == SDL_SCANCODE_KP_ENTER) {
                            if (ev.key.repeat == 0) {
                                oot::ui::shell::post_text_accept();
                            }
                        }
                        else if (code == SDL_SCANCODE_ESCAPE) {
                            if (ev.key.repeat == 0) {
                                oot::ui::shell::post_text_escape();
                            }
                        }
                        else {
                            oot::ui::shell::post_key(code, (ev.key.keysym.mod & KMOD_CTRL) != 0,
                                                     (ev.key.keysym.mod & KMOD_SHIFT) != 0);
                        }
                        break;
                    }
                    // Handled here rather than in get_input because these are EDGES. get_input
                    // sees the key held down every frame, so a toggle driven from there would
                    // flicker the menu sixty times a second for as long as the key was down.
                    // A capture armed on the keyboard takes the key first, so the key that is
                    // being bound never also opens or navigates a document.
                    if (ev.key.repeat == 0) {
                        oot::ui::shell::post_activity();
                        if (!oot::input::devices::capture_event(ev)) {
                            handle_key(ev.key.keysym.scancode);
                        }
                    }
                    break;

                case SDL_TEXTINPUT:
                    // Typed text reaches only the path field, when it has the focus.
                    if (oot::ui::shell::text_entry_active()) {
                        oot::ui::shell::post_text(ev.text.text);
                    }
                    break;

                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERAXISMOTION:
                case SDL_JOYBUTTONDOWN:
                case SDL_JOYAXISMOTION:
                case SDL_JOYHATMOTION:
                    // A pad's edges: only a capture cares, since get_input reads pad state.
                    oot::input::devices::capture_event(ev);
                    break;

                case SDL_JOYDEVICEADDED:
                case SDL_JOYDEVICEREMOVED:
                    // A pad plugged in or pulled out. The device layer opens and closes it and
                    // says so in the trace; get_input reads whatever is open.
                    oot::input::devices::handle_event(ev);
                    break;

                case SDL_DROPFILE:
                    // A file dropped on the window is a ROM offered to the launcher. SDL hands
                    // the path out as memory it expects back.
                    if (ev.drop.file != nullptr) {
                        // SDL gives the path in UTF-8; a path built from that keeps every character.
                        oot::launch::request_select_rom(
                            std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(ev.drop.file))));
                        SDL_free(ev.drop.file);
                    }
                    break;

                default:
                    // Everything else is consumed for its side effect. SDL updates the keyboard
                    // and pad state as it processes events, and get_input reads that state, so
                    // draining the queue here is what makes the controller work at all.
                    break;
            }
        }
        // The virtual pad's script, the rumble pulses and the pads' edges, once per pump.
        oot::input::devices::tick();
        handle_pad_edges(oot::input::devices::take_pad_edges(0));
        // The launcher's requests: a ROM to validate, Play, Quit, Minimize. Main thread, like
        // the runtime. The tick that starts the game gives the window its game shape and lets
        // the settings' window mode through.
        if (oot::launch::tick()) {
            oot::launch::shape_for_game();
            oot::ui::hold_window_mode(false);
        }
        // Whether the window is fullscreen right now, for the frame: no edge to draw there.
        // SDL's flag for the mode SDL set, or the window covering its display for the mode the
        // renderer set by its own means (which left a brass edge around a fullscreen picture
        // until the user's note of 2026-09-19).
        oot::ui::shell::set_fullscreen((SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 ||
                                       oot::launch::covers_display());
    }

    // ------------------------------------------------------------------------------------------
    // The rest
    // ------------------------------------------------------------------------------------------

    void vi_callback() {
        // Counted on EVERY retrace, before anything else and whatever the probe is doing: it is
        // half of the pair that says why the game ran a burst of updates between two pictures.
        oot::game_state::note_vi();

        // The VI is the probe's clock. It keeps counting through a slow frame, which is
        // exactly when audio goes wrong, so it is a better tick source than the frame.
        oot::probe::tick();

        // Report the game's own state when it CHANGES, which is the useful moment and also the
        // cheap one: a line per transition rather than sixty lines a second of the same numbers.
        //
        // This exists because of a crash that took three runs to place. The harness could say it
        // died at round 30 and could not say where the game was, so a scene transition stayed a
        // theory. An entrance index in the log a moment before a crash turns that into a fact.
        if (!oot::probe::enabled()) {
            return;
        }

        // The rate the renderer believes the display runs at, which is what it interpolates
        // toward when the frame rate setting asks for the display. Printed when it changes, so a
        // zero here (the swap chain never reported one) explains a picture that will not smooth.
        static uint32_t last_display_rate = UINT32_MAX;
        const uint32_t display_rate = ultramodern::get_display_refresh_rate();
        if (display_rate != last_display_rate) {
            last_display_rate = display_rate;
            std::fprintf(stderr, "[gfx] display rate = %u\n", static_cast<unsigned>(display_rate));
        }

        static oot::game_state::Snapshot last{};
        const oot::game_state::Snapshot now = oot::game_state::read();
        if (!now.valid) {
            return;
        }

        if (now.entrance_index != last.entrance_index || now.link_age != last.link_age ||
            now.game_mode != last.game_mode) {
            // "saved_scene" and not "scene", because that is what it is: the scene the FILE was
            // last saved in, which does not change as you move about. Labeling it "scene" made
            // every line of a sweep read 52 and invited exactly the wrong conclusion. The live
            // indicator of where the game is, is the entrance index.
            std::fprintf(stderr, "[state] entrance 0x%04X  age %d  mode %d  saved_scene %d\n",
                         static_cast<unsigned>(now.entrance_index) & 0xFFFF,
                         static_cast<int>(now.link_age),
                         static_cast<int>(now.game_mode),
                         static_cast<int>(now.saved_scene));
        }
        // Watch the object bank's base move. A queued display list carries the base it was built
        // with, so the moment this changes is the moment any list already in flight becomes stale.
        // Seeing the change and the bad list in the same trace is what turns a coincidence of
        // sampling into a sequence.
        for (int seg = 2; seg <= 6; ++seg) {
            if (now.segment[seg] != last.segment[seg]) {
                std::fprintf(stderr, "[seg] segment %d moved from 0x%06X to 0x%06X\n",
                             seg, last.segment[seg], now.segment[seg]);
            }
        }

        // Once, when the boot code has saved what the boot ROM left behind: the values the game's
        // anti-piracy checks compare against (game_state.h says which and where).
        if (now.cic_id != last.cic_id || now.boot_magic0 != last.boot_magic0) {
            std::fprintf(stderr, "[boot] osCicId %d  boot magic 0x%08X (6105 and 0xAD090010 are what a real boot leaves)\n",
                         static_cast<int>(now.cic_id), static_cast<unsigned>(now.boot_magic0));
        }
        if (now.health != last.health || now.rupees != last.rupees) {
            std::fprintf(stderr, "[state] health %d of %d  rupees %d\n",
                         static_cast<int>(now.health),
                         static_cast<int>(now.health_capacity),
                         static_cast<int>(now.rupees));
        }
        if (now.update_rate != last.update_rate) {
            std::fprintf(stderr, "[state] update rate %d (a game frame every %d retraces)\n",
                         static_cast<int>(now.update_rate), static_cast<int>(now.update_rate));
        }
        // The sun's depth test coordinates: printed when they change by more than a few pixels,
        // and loudly when they leave the screen by a wide margin, because the environment's graph
        // callback reads the depth buffer at them without a bounds check and a value like -3000
        // is a read outside the console's memory.
        {
            static int env_lines = 0;
            const int dx = static_cast<int>(now.sun_test_x) - static_cast<int>(last.sun_test_x);
            const int dy = static_cast<int>(now.sun_test_y) - static_cast<int>(last.sun_test_y);
            const bool wild = now.sun_test_x < -400 || now.sun_test_x > 800 || now.sun_test_y < -400 ||
                              now.sun_test_y > 700;
            if ((dx > 8 || dx < -8 || dy > 8 || dy < -8 || wild) && (wild || env_lines < 400)) {
                ++env_lines;
                std::fprintf(stderr, "[env] sun depth test x %d y %d%s\n", static_cast<int>(now.sun_test_x),
                             static_cast<int>(now.sun_test_y), wild ? "  WILD: off the screen by far" : "");
            }
        }
        last = now;
    }
    void gfx_init_callback() {
        // Called once, before the graphics loop begins.
    }

    void message_box(const char* msg) {
        // The runtime still runs its own stored-ROM step when the game starts, and since
        // 2026-09-20 this program does not keep a stored ROM: it reads the user's own file where
        // it lies and hands the runtime the bytes before starting (main/rom_source.h). So that
        // step finds nothing and says so, every single start, in a sentence that reads like a
        // fault and tells the user to restart. It is not a fault, and the ROM is already loaded
        // by the time it is printed, so it is answered here rather than passed on.
        //
        // Matched on the runtime's own wording because that is all there is to match on. If a
        // runtime update changes the sentence the line comes back, which is the safe direction
        // to fail in: a spurious line in the log, never a real one swallowed.
        if (msg != nullptr && std::strstr(msg, "Error opening stored ROM") != nullptr) {
            std::fprintf(stderr, "[runtime] no stored ROM, which is expected: the game was read from the user's own file\n");
            return;
        }
        // Console, not a native dialog: CLAUDE.md forbids OS dialogs, and during bring-up a line
        // that stays in the log beats a modal that has to be dismissed before it can be read.
        std::fprintf(stderr, "[runtime] %s\n", msg);
    }

    std::string get_game_thread_name(const OSThread* t) {
        return "Game " + std::to_string(t->id);
    }

} // namespace

void oot::boot::window_size(int& width, int& height) {
    width = g_window_width.load(std::memory_order_relaxed);
    height = g_window_height.load(std::memory_order_relaxed);
}

void oot::boot::run(SDL_Window* window,
                    ultramodern::renderer::WindowHandle window_handle,
                    int argc,
                    char** argv) {
    g_window = window;
    {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);
        g_window_width.store(width, std::memory_order_relaxed);
        g_window_height.store(height, std::memory_order_relaxed);
    }

    recomp::Configuration cfg{};
    cfg.argc = argc;
    cfg.argv = argv;
    cfg.project_version = recomp::Version{ 0, 1, 0 };
    cfg.window_handle = window_handle;

    cfg.rsp_callbacks = { .get_rsp_microcode = get_rsp_microcode };

    cfg.renderer_callbacks = { .create_render_context = oot::renderer::create_render_context };

    cfg.audio_callbacks = {
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    cfg.input_callbacks = {
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };

    cfg.gfx_callbacks = {
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    cfg.events_callbacks = {
        .vi_callback = vi_callback,
        .gfx_init_callback = gfx_init_callback,
    };

    cfg.error_handling_callbacks = { .message_box = message_box };
    cfg.threads_callbacks = { .get_game_thread_name = get_game_thread_name };

    // ------------------------------------------------------------------------------------------
    // Find the ROM, validate it, and mark the game as started.
    //
    // Without this the runtime comes up, presents dummy frames because is_game_started() is false,
    // and shows a BLACK WINDOW FOREVER with no error: every part of it is working and nothing has
    // been asked to run. That is what the first successful boot attempt actually did.
    // ------------------------------------------------------------------------------------------

    // The interface, brought up before the runtime starts because RT64 reads the render hooks
    // when it creates its device. Installing them afterward is a no-op that looks like a working
    // call. Nothing here can fail the program: if the device refuses the pipeline the interface
    // stays dark and the game is unaffected.
    {
        // Every path comes from one place (src/main/places.h), which has already decided whether
        // this run is portable or installed. The user's settings and bindings are THEIRS and sit
        // in the data folder; assets are the program's and sit in bin\. In a build tree both are
        // the executable's own directory, so the harness reads exactly what it always did.
        // THE PACKS ARE SCANNED BEFORE THE SETTINGS ARE READ, and the order is load bearing:
        // the settings clamp the texture pack row against how many packs exist, so a scan that
        // ran afterward would see an empty list, clamp a person's choice to Off, and save that
        // back over their file. The scan also makes the folder, so Your files can open it on a
        // machine that has never used one.
        oot::packs::rescan();
        oot::ui::load_settings(oot::places::settings_file().string());
        // The lighting controls, in their own file beside the settings (2026-09-24).
        oot::ui::lighting::load((oot::places::data() / "lighting.txt").string());
        // The bindings beside the settings, read the same way: a missing file is the defaults.
        oot::input::set_path(oot::places::controls_file().string());
        oot::input::set_live(oot::input::load(oot::input::path()));
        oot::ui::shell::configure(oot::places::assets().string(), oot::places::settings_file().string());
        oot::ui::render::install_hooks();
        // The one network connection this program makes, and only if the setting says so. It runs
        // on its own thread and nothing waits for it; a failure is a silent no-op. The settings
        // have just been loaded above, which is why it is started here rather than earlier.
        oot::updates::start_if_enabled();
    }

    // --probe <path> turns on the health line. Off by default and absent from a normal run.
    // A flag rather than an environment variable, deliberately: CLAUDE.md rules out reading
    // configuration from the environment, and a flag is visible in the command that produced the
    // file, which is what you want when reading one back later.
    std::filesystem::path cli_rom_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--probe") {
            if (i + 1 < argc) {
                oot::probe::enable(argv[++i]);
                oot::dl_check::enable(true);
                oot::camera_cuts::enable(true);
            }
            else {
                std::fprintf(stderr, "--probe needs a path to write to.\n");
            }
        }
        else if (arg == "--warp") {
            // Go straight to an entrance once the file is loaded. This is what lets a test reach a
            // scene instead of walking to it, and it is a per launch request on purpose: each
            // scene gets a fresh process, so one scene crashing cannot confuse the next.
            if (i + 1 < argc) {
                // A comma separated list, so one launch can visit many scenes. The first is
                // applied as soon as the file is loaded and the rest follow on a dwell.
                const std::string list = argv[++i];
                size_t at = 0;
                bool first = true;
                while (at <= list.size()) {
                    const size_t comma = list.find(',', at);
                    const std::string one = list.substr(at, comma - at);
                    if (!one.empty()) {
                        const int32_t entrance =
                            static_cast<int32_t>(std::strtol(one.c_str(), nullptr, 0));
                        if (first) {
                            oot::game_state::set_pending_warp(entrance);
                            first = false;
                        }
                        else {
                            oot::game_state::queue_pending_warp(entrance);
                        }
                    }
                    if (comma == std::string::npos) {
                        break;
                    }
                    at = comma + 1;
                }
            }
            else {
                std::fprintf(stderr, "--warp needs an entrance index.\n");
            }
        }
        else if (arg == "--autosave-ticks") {
            // The harness's autosave interval in game updates, overriding the row (phase 78),
            // because the row's shortest choice is five minutes and a test should not take that.
            if (i + 1 < argc) {
                oot::moments::set_autosave_override(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10)));
            }
            else {
                std::fprintf(stderr, "--autosave-ticks needs a count.\n");
            }
        }
        else if (arg == "--moment-load") {
            // Resume a moment from a slot once the game is in play and the frame is safe
            // (phase 76). Read and checked now, so a damaged file is refused before anything
            // starts; applied on the first safe frame.
            if (i + 1 < argc) {
                const int slot = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
                oot::moments::request_load(slot);
            }
            else {
                std::fprintf(stderr, "--moment-load needs a slot, 1 to 8.\n");
            }
        }
        else if (arg == "--moment-save") {
            // Capture a moment into a slot once the game is in play and the frame is safe
            // (phase 75). A harness flag: the surface and the quick keys arrive in phase 77. The
            // request outlives a warp's transition, so `--warp <e> --moment-save 1` captures on
            // the far side of the door.
            // `<slot>` or `<slot>,<delay>`: the delay in game updates after play is first seen,
            // so a run can walk before the capture.
            if (i + 1 < argc) {
                const std::string spec = argv[++i];
                const size_t comma = spec.find(',');
                const int slot = static_cast<int>(std::strtol(spec.c_str(), nullptr, 10));
                const uint32_t delay = (comma == std::string::npos) ? 0u
                    : static_cast<uint32_t>(std::strtoul(spec.c_str() + comma + 1, nullptr, 10));
                oot::moments::request_capture(slot, 3000, "", delay);
            }
            else {
                std::fprintf(stderr, "--moment-save needs a slot, 1 to 8.\n");
            }
        }
        else if (arg == "--transition") {
            // The transition type the warps use, as the game numbers them (0 the wipe, 1 the
            // triforce, 2 the fade to black, 0x20 the circle). A harness flag for the phase that
            // draws a transition in a wide frame, since in play each type is reached only
            // through particular doors.
            if (i + 1 < argc) {
                oot::game_state::set_warp_transition(
                    static_cast<int32_t>(std::strtol(argv[++i], nullptr, 0)));
            }
            else {
                std::fprintf(stderr, "--transition needs a type.\n");
            }
        }
        else if (arg == "--dwell") {
            // Game frames between queued warps, twenty to a second. The default is a few
            // seconds, which suits a sweep that only wants to see each scene load; a harness
            // that opens menus and walks about in each scene asks for more.
            if (i + 1 < argc) {
                oot::game_state::set_warp_dwell(
                    static_cast<int>(std::strtol(argv[++i], nullptr, 0)));
            }
            else {
                std::fprintf(stderr, "--dwell needs a frame count.\n");
            }
        }
        else if (arg == "--seed") {
            // The seed the game takes at every scene load in place of its clock (phase 52), so
            // two runs of one scene spawn and animate it the same. A harness flag; see shots.h.
            if (i + 1 < argc) {
                oot::shots::set_seed(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
            }
            else {
                std::fprintf(stderr, "--seed needs a number.\n");
            }
        }
        else if (arg == "--freeze") {
            // `<entrance>,<ticks>`: stop the game's updates that many updates after arriving at
            // that entrance, so every frame presented from then on is the same frame and a
            // capture is the same picture whenever it is taken (phase 52). A harness flag.
            if (i + 1 < argc) {
                const std::string spec = argv[++i];
                const size_t comma = spec.find(',');
                const int32_t entrance = static_cast<int32_t>(std::strtol(spec.c_str(), nullptr, 0));
                const uint32_t ticks = (comma == std::string::npos) ? 100u
                    : static_cast<uint32_t>(std::strtoul(spec.c_str() + comma + 1, nullptr, 0));
                oot::shots::set_freeze(entrance, ticks);
            }
            else {
                std::fprintf(stderr, "--freeze needs an entrance and a count.\n");
            }
        }
        else if (arg == "--gpu-breadcrumbs") {
            // Device Removed Extended Data (phase 53): a hung or faulting GPU then reports, in
            // the crash report, the command it stopped on and the allocation a page fault hit.
            oot::renderer::request_gpu_breadcrumbs();
        }
        else if (arg == "--rt-level") {
            // The ray tracing level for this run (phase 53): 0 Off, above 0 traces. A harness
            // flag until the settings row of phase 58; ignored by a build without the path.
            if (i + 1 < argc) {
                oot::renderer::request_raytracing_level(static_cast<int>(std::strtol(argv[++i], nullptr, 0)));
            }
            else {
                std::fprintf(stderr, "--rt-level needs a number.\n");
            }
        }
        else if (arg == "--record") {
            // A frame recording started by the game itself once it has run this many updates in
            // play (phase 57): `--record <seconds>,<play ticks>`. The harness's way to record a
            // moment it cannot click the recorder's plate for.
            if (i + 1 < argc) {
                const std::string spec = argv[++i];
                const std::size_t comma = spec.find(',');
                const int seconds = static_cast<int>(std::strtol(spec.c_str(), nullptr, 0));
                const uint32_t ticks = (comma == std::string::npos) ? 0u : static_cast<uint32_t>(std::strtoul(spec.c_str() + comma + 1, nullptr, 0));
                oot::recorder::request_at_play_ticks(seconds, ticks);
            }
            else {
                std::fprintf(stderr, "--record needs <seconds>,<play ticks>.\n");
            }
        }
        else if (arg == "--video") {
            // A video recorded by the game itself (main/video.h): `--video <start>,<stop>` or
            // `--video <start>,<pause>,<resume>,<stop>`, in game updates in play.
            uint32_t ticks[4] = {};
            int count = 0;
            if (i + 1 < argc) {
                const char* at = argv[++i];
                while (*at != 0 && count < 4) {
                    char* end = nullptr;
                    ticks[count++] = static_cast<uint32_t>(std::strtoul(at, &end, 0));
                    at = (end != nullptr && *end == ',') ? end + 1 : (end != nullptr ? end : at + 1);
                    if (end != nullptr && *end == 0) {
                        break;
                    }
                }
            }
            oot::video::request_at_play_ticks(ticks, count);
        }
        else if (arg == "--rt-debug") {
            // The debug view that replaces the picture while tracing (lighting-design.md §8):
            // distance, normal, position, instance, diffuse, shadow, direct, indirect,
            // reflection, flow, or final.
            if (i + 1 < argc) {
                oot::renderer::request_raytracing_view(argv[++i]);
            }
            else {
                std::fprintf(stderr, "--rt-debug needs a view.\n");
            }
        }
        else if (arg == "--freeze-clock") {
            // The game's 16 bit clock to set when the frozen run arrives at its entrance
            // (0x8000 is noon), so the sky and the light are the same in every run.
            if (i + 1 < argc) {
                oot::shots::set_clock(static_cast<int32_t>(std::strtol(argv[++i], nullptr, 0)) & 0xFFFF);
            }
            else {
                std::fprintf(stderr, "--freeze-clock needs the clock.\n");
            }
        }
        else if (arg == "--present-early") {
            // Ask the renderer to present each frame as soon as it is rendered rather than at the
            // game's own buffer swap. A harness flag while the frame interpolation is being
            // brought up: the renderer only generates frames between the game's when the image it
            // presents is one the frame drew, and this is the presentation mode that makes that
            // true by construction.
            oot::renderer::request_present_early();
        }
        else if (arg == "--virtual-controller") {
            // Attach an SDL virtual joystick and drive it from a script of timed events: the
            // harness's hardware-free way through the device layer. Absent from a normal run.
            if (i + 1 < argc) {
                g_virtual_script = argv[++i];
            }
            else {
                std::fprintf(stderr, "--virtual-controller needs a script path.\n");
            }
        }
        else if (arg == "--virtual-raw") {
            g_virtual_raw = true;
        }
        else if (arg == "--play") {
            g_play_now = true;
        }
        // Windows runs this when somebody picks Uninstall in its installed apps list, and the
        // launcher's own Your files row reaches the same surface. It is NOT a silent removal:
        // the program comes up on that surface with the real paths listed and a button that has
        // to be pressed twice, because deleting somebody's folder without showing them what is
        // in it first is not a thing this program does.
        else if (arg == "--uninstall") {
            g_open_files = true;
        }
        else if (arg == "--controls-selftest") {
            // Exercise the bindings file without the game and exit with its verdict: 0 when a
            // file full of every kind of value, garbage included, reads back clamped, round trips,
            // and a missing file is the defaults. The scratch file is written beside the
            // executable and removed.
            std::exit(oot::input::selftest((oot::launch::executable_directory() / "controls.selftest.txt").string()));
        }
        else if (arg == "--registry-selftest") {
            // Exercise the actor registry without the game and exit with its verdict: 0 when
            // it filled to capacity, reported FULL exactly once, released, and reset cleanly.
            std::exit(oot::actor_registry::self_test());
        }
        else if (arg.rfind("--", 0) == 0) {
            // Something for the runtime's own parser. It ignores what it does not know, and so
            // does this loop; what matters here is not mistaking it for the ROM path.
            if (arg == "--game" || arg == "--game-mode") {
                ++i;
            }
        }
        else if (cli_rom_path.empty()) {
            cli_rom_path = arg;
        }
    }

    // The pads, now that the flags are known: the mapping database beside the executable, the
    // virtual pad if a script was given, and every pad already plugged in. The bindings were
    // loaded above; a pad's profile joins them the moment it is seen.
    oot::input::devices::init((oot::places::assets() / "input" / "gamecontrollerdb.txt").string(),
                              g_virtual_script, g_virtual_raw);

    const std::u8string game_id = oot::game_entry().game_id;

    // The ROM: a stored copy from an earlier run, the path on the command line, or any file
    // beside the executable that validates as the game (main/launch.cpp, judged by content and
    // never by name). With --play the game starts at once, the harness's way; otherwise the
    // runtime comes up with the launcher over its dummy frames and Play starts the game from
    // there, which is the reference project's own shape.
    oot::launch::init(game_id, cli_rom_path);

    // The window's shape, then the window: as the launcher, a small borderless window with our
    // own chrome and the settings' window mode held back until Play; with --play, the game's
    // window as it has always been. Shown here rather than in main.cpp so it never appears in
    // one shape and jumps to the other.
    oot::launch::attach_window(window);
    if (!g_play_now) {
        oot::launch::shape_for_launcher();
        oot::ui::hold_window_mode(true);
    }
    else {
        oot::launch::shape_for_game();
    }
    SDL_ShowWindow(window);

    if (g_play_now) {
        if (!oot::launch::rom_ready()) {
            std::fprintf(stderr,
                         "No ROM found. Put the NTSC-U 1.0 ROM beside the executable, pass its path as the\n"
                         "first argument, or run without --play and choose it in the launcher.\n");
            return;
        }
        if (!oot::launch::load_rom_into_runtime()) {
            std::fprintf(stderr, "The ROM could not be read.\n");
            return;
        }
        std::printf("ROM loaded, %zu bytes.\n", recomp::get_rom().size());

        // Marks the game as running so the runtime boots it rather than idling on dummy frames.
        recomp::start_game(game_id, "");
        oot::launch::mark_started();
        std::printf("Starting the runtime. This is the first time the game's own code runs.\n\n");
    }
    else if (g_open_files) {
        oot::ui::shell::post_open(oot::ui::shell::Screen::Files);
        std::printf("Starting the runtime on the files surface, asked for by --uninstall.\n\n");
    }
    else {
        std::printf("Starting the runtime with the launcher. Play starts the game.\n\n");
    }

    // Does not return until the game exits: the runtime takes over the thread.
    recomp::start(cfg);

    oot::probe::close();

    if (g_audio_device != 0) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
}
