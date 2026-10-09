// OoT: Recompiled, entry point.
//
// Phase 13 scope: open a window, stand an RT64 render context up against it, and report what the
// renderer actually chose. The game is not registered and no ROM is read yet; Phase 14 links the
// recompiler output and Phase 15 onward makes it boot.
//
// Crash diagnostics are wired here rather than later, per CLAUDE.md's "day one" rule. There is no
// renderer process to lose yet, but the point of the rule is that the handler exists BEFORE the
// first thing that can crash, not after the first crash nobody could explain.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <io.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/overlays.hpp"

#include <filesystem>

#include "game/render.h"
#include "game/game.h"
#include "main/boot.h"
#include "main/crash_handler.h"
#include "main/launch.h"
#include "build_info.h"
#include "main/places.h"
#include "main/install.h"
#include "main/recorder.h"
#include "main/video.h"
#include "main/updates.h"
#include "game/moments.h"

// Implemented in src/game/recomp_api.cpp, reached by patches through patches/syms.ld.
extern "C" void recomp_load_overlays(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_take_warp(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_actor_register(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_actor_index(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_actor_release(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_actor_reset(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_camera_smooth(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_wide_frame_scale_q16(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_warp_transition(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_take_day_time(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_moment_pending(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_moment_captured(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_moment_refused(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_moment_pending_load(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_moment_load_into(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_moment_boot_load(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_note_play_tick(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_harness_frozen(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_harness_tick(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_take_seed(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_note_dropped_frame(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_prerendered_room(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_render_distance_q16(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_light_context(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_traced_shadows(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_free_camera_mode(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_message_assist(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_low_health_beep(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_free_camera_stick(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_free_camera_holds(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_caster_reach(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_letterbox(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_traced_light_effects(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_glow_test(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_window_rays_hidden(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_photo_mode(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_photo_picture_room(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_photo_input(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_photo_hud_hidden(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_hud_note(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_hud_part(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_hud_layout(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_hud_place(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_hud_frame_q16(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_air_movers(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_room_pieces_begin(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_room_piece_add(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_room_pieces_present(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_room_piece_state(uint8_t* rdram, recomp_context* ctx);

namespace {

    constexpr int DEFAULT_WIDTH = 960;   // 4x the console's 240 lines, at its 4:3 ratio
    constexpr int DEFAULT_HEIGHT = 720;

    // setup_result_name and api_name lived here while this file stood the renderer up itself.
    // The runtime creates the render context through a callback now, so it reports its own
    // setup, and keeping unused copies here would be dead code the compiler is right to refuse.

} // namespace

int main(int argc, char** argv) {

    // FIRST OF ALL, because every path below depends on the answer: which shape is this program
    // running in. A `.installed` marker beside the executable means the setup file put us in the
    // user's application data, with program files in bin\ and the user's own files beside it.
    // No marker means portable: everything together in this folder, nothing written anywhere
    // else, which is the build tree, the harness and the portable download. See src/main/places.h.
    oot::places::init();

    // THEN, before the crash handler and before anything that can fail: where everything
    // printed goes. Under the Windows subsystem there is no console. When nothing handed us
    // standard handles (a double click, a shortcut), everything printed goes to a log beside
    // the executable, written fresh each run; when something did (the harness redirecting them
    // into its trace files), they are kept exactly as they are.
    //
    // Both streams end up on ONE open file sharing ONE file position, and each stays a valid
    // stream whatever happens. The CRT closes a stream before it reopens it, so a reopen that
    // fails leaves the stream dead, and the first print through a dead stream is an invalid
    // parameter, which the release CRT answered, before crash_handler.cpp hooked it, by ending
    // the process on the spot with nothing printed. That is what a double click on 0.1.0 did:
    // freopen_s opens its file exclusively, the second stream's reopen of the same file was
    // refused, stdout died, and the first printf killed the program before its window. The
    // harness always passes handles, so it never saw it; tools/harness/dblclick.ps1 does now.
    // So: stderr reopens the log (or the null device when the folder refuses a file), stdout
    // reopens the null device, and its descriptor is then made a duplicate of stderr's, which
    // shares the file position, so the two streams' lines land in the order they were printed.
    {
        const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        const bool no_err = (err == nullptr || err == INVALID_HANDLE_VALUE);
        const bool no_out = (out == nullptr || out == INVALID_HANDLE_VALUE);
        if (no_err && no_out) {
            // In the user's own folder, not the program's: a log is something they may be asked
            // to send, so it belongs where the rest of their files are rather than inside bin\,
            // which an update replaces wholesale. In portable mode the two are the same place.
            const std::wstring log = (oot::places::data() / L"oot-recompiled.log").wstring();
            FILE* file = nullptr;
            const bool have_log = (_wfreopen_s(&file, log.c_str(), L"w", stderr) == 0);
            if (!have_log) {
                (void)_wfreopen_s(&file, L"NUL", L"w", stderr);
            }
            if (_wfreopen_s(&file, L"NUL", L"w", stdout) == 0 && have_log) {
                (void)_dup2(_fileno(stderr), _fileno(stdout));
            }
        }
    }

    // Unbuffered, deliberately. Everything printed before the window comes up is boot diagnostics,
    // and the case that matters is the one where the program dies before it would have flushed: a
    // buffered report of how far it got is lost exactly when it was needed. The cost is nothing,
    // because this stream carries a dozen lines at startup and nothing during play.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // The first line names the build and the moment, so an empty log means the program never
    // reached this point (a missing DLL, which Windows reports itself) and a log that stops
    // says how far it got.
    {
        char stamp[32] = "?";
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        if (localtime_s(&local, &now) == 0) {
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        }
        std::printf("OoT: Recompiled %s, started %s\n", oot::build_info::version, stamp);
    }

    std::printf("[places] running %s, program %s, data %s\n",
                oot::places::mode_word(),
                oot::places::program().string().c_str(),
                oot::places::data().string().c_str());

    oot::crash::install();

    // Keep the Start menu entry and Windows' installed apps entry current. The setup file wrote
    // them, and this refreshes them on every run, so a shortcut somebody deleted comes back and
    // the listed version never names a build that has since been replaced. A no-op when portable,
    // which installs nothing and so registers nothing.
    {
        wchar_t self[MAX_PATH * 4] = {};
        const DWORD length = GetModuleFileNameW(nullptr, self,
                                                static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
        if (length != 0) {
            oot::install::register_for(std::filesystem::path(std::wstring(self, length)));
        }
    }

    // --crash-test <kind>: one ending made to happen here, before anything else, so a check can
    // prove its report reaches the log from the earliest moment (tools/harness/dblclick.ps1).
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--crash-test") == 0) {
            if (!oot::crash::self_test(argv[i + 1])) {
                std::fprintf(stderr, "--crash-test needs one of: invalid-parameter, terminate, abort, "
                                     "purecall, access-violation\n");
                return 2;
            }
            std::printf("[crash-test] %s: the program goes on\n", argv[i + 1]);
            return 0;
        }
    }

    // --moment-dump <file>: print a saved moment's header and leave (phase 75). No window, no game.
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--moment-dump") == 0) {
            return oot::moments::dump(argv[i + 1]);
        }
    }

    // The PlayStation 4 and 5 pads rumble only through SDL's HIDAPI drivers, and the hints have
    // to be set before the joystick subsystem starts (phase 48).
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // A setup file fetched and verified by a previous session waits in the data folder; if it
    // names a newer version and is still signed by us, it runs now and this copy leaves before
    // any window opens. That is the "installs at next start" the update plate promised (the
    // user's ask of 2026-09-23). Portable copies never take this path.
    if (oot::updates::install_pending()) {
        return 0;
    }

    SDL_Window* window = SDL_CreateWindow(
        "OoT: Recompiled",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DEFAULT_WIDTH, DEFAULT_HEIGHT,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);

    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // RT64 wants the native handle and the thread that owns the window, not the SDL wrapper.
    SDL_SysWMinfo wmInfo{};
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
        std::fprintf(stderr, "SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    ultramodern::renderer::WindowHandle window_handle{};
    window_handle.window = wmInfo.info.win.window;
    window_handle.thread_id = GetCurrentThreadId();

    // The window's icon: the mark, from the executable's own resource (src/main/app.rc), at the
    // two sizes the title bar and the taskbar use. SDL registers its window class with the
    // stock application icon, so the mark has to be set on the window itself.
    {
        HMODULE self = GetModuleHandleW(nullptr);
        HICON big = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
        HICON small = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        if (big != nullptr) {
            SendMessageW(wmInfo.info.win.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        }
        if (small != nullptr) {
            SendMessageW(wmInfo.info.win.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
        }
        if (big == nullptr && small == nullptr) {
            std::fprintf(stderr, "The window icon could not be loaded from the executable.\n");
        }
    }

    // rdram is the emulated memory the game will live in. Recompiled code addresses THROUGH it
    // rather than using host pointers, which is what keeps a bug in thirty year old game code from
    // corrupting this process. Nothing runs in it yet; RT64 only needs the base pointer.
    //
    // recomp::mem_size is the runtime's own figure (the kseg0 size, 512MB), not a number chosen
    // here. The console had 4MB or 8MB; the extra room is where the runtime puts the PI handles and
    // anything a patch allocates, so sizing this by the console's RAM would be wrong in a way that
    // only shows up much later.
    auto rdram = std::make_unique<uint8_t[]>(recomp::mem_size);

    // Export the native functions patches call. A patch reaches these through a fake address in
    // patches/syms.ld; this is what puts a real function behind that name. A missing registration
    // is a link error in the main binary, which is the right time to find out.
    recomp::overlays::register_base_export("recomp_load_overlays", recomp_load_overlays);
    recomp::overlays::register_base_export("recomp_take_warp", recomp_take_warp);
    // The actor registry, for the transform tagging patches (post-parity phase 35).
    recomp::overlays::register_base_export("recomp_actor_register", recomp_actor_register);
    recomp::overlays::register_base_export("recomp_actor_index", recomp_actor_index);
    recomp::overlays::register_base_export("recomp_actor_release", recomp_actor_release);
    recomp::overlays::register_base_export("recomp_actor_reset", recomp_actor_reset);
    // The camera's smooth-or-cut verdict (post-parity phase 36).
    recomp::overlays::register_base_export("recomp_camera_smooth", recomp_camera_smooth);
    // How much wider than 4:3 the frame is, for the 3D transitions (post-parity phase 43), and
    // the transition type the harness warp uses so each of those transitions can be reached.
    recomp::overlays::register_base_export("recomp_wide_frame_scale_q16", recomp_wide_frame_scale_q16);
    recomp::overlays::register_base_export("recomp_warp_transition", recomp_warp_transition);
    // The debug menu's time of day, applied by the same patch that takes a warp.
    recomp::overlays::register_base_export("recomp_take_day_time", recomp_take_day_time);
    recomp::overlays::register_base_export("recomp_note_play_tick", recomp_note_play_tick);
    recomp::overlays::register_base_export("recomp_harness_frozen", recomp_harness_frozen);
    recomp::overlays::register_base_export("recomp_harness_tick", recomp_harness_tick);
    recomp::overlays::register_base_export("recomp_take_seed", recomp_take_seed);
    recomp::overlays::register_base_export("recomp_note_dropped_frame", recomp_note_dropped_frame);
    // Saved moments (phase 75).
    recomp::overlays::register_base_export("recomp_moment_pending", recomp_moment_pending);
    recomp::overlays::register_base_export("recomp_moment_captured", recomp_moment_captured);
    recomp::overlays::register_base_export("recomp_moment_refused", recomp_moment_refused);
    recomp::overlays::register_base_export("recomp_moment_pending_load", recomp_moment_pending_load);
    recomp::overlays::register_base_export("recomp_moment_load_into", recomp_moment_load_into);
    recomp::overlays::register_base_export("recomp_moment_boot_load", recomp_moment_boot_load);
    recomp::overlays::register_base_export("recomp_prerendered_room", recomp_prerendered_room);
    recomp::overlays::register_base_export("recomp_render_distance_q16", recomp_render_distance_q16);
    // The scene's light list, for the ray traced local lights (upgrades phase 55).
    recomp::overlays::register_base_export("recomp_light_context", recomp_light_context);
    // Whether the traced shadows stand, so the game hides its painted ones (upgrades phase 57).
    recomp::overlays::register_base_export("recomp_traced_shadows", recomp_traced_shadows);
    // The free camera on the right stick (upgrades phase 79, patches/free_camera.c).
    recomp::overlays::register_base_export("recomp_free_camera_mode", recomp_free_camera_mode);
    // The text rows and the Low health beep row (patches/message_assist.c, patches/health_beep.c).
    recomp::overlays::register_base_export("recomp_message_assist", recomp_message_assist);
    recomp::overlays::register_base_export("recomp_low_health_beep", recomp_low_health_beep);
    recomp::overlays::register_base_export("recomp_free_camera_stick", recomp_free_camera_stick);
    recomp::overlays::register_base_export("recomp_free_camera_holds", recomp_free_camera_holds);
    // The Shadow casters row (patches/culling.c, patches/room_behind.c).
    recomp::overlays::register_base_export("recomp_caster_reach", recomp_caster_reach);
    recomp::overlays::register_base_export("recomp_letterbox", recomp_letterbox);
    // The traced light effects (patches/beam_zone.c, patches/effect_lights.c).
    recomp::overlays::register_base_export("recomp_traced_light_effects", recomp_traced_light_effects);
    // The Glow test row (patches/light_glow.c).
    recomp::overlays::register_base_export("recomp_glow_test", recomp_glow_test);
    // The painted rays through the Temple of Time's side windows (patches/window_rays.c).
    recomp::overlays::register_base_export("recomp_window_rays_hidden", recomp_window_rays_hidden);
    // Photo mode (patches/photo_mode.c, patches/photo_audio.c).
    recomp::overlays::register_base_export("recomp_photo_mode", recomp_photo_mode);
    recomp::overlays::register_base_export("recomp_photo_picture_room", recomp_photo_picture_room);
    recomp::overlays::register_base_export("recomp_photo_input", recomp_photo_input);
    recomp::overlays::register_base_export("recomp_photo_hud_hidden", recomp_photo_hud_hidden);
    // The HUD's fading (patches/hud_anchoring.c).
    recomp::overlays::register_base_export("recomp_hud_note", recomp_hud_note);
    recomp::overlays::register_base_export("recomp_hud_part", recomp_hud_part);
    recomp::overlays::register_base_export("recomp_hud_layout", recomp_hud_layout);
    recomp::overlays::register_base_export("recomp_hud_place", recomp_hud_place);
    recomp::overlays::register_base_export("recomp_hud_frame_q16", recomp_hud_frame_q16);
    // The actors that moved through the air (patches/air_movers.c).
    recomp::overlays::register_base_export("recomp_air_movers", recomp_air_movers);
    // The painted elements of the rooms where the player is (patches/room_pieces.c).
    recomp::overlays::register_base_export("recomp_room_pieces_begin", recomp_room_pieces_begin);
    recomp::overlays::register_base_export("recomp_room_piece_add", recomp_room_piece_add);
    recomp::overlays::register_base_export("recomp_room_pieces_present", recomp_room_pieces_present);
    recomp::overlays::register_base_export("recomp_room_piece_state", recomp_room_piece_state);

    // The patch binary and its sections. The patch code is linked natively and would run
    // without this; a patch's DATA (its constants, its initialized statics) only reaches the
    // emulated memory because the runtime copies this binary in at start. Five phases ran
    // without it before a sun drawn at the camera's eye gave it away (issues.md, PHASE-41).
    oot::register_patches();

    // Register the recompiled sections before anything can need them. The counts are ASSERTED
    // rather than assumed: a table that came out short does not fail here, it fails later as a
    // crash in code that looks correct, because an unregistered overlay never gets relocated.
    oot::register_overlays();

    const size_t sections = oot::registered_section_count();
    const size_t overlays = oot::registered_overlay_count();
    std::printf("Sections registered: %zu, of which %zu are overlays\n", sections, overlays);

    constexpr size_t EXPECTED_SECTIONS = 472;
    constexpr size_t EXPECTED_OVERLAYS = 468;
    if (sections != EXPECTED_SECTIONS || overlays != EXPECTED_OVERLAYS) {
        std::fprintf(stderr,
                     "  MISMATCH: expected %zu sections and %zu overlays. The recompiler output and\n"
                     "  this build disagree, so one of them is stale. Re-run the recompiler.\n",
                     EXPECTED_SECTIONS, EXPECTED_OVERLAYS);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const auto& game = oot::game_entry();
    std::printf("Game: %s\n", game.display_name.c_str());
    std::printf("  internal name  %s\n", game.internal_name.c_str());
    std::printf("  rom hash       0x%016llX\n", static_cast<unsigned long long>(game.rom_hash));
    std::printf("  entrypoint     0x%08X\n", static_cast<unsigned int>(game.entrypoint_address));
    std::printf("  entry fn       %s\n\n", game.entrypoint != nullptr ? "linked" : "MISSING");

    // Register the game with the runtime. From here the runtime owns the ROM, the threads and the
    // main loop; it creates the render context itself through the callback, so nothing is stood up
    // here first.
    if (!recomp::register_game(oot::game_entry())) {
        std::fprintf(stderr, "register_game refused this entry.\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The window is shown by boot::run once it has its shape: the launcher's small one, or
    // the game's.

    // Does not return until the game exits.
    oot::boot::run(window, window_handle, argc, argv);

    std::printf("Shutting down.\n");
    // A recording still running gets its remaining frames written and its threads joined before
    // the window goes. Without this, quitting mid-recording would leave the last few hundred
    // frames in a queue nobody ever drains, and the encoder threads would be torn down under the
    // process rather than stopped.
    oot::recorder::shutdown();
    // A video still recording is finished the same way: its file is closed and playable.
    oot::video::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
