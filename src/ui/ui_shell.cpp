#include "ui/ui_shell.h"
#include "ui/ui_fonts.h"
#include "ui/ui_warps.h"
#include "game/warp_places.h"
#include "game/game_state.h"
#include "main/recorder.h"
#include "main/video.h"
#include "main/microphone.h"
#include "main/sweep.h"
#include "game/moments.h"
#include "game/room_pieces.h"
#include "game/rt_state.h"
#include "ui/ui_controls.h"
#include "ui/ui_render.h"
#include "ui/ui_settings.h"
#include "ui/ui_lighting.h"
#include "ui/ui_system.h"
#include "ui/models/rows_model.h"
#include "ui/models/shell_model.h"
#include "ui/models/credits.h"
#include "ui/prompts/prompts.h"
#include "ui/prompts/photo_guide.h"
#include "ui/keyboard/keyboard.h"
#include "ui/sound/ui_sound.h"
#include "ui_tokens.h"
#include "ui/nav/nav_rml.h"
#include "ui/shell/shell.h"
#include "ui/input/ui_input.h"

#include "main/input.h"
#include "main/launch.h"
#include "main/photo.h"
#include "main/places.h"
#include "main/install.h"
#include "main/shots.h"
#include "main/updates.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <SDL_gamecontroller.h>
#include <SDL_keyboard.h>
#include <SDL_scancode.h>

#include "build_info.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <json/json.hpp>

#include "main/signature.h"

namespace {

    // ------------------------------------------------------------------------------------------
    // The boundary between the game's thread and the render thread
    // ------------------------------------------------------------------------------------------

    struct Command {
        enum class Kind {
            Open, Close, Toggle, Nav, Activity, MouseMove, MouseButton, MouseWheel,
            Text, Key, TextAccept, TextEscape, BrowsePath
        } kind;
        oot::ui::shell::Screen screen = oot::ui::shell::Screen::None;
        oot::ui::shell::Nav nav = oot::ui::shell::Nav::Accept;
        int x = 0;          // MouseMove: the position; MouseButton: the button; Key: the scancode
        int y = 0;
        bool down = false;  // MouseButton; Key: control held
        bool shift = false; // Key
        float delta = 0.0f; // MouseWheel
        std::string text{}; // Text, BrowsePath
    };

    std::mutex g_queue_mutex;
    std::deque<Command> g_queue;

    // Read from the game's thread every frame, so it is an atomic rather than something the
    // mutex guards: a stale answer for one frame is harmless, a stall on the game thread is not.
    std::atomic<bool> g_capturing{ false };
    std::atomic<uint32_t> g_capture_releases{ 0 };

    // Whether the ROM browser's path field has the focus, read by the pump to route the keys.
    std::atomic<bool> g_text_entry{ false };

    // Whether the hint is up, so the pump only posts activity while there is something for it
    // to dismiss rather than on every key press for the life of the program.
    std::atomic<bool> g_hint_wants_activity{ false };

    void post(const Command& command) {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        // A bound on the queue, because the game thread can post faster than the render thread
        // drains if a frame takes a long time. Dropping the oldest navigation is better than
        // growing without limit and then replaying a hundred stale key presses at once.
        if (g_queue.size() > 64) {
            g_queue.pop_front();
        }
        g_queue.push_back(command);
    }

    // ------------------------------------------------------------------------------------------
    // Render thread state. Nothing below is touched from anywhere else.
    // ------------------------------------------------------------------------------------------

    using Screen = oot::ui::shell::Screen;
    using Nav = oot::ui::shell::Nav;
    namespace controls = oot::ui::controls;
    namespace devices = oot::input::devices;
    namespace launch = oot::launch;
    namespace warps = oot::ui::warps;

    std::string g_assets_dir;
    std::string g_settings_path;

    oot::ui::SystemInterface g_system;
    Rml::Context* g_context = nullptr;
    Rml::ElementDocument* g_settings_doc = nullptr;
    Rml::ElementDocument* g_about_doc = nullptr;
    Rml::ElementDocument* g_controls_doc = nullptr;
    Rml::ElementDocument* g_hint_doc = nullptr;
    Rml::ElementDocument* g_launch_doc = nullptr;
    Rml::ElementDocument* g_files_doc = nullptr;
    Rml::ElementDocument* g_lighting_doc = nullptr;
    Rml::ElementDocument* g_debug_doc = nullptr;
    Rml::ElementDocument* g_hud_doc = nullptr;   // 2026-10-09: the HUD's fading
    Rml::ElementDocument* g_ask_doc = nullptr;
    // How many row elements assets/ui/files.rml carries.
    // Raised to 12 with the texture packs row (2026-09-30). The document holds this many row
    // elements and the shell hides the ones it does not fill, so the two numbers must agree:
    // a row past the last slot is simply never drawn, with nothing to say it was dropped.
    constexpr size_t FILES_ROW_SLOTS = 12;
    // How much of a path fits on one line beside its label at the case's width. Longer than this
    // and the front is dropped rather than the end, because the end is the part that identifies
    // it. An installed path (`...\AppData\Local\OoT Recompiled\saves`) fits whole.
    // In the shell (phase 96) the page's box is narrower than the launcher's old window, so the
    // path keeps its last thirty characters, which still name the folder it is.
    constexpr size_t FILES_PATH_CHARS = 30;
    Rml::ElementDocument* g_browse_doc = nullptr;
    Screen g_screen = Screen::None;

    bool g_started = false;
    bool g_failed = false;
    int g_width = 0;
    int g_height = 0;

    // Until the game has been started, "nothing open" means the launcher (play milestone).
    bool g_game_started = false;
    bool g_defaults_done = false;
    std::string g_rom_seen;        // the ROM's status as last reflected, to notice a change
    std::string g_update_seen;     // the update line as last written, for the same reason
    std::string g_launch_note;     // a note that overrides the ROM's sentence until the next change

    using Clock = std::chrono::steady_clock;

    double seconds_since(Clock::time_point then) {
        return std::chrono::duration<double>(Clock::now() - then).count();
    }

    // The startup hint (phase 50). Five seconds is the user's number. The fade is a little past
    // the slow duration in tokens.rcss, so the document hides after its transition has finished
    // rather than cutting it short.
    constexpr double HINT_SECONDS = 5.0;
    // A state line is read at a glance and then in the way, so it goes sooner than the startup
    // hint, which is teaching something.
    constexpr double NOTICE_SECONDS = 1.8;
    constexpr double HINT_FADE_SECONDS = 0.3;
    bool g_hint_visible = false;
    double g_hint_seconds = HINT_SECONDS;
    bool g_hint_fading = false;
    Clock::time_point g_hint_since;
    std::string g_hint_last;   // what it last said, so a changed shortcut shows it again

    // THE SPLASH (the menus plan, phase 99; the user, 2026-10-12: "Loading for a few seconds before
    // the launher"): shown once, before the launcher, in its window, then fading as the launcher
    // appears beneath it. Any press or click after its first moments skips the rest of the minimum:
    // the program has loaded by the time it can be drawn, so the wait is only for the eye.
    Rml::ElementDocument* g_splash_doc = nullptr;
    enum class SplashState { Off, Showing, Fading };
    SplashState g_splash = SplashState::Off;
    Clock::time_point g_splash_since;
    bool g_resized = false;   // the window or the interface size changed this frame
    bool g_splash_poured = false;
    bool g_splash_skip = false;
    int g_splash_frames = 0;   // frames drawn while it shows, for the pour (poll_splash)
    // The splash stays until the pour has filled and a beat after (tokens.rcss, --splash-pour-*).
    constexpr double SPLASH_MIN_SECONDS = static_cast<double>(oot::ui::tokens::splash_pour_first) +
                                          oot::ui::tokens::splash_pour_hold + oot::ui::tokens::splash_pour_rest +
                                          oot::ui::tokens::splash_pour_after;
    constexpr double SPLASH_SKIP_AFTER_SECONDS = 0.8;
    constexpr double SPLASH_FADE_SECONDS = 0.6;   // tokens.rcss, --splash-fade

    // The controls document.
    int g_cfocus = 0;
    int g_cslot = 0;
    bool g_capture_open = false;       // the capture panel is showing
    bool g_capture_settling = false;   // bound or timed out: the panel lingers a moment
    Clock::time_point g_capture_since;
    int g_pads_seen = 0;
    constexpr double CAPTURE_LINGER_SECONDS = 1.2;
    constexpr int CAPTURE_SECONDS = 5;   // the device layer's timeout, for the countdown
    // A scrolling list of rows takes this share of the window's height: a share of the window
    // rather than of the case, because the case is sized by its contents.
    constexpr float ROWS_SHARE = 0.58f;
    // About's scrolling body: a smaller share, since its case also holds a prose lid and a foot,
    // and the case's own limit is 78% of the window (tokens.rcss).
    constexpr float ABOUT_SHARE = 0.50f;
    // One press of up or down in About, in pixels.
    constexpr float ABOUT_SCROLL_STEP = 48.0f;

    // The ROM browser.
    std::filesystem::path g_browse_dir;   // empty is the drives
    std::vector<launch::Entry> g_browse;
    int g_bfocus = 0;

    // The game window's frame (play milestone): a brass edge in a window, and the header (the
    // mark, the title, minimize, maximize, close) riding over the picture when the pointer is
    // in the top band, for a while after it leaves, and for a few seconds when the game window
    // first appears, so nothing covers play the rest of the time.
    Rml::ElementDocument* g_frame_doc = nullptr;
    // The frame rate counter's plate (2026-09-19), placed against the frame's title bar.
    Rml::ElementDocument* g_counter_doc = nullptr;
    Rml::ElementDocument* g_guide_doc = nullptr;   // photo mode's button guide (2026-10-13)
    Rml::ElementDocument* g_letterbox_doc = nullptr;

    // The frame recorder's plate (2026-09-22): the counter's twin in the opposite corner, and
    // unlike the counter it is a button. Shown while the recorder row is on and the game runs.
    Rml::ElementDocument* g_recorder_doc = nullptr;
    Rml::ElementDocument* g_update_doc = nullptr;
    // Up here beside the document rather than down with the rest of the recorder's state, because
    // note_pointer reads it: the bar's own logic has to know whether there is a plate to be on.
    bool g_recorder_shown = false;
    bool g_update_shown = false;

    // The debug menu: every warp, nested by location.
    Rml::ElementDocument* g_warps_doc = nullptr;
    Rml::ElementDocument* g_moments_doc = nullptr;
    // The Moments screen (phase 77): the focused slot (0 the autosave, 1 to 8), whether each
    // slot holds a file, and whether Delete has been pressed once.
    int g_mfocus = 1;
    // Indexed by slot, which runs from AUTOSAVE_OLDEST (negative) to SLOT_COUNT and then the
    // reserved from 101, so every read goes through moment_index() rather than the slot itself.
    bool g_moment_present[oot::moments::SLOT_TOTAL] = {};
    bool g_moment_delete_armed = false;

    // THE SCREEN HAS TWO ZONES (2026-09-25, the user could not select a moment and load it).
    // The rows are one, the buttons beneath them the other. A press on a row makes it the
    // selection and moves the cursor to the buttons, where the next press is Load or Cancel;
    // Back from the buttons returns to the rows with the selection kept. Mouse, keyboard and pad
    // all drive the same two zones, so none of them can reach a state the others cannot.
    enum class MomentZone { Rows, Buttons };
    MomentZone g_mzone = MomentZone::Rows;
    int g_mbutton = 0;   // an index into the buttons the current state offers

    // The rows in the order they are shown: the autosaves newest first, the player's, then the
    // reserved that hold a moment (2026-10-13). Twenty empty reserved rows would be a long walk to
    // nothing, and a reserved slot is only ever filled by moving a moment into it, never chosen.
    std::vector<int> moment_order() {
        std::vector<int> order;
        for (int slot = oot::moments::AUTOSAVE_SLOT; slot >= oot::moments::AUTOSAVE_OLDEST; --slot) {
            order.push_back(slot);
        }
        for (int slot = 1; slot <= oot::moments::SLOT_COUNT; ++slot) {
            order.push_back(slot);
        }
        for (int slot = oot::moments::RESERVED_FIRST; slot <= oot::moments::RESERVED_LAST; ++slot) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(oot::moments::slot_file(slot, ".moment"), ec)) {
                order.push_back(slot);
            }
        }
        return order;
    }

    int moment_index(int slot) {
        return oot::moments::slot_index(slot);
    }

    bool moment_present(int slot) {
        return oot::moments::slot_exists(slot) && g_moment_present[moment_index(slot)];
    }
    std::atomic<bool> g_fullscreen{ false };
    bool g_frame_shown = false;
    bool g_pointer_in_band = false;
    Clock::time_point g_band_left_at;
    Clock::time_point g_frame_since;
    bool g_header_shown = false;
    bool g_edge_shown = false;
    constexpr int HEADER_BAND = 30;   // tokens.rcss's --chrome-bar, in pixels
    constexpr double HEADER_LINGER_SECONDS = 1.2;
    constexpr double HEADER_INTRO_SECONDS = 4.0;

    void show(Screen screen);

    void say(const char* what) {
        std::fprintf(stderr, "[ui] %s\n", what);
    }

    // Fonts, in order of preference. Ours first when they exist, then the faces the design
    // decision itself names as fallbacks, which are the ones Windows already has.
    //
    // NOTHING IS BUNDLED. A typeface is somebody's work, and shipping one we have not licensed
    // would be exactly the kind of thing the asset rules exist to prevent. Dropping licensed
    // files into assets/fonts is all it takes to use them instead.
    Rml::ElementDocument* load(const char* name) {
        const std::string path = g_assets_dir + "/ui/" + name;
        if (!std::filesystem::exists(path)) {
            std::fprintf(stderr, "[ui] %s is missing, so that screen will not open\n", path.c_str());
            return nullptr;
        }
        Rml::ElementDocument* doc = g_context->LoadDocument(path);
        if (doc == nullptr) {
            std::fprintf(stderr, "[ui] %s would not load; the rml log above says why\n", path.c_str());
        }
        return doc;
    }

    // Text into a document goes through here: RmlUi reads RML, so a name that happens to hold
    // an ampersand or an angle bracket (a pad's name is whatever its maker wrote, a file's name
    // is whatever the user typed) has to be escaped or it would be parsed as markup.
    std::string escape(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (const char c : text) {
            switch (c) {
                case '&':  out += "&amp;"; break;
                case '<':  out += "&lt;"; break;
                case '>':  out += "&gt;"; break;
                default:   out += c; break;
            }
        }
        return out;
    }

    void set_text(Rml::ElementDocument* doc, const std::string& id, const std::string& text) {
        if (doc == nullptr) {
            return;
        }
        if (Rml::Element* e = doc->GetElementById(id)) {
            e->SetInnerRML(escape(text));
        }
    }

    // An action row's value: the text sits in a span the case's stylesheet keeps hidden until
    // the row is focused or under the pointer, so the rows read quietly at rest.
    // Is a row already at the bottom or the top of its range? A step that cannot go anywhere is
    // drawn as spent rather than removed, so the row does not change shape as it is walked.
    // Both kinds of row start at zero: a choice at its first option, a percentage at silence.
    bool at_low(int, int value) {
        return value <= 0;
    }
    // The top end differs: a choice ends at its last option, a percentage at a hundred.
    bool at_high(int row, int value) {
        const int count = oot::ui::option_count(row);
        return count == 0 ? value >= 100 : value >= count - 1;
    }

    // A value with a step either side of it. The ids carry the ROW so a click knows what it is
    // stepping without the listener having to look anything up.


    // A path for a document, in UTF-8 whatever the user's code page.
    std::string utf8(const std::filesystem::path& path) {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    // A long name cut in the middle, the extension kept, so a row's value stays a row.
    // A PATH shortened the other way round from a name: the interesting end of a path is the
    // last part of it, so what goes is the middle of the front. Used only for display; the click
    // that opens the folder always uses the whole thing.
    // The size of the file the copy offer is about. A person deciding where their files go
    // deserves the number before they answer, not after.
    std::string rom_size_text() {
        std::error_code ec;
        const std::uintmax_t bytes = std::filesystem::file_size(launch::rom_path(), ec);
        if (ec || bytes == 0) {
            return "about 32 MB";
        }
        return std::to_string(bytes / (1024 * 1024)) + " MB";
    }

    std::string shorten_path(const std::string& path, size_t limit) {
        if (path.size() <= limit) {
            return path;
        }
        return "..." + path.substr(path.size() - (limit - 3));
    }

    std::string shorten(const std::string& name, size_t limit) {
        if (name.size() <= limit) {
            return name;
        }
        const size_t dot = name.rfind('.');
        const std::string ext = (dot != std::string::npos && name.size() - dot <= 5) ? name.substr(dot) : "";
        const size_t keep = limit > ext.size() + 3 ? limit - ext.size() - 3 : 0;
        return name.substr(0, keep) + "..." + ext;
    }

    // A click on a document (play milestone: the launcher takes the mouse as well as the keys).
    // The element under the pointer, or a parent of it, says what was clicked by its id: a row
    // of one of the rows documents, or the launcher's own minimize and close. A clicked row is
    // focused and accepted, so one click does what Enter does on it.
    void click_row(Screen screen, int row);
    void click_chrome(const std::string& id);
    // The action bar's two buttons, which are not rows and so are not matched by the row
    // prefixes. Declared here because the row kinds are defined further down.
    void click_action(const std::string& id);
    void click_step(const std::string& id);
    void click_pick(const std::string& id);
    void click_files(const std::string& id);
    void click_moments(const std::string& id);
    void reflect_moments();
    void moments_nav(Nav nav);
    std::string with_bindings(const std::string& note);
    void click_ask(bool yes);
    bool ask_next_question();
    void reflect_files();
    void nudge_setting(int row, int direction);
    void nudge_lighting(int row, int direction);
    void click_recorder();
    void click_update();
    void click_warp_row(int index);
    void click_warp_step(const std::string& id);
    void nudge_time(int by);
    void nudge_piece(int index, int direction);
    // The chosen time as a person reads it. An accessor because the table lives down with the
    // warps code and the row that shows it is drawn long before that point in this file.
    const char* time_label();
    const char* screen_name(Screen screen);
    void apply_rows_focus(Screen screen);
    // The pages the settings document draws: Settings and the settings split out of it (2026-10-13).
    bool settings_like(Screen screen) {
        return screen == Screen::Settings || screen == Screen::Graphics || screen == Screen::Gameplay ||
               screen == Screen::Capture || screen == Screen::Photo || screen == Screen::Video;
    }
    // Takes a row out of a list entirely. Only the files surface needs this now: the settings and
    // launcher lists build their rows from the model, so a row that should not be drawn is simply
    // not emitted, and the default prefix that used to serve them has gone with it.
    void hide_row(Rml::ElementDocument* doc, int row, const char* prefix);
    void show(Screen screen);
    void handle_nav(Nav nav);
    void click_section(const std::string& id);
    void click_prompt(const std::string& id);
    void click_key(const std::string& id);
    void open_keyboard();
    void close_keyboard();
    bool g_kb_open_flag();
    void refresh_shell();
    // The first page of the section the router is on (phase 96): where Back from a page goes.
    Screen section_root_screen();
    // The two warnings the user asked for on 2026-09-26. Declared here because they are raised
    // from the row handlers well above the confirmation machinery they are built on.
    void confirm_debugging(Screen back);
    bool recorder_needs_warning(int row, int wanted);
    void confirm_recorder(int row, Screen back);

    // A PRESS IS THE MOUSE BUTTON COMING UP OVER THE ELEMENT, not the library's own "click".
    //
    // The library makes a click only when the element under the button's release is the very one
    // it made active at the press, and it makes nothing active at all when that element's Focus()
    // is refused (Context::ProcessMouseButtonDown returns before setting `active`), which happens
    // around the rebuilds these documents do on every change: the pressed element is gone by the
    // release, or the focus the press asked for was refused, and the click is silently dropped.
    // The user saw it as "the side arrows to change settings, and even just clicking certain items
    // do not register clicks" (2026-09-24). The mouse up is dispatched to whatever is under the
    // pointer unconditionally, so this listens for that, on the primary button, and every control
    // answers a press the way a pad's button does. The walk up the parents is what lets a press on
    // a glyph or a label count as a press on the thing it belongs to.
    class ClickListener final : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override {
            if (event.GetId() == Rml::EventId::Mouseup && event.GetParameter<int>("button", 0) != 0) {
                return;
            }
            // A click makes the Select sound once it has acted (phase 111), whichever branch below
            // took it; not the window's buttons, the path field, or a press that is already a
            // key's (the modal's close is Back, which sounds as Back).
            struct ClickSound {
                bool armed = false;
                ~ClickSound() {
                    if (armed) {
                        oot::ui::sound::play(oot::ui::sound::Cue::Select);
                    }
                }
            } sound;
            for (Rml::Element* e = event.GetTargetElement(); e != nullptr; e = e->GetParentNode()) {
                const std::string id = e->GetId();
                if (id.empty()) {
                    continue;
                }
                // A BUTTON'S WORDS ARE NEVER THE BUTTON (the user, 2026-10-12: "the "your slots" do
                // not allow clicking/selecting and then choosing save over or delete? its almost like
                // those tow options are never available?"). A button whose words change carries them
                // in a span with an id of its own ("moments-delete-label", so the shell can write
                // "Save over" or "Yes, delete it"), and the Moments match below takes every id that
                // begins "moments-", so a press on the words of Delete, Save over or Load reached it
                // as the label's id, which is no button, and nothing happened; only a press on the
                // button's edge worked. A label passes the press up to the button it sits in, as an
                // unnamed glyph always has. The other screens matched their buttons by exact name and
                // so had been walking past their labels already.
                if ((id.size() > 6) && (id.compare(id.size() - 6, 6, "-label") == 0)) {
                    continue;
                }
                std::fprintf(stderr, "[ui] press on \"%s\"\n", id.c_str());
                sound.armed = id.compare(0, 4, "win-") != 0 && id != "modal-close" && id != "browse-input" &&
                              id != "prompt-accept" && id != "prompt-back" && id != "prompt-change" && id != "prompt-close" &&
                              id != "prompt-close-also";
                if (id == "win-min" || id == "win-max" || id == "win-close") {
                    click_chrome(id);
                    return;
                }
                // The launcher's signature line opens About (the user's ask of 2026-09-19).
                if (id == "launch-signed") {
                    show(Screen::About);
                    return;
                }
                // A modal's own close button (the user's ask of 2026-09-19): what Escape does
                // on the document that is open.
                if (id == "modal-close") {
                    handle_nav(Nav::Back);
                    return;
                }
                // The browser's Browse button: the file picker, run by the main thread.
                if (id == "browse-pick") {
                    launch::request_pick_rom();
                    return;
                }
                // THE WHOLE RECORDER PLATE IS THE BUTTON, which is why this matches the plate's
                // own id and the walk up the parents above is what makes a click on the dot or on
                // the text count as a click on the plate.
                if (id == "recorder") {
                    click_recorder();
                    return;
                }
                // The update plate is a button the same way: a click while the fetched file is
                // ready installs it now rather than at the next start.
                if (id == "update") {
                    click_update();
                    return;
                }
                // The path field itself takes the focus on its own; nothing to do here.
                if (id == "browse-input") {
                    return;
                }
                // The two action buttons. They are drawn outside the list, so the row prefix
                // below never matches them; they look up their own index so a click and Enter
                // run the same code.
                if (id == "act-play" || id == "act-quit") {
                    click_action(id);
                    return;
                }
                // A step chevron. It carries its own row, and it focuses that row as well as
                // changing it, so clicking and walking never disagree about where you are.
                if (id == "ask-yes" || id == "ask-no") {
                    click_ask(id == "ask-yes");
                    return;
                }
                if (id == "files-left" || id == "files-right" || id.compare(0, 5, "frow-") == 0 || id.compare(0, 6, "fopen-") == 0) {
                    click_files(id);
                    return;
                }
                if (id.compare(0, 5, "prev-") == 0 || id.compare(0, 5, "next-") == 0) {
                    click_step(id);
                    return;
                }
                if (id.compare(0, 5, "pick-") == 0) {
                    click_pick(id);
                    return;
                }
                if (id.compare(0, 6, "wprev-") == 0 || id.compare(0, 6, "wnext-") == 0) {
                    click_warp_step(id);
                    return;
                }
                if (id.compare(0, 5, "wrow-") == 0 && id.size() > 5) {
                    click_warp_row(std::atoi(id.c_str() + 5));
                    return;
                }
                if (id.compare(0, 5, "mrow-") == 0 || id.compare(0, 8, "moments-") == 0) {
                    click_moments(id);
                    return;
                }
                // The dial's medallions and the prompts (phase 96): a medallion opens its
                // section, a prompt does what it says.
                if (id.compare(0, 4, "sec-") == 0) {
                    click_section(id);
                    return;
                }
                if (id.compare(0, 4, "key-") == 0) {
                    click_key(id);
                    return;
                }
                if (id.compare(0, 7, "prompt-") == 0) {
                    click_prompt(id);
                    return;
                }
                const char* prefixes[] = { "row-", "brow-", "crow-" };
                for (const char* prefix : prefixes) {
                    const size_t n = std::strlen(prefix);
                    if (id.compare(0, n, prefix) == 0 && id.size() > n) {
                        click_row(g_screen, std::atoi(id.c_str() + n));
                        return;
                    }
                }
            }

            // NOTHING MATCHED, so say what was hit. A click that lands on an element this walk
            // does not recognize is silent otherwise, and that silence is expensive: the frame
            // recorder's plate was drawn, lit and completely dead for an afternoon because every
            // click was being taken by the frame document's body one layer above it (see
            // case.rcss, body.overlay). One line naming the element would have found it at once.
            if (Rml::Element* target = event.GetTargetElement()) {
                std::fprintf(stderr, "[ui] a click on <%s id=\"%s\"> matched nothing\n",
                             target->GetTagName().c_str(), target->GetId().c_str());
            }
        }
    };
    ClickListener g_clicks;

    // A LIST'S ROWS ARE REPLACED WITHOUT LOSING WHERE THE PERSON HAD SCROLLED TO.
    //
    // Every rows document rebuilds its whole list on a change, which puts the list back at its top,
    // and the focus's nearest scroll then drags the changed row to an edge: a row reached by
    // scrolling moves under the pointer with every step. The settings list had this and had it
    // fixed (2026-09-24, the user: "it bounces around the menu scroll position ... any item you
    // must scroll to access"), and the fix lived INLINE in that one rebuild, so the controls list
    // still had it four days later (2026-09-28: "when bounding controls it does the same menu
    // bounce or scroll upward each value change like the other menus used to"), as did the
    // moments, the warps and the browser. It is one function now, used by every one of them, so
    // the next list cannot be built without it.
    //
    // THE LAYOUT BEFORE THE SCROLL. New elements have no geometry until the document is laid out
    // again, so a scroll set before that is clamped against a content height of nothing and the
    // list goes back to its top whatever was asked for (the user, 2026-09-24: "the menu still
    // scrolls up on setting change?"). Hence size_rows, which is whatever the caller does to the
    // list's own height, running BETWEEN the rows and the layout: it changes the geometry the
    // scroll is about to be clamped against.
    void replace_rows(Rml::ElementDocument* doc, Rml::Element* rows, const std::string& rml,
                      const std::function<void()>& size_rows = nullptr) {
        if (doc == nullptr || rows == nullptr) {
            return;
        }
        const float scroll_top = rows->GetScrollTop();
        rows->SetInnerRML(rml);
        if (size_rows) {
            size_rows();
        }
        doc->UpdateDocument();
        rows->SetScrollTop(scroll_top);
    }

    void size_long_rows(Rml::ElementDocument* doc, const char* id, float share = ROWS_SHARE) {
        if (doc == nullptr) {
            return;
        }
        if (Rml::Element* rows = doc->GetElementById(id)) {
            // A PAGE IN THE SHELL (phase 96) is the shell's box high, and its list fills it.
            if (doc->IsClassSet("in-shell")) {
                rows->RemoveProperty(Rml::PropertyId::Height);
                return;
            }
            // In a side panel the case is the window's full height and the stylesheet gives the
            // list everything between the lid and the foot (case.rcss), so no height is set. The
            // launcher's own window (Browse) is never a panel.
            if ((oot::ui::settings().menu_side != 0) && (doc != g_browse_doc)) {
                rows->RemoveProperty(Rml::PropertyId::Height);
                return;
            }
            rows->SetProperty(Rml::PropertyId::Height,
                              Rml::Property(static_cast<float>(g_height) * share, Rml::Unit::PX));
        }
    }

    // The same thing as a CAP rather than a height, for the lists whose length varies.
    //
    // A fixed height is right for a list that is always long (the controls, the About body). It is
    // wrong for one that is sometimes two rows: the settings list in play, the launcher's, and the
    // debug menu's ways-into-a-place all leave a tall empty box under themselves at a fixed
    // height. With a cap the box is its own size until it would overflow the case, and only then
    // does it scroll.
    void cap_long_rows(Rml::ElementDocument* doc, const char* id, float pixels) {
        if (doc == nullptr) {
            return;
        }
        Rml::Element* rows = doc->GetElementById(id);
        if (rows == nullptr) {
            return;
        }
        if (doc->IsClassSet("in-shell")) {
            pixels = 0.0f;
        }
        // NO CAP AT ALL IN A SIDE PANEL (2026-09-24): the case is the window's full height there,
        // so the list grows into everything between the lid and the actions and the foot sits on
        // the window's bottom edge (case.rcss, ".scrim.side .case .rows.long"). A cap set here
        // would win over the stylesheet, which is why it is removed rather than raised.
        if (pixels <= 0.0f) {
            rows->RemoveProperty(Rml::PropertyId::MaxHeight);
            return;
        }
        rows->SetProperty(Rml::PropertyId::MaxHeight, Rml::Property(pixels, Rml::Unit::PX));
    }

    // How tall the rows of a rows document may be before they scroll.
    //
    // THE TWO DOCUMENTS WANT DIFFERENT ANSWERS AND THE REASON IS WHICH WAY THE SIZING RUNS.
    // The settings list is a modal over a window somebody else sized, so it takes a share of that
    // window and leaves room for the game behind it. The LAUNCHER's window height is derived from
    // its own row count, so it is handed exactly what that arithmetic set aside for rows: any
    // fixed share of it either scrolls a list that fits (58 percent did, leaving dead space under
    // the foot) or runs the list past the bottom of the case.
    float rows_cap(Screen screen) {
        if (screen == Screen::Launch) {
            return static_cast<float>(g_height - launch::launcher_chrome_height());
        }
        // A panel against an edge fills the window's height: no cap, and the stylesheet gives the
        // list the room between the lid and the foot (2026-09-24). A modal over the picture keeps
        // its share, so the game stays visible around it.
        if (oot::ui::settings().menu_side != 0) {
            return 0.0f;
        }
        return static_cast<float>(g_height) * ROWS_SHARE;
    }

    // ------------------------------------------------------------------------------------------
    // Settings rows: the settings document in play, and the launcher before it (play milestone).
    // One table, one reflection, one nudge; the two documents differ in the rows around the
    // settings: the launcher puts the ROM before them and Play and Quit after.
    // ------------------------------------------------------------------------------------------

    // Lighting is a row of the lighting sub menu (its index in that model), LightingMenu the row
    // in the settings that opens it, LightingReset the sub menu's last row (2026-09-24).
    // LightingSweep is the sub menu's harness row: every option of every row captured in turn
    // (main/sweep.h, the user's ask of 2026-09-24).
    enum class RowKind { Setting, Rom, Controls, Files, Moments, Debug, Play, Quit, Lighting, LightingMenu, LightingReset, LightingSweep, Warps, TimeOfDay, Piece, HudMenu,
                         CapturePhoto, CaptureVideo };

    struct RowSpec {
        RowKind kind = RowKind::Setting;
        int setting = 0;
    };

    // ------------------------------------------------------------------------------------------
    // WHAT BELONGS TO DEVELOPMENT RATHER THAN TO PLAYING (the user, 2026-09-26: "the experimental
    // and debug settings and warping, recording etc and all non normal settings all placed under
    // one debugging menu item").
    //
    // Named by KEY, never by index. Two rows were once inserted above a hard coded 9 and the first
    // run question started switching on the frame recorder instead of the updates (see click_ask),
    // and this list would fail the same way and just as quietly: the wrong rows would move to the
    // wrong menu and both menus would still look entirely normal.
    //
    // The frame rate counter is deliberately NOT here. It draws a number on the picture and does
    // nothing else, which is an ordinary thing for a program to offer; the rows below either write
    // gigabytes, move the player through the world, or replace the picture with a diagnostic.
    // ------------------------------------------------------------------------------------------
    bool is_debug_setting(int row) {
        static const int recorder = oot::ui::row_index("recorder");
        static const int length = oot::ui::row_index("recordlength");
        static const int comparison = oot::ui::row_index("comparisonshots");   // 2026-10-09
        // The resize candidates (2026-10-13, ui_settings.h), until the cause is confirmed.
        static const int resize_refresh = oot::ui::row_index("resizerefresh");
        static const int scale_reload = oot::ui::row_index("scalereload");
        static const int smooth_scroll = oot::ui::row_index("smoothscroll");
        return (row >= 0) && (row == recorder || row == length || row == comparison || row == resize_refresh ||
                              row == scale_reload || row == smooth_scroll);
    }

    // COMPARISON SHOTS SHOWS ONLY WHILE SCREENSHOTS IS ON (the user, 2026-10-09: "this being enabled
    // should dynamically then show an option for comparison mode"). The list is rebuilt on every
    // step, so turning Screenshots on brings the row at once.
    // A ROW TO TICK (the user, 2026-10-09: "'comes back for' needs to be all separate options as just
    // checkbox toggles"): what brings the HUD back, each drawn as a box rather than a stepper, and
    // switched by any press on it, Enter, a click, Left or Right.
    bool is_check_setting(int row) {
        return std::strncmp(oot::ui::row_key(row), "hudwake", 7) == 0;
    }

    // THE HUD MENU'S SECTIONS, a divider before the first row of each (2026-10-09): what brings it
    // back, which parts fade, and the size. Never a row, so nothing focuses it.
    const char* section_before(Screen screen, const RowSpec& spec) {
        if (screen != Screen::Hud || spec.kind != RowKind::Setting) {
            return nullptr;
        }
        const std::string key = oot::ui::row_key(spec.setting);
        if (key == "hudwaketarget" || (key == "hudwakelow" && oot::ui::settings().hud_fade == 2)) {
            return "Comes back for";
        }
        if (key == "hudfadehearts") {
            return "Which parts fade";
        }
        if (key == "hudsizes") {
            return "Size";
        }
        if (key == "hud") {
            return "Place";
        }
        if (key == "hudreticle") {
            return "Targeting";
        }
        return nullptr;
    }

    bool is_hidden_debug_setting(int row, const oot::ui::Settings& s) {
        static const int comparison = oot::ui::row_index("comparisonshots");
        return (row >= 0) && (row == comparison) && (s.screenshots == 0);
    }


    bool is_debug_lighting(int row) {
        const std::string key = oot::ui::lighting::row_key(row);
        return key == "inspect" || key == "experiment" ||
               (key.compare(0, 6, "window") == 0 && key != "windowshafts") ||   // the window tuning tools
               key == "lightstone" || key == "glowtest" ||   // the candidates of 2026-10-08
               key.compare(0, 3, "fix") == 0;                // a candidate fix, one switch each (the rule of 2026-10-12)
    }

    // THE REFLECTIONS ARE GONE (the user, 2026-09-30, after six candidates: "nope none work and
    // all look terrible. honestly, no reflections and no debug looks best. rather than fight it,
    // lets remove reflections as options").
    //
    // Removed from the menus rather than defaulted to Off, because a row that offers something
    // nobody should pick is worse than no row. That is this project's own rule and the reason
    // the top bar row is still commented out of the settings table.
    //
    // BY KEY AND NOT BY INDEX, and not by deleting them from the table, so nothing renumbers:
    // the lighting rows map to their fields by a literal index, and this file carries the scar
    // of a row inserted above a hard coded number. The value is forced to zero in clamp()
    // instead, which is what actually stands the pass down.
    bool is_removed_lighting(int row) {
        const std::string key = oot::ui::lighting::row_key(row);
        return key == "reflections" || key == "reflectionrange";
    }

    // The rows of a screen under the settings `s`: which rows there are, and in what order. The
    // rows come and go with the choices they depend on (a pack brings Texture detail, and so on).
    std::vector<RowSpec> rows_for(Screen screen, const oot::ui::Settings& s) {
        std::vector<RowSpec> rows;
        if (screen == Screen::Hud) {
            const auto add = [&rows](const char* key) {
                const int row = oot::ui::row_index(key);
                if (row >= 0) {
                    rows.push_back(RowSpec{ RowKind::Setting, row });
                }
            };
            add("hudfade");
            if (s.hud_fade != 0) {
                add("hudfadeto");
                if (s.hud_fade_to == 0) {
                    add("hudopacity");
                    add("hudcolor");
                }
                if (s.hud_fade == 1) {
                    add("huddelay");
                }
                add("hudspeed");
                // What brings it back, one box each, while it fades when idle.
                if (s.hud_fade == 1) {
                    add("hudwaketarget");
                    add("hudwakec");
                    add("hudwaketalk");
                    add("hudwakehealth");
                    add("hudwakelow");
                    add("hudwakeitems");
                    add("hudwakepause");
                    add("hudwakemap");
                    add("hudwakeany");
                }
                // AND LOW HEALTH WHILE IT IS ALWAYS FADED (the user, 2026-10-13, on the hearts coming
                // back at low health with no row to say so: "needs to function in a way that makes
                // sense"). hud.cpp has always let the hearts up at low health in Always, and the row
                // that decides it was shown only When idle, so it acted unseen. It shows here too now.
                else if (s.hud_fade == 2) {
                    add("hudwakelow");
                }
                add("hudfadehearts");
                add("hudfademagic");
                add("hudfaderupees");
                add("hudfadebuttons");
                add("hudfademap");
            }
            // The size: one for the whole HUD, or each part its own (2026-10-09).
            add("hudsizes");
            if (s.hud_sizes == 0) {
                add("hudsize");
            }
            else {
                add("hudsizehearts");
                add("hudsizemagic");
                add("hudsizerupees");
                add("hudsizebuttons");
                add("hudsizemap");
            }
            // Where it sits (2026-10-09): to a ratio, the window's edges, or custom distances from
            // each edge as a share of the picture.
            add("hud");
            if (s.hud_ratio == 3) {
                add("hudleft");
                add("hudright");
                add("hudtop");
                add("hudbottom");
            }
            // The targeting arrows (2026-10-13), each its own row.
            add("hudreticle");
            add("hudmarker");
            return rows;
        }
        if (screen == Screen::Debug) {
            // In the order a person meets them: where you are, what is recorded, then the two
            // ways of looking at the lighting itself.
            rows.push_back(RowSpec{ RowKind::Warps, 0 });
            // THE TIME OF DAY, SECOND (the user, 2026-09-30: "lets also move the time out day
            // outside of warp menu and just make it the second option right below warps item").
            // It was a row inside the warps document, which meant opening that document to reach
            // the one control he touches most while testing lighting. Same control, same state,
            // same apply: only where it is drawn has changed.
            rows.push_back(RowSpec{ RowKind::TimeOfDay, 0 });
            for (int i = 0; i < oot::ui::row_count(); ++i) {
                if (is_debug_setting(i) && !is_hidden_debug_setting(i, s)) {
                    rows.push_back(RowSpec{ RowKind::Setting, i });
                }
            }
            for (int i = 0; i < oot::ui::lighting::row_count(); ++i) {
                if (is_removed_lighting(i)) {
                    continue;
                }
                if (is_debug_lighting(i)) {
                    rows.push_back(RowSpec{ RowKind::Lighting, i });
                }
            }
            rows.push_back(RowSpec{ RowKind::LightingSweep, 0 });
            // THE PAINTED ELEMENTS OF THE ROOMS WHERE THE PLAYER IS (the user, 2026-10-08: "I can
            // actually just go down the list and turn on and off a painted element within the place
            // that I'm currently at"). One row a display list of each room loaded now, in the
            // order the game draws them, each cycling Shown, Paint taken out, Hidden
            // (game/room_pieces.cpp, patches/room_pieces.c). Read afresh whenever the rows are.
            const int pieces = static_cast<int>(oot::room_pieces::list().size());
            for (int i = 0; i < pieces; ++i) {
                rows.push_back(RowSpec{ RowKind::Piece, i });
            }
            return rows;
        }
        if (screen == Screen::Lighting) {
            // THE WINDOWS' SHAFTS SIT WITH THE GLOW (2026-10-09): appended to the table so nothing
            // renumbers, shown under Glow size, by key.
            static const int window_shafts = oot::ui::lighting::row_index("windowshafts");
            static const int shaft_length = oot::ui::lighting::row_index("shaftlength");
            static const int glow_size = oot::ui::lighting::row_index("glowsize");
            for (int i = 0; i < oot::ui::lighting::row_count(); ++i) {
                if (is_removed_lighting(i) || i == window_shafts || i == shaft_length) {
                    continue;
                }
                if (!is_debug_lighting(i)) {
                    rows.push_back(RowSpec{ RowKind::Lighting, i });
                }
                if (i == glow_size) {
                    rows.push_back(RowSpec{ RowKind::Lighting, window_shafts });
                    rows.push_back(RowSpec{ RowKind::Lighting, shaft_length });
                }
            }
            // The sweep moved to the Debugging menu with the rest of the development rows; Reset
            // stays, because putting the lighting back is something a person playing does.
            rows.push_back(RowSpec{ RowKind::LightingReset, 0 });
            return rows;
        }
        // THE PLAY SECTION (the menus plan, phase 99): the launcher's page is the ROM and the two
        // actions, as the design decision draws it. Every setting is in its own section on the
        // dial now (Settings, Lighting, HUD, Controls, Moments), so the long list it carried
        // before the shell would only repeat them.
        if (screen == Screen::Launch) {
            rows.push_back(RowSpec{ RowKind::Rom, 0 });
            rows.push_back(RowSpec{ RowKind::Play, 0 });
            rows.push_back(RowSpec{ RowKind::Quit, 0 });
            return rows;
        }
        // THE SETTINGS SPLIT (the user, 2026-10-13: "right now the settings menu is huge ... people
        // don't have to scroll so much through the main menu"): each page lists only its own rows,
        // by key, in the order drawn. Settings keeps "anything that doesn't directly affect the
        // game"; Graphics the picture and the textures, with the lighting a page of it; Gameplay "the
        // in-game stuff behave a little differently"; Capture its pages, Photo and Video (Streaming
        // will be a third). Rows still come and go with the ones they depend on.
        const auto add = [&rows](const char* key) {
            const int row = oot::ui::row_index(key);
            if (row >= 0) {
                rows.push_back(RowSpec{ RowKind::Setting, row });
            }
        };
        if (screen == Screen::Graphics) {
            for (const char* key : { "window", "resolution", "downsampling", "aspect", "antialiasing", "renderdistance",
                                     "framerate", "counter", "texturepack" }) {
                add(key);
            }
            // Texture detail only while a pack is chosen (the user, 2026-10-07: "when you enable it,
            // then it adds an additional option right underneath it").
            if (s.texture_pack != 0) {
                add("texturedetail");
            }
            rows.push_back(RowSpec{ RowKind::LightingMenu, 0 });
            return rows;
        }
        if (screen == Screen::Gameplay) {
            for (const char* key : { "camera", "cameraaxes", "aimaxes", "aimaxesleft", "cameradistance", "cameradrift",
                                     "textspeed", "textskip", "lowhealthbeep" }) {
                add(key);
            }
            return rows;
        }
        if (screen == Screen::Capture) {
            rows.push_back(RowSpec{ RowKind::CapturePhoto, 0 });
            rows.push_back(RowSpec{ RowKind::CaptureVideo, 0 });
            return rows;
        }
        if (screen == Screen::Photo) {
            add("screenshots");
            // What a picture holds, for F12's and for photo mode's own (which Screenshots does not gate).
            if (s.screenshots != 0 || s.photo_mode != 0) {
                add("screenshotshows");
                add("screenshotsize");
            }
            add("photomode");
            // Photo mode's own HUD (ui_settings.h): where it starts, and how far Dimmed takes it, which
            // the control reaches whatever the start.
            if (s.photo_mode != 0) {
                add("photohud");
                add("photohuddim");
            }
            return rows;
        }
        if (screen == Screen::Video) {
            add("videorecording");
            if (s.video_recording != 0) {
                for (const char* key : { "videoshows", "videosize", "videoquality", "videosound", "microphone" }) {
                    add(key);
                }
                if (s.microphone != 0) {
                    add("micdevice");
                    add("miclevel");
                }
                add("videomode");
            }
            return rows;
        }
        // Settings itself: the program, not the game.
        for (const char* key : { "updates", "autosave", "saveonquit", "resumeonstart", "menu", "interfacesize", "volume",
                                 "menusounds" }) {
            add(key);
        }
        if (s.menu_sounds != 0) {
            add("menusoundvolume");
        }
        // Lighting, the HUD, Controls and Moments are sections on the dial now (phase 96), so their
        // rows here went; Your files is a page of this section.
        rows.push_back(RowSpec{ RowKind::Files, 0 });
        // The debug menu, and IN PLAY ONLY. Every row in it asks the game to go somewhere, and
        // there is no game to send anywhere from the launcher: offering it there would be a menu
        // whose every row does nothing, which this project's own settings note calls worse than
        // no menu at all.
        if (g_game_started) {
            rows.push_back(RowSpec{ RowKind::Debug, 0 });
        }
        {
            // QUIT IN PLAY TOO. When Play and Quit moved out of the list into the launcher's
            // action bar, the in-game menu was left with no way out of the program at all (the
            // user, 2026-09-23: "The quit button is actually missing inside the game"), which
            // matters most with the title bar off, where this menu IS the way out. Play has no
            // place here; Quit does, in the same bar, on its own.
            rows.push_back(RowSpec{ RowKind::Quit, 0 });
        }
        return rows;
    }

    std::vector<RowSpec> rows_for(Screen screen) {
        return rows_for(screen, oot::ui::settings());
    }

    Rml::ElementDocument* rows_doc(Screen screen) {
        if (screen == Screen::Lighting) {
            return g_lighting_doc;
        }
        if (screen == Screen::Debug) {
            return g_debug_doc;
        }
        if (screen == Screen::Hud) {
            return g_hud_doc;
        }
        return screen == Screen::Launch ? g_launch_doc : g_settings_doc;
    }

    // THE PICTURE MOVES OUT FROM UNDER A SIDE PANEL (the user, 2026-09-24: "menu doesn't push
    // over game, it covers it"; "it must maintain its ratio and resolution just scale as is").
    // The renderer is told how many pixels the panel holds at which edge and scales the whole
    // picture, ratio kept, into the rest. Zero when no menu is up or the menus sit over the
    // picture. The panel's width is read from its case once it is laid out; before that, the
    // case's designed width (tokens.rcss, --case-width).
    // EVERY MENU, SUB MENU AND MODAL IN PLAY IS A PANEL WHEN THE ROW SAYS SO (the user, 2026-10-08:
    // "some modals and like some settings are center of screen rather than using the setting ...
    // the debug menu currently still overlays over the entire game in the center of the screen. So
    // I think all menus, submenus, modals, and everything should all be ... docked and they should
    // all be full height of the window itself"). Only Settings and Lighting were; the Debugging
    // menu sat over the very floor being tested. The launcher's own window and its file browser
    // are a window of their own and are not in this list.
    Rml::ElementDocument* panel_doc(Screen screen) {
        switch (screen) {
            case Screen::Settings:
            case Screen::Graphics:
            case Screen::Gameplay:
            case Screen::Capture:
            case Screen::Photo:
            case Screen::Video:    return g_settings_doc;
            case Screen::Lighting: return g_lighting_doc;
            case Screen::Debug:    return g_debug_doc;
            case Screen::Hud:      return g_hud_doc;
            case Screen::Warps:    return g_warps_doc;
            case Screen::Moments:  return g_moments_doc;
            case Screen::Controls: return g_controls_doc;
            case Screen::About:    return g_about_doc;
            case Screen::Ask:      return g_ask_doc;
            default:               return nullptr;
        }
    }

    Rml::ElementDocument* g_shell_doc_for_inset = nullptr;   // the shell, once loaded (phase 96)

    // THE PICTURE EASES ASIDE AND BACK (phase 114): the panel's opening and closing move the game's
    // picture, and it moves over the base duration on the quartic curve rather than snapping, from
    // wherever it stands now (a turn of the placement row mid-move simply sets a new end).
    struct InsetMotion {
        float from_left = 0.0f;
        float from_right = 0.0f;
        float left = 0.0f;       // where it stands now
        float right = 0.0f;
        int to_left = 0;
        int to_right = 0;
        std::chrono::steady_clock::time_point start{};
        bool running = false;
    };
    InsetMotion g_inset;

    float quartic_in_out(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        const float u = 1.0f - t;
        return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - 8.0f * u * u * u * u;
    }

    void inset_toward(int left, int right) {
        if (left == g_inset.to_left && right == g_inset.to_right) {
            return;
        }
        g_inset.from_left = g_inset.left;
        g_inset.from_right = g_inset.right;
        g_inset.to_left = left;
        g_inset.to_right = right;
        g_inset.start = std::chrono::steady_clock::now();
        g_inset.running = true;
    }

    // Once a frame: the picture's inset a step further along its ease.
    void poll_inset() {
        if (!g_inset.running) {
            return;
        }
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_inset.start).count();
        const float t = seconds / oot::ui::tokens::duration_base;
        const float e = quartic_in_out(t);
        g_inset.left = g_inset.from_left + (static_cast<float>(g_inset.to_left) - g_inset.from_left) * e;
        g_inset.right = g_inset.from_right + (static_cast<float>(g_inset.to_right) - g_inset.from_right) * e;
        if (t >= 1.0f) {
            g_inset.left = static_cast<float>(g_inset.to_left);
            g_inset.right = static_cast<float>(g_inset.to_right);
            g_inset.running = false;
        }
        oot::renderer::set_picture_inset(static_cast<int>(std::lround(g_inset.left)), static_cast<int>(std::lround(g_inset.right)));
    }

    void apply_picture_inset(Screen screen) {
        const int side = oot::ui::settings().menu_side;
        // THE PICTURE RUNS UNDER THE CUT (phase 96): it moves aside by the docked panel's ground,
        // so the slanted edge and its gold line lie over the picture, as the decision draws them.
        const bool panel = (side != 0) && g_game_started && (screen != Screen::None);
        int pixels = 0;
        if (panel) {
            Rml::Element* ground = (g_shell_doc_for_inset != nullptr) ? g_shell_doc_for_inset->QuerySelector(".sh-ground") : nullptr;
            float width = (ground != nullptr) ? ground->GetBox().GetSize(Rml::BoxArea::Border).x : 0.0f;
            if (width <= 0.0f) {
                // The panel less its cut (tokens.rcss, --panel-width less --cut-edge-width), until
                // the shell is laid out.
                width = 580.0f * ((g_context != nullptr) ? g_context->GetDensityIndependentPixelRatio() : 1.0f);
            }
            pixels = std::min(static_cast<int>(std::lround(width)), g_width / 2);
        }
        inset_toward(side == 2 ? pixels : 0, side == 1 ? pixels : 0);
    }

    // Where the two menus sit: over the picture, as a panel on the right, or as a panel on the
    // left (the Menu placement row). The classes go on each document's scrim; the stylesheet
    // does the rest, and the picture moves aside with it.
    void apply_menu_placement() {
        const int placement = oot::ui::settings().menu_side;
        const bool side = placement != 0;
        const bool left = placement == 2;
        // A DIALOG IS A LAYER OVER THE MENU, NOT A PANEL OF ITS OWN (phase 107): the confirmation and
        // the first run's questions stay centered over the shell with their scrim, whatever the
        // placement, so they read as a question asked over the page rather than a page replacing it.
        for (Screen screen : { Screen::Settings, Screen::Lighting, Screen::Debug, Screen::Hud, Screen::Warps, Screen::Moments,
                               Screen::Controls, Screen::About }) {
            Rml::ElementDocument* doc = panel_doc(screen);
            if (doc == nullptr) {
                continue;
            }
            if (Rml::Element* e = doc->QuerySelector(".scrim")) {
                e->SetClass("side", side);
                e->SetClass("left", left);
            }
        }
        apply_picture_inset(g_screen);
        // The shell takes the placement too: docked, mirrored or centered (phase 96).
        refresh_shell();
    }

    int g_focus = 0;

    std::string rom_value() {
        const launch::RomStatus s = launch::rom_status();
        switch (s.state) {
            case launch::RomState::Stored:   return "Ready, from where you keep it";
            case launch::RomState::Beside:
            case launch::RomState::Accepted: return shorten(s.name, 34);
            case launch::RomState::Refused:  return "Refused";
            default:                         return "Not found";
        }
    }

    std::string rom_note() {
        const launch::RomStatus s = launch::rom_status();
        switch (s.state) {
            case launch::RomState::Stored:
                return "Reading the file you chose last time, where you left it. Enter on the ROM row chooses another";
            case launch::RomState::Beside:
                return "Found beside the program and accepted: " + s.name;
            case launch::RomState::Accepted:
                return "Accepted: " + s.name;
            case launch::RomState::Refused:
                return s.name + " was refused: " + s.detail + ". Choose another";
            default:
                return "No ROM found beside the program. Drop the file on the window, or Enter on the ROM row to browse";
        }
    }

    // Which element the rows are built into. One per rows document, the way rows_doc picks the
    // document itself.
    const char* rows_container(Screen screen) {
        if (screen == Screen::Lighting) {
            return "lighting-rows";
        }
        if (screen == Screen::Debug) {
            return "debug-rows";
        }
        if (screen == Screen::Hud) {
            return "hud-rows";
        }
        return screen == Screen::Launch ? "launch-rows" : "settings-rows";
    }

    // Which of the model's lists a rows document draws.
    oot::ui::rows::List rows_list(Screen screen) {
        switch (screen) {
            case Screen::Launch:   return oot::ui::rows::List::Launch;
            case Screen::Lighting: return oot::ui::rows::List::Lighting;
            case Screen::Hud:      return oot::ui::rows::List::Hud;
            case Screen::Debug:    return oot::ui::rows::List::Debug;
            default:               return oot::ui::rows::List::Settings;
        }
    }

    // WHAT EACH ROW SAYS (the menus plan, phase 95), from the tables and the settings `s` and
    // lighting `l` alone: its label, the value shown and how it is drawn, a note, a divider before
    // it. Play and Quit are not rows here; they are the action bar's buttons and keep their place
    // in rows_for only so the focus can walk to them.
    // THE GLYPH IN A ROW'S RING (the menus plan, phase 100, the decision's plates): one of the
    // bundled icons for what the row is about, by its key's start, so a row added to a table later
    // takes its family's glyph without a line here. The first match wins, so the longer keys come
    // before the shorter ones they begin with.
    const char* row_icon(const char* key) {
        static const std::pair<const char*, const char*> TABLE[] = {
            // Settings
            { "window", "app-window" }, { "resolution", "scan" }, { "downsampling", "scaling" },
            { "aspect", "ratio" }, { "antialiasing", "sparkles" }, { "renderdistance", "eye" },
            { "framerate", "gauge" }, { "counter", "gauge" }, { "recorder", "film" }, { "recordlength", "timer" },
            { "updates", "download" }, { "autosave", "save" }, { "saveonquit", "save" },
            { "resumeonstart", "rotate-ccw" }, { "cameraaxes", "move-3d" }, { "cameradistance", "camera" },
            { "cameradrift", "move-3d" }, { "camera", "camera" }, { "aimaxes", "crosshair" }, { "menusound", "volume-2" }, { "menu", "list" }, { "interfacesize", "scaling" },
            { "volume", "volume-2" }, { "lowhealthbeep", "volume-2" }, { "texture", "image" }, { "text", "type" },
            { "screenshot", "image" }, { "comparisonshots", "image" }, { "video", "video" },
            { "photomode", "aperture" }, { "photohud", "layers" }, { "mic", "volume-2" },
            { "hudwake", "eye" }, { "hudsize", "scaling" }, { "hudcolor", "palette" }, { "huddelay", "timer" },
            { "hudspeed", "timer" }, { "hudleft", "map-pin" }, { "hudright", "map-pin" }, { "hudtop", "map-pin" },
            { "hudbottom", "map-pin" }, { "hudreticle", "crosshair" }, { "hudmarker", "crosshair" }, { "hud", "layers" },
            // Lighting
            { "raytracing", "sun" }, { "shadow", "moon" }, { "casters", "moon" }, { "indoorlight", "house" },
            { "indoorambience", "mountain" }, { "outdoorambience", "mountain" }, { "indoordust", "wind" },
            { "outdoordust", "wind" }, { "darkness", "moon" }, { "light", "lightbulb" }, { "occlusion", "layers" },
            { "bounce", "refresh-cw" }, { "reflection", "sparkles" }, { "glowtest", "bug" }, { "glow", "sparkles" },
            { "wind", "wind" }, { "haze", "sun" }, { "fogfade", "mountain" }, { "distancefade", "mountain" },
            { "smoothing", "sliders-horizontal" }, { "filter", "sliders-horizontal" }, { "inspect", "eye" },
            { "experiment", "bug" }, { "windowshafts", "sun" }, { "shaftlength", "sun" }, { "windowaim", "crosshair" },
            { "windowtilt", "crosshair" }, { "windowturn", "crosshair" }, { "windowmove", "crosshair" },
        };
        for (const auto& [start, icon] : TABLE) {
            if (std::strncmp(key, start, std::strlen(start)) == 0) {
                return icon;
            }
        }
        return "sliders-horizontal";
    }

    // HOW A STEPPED VALUE IS DRAWN (phase 100, the decision's plates): a short set of short options
    // side by side, every one shown and the one in force lit (Windowed and Fullscreen; Off, 2x, 4x,
    // 8x); a longer set as a tick per option beside the option's words; past a dozen, the words
    // alone. `label` gives option i's words, or null for a number.
    template <typename Label>
    void draw_options(oot::ui::rows::Row& row, int count, int number, Label label) {
        constexpr int MOST_SIDE_BY_SIDE = 4;
        constexpr size_t LONGEST_SIDE_BY_SIDE = 11;
        constexpr int MOST_TICKS = 12;
        bool short_words = count >= 2 && count <= MOST_SIDE_BY_SIDE;
        for (int i = 0; short_words && i < count; ++i) {
            const char* words = label(i);
            short_words = words != nullptr && std::strlen(words) <= LONGEST_SIDE_BY_SIDE;
        }
        if (short_words) {
            for (int i = 0; i < count; ++i) {
                row.choices.push_back({ label(i), i == number });
            }
        }
        else if (count >= 2 && count <= MOST_TICKS) {
            for (int i = 0; i < count; ++i) {
                row.ticks.push_back(i == number ? 1 : 0);
            }
        }
    }

    std::vector<oot::ui::rows::Row> row_views(Screen screen, const oot::ui::Settings& s,
                                              const oot::ui::lighting::Lighting& l) {
        using oot::ui::rows::Row;
        const std::vector<RowSpec> specs = rows_for(screen, s);
        std::vector<Row> out;
        out.reserve(specs.size());
        for (size_t i = 0; i < specs.size(); ++i) {
            Row row;
            row.n = static_cast<int>(i);
            switch (specs[i].kind) {
                case RowKind::Setting: {
                    const int setting = specs[i].setting;
                    row.label = oot::ui::row_label(setting);
                    const int number = oot::ui::get_row(s, setting);
                    const char* option = oot::ui::option_label(setting, number);
                    // A null label means the row is a number rather than a choice, which is how
                    // the volume row says so without needing a type tag.
                    row.value = option != nullptr ? std::string(option) : std::to_string(number) + "%";
                    if (is_check_setting(setting)) {
                        row.shape = oot::ui::rows::Check;
                        row.on = number != 0;
                    }
                    else {
                        row.shape = oot::ui::rows::Stepper;
                        row.low = at_low(setting, number);
                        row.high = at_high(setting, number);
                    }
                    // What the chosen option means, under the row, for a row that has one (2026-10-09),
                    // naming each control by what it is bound to now (with_bindings).
                    if (const char* note = oot::ui::option_note(setting, number)) {
                        row.note = with_bindings(note);
                    }
                    row.icon = row_icon(oot::ui::row_key(setting));
                    if (row.shape == oot::ui::rows::Stepper) {
                        draw_options(row, oot::ui::option_count(setting), number,
                                     [setting](int i) { return oot::ui::option_label(setting, i); });
                    }
                    break;
                }
                case RowKind::Lighting: {
                    const int setting = specs[i].setting;
                    row.label = oot::ui::lighting::row_label(setting);
                    const int number = oot::ui::lighting::get_row(l, setting);
                    row.value = oot::ui::lighting::option_label(setting, number);
                    row.shape = oot::ui::rows::Stepper;
                    row.low = number <= 0;
                    row.high = number >= oot::ui::lighting::option_count(setting) - 1;
                    row.icon = row_icon(oot::ui::lighting::row_key(setting));
                    draw_options(row, oot::ui::lighting::option_count(setting), number,
                                 [setting](int i) { return oot::ui::lighting::option_label(setting, i); });
                    break;
                }
                case RowKind::LightingMenu:
                    row.label = "Lighting";
                    row.value = "Switch each part of it on or off, and tune it";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::HudMenu:
                    row.label = "HUD";
                    row.value = "Fading, size and place";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::CapturePhoto:
                    row.label = "Photo";
                    row.value = "Screenshots and photo mode";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::CaptureVideo:
                    row.label = "Video";
                    row.value = "Recording, its size, quality and sound, the microphone";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::LightingSweep:
                    row.label = "Sweep";
                    row.value = oot::sweep::running() ? "Running; the frames and the log go where recordings go"
                                                      : "Capture one frame per option of every row, then put yours back";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::LightingReset:
                    row.label = "Reset";
                    row.value = "Low starting values, ray tracing Off";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::Rom:
                    row.label = "ROM";
                    row.value = rom_value();
                    row.shape = oot::ui::rows::Text;
                    // ITS SENTENCE, UNDER THE ROW (the user, 2026-10-13, asked whether to bring back the
                    // reason a ROM was refused: "yes"). The sentence lost its element on 2026-09-20
                    // (102f1e4), so a refused file said only "Refused" while the website listed why. Why it
                    // was refused, how to give one when none is found, and a note an action left
                    // (Checking, Choose a ROM first), which wins until the ROM changes, as it always did.
                    row.note = g_launch_note.empty() ? rom_note() : g_launch_note;
                    break;
                case RowKind::Controls:
                    row.label = "Controls";
                    row.value = "Click or press Enter to open";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::Debug:
                    row.label = "Debugging";
                    row.value = "Tools for development, not for playing";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::TimeOfDay:
                    row.label = "Time of day";
                    // Never spent at either end: the list wraps, so a person can walk round the
                    // clock in one direction rather than back up the whole row.
                    row.value = time_label();
                    row.shape = oot::ui::rows::Stepper;
                    break;
                case RowKind::Warps:
                    row.label = "Warps";
                    row.value = "Go anywhere in the game";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::Piece: {
                    // Named by the list's place in the room's file, which is the name the
                    // decompilation's extracted files carry (DL_03xxxxxx), so a piece found by eye
                    // here can be looked up there.
                    const std::vector<oot::room_pieces::Piece> pieces = oot::room_pieces::list();
                    const int at = specs[i].setting;
                    if (at < 0 || at >= static_cast<int>(pieces.size())) {
                        continue;
                    }
                    const oot::room_pieces::Piece& piece = pieces[at];
                    char name[96];
                    std::snprintf(name, sizeof(name), "Room %d, %08X (%d of %d white%s)", piece.room, 0x03000000u | piece.offset,
                                  piece.white, piece.vertices, piece.translucent ? ", see-through" : "");
                    row.label = name;
                    row.value = oot::room_pieces::state_label(oot::room_pieces::state_of(piece));
                    row.shape = oot::ui::rows::Stepper;
                    break;
                }
                // Where everything actually is, and the way to take it all off the machine. The
                // answer to "I cannot find my saves", which is the real objection to a program
                // putting a person's files anywhere they did not pick themselves.
                case RowKind::Files:
                    row.label = "Your files";
                    row.value = "Click or press Enter to open";
                    row.shape = oot::ui::rows::Action;
                    break;
                case RowKind::Moments:
                    row.label = "Moments";
                    row.value = g_game_started ? "Save anywhere, resume there" : "Resume a saved moment";
                    row.shape = oot::ui::rows::Action;
                    break;
                // Play and Quit are ACTIONS and are drawn in the action bar below the list (the
                // user, twice, most recently 2026-09-20: "I asked for the play and quit buttons to
                // be outside of this main list"). They keep their place in the row order, so
                // everything that walks the rows still walks them, and apply_rows_focus puts the
                // focus on the button instead.
                case RowKind::Play:
                case RowKind::Quit:
                    continue;
            }
            if (const char* section = section_before(screen, specs[i])) {
                row.section = section;
            }
            // The rows that are not a setting take their glyph from what they are.
            if (row.icon.empty()) {
                switch (specs[i].kind) {
                    case RowKind::Rom:           row.icon = "folder"; break;
                    case RowKind::Files:         row.icon = "folder-open"; break;
                    case RowKind::Debug:         row.icon = "bug"; break;
                    case RowKind::Warps:         row.icon = "map-pin"; break;
                    case RowKind::TimeOfDay:     row.icon = "clock"; break;
                    case RowKind::Piece:         row.icon = "layers"; break;
                    case RowKind::LightingReset: row.icon = "rotate-ccw"; break;
                    case RowKind::LightingSweep: row.icon = "film"; break;
                    case RowKind::LightingMenu:  row.icon = "sun"; break;
                    case RowKind::HudMenu:       row.icon = "layers"; break;
                    case RowKind::CapturePhoto:  row.icon = "image"; break;
                    case RowKind::CaptureVideo:  row.icon = "video"; break;
                    case RowKind::Controls:      row.icon = "gamepad-2"; break;
                    case RowKind::Moments:       row.icon = "aperture"; break;
                    default:                     row.icon = "sliders-horizontal"; break;
                }
            }
            out.push_back(std::move(row));
        }
        return out;
    }

    // Put the live values into a rows document.
    //
    // THE LIST IS THE MODEL'S, and the document holds no rows of its own. It used to fill a fixed
    // run of row elements typed into the .rml, and that arrangement failed the way it was always
    // going to: on 2026-09-22 two new settings pushed Controls, Your files and Debug past the last
    // slot, and a row with no element simply does not appear. Then it was built as markup in code
    // (until phase 95); now the rows are data and one template draws them (models/rows_model).
    //
    // Recomputed whole on every change, which at the rate a person presses keys costs nothing and
    // keeps one path rather than a fast one and a slow one that have to agree. The context redraws
    // only what changed.
    void refresh_rows(Screen screen) {
        Rml::ElementDocument* doc = rows_doc(screen);
        if (doc == nullptr) {
            return;
        }

        // The two actions' words, which live in the action bar rather than in a row.
        for (const RowSpec& spec : rows_for(screen)) {
            if (spec.kind == RowKind::Play) {
                set_text(doc, "act-play-label", launch::rom_ready() ? "Play" : "Needs a ROM");
            }
            else if (spec.kind == RowKind::Quit) {
                set_text(doc, "act-quit-label", "Quit");
            }
        }

        oot::ui::rows::set(rows_list(screen), row_views(screen, oot::ui::settings(), oot::ui::lighting::current()));
        cap_long_rows(doc, rows_container(screen), rows_cap(screen));
        apply_rows_focus(screen);
    }

    // A row whose content lives somewhere else (Play and Quit, which are buttons now) is
    // taken out of the list entirely rather than left as an empty stripe.
    // A click on Play or Quit runs exactly what Enter on them runs, by finding their own place
    // in the row order rather than duplicating what they do.
    // A step does exactly what Left and Right do on the focused row, so there is one behavior
    // and one place it is written.
    // A CLICK ON ONE OPTION OF A SHORT SET (phase 100): steps the row to it one option at a time,
    // through the same nudge a press of Left or Right makes, so every rule a step obeys (a warning
    // before the recorder, the menu's placement moving) holds for a click too.
    void click_pick(const std::string& id) {
        int index = -1;
        int wanted = -1;
        if (std::sscanf(id.c_str(), "pick-%d-%d", &index, &wanted) != 2) {
            return;
        }
        const std::vector<RowSpec>& specs = rows_for(g_screen);
        if (index < 0 || index >= static_cast<int>(specs.size())) {
            return;
        }
        const RowSpec spec = specs[static_cast<size_t>(index)];
        const auto now = [&spec]() {
            return spec.kind == RowKind::Lighting ? oot::ui::lighting::get_row(oot::ui::lighting::current(), spec.setting)
                                                  : oot::ui::get_row(oot::ui::settings(), spec.setting);
        };
        g_focus = index;
        for (int guard = 0; guard < 16 && now() != wanted; ++guard) {
            const int before = now();
            if (spec.kind == RowKind::Lighting) {
                nudge_lighting(spec.setting, wanted > before ? 1 : -1);
            }
            else if (spec.kind == RowKind::Setting) {
                nudge_setting(spec.setting, wanted > before ? 1 : -1);
            }
            if (now() == before) {
                break;   // refused (a warning is up, or the end was reached)
            }
        }
        refresh_rows(g_screen);
        apply_rows_focus(g_screen);
    }

    void click_step(const std::string& id) {
        const int index = std::atoi(id.c_str() + 5);
        const std::vector<RowSpec>& specs = rows_for(g_screen);
        if (index < 0 || index >= static_cast<int>(specs.size())) {
            return;
        }
        if (specs[index].kind != RowKind::Setting && specs[index].kind != RowKind::Lighting &&
            specs[index].kind != RowKind::TimeOfDay && specs[index].kind != RowKind::Piece) {
            return;
        }
        g_focus = index;
        const int direction = id.compare(0, 5, "prev-") == 0 ? -1 : 1;
        if (specs[index].kind == RowKind::Lighting) {
            nudge_lighting(specs[index].setting, direction);
        } else if (specs[index].kind == RowKind::Piece) {
            nudge_piece(specs[index].setting, direction);
        } else if (specs[index].kind == RowKind::TimeOfDay) {
            nudge_time(direction);
        } else {
            nudge_setting(specs[index].setting, direction);
        }
        refresh_rows(g_screen);
        apply_rows_focus(g_screen);
    }

    void click_action(const std::string& id) {
        const RowKind want = (id == "act-play") ? RowKind::Play : RowKind::Quit;
        const std::vector<RowSpec>& specs = rows_for(g_screen);
        for (size_t i = 0; i < specs.size(); ++i) {
            if (specs[i].kind == want) {
                click_row(g_screen, static_cast<int>(i));
                return;
            }
        }
    }

    void hide_row(Rml::ElementDocument* doc, int row, const char* prefix) {
        if (doc == nullptr) {
            return;
        }
        if (Rml::Element* e = doc->GetElementById(std::string(prefix) + std::to_string(row))) {
            e->SetProperty("display", "none");
        }
    }

    // The rows document whose focused row should be brought into view once the context has laid
    // it out (follow_rows_focus, from draw). The rows are drawn by the data model during the
    // context's update, so a row added this frame has no box until then.
    Screen g_follow = Screen::None;

    void apply_rows_focus(Screen screen) {
        Rml::ElementDocument* doc = rows_doc(screen);
        if (doc == nullptr) {
            return;
        }
        oot::ui::rows::focus(rows_list(screen), g_focus);
        g_follow = screen;
        // The two actions are drawn outside the list, so the focus has to follow them there or
        // walking down the screen with a pad appears to stop at the last setting.
        const std::vector<RowSpec>& specs = rows_for(screen);
        for (int row = 0; row < static_cast<int>(specs.size()); ++row) {
            const char* action_id = nullptr;
            if (specs[row].kind == RowKind::Play) {
                action_id = "act-play";
            }
            else if (specs[row].kind == RowKind::Quit) {
                action_id = "act-quit";
            }
            if (action_id != nullptr) {
                if (Rml::Element* a = doc->GetElementById(action_id)) {
                    a->SetClass("on", row == g_focus);
                }
            }
        }
        // The prompts follow the focus: Change shows on a value row and goes on an action (phase 109).
        refresh_shell();
    }

    // THE LIST SCROLLS, so walking off the bottom of the case has to bring the row with it.
    // Without this the focus carries on down a list nobody can see, which is a worse version of
    // the bug the scrolling was meant to fix.
    //
    // MEASURED HERE RATHER THAN ASKED OF ScrollIntoView (2026-09-24): its nearest alignment moved
    // the list on rows that were already in full view, so changing a value scrolled the menu. The
    // list moves only when the row is actually outside it, and then by exactly the amount that
    // brings it back. Run after the context's update and before it renders.
    // How the lists scroll to the focus: smoothly (phase 114), or at once while the Debugging
    // toggle for it is off (a resize candidate, 2026-10-13).
    Rml::ScrollBehavior list_scroll() {
        return oot::ui::settings().smooth_scroll != 0 ? Rml::ScrollBehavior::Smooth : Rml::ScrollBehavior::Instant;
    }

    // Brings `e` into view in the scrolling `list`: only when it is outside it, and by exactly the
    // amount that brings it back (the measured way, above). Shared by every list the focus walks.
    void keep_in_view(Rml::Element* list, Rml::Element* e) {
        if (list == nullptr || e == nullptr) {
            return;
        }
        const float scroll = list->GetScrollTop();
        const float top = e->GetAbsoluteOffset().y - list->GetAbsoluteOffset().y + scroll;
        const float bottom = top + e->GetOffsetHeight();
        const float view = list->GetClientHeight();
        const float slack = 2.0f;   // a pixel of rounding is not a reason to move
        // Smoothly, as everything in the menus moves (phase 114).
        if (top < scroll - slack) {
            list->ScrollTo(Rml::Vector2f(list->GetScrollLeft(), top), list_scroll());
        }
        else if (bottom > scroll + view + slack) {
            list->ScrollTo(Rml::Vector2f(list->GetScrollLeft(), bottom - view), list_scroll());
        }
    }

    void follow_rows_focus() {
        const Screen screen = g_follow;
        g_follow = Screen::None;
        if (screen == Screen::None || screen != g_screen) {
            return;
        }
        Rml::ElementDocument* doc = rows_doc(screen);
        if (doc == nullptr) {
            return;
        }
        keep_in_view(doc->GetElementById(rows_container(screen)), doc->GetElementById("row-" + std::to_string(g_focus)));
    }

    // Change a setting. A choice steps through its options and stops at the ends rather than
    // wrapping: wrapping a two option row makes left and right do the same thing, which reads
    // as a broken control.
    void nudge_setting(int row, int direction) {
        oot::ui::Settings next = oot::ui::settings();
        const int count = oot::ui::option_count(row);
        const int current = oot::ui::get_row(next, row);

        // THE RECORDER IS NOT SWITCHED ON BY A KEYPRESS (the user, 2026-09-26: "When attempting to
        // enable the recorder it must even show an additional warning and confirmation about the
        // space this feature takes up"). Every path that changes a setting comes through here, the
        // mouse's steppers included, so one gate covers all of them. The confirmation applies the
        // row itself when it is accepted.
        if (recorder_needs_warning(row, current + direction)) {
            confirm_recorder(row, g_screen);
            return;
        }

        if (is_check_setting(row)) {
            // A box: any step switches it, so Enter, a click and either arrow all tick or clear it.
            oot::ui::set_row(next, row, current != 0 ? 0 : 1);
        }
        else if (count == 0) {
            // A number. Five at a time is a usable step for a percentage without being coarse.
            oot::ui::set_row(next, row, current + direction * 5);
        }
        else {
            const int wanted = current + direction;
            if (wanted < 0 || wanted >= count) {
                return;
            }
            oot::ui::set_row(next, row, wanted);
        }

        oot::ui::apply(next);
        // The menu placement row moves the case the moment it changes.
        apply_menu_placement();
    }

    // The same for a lighting row: a choice that stops at its ends, applied to the renderer and
    // the file as it changes.
    void nudge_lighting(int row, int direction) {
        oot::ui::lighting::Lighting next = oot::ui::lighting::current();
        const int count = oot::ui::lighting::option_count(row);
        const int wanted = oot::ui::lighting::get_row(next, row) + direction;
        if (wanted < 0 || wanted >= count) {
            return;
        }
        oot::ui::lighting::set_row(next, row, wanted);
        oot::ui::lighting::apply(next);
    }

    void rows_nav(Screen screen, Nav nav) {
        const std::vector<RowSpec> rows = rows_for(screen);
        const int count = static_cast<int>(rows.size());
        if (count == 0) {
            return;
        }
        if (g_focus < 0 || g_focus >= count) {
            g_focus = 0;
        }
        const RowSpec& spec = rows[static_cast<size_t>(g_focus)];

        /* PLAY AND QUIT SIT SIDE BY SIDE, SO SIDEWAYS IS WHAT MOVES BETWEEN THEM.
           They are rows in this list like everything else, which is how they come to be reached
           with Up and Down, and the user's objection is simply that the direction you press does
           not match the direction they are laid out in. So the pair at the end is treated as one
           stop: Up and Down step onto it and off it, and Left and Right move within it.
           Done here rather than per device on purpose: the keyboard's arrows, the pad's D-pad and
           its stick all arrive as the same Nav, so this is every control type at once. */
        const bool is_button = (spec.kind == RowKind::Play || spec.kind == RowKind::Quit);
        int first_button = count;
        for (int i = 0; i < count; ++i) {
            if (rows[static_cast<size_t>(i)].kind == RowKind::Play ||
                rows[static_cast<size_t>(i)].kind == RowKind::Quit) {
                first_button = i;
                break;
            }
        }

        switch (nav) {
            case Nav::Default: case Nav::SectionPrev: case Nav::SectionNext:
                return;   // the shell's (phase 96); these older screens have none
            case Nav::Up:
                // Off the pair goes to the last ordinary row, not to the other button.
                if (is_button) {
                    g_focus = (first_button > 0) ? first_button - 1 : count - 1;
                } else {
                    g_focus = (g_focus + count - 1) % count;
                }
                apply_rows_focus(screen);
                break;
            case Nav::Down:
                // Onto the pair lands on the first of them; off the pair wraps to the top.
                if (is_button) {
                    g_focus = 0;
                } else if (g_focus + 1 >= first_button) {
                    g_focus = (first_button < count) ? first_button : 0;
                } else {
                    g_focus = g_focus + 1;
                }
                apply_rows_focus(screen);
                break;
            case Nav::Left:
            case Nav::Right:
                if (is_button) {
                    /* BY WHERE THEY ARE DRAWN, NOT BY THEIR ORDER IN THE LIST. Quit is the first
                       element in launch.rml and Play the second, so Quit is on the LEFT and Play
                       on the right; in the row list the order is the other way round. Moving by
                       index therefore sent Left to the right hand button, which is what was
                       reported. Naming the kinds makes the code say what the screen shows. */
                    const RowKind want = (nav == Nav::Left) ? RowKind::Quit : RowKind::Play;
                    for (int i = 0; i < count; ++i) {
                        if (rows[static_cast<size_t>(i)].kind == want) {
                            g_focus = i;
                            break;
                        }
                    }
                    apply_rows_focus(screen);
                }
                else if (spec.kind == RowKind::Setting) {
                    nudge_setting(spec.setting, nav == Nav::Right ? 1 : -1);
                    refresh_rows(screen);
                }
                else if (spec.kind == RowKind::Lighting) {
                    nudge_lighting(spec.setting, nav == Nav::Right ? 1 : -1);
                    refresh_rows(screen);
                }
                else if (spec.kind == RowKind::TimeOfDay) {
                    nudge_time(nav == Nav::Right ? 1 : -1);
                    refresh_rows(screen);
                }
                else if (spec.kind == RowKind::Piece) {
                    nudge_piece(spec.setting, nav == Nav::Right ? 1 : -1);
                    refresh_rows(screen);
                }
                break;
            case Nav::Accept:
                // THE BASELINE'S SEVENTH DEFECT: Accept on a value at its last option did nothing.
                // Select steps forward and wraps to the first after the last (nav_values.h);
                // Left and Right still stop at the ends.
                switch (spec.kind) {
                    case RowKind::Setting: {
                        const int count = oot::ui::option_count(spec.setting);
                        const int current = oot::ui::get_row(oot::ui::settings(), spec.setting);
                        const bool last = count > 0 && !is_check_setting(spec.setting) && current + 1 >= count;
                        nudge_setting(spec.setting, last ? -current : 1);
                        refresh_rows(screen);
                        break;
                    }
                    case RowKind::Lighting: {
                        const int count = oot::ui::lighting::option_count(spec.setting);
                        const int current = oot::ui::lighting::get_row(oot::ui::lighting::current(), spec.setting);
                        nudge_lighting(spec.setting, (count > 0 && current + 1 >= count) ? -current : 1);
                        refresh_rows(screen);
                        break;
                    }
                    case RowKind::TimeOfDay:
                        nudge_time(1);
                        refresh_rows(screen);
                        break;
                    case RowKind::Piece:
                        nudge_piece(spec.setting, 1);
                        refresh_rows(screen);
                        break;
                    case RowKind::LightingMenu:
                        show(Screen::Lighting);
                        break;
                    case RowKind::HudMenu:
                        show(Screen::Hud);
                        break;
                    case RowKind::CapturePhoto:
                        show(Screen::Photo);
                        break;
                    case RowKind::CaptureVideo:
                        show(Screen::Video);
                        break;
                    case RowKind::LightingReset:
                        oot::ui::lighting::apply(oot::ui::lighting::defaults());
                        refresh_rows(screen);
                        break;
                    case RowKind::LightingSweep:
                        // The menu closes so the frames show the picture alone; the sweep runs
                        // on the renderer's ticks and puts the person's values back at the end.
                        if (oot::sweep::start()) {
                            show(Screen::None);
                        }
                        break;
                    case RowKind::Rom:
                        show(Screen::Browse);
                        break;
                    case RowKind::Controls:
                        show(Screen::Controls);
                        break;
                    case RowKind::Files:
                        show(Screen::Files);
                        break;
                    case RowKind::Moments:
                        show(Screen::Moments);
                        break;
                    case RowKind::Debug:
                        confirm_debugging(screen);
                        break;
                    case RowKind::Warps:
                        show(Screen::Warps);
                        break;
                    case RowKind::Play:
                        if (launch::rom_ready()) {
                            g_launch_note = "Starting";
                            refresh_rows(screen);
                            launch::request_play();
                        }
                        else {
                            g_launch_note = "Choose a ROM first: drop it on the window, or Enter on the ROM row";
                            refresh_rows(screen);
                        }
                        break;
                    case RowKind::Quit:
                        launch::request_quit();
                        break;
                }
                break;
            case Nav::Back:
                // Lighting is a page of Graphics, Photo and Video pages of Capture (2026-10-13).
                if (screen == Screen::Lighting) {
                    show(Screen::Graphics);
                }
                else if (screen == Screen::Photo || screen == Screen::Video) {
                    show(Screen::Capture);
                }
                else if (settings_like(screen)) {
                    show(Screen::None);
                }
                // Back to where it was opened from: the settings in play, the launcher before.
                //
                // THE DEBUGGING DOCUMENT WAS MISSING FROM THIS LINE (the user, 2026-09-30: "the
                // debug menus close button or ability to go back doesn't work at all. when im in
                // that menu i have to close the entire launcher menu and re-open"). Both ways out
                // run through here: the pad's B and Escape arrive as Nav::Back, and the x in the
                // corner is a click on `modal-close`, which calls handle_nav(Nav::Back) so that
                // the button and the key can never mean two different things. With no branch for
                // the screen, both did nothing at all, while the document's own hint line went on
                // promising that Escape and B would go back.
                //
                // The two screens share the branch rather than getting one each, because they are
                // the same case (a document opened from the settings) and a third copy of this
                // line is how the next one gets forgotten in the same way.
                else if (screen == Screen::Debug || screen == Screen::Hud) {
                    // A section's first page (phase 96): Back closes the menu in the game, and
                    // before Play goes back to Play, as every section's does.
                    show(Screen::None);
                }
                break;
            case Nav::Clear:
                break;
        }
    }

    // The About surface's provenance, filled at runtime from the values CMake read out of the
    // checkouts at build time. Written here rather than into the document so it cannot go stale:
    // a version number typed into markup is wrong the moment anything moves.
    // The executable's own signature, read once at start (main/signature.cpp), for the
    // launcher's foot and About.
    oot::signature::Info g_signature;

    // The changelog as RML, built once at start from assets/changelog.json.
    std::string g_changes_rml;

    // The changelog (the user's ask of 2026-09-19, in the global rules' shape): a JSON list of
    // versions, newest first, each with a date, a title, a summary and sections of one-line
    // changes. Hostile like any file: a parse error becomes one line in the document.
    void load_changelog() {
        const std::string path = g_assets_dir + "/changelog.json";
        std::string rml;
        try {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                throw std::runtime_error("the file is missing");
            }
            const nlohmann::json doc = nlohmann::json::parse(in);
            if (!doc.is_array()) {
                throw std::runtime_error("the file is not a list of versions");
            }
            static const char* const ORDER[] = { "added", "changed", "fixed", "removed", "security" };
            static const char* const LABEL[] = { "Added", "Changed", "Fixed", "Removed", "Security" };
            for (const auto& entry : doc) {
                if (!entry.is_object()) {
                    continue;
                }
                rml += "<div class=\"entry\"><div class=\"entry-head\"><span class=\"entry-version\">" +
                       escape(entry.value("version", "")) + "</span><span class=\"entry-date\">" +
                       escape(entry.value("date", "")) + "</span></div>";
                if (entry.contains("title")) {
                    rml += "<div class=\"entry-title\">" + escape(entry.value("title", "")) + "</div>";
                }
                if (entry.contains("summary")) {
                    rml += "<div class=\"entry-summary\">" + escape(entry.value("summary", "")) + "</div>";
                }
                const auto sections = entry.find("sections");
                if (sections != entry.end() && sections->is_object()) {
                    for (size_t i = 0; i < 5; i++) {
                        const auto list = sections->find(ORDER[i]);
                        if (list == sections->end() || !list->is_array() || list->empty()) {
                            continue;
                        }
                        rml += "<div class=\"entry-section\">" + std::string(LABEL[i]) + "</div>";
                        for (const auto& item : *list) {
                            if (item.is_string()) {
                                rml += "<div class=\"entry-item\">" + escape(item.get<std::string>()) + "</div>";
                            }
                        }
                    }
                }
                rml += "</div>";
            }
            if (rml.empty()) {
                rml = "<div class=\"entry-item\">No versions listed</div>";
            }
            std::fprintf(stderr, "[ui] changelog: %zu version(s)\n", static_cast<size_t>(doc.size()));
        }
        catch (const std::exception& e) {
            rml = "<div class=\"entry-item\">The changelog could not be read: " + escape(e.what()) + "</div>";
            std::fprintf(stderr, "[ui] changelog: %s (%s)\n", e.what(), path.c_str());
        }
        g_changes_rml = rml;
    }

    // ------------------------------------------------------------------------------------------
    // THE FILES SURFACE. Where everything actually is, openable, and the way to take it all back
    // off the machine.
    //
    // It exists because of the objection to the packaging change rather than in spite of it: the
    // reason not to put somebody's saves somewhere they did not choose is that they cannot find
    // them again, and a surface that names the REAL resolved paths and opens them answers that
    // directly. The uninstall lives here too, because "where did this put things" and "how do I
    // get rid of it" are the same worry asked twice.
    // ------------------------------------------------------------------------------------------

    // One row: what it is, where it is, and the folder a click should open. A file's row opens the
    // folder holding it, because opening a settings file in whatever claims .txt is not what
    // anybody meant by "show me".
    struct FileRow {
        const char* label;
        std::filesystem::path shown;
        std::filesystem::path open;
    };

    std::vector<FileRow> g_file_rows;
    // Has the destructive button been pressed once? The second press is the one that acts.
    bool g_remove_armed = false;

    std::vector<FileRow> file_rows() {
        std::vector<FileRow> rows;
        const std::filesystem::path settings = oot::places::settings_file();
        const std::filesystem::path controls = oot::places::controls_file();
        // The folder everything below sits in comes first (the user, 2026-09-24: "the main root
        // of the app data"), then the program's own files, then the person's.
        rows.push_back({ "Everything", oot::places::data(), oot::places::data() });
        rows.push_back({ "Program files", oot::places::program(), oot::places::program() });
        rows.push_back({ "Saves", oot::places::saves(), oot::places::saves() });
        rows.push_back({ "Mods", oot::places::mods(), oot::places::mods() });
        // Where a person puts a texture pack (2026-09-30). It is made at start rather than on
        // demand, so this row opens a folder that is there rather than reporting a missing one
        // on a machine that has never used a pack.
        rows.push_back({ "Texture packs", oot::places::texture_packs(), oot::places::texture_packs() });
        rows.push_back({ "Settings", settings, settings.parent_path() });
        rows.push_back({ "Controls", controls, controls.parent_path() });
        rows.push_back({ "Recordings", oot::places::captures(), oot::places::captures() });

        // The ROM is the user's own file in the user's own place, and saying exactly where it is
        // matters more here than anywhere else on this screen: this program reads it where it
        // lies and keeps no copy of its own.
        const std::filesystem::path& rom = launch::rom_path();
        rows.push_back({ "Your copy of the game",
                         rom.empty() ? std::filesystem::path("none chosen yet") : rom,
                         rom.empty() ? std::filesystem::path() : rom.parent_path() });

        if (oot::places::mode() == oot::places::Mode::Installed) {
            const oot::install::Options chosen = oot::install::options();
            if (chosen.start_menu) {
                const std::filesystem::path link = oot::install::shortcut_path();
                rows.push_back({ "Start menu", link, link.parent_path() });
            }
            if (chosen.desktop) {
                const std::filesystem::path link = oot::install::desktop_shortcut_path();
                rows.push_back({ "Desktop shortcut", link, link.parent_path() });
            }
        }
        return rows;
    }

    // The focus on the files surface: the rows first, then the two buttons (Close, Remove), so the
    // pad and the keyboard reach every folder and both buttons (the user, 2026-09-24: "the buttons
    // for delete and back need focus and hover styles"). The buttons take the `on` class the whole
    // interface uses for a lit control.
    int g_ffocus = 0;

    void apply_files_focus() {
        if (g_files_doc == nullptr) {
            return;
        }
        const int rows = static_cast<int>(g_file_rows.size());
        for (int i = 0; i < static_cast<int>(FILES_ROW_SLOTS); ++i) {
            if (Rml::Element* e = g_files_doc->GetElementById("frow-" + std::to_string(i))) {
                e->SetClass("on", i == g_ffocus);
            }
        }
        // THE LIST FOLLOWS THE FOCUS (the user, 2026-10-13: "it doesn't scroll down to other
        // options as i use the controller, instread the selections just continue to move but out
        // of view"): the list scrolled for the wheel but never for a move.
        if (g_ffocus < rows) {
            keep_in_view(g_files_doc->GetElementById("files-rows"), g_files_doc->GetElementById("frow-" + std::to_string(g_ffocus)));
        }
        if (Rml::Element* e = g_files_doc->GetElementById("files-left")) {
            e->SetClass("on", g_ffocus == rows);
        }
        if (Rml::Element* e = g_files_doc->GetElementById("files-right")) {
            e->SetClass("on", g_ffocus == rows + 1);
        }
    }

    void reflect_files() {
        if (g_files_doc == nullptr) {
            return;
        }
        g_file_rows = file_rows();

        std::string mode = oot::places::mode_word();
        if (oot::places::mode() == oot::places::Mode::Installed) {
            mode += ", version " + oot::places::installed_version();
        }
        else {
            mode += ", nothing was installed on this machine";
        }
        set_text(g_files_doc, "files-mode", mode);

        // The document carries a fixed number of row elements, like every other rows document
        // here. Any it does not need is taken out rather than left as an empty stripe.
        for (size_t i = 0; i < FILES_ROW_SLOTS; ++i) {
            const std::string row_id = "frow-" + std::to_string(i);
            if (i >= g_file_rows.size()) {
                hide_row(g_files_doc, static_cast<int>(i), "frow-");
                continue;
            }
            if (Rml::Element* e = g_files_doc->GetElementById(row_id)) {
                e->SetProperty("display", "flex");
            }
            set_text(g_files_doc, "flabel-" + std::to_string(i), g_file_rows[i].label);
            set_text(g_files_doc, "fvalue-" + std::to_string(i),
                     shorten_path(g_file_rows[i].shown.string(), FILES_PATH_CHARS));
        }

        // The two states of the action bar. Nothing destructive is ever one click away: the first
        // press turns the bar into the question, and only the second answers it (the user,
        // 2026-09-24: "delete files MUST have a secondary confirmation"). The words change on both
        // buttons and in the foot, and the button lights its edge, so the second press can never
        // be mistaken for the first.
        // Uninstalling removes the program and nothing of the person's (install.h, remove_program).
        set_text(g_files_doc, "files-left-label", g_remove_armed ? "No, keep it" : "Close");
        if (oot::places::mode() == oot::places::Mode::Installed) {
            set_text(g_files_doc, "files-right-label", g_remove_armed ? "Yes, uninstall" : "Uninstall");
            set_text(g_files_doc, "files-foot",
                     g_remove_armed
                         ? "Uninstall the program? Its Program files folder above is deleted. Your saves and moments, your ROM, texture packs, mods, settings and controls all stay where they are"
                         : "Click a row, or its Open, to open that folder");
        }
        else {
            // Portable installed nothing, so there is nothing here to remove. Saying so is more
            // use than a button that would do nothing.
            set_text(g_files_doc, "files-right-label", "Nothing to remove");
            set_text(g_files_doc, "files-foot",
                     "Click a row, or its Open, to open that folder. This copy runs portable, so it installed nothing");
        }
        if (Rml::Element* e = g_files_doc->GetElementById("files-right")) {
            e->SetClass("armed", g_remove_armed);
        }
        apply_files_focus();
    }

    // Opening a folder is ShellExecute on a directory, which asks the operating system's FILE
    // BROWSER to show it. That is not a dialog, so the rule against native dialogs does not bite
    // here, the same way it does not bite the file picker. Worth saying because it looks like it
    // might.
    void open_in_file_browser(const std::filesystem::path& folder) {
        if (folder.empty()) {
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(folder, ec)) {
            return;
        }
        ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void click_files(const std::string& id) {
        if (id == "files-left") {
            // Cancel the arming if it is armed, otherwise leave.
            if (g_remove_armed) {
                g_remove_armed = false;
                reflect_files();
            }
            else {
                // Your files is a page of the section it was opened from (phase 96).
                show(section_root_screen());
            }
            return;
        }
        if (id == "files-right") {
            if (oot::places::mode() != oot::places::Mode::Installed) {
                return;
            }
            if (!g_remove_armed) {
                g_remove_armed = true;
                reflect_files();
                return;
            }
            const std::string said = oot::install::remove_program();
            std::fprintf(stderr, "[install] %s\n", said.c_str());
            // Nothing is left to run from, so this is the end. The message goes to the log, which
            // stays in the data folder with the person's things.
            launch::request_quit();
            return;
        }
        if (id.compare(0, 5, "frow-") == 0 || id.compare(0, 6, "fopen-") == 0) {
            const int index = std::atoi(id.c_str() + (id[1] == 'r' ? 5 : 6));
            if (index >= 0 && index < static_cast<int>(g_file_rows.size())) {
                g_ffocus = index;
                apply_files_focus();
                open_in_file_browser(g_file_rows[index].open);
            }
        }
    }

    // The pad and the keyboard on the files surface, BY WHERE THINGS ARE DRAWN (the user,
    // 2026-10-13: "the buttons at bottom still behave as a list in one column rather than proper
    // left/right movement like rest of menu"), as the settings list treats its Play and Quit pair:
    // Up and Down walk the rows and step onto the two buttons as one stop (onto the left one),
    // Left and Right move between the two, Up from them goes back to the last row, and the ends
    // wrap as the settings list's do. Accept opens the focused folder or presses the focused
    // button; Back leaves (and disarms a removal on the way, so a half pressed destructive button
    // never waits for the next visit).
    void files_nav(Nav nav) {
        const int rows = static_cast<int>(g_file_rows.size());
        const int left = rows;        // Close, or No, keep everything
        const int right = rows + 1;   // Uninstall, or Yes, uninstall
        const bool on_buttons = g_ffocus >= rows;
        switch (nav) {
            case Nav::Up:
                if (on_buttons) {
                    g_ffocus = rows > 0 ? rows - 1 : left;
                } else {
                    g_ffocus = g_ffocus > 0 ? g_ffocus - 1 : left;
                }
                apply_files_focus();
                break;
            case Nav::Down:
                if (on_buttons) {
                    g_ffocus = rows > 0 ? 0 : g_ffocus;
                } else {
                    g_ffocus = g_ffocus + 1 < rows ? g_ffocus + 1 : left;
                }
                apply_files_focus();
                break;
            case Nav::Left:
            case Nav::Right:
                // Sideways only within the pair: a row has nothing beside it.
                if (on_buttons) {
                    g_ffocus = nav == Nav::Left ? left : right;
                    apply_files_focus();
                }
                break;
            case Nav::Accept:
                if (g_ffocus < rows) {
                    click_files("frow-" + std::to_string(g_ffocus));
                } else if (g_ffocus == rows) {
                    click_files("files-left");
                } else {
                    click_files("files-right");
                }
                break;
            case Nav::Back:
                g_remove_armed = false;
                show(section_root_screen());
                break;
            default:
                break;
        }
    }

    // ------------------------------------------------------------------------------------------
    // THE FIRST RUN QUESTIONS. Two of them, asked once each, in the same sitting.
    //
    // Both were conditions the user attached to the features they belong to rather than polish:
    // the updater "must prompt the user on their first open whether they want to keep that setting
    // enabled or not, and then it should always be an option to enable or disable inside their
    // settings", and the ROM copy "should offer to the user if they would like to copy the ROM
    // into their workspace folder".
    //
    // NEITHER HAS A DEFAULT THAT ACTS BEFORE IT IS ANSWERED. Updates are off until somebody says
    // on, because a program that checks while it is asking permission to check has already done
    // the thing it is asking about. Nothing is copied until somebody says copy.
    // ------------------------------------------------------------------------------------------

    enum class Question { None, Updates, RomCopy };
    Question g_question = Question::None;

    // ------------------------------------------------------------------------------------------
    // A CONFIRMATION ANY PART OF THE INTERFACE CAN ASK FOR, on this same document.
    //
    // Added 2026-09-26 for the two the user asked for: one on the way into the Debugging menu
    // ("it must say that these features are meant for debugging development only and that users
    // must use caution before proceeding") and one on the frame recorder ("an additional warning
    // and confirmation about the space this feature takes up").
    //
    // ON THIS DOCUMENT AND NOT A SECOND ONE. The first run questions already had a case, a title,
    // a question, a detail, two labeled buttons and a foot, which is exactly what a confirmation
    // is; a second document would have been the same thing built twice, drifting apart the moment
    // one of them was restyled. So the questions became the first caller of this rather than the
    // only thing the document can do.
    struct Confirm {
        std::string title;
        std::string question;
        std::string detail;
        std::string no_label = "No";
        std::string yes_label = "Yes";
        std::string foot;
        std::function<void(bool)> answered;
        Screen back = Screen::Settings;   // where No, Escape and a finished Yes return to
    };
    bool g_confirm_open = false;
    Confirm g_confirm;
    void reflect_ask();

    // Put a question up and take the answer to the callback. The caller says where No goes.
    void ask_confirm(Confirm confirm) {
        g_confirm = std::move(confirm);
        g_confirm_open = true;
        reflect_ask();
        show(Screen::Ask);
    }

    // THE WAY INTO THE DEBUGGING MENU, and the warning is the door rather than a line of small
    // print inside it (the user, 2026-09-26: "before opening it, it must say that these features
    // are meant for debugging development only and that users must use caution when proceeding").
    void confirm_debugging(Screen back) {
        Confirm c;
        c.title = "Debugging";
        c.question = "Open the development and debugging tools?";
        c.detail =
            "What is in here is meant for developing and debugging this program, not for playing "
            "the game. These tools move the player between places the game never sends them, "
            "replace the picture with a single stage of how it was drawn, and write every frame to "
            "disk as a file. They can leave the game looking wrong, put you somewhere a save was "
            "not meant to happen, and fill a drive. Nothing here is needed to play, and nothing "
            "here is supported. Go carefully, and put anything you change back when you are done.";
        c.no_label = "No, go back";
        c.yes_label = "Yes, I understand";
        c.foot = "The lighting and the settings menus hold everything meant for playing";
        c.back = back;
        c.answered = [](bool yes) {
            if (yes) {
                show(Screen::Debug);
            }
        };
        ask_confirm(std::move(c));
    }

    // Is this change the frame recorder being switched ON? Only that one direction is gated: a
    // person turning it off, or moving any other row, is never asked anything.
    bool recorder_needs_warning(int row, int wanted) {
        static const int recorder = oot::ui::row_index("recorder");
        return (recorder >= 0) && (row == recorder) && (wanted == 1) &&
               (oot::ui::get_row(oot::ui::settings(), row) == 0);
    }

    // WHAT A RECORDING ACTUALLY COSTS, in this person's own numbers rather than in general terms
    // (the user, 2026-09-26: "explaining that it's png sequences and at the resolution and frame
    // rate set so several hundred gigs can be eaten up very very quickly if not careful").
    //
    // The per frame figure is MEASURED, not guessed: the frames this program has written at 1920
    // by 1080 and 2560 by 1440 run between two and three megabytes each, so a byte and a fifth per
    // pixel is a fair middle for this game's art. It is presented as "about" because the real size
    // depends on what is on screen, and a dark room compresses to a fraction of a sunlit field.
    std::string recording_cost_text() {
        const oot::ui::Settings& s = oot::ui::settings();
        const int width = (g_width > 0) ? g_width : 1920;
        const int height = (g_height > 0) ? g_height : 1080;

        // The rate the row asks for. "Match the display" cannot be known from here, so sixty
        // stands in for it and the sentence says "about"; the game's own rate is twenty.
        static const int rate_row = oot::ui::row_index("framerate");
        const int rate_choice = (rate_row >= 0) ? oot::ui::get_row(s, rate_row) : 1;
        static constexpr int FIXED[] = { 30, 60, 75, 90, 120, 144, 165, 240 };
        const int rate = (rate_choice >= 2 && rate_choice - 2 < static_cast<int>(sizeof(FIXED) / sizeof(FIXED[0])))
                             ? FIXED[rate_choice - 2]
                             : (rate_choice == 0 ? 20 : 60);
        const char* rate_label = (rate_row >= 0) ? oot::ui::option_label(rate_row, rate_choice) : "";

        const int seconds = oot::recorder::length_seconds(s.record_length);
        const double per_frame = double(width) * double(height) * 1.2;
        const double one = per_frame * double(rate) * double(seconds);
        const double minute = per_frame * double(rate) * 60.0;

        // How long until a hundred gigabytes have gone, which is the number that actually says
        // "be careful" to a person looking at a drive with room on it.
        const double minutes_to_100gb = (minute > 0.0) ? (100e9 / minute) : 0.0;

        char text[1000];
        std::snprintf(text, sizeof(text),
                      "This writes ONE PNG FILE FOR EVERY FRAME the program shows. At %d by %d and "
                      "%s, that is about %d files and roughly %.1f GB for each %d second recording, "
                      "and about %.0f GB for every minute it is left running. At that rate a "
                      "hundred gigabytes is about %.0f minutes of recording, and a drive can go "
                      "from comfortable to full while you are still playing. The files are written "
                      "to %s and nothing removes them for you. A recorded frame is also a picture "
                      "of the game's own art, so keep them off anything public.",
                      width, height, (rate_label != nullptr && *rate_label != '\0') ? rate_label : "the current frame rate",
                      rate * seconds, one / 1e9, seconds, minute / 1e9, minutes_to_100gb,
                      oot::places::captures().string().c_str());
        return std::string(text);
    }

    void confirm_recorder(int row, Screen back) {
        Confirm c;
        c.title = "The frame recorder";
        c.question = "Switch on the frame recorder?";
        c.detail = recording_cost_text();
        c.no_label = "No, leave it off";
        c.yes_label = "Yes, I have the space";
        c.foot = "Recording also slows the game a little, and the notes file says by how much";
        c.back = back;
        c.answered = [row](bool yes) {
            if (!yes) {
                return;
            }
            oot::ui::Settings next = oot::ui::settings();
            oot::ui::set_row(next, row, 1);
            oot::ui::apply(next);
        };
        ask_confirm(std::move(c));
    }

    // The ROM question only makes sense once there IS a ROM to copy and somewhere of our own to
    // put it, so it waits for both rather than being asked into the void on a fresh install.
    bool rom_copy_worth_asking() {
        if (oot::ui::rom_choice_made() || oot::places::mode() != oot::places::Mode::Installed) {
            return false;
        }
        const std::filesystem::path& rom = launch::rom_path();
        if (rom.empty()) {
            return false;
        }
        // Already inside our own folder, which is what accepting the offer produces. Asking then
        // would be asking to copy a file onto itself.
        std::error_code ec;
        return !rom.parent_path().empty() &&
               !std::filesystem::equivalent(rom.parent_path(), oot::places::roms(), ec);
    }

    // Which question is due, if any. Returns false when there is nothing left to ask, which is
    // the normal state after the first run.
    bool ask_next_question() {
        if (!oot::ui::update_choice_made()) {
            g_question = Question::Updates;
            return true;
        }
        if (rom_copy_worth_asking()) {
            g_question = Question::RomCopy;
            return true;
        }
        g_question = Question::None;
        return false;
    }

    // Which of a question's two answers has the focus (Left and Right move it): No by default.
    bool g_ask_yes = false;

    void ask_focus_draw() {
        if (g_ask_doc == nullptr) {
            return;
        }
        if (Rml::Element* no = g_ask_doc->GetElementById("ask-no")) {
            no->SetClass("on", !g_ask_yes);
        }
        if (Rml::Element* yes = g_ask_doc->GetElementById("ask-yes")) {
            yes->SetClass("on", g_ask_yes);
        }
    }

    void reflect_ask() {
        if (g_ask_doc == nullptr) {
            return;
        }
        ask_focus_draw();
        // A confirmation wins over the first run questions, which only ever run before there is
        // anything else on screen anyway.
        if (g_confirm_open) {
            set_text(g_ask_doc, "ask-title", g_confirm.title);
            set_text(g_ask_doc, "ask-question", g_confirm.question);
            set_text(g_ask_doc, "ask-detail", g_confirm.detail);
            set_text(g_ask_doc, "ask-no-label", g_confirm.no_label);
            set_text(g_ask_doc, "ask-yes-label", g_confirm.yes_label);
            set_text(g_ask_doc, "ask-foot", g_confirm.foot);
            return;
        }
        if (g_question == Question::Updates) {
            set_text(g_ask_doc, "ask-title", "Before you start");
            set_text(g_ask_doc, "ask-question", "Should this check for updates?");
            // What the update check does (updates.cpp), and word for word what the website's install
            // page shows of this question (ProgramMenu.astro, ASK): change both together.
            set_text(g_ask_doc, "ask-detail",
                     "When it opens, and every four hours while it runs, it would ask the release "
                     "page whether a newer version exists. On an installed copy a newer one "
                     "downloads in the background and installs the next time it opens. That is the "
                     "only connection this program ever makes, and it is off until you say "
                     "otherwise. Nothing about you, your machine or your copy of the game is sent.");
            set_text(g_ask_doc, "ask-no-label", "No, stay offline");
            set_text(g_ask_doc, "ask-yes-label", "Yes, check for updates");
            set_text(g_ask_doc, "ask-foot", "You can change this at any time in the settings");
            return;
        }
        if (g_question == Question::RomCopy) {
            const std::string size = rom_size_text();
            set_text(g_ask_doc, "ask-title", "Your copy of the game");
            set_text(g_ask_doc, "ask-question", "Keep a copy of it with this program?");
            set_text(g_ask_doc, "ask-detail",
                     "Right now this reads your file where you keep it, and remembers the path. "
                     "A copy here (" + size + ") means it is always in one place, even if you "
                     "move or tidy the original. Your own file is never moved, changed or deleted "
                     "either way, and neither copy is ever sent anywhere.");
            set_text(g_ask_doc, "ask-no-label", "No, read it where it is");
            set_text(g_ask_doc, "ask-yes-label", "Yes, keep a copy here");
            set_text(g_ask_doc, "ask-foot", "Your files shows where both of them are");
        }
    }

    void click_ask(bool yes) {
        // A confirmation answers its own caller and goes back where it came from. The callback is
        // taken by value and the flag cleared FIRST, so a callback that opens another confirmation
        // (the recorder's, from inside the Debugging menu) is not immediately closed again by this
        // one's return.
        if (g_confirm_open) {
            const Confirm asked = g_confirm;
            g_confirm_open = false;
            g_confirm = Confirm{};
            if (asked.answered) {
                asked.answered(yes);
            }
            // The callback may have raised another question, or gone somewhere itself. Only when
            // it did neither does this return to where the question was raised from, so a Yes that
            // opens a menu is not immediately closed again by its own confirmation.
            if (!g_confirm_open && g_screen == Screen::Ask) {
                show(asked.back);
            }
            return;
        }

        // THE FLAG IS MARKED BEFORE apply(), AND THE ORDER IS THE WHOLE THING. apply() is what
        // writes the file, and the two "already asked" flags go out with it, so marking one
        // afterward sets it in memory and never records it: the question comes back on the next
        // launch, for ever. That is exactly what the first version of this did.
        if (g_question == Question::Updates) {
            // The row is found by its key, never by a number: a fixed index (9) sat here from
            // the day the prompt was written until 2026-09-23, and when two rows were inserted
            // above it on the 22nd, Yes started switching on the frame recorder and Updates
            // stayed off. The user saw both.
            oot::ui::Settings next = oot::ui::settings();
            const int row = oot::ui::row_index("updates");
            if (row >= 0) {
                oot::ui::set_row(next, row, yes ? 1 : 0);
            }
            oot::ui::mark_update_choice_made();
            oot::ui::apply(next);
        }
        else if (g_question == Question::RomCopy) {
            if (yes) {
                launch::request_keep_rom_copy();
            }
            oot::ui::mark_rom_choice_made();
            oot::ui::apply(oot::ui::settings());
        }

        // Straight on to the next question if there is one, so first run asks once rather than
        // coming back a second time.
        if (ask_next_question()) {
            reflect_ask();
            return;
        }
        show(g_game_started ? Screen::None : Screen::Launch);
    }

    void reflect_about() {
        if (g_about_doc == nullptr) {
            return;
        }
        auto set = [](const char* id, const std::string& text) {
            if (Rml::Element* e = g_about_doc->GetElementById(id)) {
                e->SetInnerRML(escape(text));
            }
        };
        set("about-version", oot::build_info::version);
        set("about-author", oot::build_info::author);
        set("about-recompiler", oot::build_info::recompiler);
        set("about-runtime", oot::build_info::runtime);
        set("about-renderer", oot::build_info::renderer);
        set("about-decomp", oot::build_info::decomp);
        set("about-built", oot::build_info::built_on);

        // The signature: what Windows said of this executable, offline, at start.
        const oot::signature::Info& s = g_signature;
        std::string status = s.present ? (s.valid ? "Valid" : "Not verified offline") : "Not signed";
        if (!s.error.empty()) {
            status += " (" + s.error + ")";
        }
        set("about-sign-status", status);
        set("about-signer", s.signer.empty() ? "none" : s.signer);
        set("about-issuer", s.issuer.empty() ? "none" : s.issuer);
        set("about-signed-on", s.signed_on.empty() ? (s.present ? "no timestamp" : "never") : s.signed_on);
        set("about-valid", s.valid_from.empty() ? "no certificate" : s.valid_from + " to " + s.valid_until);

        if (Rml::Element* e = g_about_doc->GetElementById("about-changes")) {
            e->SetInnerRML(g_changes_rml);
        }
        // THE CREDITS (phase 106): every bundled face and icon set with its license, from the
        // manifest the build checks, so a file added to the bundle is credited without a line here.
        if (Rml::Element* e = g_about_doc->GetElementById("about-credits")) {
            std::ifstream manifest(std::filesystem::path(g_assets_dir) / "licenses" / "MANIFEST.txt", std::ios::binary);
            std::stringstream text;
            text << manifest.rdbuf();
            std::string rml;
            for (const oot::ui::credits::Credit& c : oot::ui::credits::from_manifest(text.str())) {
                rml += "<div class=\"field\"><span class=\"datum\">" + escape(oot::ui::credits::describe(c)) + "</span></div>";
            }
            rml += "<div class=\"field\"><span class=\"datum\">Everything else drawn here is this program's own, GPL-3.0</span></div>";
            e->SetInnerRML(rml);
        }
        size_long_rows(g_about_doc, "about-body", ABOUT_SHARE);
        if (Rml::Element* body = g_about_doc->GetElementById("about-body")) {
            body->SetScrollTop(0.0f);
        }
    }

    // ------------------------------------------------------------------------------------------
    // The ROM browser (play milestone): the drives, then a folder's folders and ROM files.
    // ------------------------------------------------------------------------------------------

    void apply_browse_focus() {
        if (g_browse_doc == nullptr) {
            return;
        }
        for (size_t i = 0; i < g_browse.size(); ++i) {
            if (Rml::Element* e = g_browse_doc->GetElementById("brow-" + std::to_string(i))) {
                const bool on = static_cast<int>(i) == g_bfocus;
                e->SetClass("on", on);
                if (on) {
                    e->ScrollIntoView(Rml::ScrollIntoViewOptions{ Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest, list_scroll() });
                }
            }
        }
    }

    void reflect_browse() {
        if (g_browse_doc == nullptr) {
            return;
        }
        Rml::Element* rows = g_browse_doc->GetElementById("browse-rows");
        if (rows == nullptr) {
            return;
        }
        std::string rml;
        if (g_browse.empty()) {
            rml = "<div class=\"row\" id=\"brow-0\"><span class=\"label\">Nothing here</span><span class=\"value\"></span></div>";
        }
        for (size_t i = 0; i < g_browse.size(); ++i) {
            const launch::Entry& e = g_browse[i];
            const std::string n = std::to_string(i);
            rml += "<div class=\"row\" id=\"brow-" + n + "\"><span class=\"label\">" + escape(e.name) +
                   "</span><span class=\"value\">" + (e.directory ? "folder" : "ROM") + "</span></div>";
        }
        replace_rows(g_browse_doc, rows, rml, [&]() { size_long_rows(g_browse_doc, "browse-rows"); });
        if (g_bfocus >= static_cast<int>(g_browse.size())) {
            g_bfocus = 0;
        }
        apply_browse_focus();
        set_text(g_browse_doc, "browse-path", g_browse_dir.empty() ? std::string("Drives") : utf8(g_browse_dir));
    }

    void browse_load(const std::filesystem::path& dir) {
        g_browse = dir.empty() ? launch::drives() : launch::list(dir);
        g_browse_dir = dir;
        g_bfocus = 0;
        reflect_browse();
    }

    // The browser's path field (2026-09-19). The text becomes a path: a file:// URL loses its
    // scheme and its percent escapes, quotes around a pasted path go, and so does the space
    // at either end.
    std::filesystem::path path_from_text(std::string text) {
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) {
                s.pop_back();
            }
            size_t start = 0;
            while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
                start++;
            }
            s.erase(0, start);
        };
        trim(text);
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            text = text.substr(1, text.size() - 2);
            trim(text);
        }
        const std::string scheme = "file://";
        if (text.compare(0, scheme.size(), scheme) == 0) {
            text.erase(0, scheme.size());
            if (!text.empty() && text.front() == '/' && text.size() > 2 && text[2] == ':') {
                text.erase(0, 1);   // file:///C:/... keeps its drive
            }
            std::string decoded;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
                    decoded += static_cast<char>(std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                }
                else {
                    decoded += text[i];
                }
            }
            text = decoded;
        }
        if (text.empty()) {
            return {};
        }
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    Rml::ElementFormControl* browse_input() {
        if (g_browse_doc == nullptr) {
            return nullptr;
        }
        return dynamic_cast<Rml::ElementFormControl*>(g_browse_doc->GetElementById("browse-input"));
    }

    void blur_text_entry() {
        if (g_context != nullptr) {
            if (Rml::Element* focused = g_context->GetFocusElement()) {
                if (focused->GetTagName() == "input") {
                    focused->Blur();
                }
            }
        }
    }

    // Enter in the field, or the picker's choice: the path goes to the main thread to be
    // validated like any other, and the launcher's row and note say what came of it.
    void use_typed_path() {
        Rml::ElementFormControl* input = browse_input();
        if (input == nullptr) {
            return;
        }
        const std::filesystem::path path = path_from_text(input->GetValue());
        if (path.empty()) {
            return;
        }
        g_launch_note = "Checking " + utf8(path.filename());
        launch::request_select_rom(path);
        blur_text_entry();
        show(Screen::Launch);
    }

    // ------------------------------------------------------------------------------------------
    // THE ON-SCREEN KEYBOARD (the menus plan, phase 110): so a pad can type a path in the browser.
    // A layer over the menu (assets/ui/keyboard.rml) opened with Y on a pad (R on the keys) or the
    // prompt; its keys are walked by the layout engine (src/ui/nav/), the first surface to move onto
    // it, and its text is src/ui/keyboard/. Done writes the line into the path field and takes it,
    // opening a folder or choosing a ROM; physical typing still goes straight to the field.
    // ------------------------------------------------------------------------------------------
    Rml::ElementDocument* g_keyboard_doc = nullptr;
    bool g_kb_open = false;
    oot::ui::keyboard::Line g_kb_line;
    int g_kb_focus = -1;
    oot::ui::nav::Memory g_kb_memory;

    bool g_kb_open_flag() {
        return g_kb_open;
    }

    void kb_draw() {
        // As many characters as the line's box holds: the mono face advances six tenths of its
        // size, so a long path shows the part at the caret rather than running off the panel.
        size_t width = 0;
        if (Rml::Element* line = g_keyboard_doc->GetElementById("kb-line")) {
            if (Rml::Element* box = line->GetParentNode()) {
                const float size = box->GetComputedValues().font_size();
                const float room = box->GetBox().GetSize(Rml::BoxArea::Content).x;
                if (size > 0.0f && room > 0.0f) {
                    width = static_cast<size_t>(room / (size * 0.6f));
                }
            }
        }
        set_text(g_keyboard_doc, "kb-line", width > 0 ? g_kb_line.shown(width) : g_kb_line.shown());
        if (Rml::Element* shift = g_keyboard_doc->GetElementById("key-3-0")) {
            shift->SetClass("on", g_kb_line.shift());
        }
    }

    void kb_focus(int node, const oot::ui::nav::RmlModel& m) {
        if (node < 0 || node >= static_cast<int>(m.elements.size())) {
            return;
        }
        for (Rml::Element* e : m.elements) {
            e->SetClass("is-f", false);
        }
        m.elements[static_cast<size_t>(node)]->SetClass("is-f", true);
        g_kb_focus = node;
        oot::ui::nav::remember(m.model, g_kb_memory, node);
    }

    void close_keyboard() {
        if (!g_kb_open) {
            return;
        }
        g_kb_open = false;
        if (g_keyboard_doc != nullptr) {
            g_keyboard_doc->Hide();
        }
        refresh_shell();
    }

    void open_keyboard() {
        if (g_keyboard_doc == nullptr || g_kb_open) {
            return;
        }
        std::string start;
        if (Rml::ElementFormControl* input = browse_input()) {
            start = input->GetValue();
        }
        if (start.empty() && !g_browse_dir.empty()) {
            start = utf8(g_browse_dir);
            if (start.back() != '\\') {
                start += '\\';
            }
        }
        g_kb_line = oot::ui::keyboard::Line(start);
        g_keyboard_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        g_keyboard_doc->PullToFront();
        g_kb_open = true;
        // Laid out now, so the engine has the keys' boxes for the first move.
        g_context->Update();
        const oot::ui::nav::RmlModel m = oot::ui::nav::build(g_keyboard_doc);
        kb_focus(g_kb_focus >= 0 ? g_kb_focus : oot::ui::nav::first_focus(m.model, g_kb_memory), m);
        kb_draw();
        refresh_shell();
        say("keyboard");
    }

    // The key an element is: its id is its row and column in the keyboard's rows.
    std::string key_of(Rml::Element* e) {
        int r = -1;
        int c = -1;
        if (e == nullptr || std::sscanf(e->GetId().c_str(), "key-%d-%d", &r, &c) != 2) {
            return "";
        }
        const auto& rows = oot::ui::keyboard::rows();
        if (r < 0 || r >= static_cast<int>(rows.size()) || c < 0 || c >= static_cast<int>(rows[static_cast<size_t>(r)].size())) {
            return "";
        }
        return rows[static_cast<size_t>(r)][static_cast<size_t>(c)];
    }

    void kb_press(const std::string& key) {
        if (key.empty()) {
            return;
        }
        if (g_kb_line.press(key)) {
            close_keyboard();
            if (Rml::ElementFormControl* input = browse_input()) {
                input->SetValue(g_kb_line.text());
            }
            const std::filesystem::path path = path_from_text(g_kb_line.text());
            std::error_code ec;
            if (!path.empty() && std::filesystem::is_directory(path, ec)) {
                browse_load(path);
            }
            else {
                use_typed_path();
            }
            return;
        }
        kb_draw();
    }

    void keyboard_nav(Nav nav) {
        namespace nv = oot::ui::nav;
        const nv::RmlModel m = nv::build(g_keyboard_doc);
        const auto go = [&](nv::Dir d) {
            const int next = nv::move(m.model, g_kb_memory, g_kb_focus, d);
            if (next >= 0) {
                kb_focus(next, m);
            }
        };
        switch (nav) {
            case Nav::Up:          go(nv::Dir::Up); break;
            case Nav::Down:        go(nv::Dir::Down); break;
            case Nav::Left:        go(nv::Dir::Left); break;
            case Nav::Right:       go(nv::Dir::Right); break;
            case Nav::Accept:
                if (g_kb_focus >= 0 && g_kb_focus < static_cast<int>(m.elements.size())) {
                    kb_press(key_of(m.elements[static_cast<size_t>(g_kb_focus)]));
                }
                break;
            case Nav::Back:        close_keyboard(); break;
            case Nav::Clear:       kb_press(oot::ui::keyboard::BACKSPACE); break;
            case Nav::SectionPrev: g_kb_line.left(); kb_draw(); break;
            case Nav::SectionNext: g_kb_line.right(); kb_draw(); break;
            default:               break;
        }
    }

    // A click on a key presses it, and the focus goes there too.
    void click_key(const std::string& id) {
        if (!g_kb_open || g_keyboard_doc == nullptr) {
            return;
        }
        Rml::Element* e = g_keyboard_doc->GetElementById(id);
        const oot::ui::nav::RmlModel m = oot::ui::nav::build(g_keyboard_doc);
        const auto found = m.index.find(e);
        if (found != m.index.end()) {
            kb_focus(found->second, m);
        }
        kb_press(key_of(e));
    }

    // Whether the field has the focus, for the pump's routing; taken once a frame.
    void poll_text_entry() {
        bool active = false;
        if (g_context != nullptr && g_screen == Screen::Browse) {
            if (Rml::Element* focused = g_context->GetFocusElement()) {
                active = focused->GetTagName() == "input";
            }
        }
        g_text_entry.store(active, std::memory_order_relaxed);
    }

    // SDL's scancodes to RmlUi's keys, for the editing a path field needs; anything else is
    // text and arrives as such.
    Rml::Input::KeyIdentifier rml_key(int scancode) {
        switch (scancode) {
            case SDL_SCANCODE_BACKSPACE: return Rml::Input::KI_BACK;
            case SDL_SCANCODE_DELETE:    return Rml::Input::KI_DELETE;
            case SDL_SCANCODE_LEFT:      return Rml::Input::KI_LEFT;
            case SDL_SCANCODE_RIGHT:     return Rml::Input::KI_RIGHT;
            case SDL_SCANCODE_HOME:      return Rml::Input::KI_HOME;
            case SDL_SCANCODE_END:       return Rml::Input::KI_END;
            case SDL_SCANCODE_A:         return Rml::Input::KI_A;
            case SDL_SCANCODE_C:         return Rml::Input::KI_C;
            case SDL_SCANCODE_V:         return Rml::Input::KI_V;
            case SDL_SCANCODE_X:         return Rml::Input::KI_X;
            default:                     return Rml::Input::KI_UNKNOWN;
        }
    }

    void browse_nav(Nav nav) {
        const int count = static_cast<int>(g_browse.size());
        switch (nav) {
            // Y on a pad, R on the keys: the on-screen keyboard, to type a path (phase 110).
            case Nav::Default:
                open_keyboard();
                break;
            case Nav::Up:
                if (count > 0) {
                    g_bfocus = (g_bfocus + count - 1) % count;
                    apply_browse_focus();
                }
                break;
            case Nav::Down:
                if (count > 0) {
                    g_bfocus = (g_bfocus + 1) % count;
                    apply_browse_focus();
                }
                break;
            case Nav::Accept: {
                if (count == 0 || g_bfocus < 0 || g_bfocus >= count) {
                    break;
                }
                const launch::Entry entry = g_browse[static_cast<size_t>(g_bfocus)];
                if (entry.directory) {
                    browse_load(entry.path);
                }
                else {
                    // Validated on the main thread; the launcher's row and note follow.
                    g_launch_note = "Checking " + entry.name;
                    launch::request_select_rom(entry.path);
                    show(Screen::Launch);
                }
                break;
            }
            case Nav::Back:
                if (g_browse_dir.empty()) {
                    show(Screen::Launch);
                }
                else if (!g_browse.empty() && g_browse.front().name == "..") {
                    browse_load(g_browse.front().path);
                }
                else {
                    browse_load(std::filesystem::path());
                }
                break;
            default:
                break;
        }
    }

    // ------------------------------------------------------------------------------------------
    // Controls (phase 50)
    // ------------------------------------------------------------------------------------------

    void set_controls_note(const std::string& text) {
        set_text(g_controls_doc, "controls-note", text);
    }

    void apply_controls_focus() {
        if (g_controls_doc == nullptr) {
            return;
        }
        for (int i = 0; i < controls::row_count(); ++i) {
            const std::string n = std::to_string(i);
            if (Rml::Element* e = g_controls_doc->GetElementById("crow-" + n)) {
                e->SetClass("on", i == g_cfocus);
                if (i == g_cfocus) {
                    e->ScrollIntoView(Rml::ScrollIntoViewOptions{ Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest, list_scroll() });
                }
            }
            for (int s = 0; s < oot::input::BINDINGS_PER_INPUT; ++s) {
                if (Rml::Element* e = g_controls_doc->GetElementById("cslot-" + n + "-" + std::to_string(s))) {
                    e->SetClass("pick", i == g_cfocus && s == g_cslot);
                }
            }
        }
    }

    // Build the rows from the model. Rebuilt whole on every change, which at the rate a person
    // presses keys costs nothing and keeps one path rather than a fast path and a slow one that
    // have to agree.
    void reflect_controls() {
        if (g_controls_doc == nullptr) {
            return;
        }
        Rml::Element* rows = g_controls_doc->GetElementById("controls-rows");
        if (rows == nullptr) {
            return;
        }
        std::string rml;
        bool headed = false;
        for (int i = 0; i < controls::row_count(); ++i) {
            const controls::Row r = controls::row(i);
            const std::string n = std::to_string(i);

            // THE COLUMNS ARE NAMED, once, above the first binding row (the user, 2026-09-27:
            // "not sure what the second column is for or it's not apparent"). Both bindings are
            // live at the same time and either one works, which is why the pad defaults put the C
            // buttons on the right stick AND on face buttons; nothing on screen said so, so the
            // second column read as a duplicate of the first. This heading is markup rather than
            // a row in the model on purpose: it must not be something the selection can land on.
            if (r.kind == controls::RowKind::Binding && !headed) {
                headed = true;
                // `cols`, NOT `heading`: case.rcss already has a `.heading` for a section title
                // and it is display: block, which turned this row from a flex row into a stack
                // and left the names sitting well left of the columns they name. The rest of that
                // story is in controls.rcss beside the rule.
                rml += "<div class=\"row cols\"><span class=\"label\">N64 control</span>"
                       "<span class=\"slots\"><span class=\"slot\">Binding</span>"
                       "<span class=\"slot\">Alternate</span></span></div>";
            }

            // A section of its own begins here (Photo mode, 2026-10-09): a divider that is markup,
            // not a row, so the selection never lands on it; its badge while the row is Off.
            const std::string section = controls::section_before(i);
            if (!section.empty()) {
                const std::string section_badge = controls::section_badge(i);
                rml += "<div class=\"divider\"><span class=\"divider-label\">" + escape(section) + "</span>" +
                       (section_badge.empty() ? std::string() : "<span class=\"badge\">" + escape(section_badge) + "</span>") +
                       "</div>";
            }

            // The badge, while the settings row this control needs is Off (2026-10-09).
            const std::string badge = controls::row_badge(i);
            // A badged label is the name with the badge on a line of its own under it: beside it,
            // the longer badges folded in two in the panel's narrow label column.
            if (badge.empty()) {
                rml += "<div class=\"row\" id=\"crow-" + n + "\"><span class=\"label\" id=\"clabel-" + n + "\">" +
                       escape(controls::row_label(i)) + "</span>";
            }
            else {
                rml += "<div class=\"row\" id=\"crow-" + n + "\"><span class=\"label badged\" id=\"clabel-" + n + "\">" +
                       "<span class=\"label-text\">" + escape(controls::row_label(i)) + "</span>" +
                       "<span class=\"badge\">" + escape(badge) + "</span></span>";
            }
            if (r.kind == controls::RowKind::Binding) {
                rml += "<span class=\"slots\">";
                for (int s = 0; s < oot::input::BINDINGS_PER_INPUT; ++s) {
                    // A binding the free camera is currently holding the stick out from under is
                    // struck through rather than removed: it is still bound, still saved, and
                    // works again the moment free aim goes off.
                    const std::string idle = controls::slot_idle(i, s) ? " idle" : "";
                    rml += "<span class=\"slot" + idle + "\" id=\"cslot-" + n + "-" + std::to_string(s) + "\">" +
                           escape(controls::row_value(i, s)) + "</span>";
                }
                rml += "</span>";
            }
            else {
                rml += "<span class=\"value\" id=\"cvalue-" + n + "\">" + escape(controls::row_value(i, 0)) + "</span>";
            }
            rml += "</div>";
        }
        replace_rows(g_controls_doc, rows, rml);

        // The reason, under the rows it explains. Empty means nothing is being ignored, and the
        // element takes no room.
        const std::string idle_note = controls::idle_note();
        set_text(g_controls_doc, "controls-notice", idle_note);
        if (Rml::Element* notice = g_controls_doc->GetElementById("controls-notice")) {
            notice->SetClass("gone", idle_note.empty());
        }

        size_long_rows(g_controls_doc, "controls-rows");
        if (g_cfocus >= controls::row_count()) {
            g_cfocus = 0;
        }
        apply_controls_focus();

        std::string note = controls::device_name(controls::device());
        if (controls::device_is_raw(controls::device())) {
            note += ", bound by its own buttons";
        }
        // Found on 2026-09-23 with the user's DualShock behind DS4Windows: the program was handed
        // an Xbox 360 pad, the touchpad click bound to Menu never arrived, and the capture could
        // not hear it either. Nothing here can see through the layer; it can say that one is there.
        if (controls::device_through_layer(controls::device())) {
            note += ". Seen as an Xbox 360 pad: if it is really a PlayStation pad behind DS4Windows or Steam Input, the touchpad and PS button arrive only as the buttons that layer maps them to";
        }
        set_controls_note(note);
    }

    void show_capture_panel(bool open) {
        if (g_controls_doc == nullptr) {
            return;
        }
        if (Rml::Element* e = g_controls_doc->GetElementById("capture")) {
            e->SetClass("open", open);
        }
        if (Rml::Element* e = g_controls_doc->GetElementById("crow-" + std::to_string(g_cfocus))) {
            e->SetClass("scanning", open && !g_capture_settling);
        }
    }

    void begin_controls_capture() {
        if (!controls::begin_capture(g_cfocus, g_cslot)) {
            return;
        }
        g_capture_open = true;
        g_capture_settling = false;
        g_capture_since = Clock::now();
        set_text(g_controls_doc, "capture-title", controls::capture_title(g_cfocus, g_cslot));
        set_text(g_controls_doc, "capture-line", "Listening \xC2\xB7 5s \xC2\xB7 Esc cancels");
        show_capture_panel(true);
    }

    void settle_capture() {
        g_capture_settling = true;
        g_capture_since = Clock::now();
        show_capture_panel(true);   // stays up, no longer scanning
    }

    void end_capture_panel() {
        g_capture_open = false;
        g_capture_settling = false;
        show_capture_panel(false);
    }

    // Once per frame while the document is open: the capture's three moments, waiting with its
    // countdown, bound, and timed out, read from the device layer.
    void poll_capture() {
        if (!g_capture_open) {
            return;
        }
        const double elapsed = seconds_since(g_capture_since);
        if (g_capture_settling) {
            if (elapsed >= CAPTURE_LINGER_SECONDS) {
                end_capture_panel();
            }
            return;
        }

        using Status = devices::CaptureStatus;
        switch (devices::capture_status()) {
            case Status::Waiting: {
                int left = CAPTURE_SECONDS - static_cast<int>(elapsed);
                if (left < 0) {
                    left = 0;
                }
                set_text(g_controls_doc, "capture-line",
                         "Listening \xC2\xB7 " + std::to_string(left) + "s \xC2\xB7 Esc cancels");
                break;
            }
            case Status::Bound:
                reflect_controls();
                set_text(g_controls_doc, "capture-line", "Bound to " + controls::row_value(g_cfocus, g_cslot));
                settle_capture();
                break;
            case Status::TimedOut:
                set_text(g_controls_doc, "capture-line", "Nothing pressed");
                settle_capture();
                break;
            case Status::Canceled:
            case Status::Idle:
                end_capture_panel();
                break;
        }
    }

    void controls_nav(Nav nav) {
        if (g_capture_open && !g_capture_settling) {
            // A capture is listening: no way out but a press or a cancel. A keyboard capture
            // takes Escape itself in the pump; this is the pad's, canceled from the keyboard.
            if (nav == Nav::Back) {
                devices::cancel_capture();
            }
            return;
        }

        const int count = controls::row_count();
        if (count == 0) {
            return;
        }
        const controls::Row r = controls::row(g_cfocus);

        switch (nav) {
            case Nav::Default: case Nav::SectionPrev: case Nav::SectionNext:
                return;   // the shell's (phase 96); these older screens have none
            case Nav::Up:
                g_cfocus = (g_cfocus + count - 1) % count;
                apply_controls_focus();
                break;
            case Nav::Down:
                g_cfocus = (g_cfocus + 1) % count;
                apply_controls_focus();
                break;
            case Nav::Left:
                if (r.kind == controls::RowKind::Binding) {
                    g_cslot = 0;
                    apply_controls_focus();
                }
                else if (controls::nudge(g_cfocus, -1)) {
                    reflect_controls();
                }
                break;
            case Nav::Right:
                if (r.kind == controls::RowKind::Binding) {
                    g_cslot = 1;
                    apply_controls_focus();
                }
                else if (controls::nudge(g_cfocus, 1)) {
                    reflect_controls();
                }
                break;
            case Nav::Accept:
                if (r.kind == controls::RowKind::Binding) {
                    begin_controls_capture();
                }
                else if (r.kind == controls::RowKind::Reset) {
                    controls::reset_defaults();
                    reflect_controls();
                    set_controls_note("Defaults restored");
                }
                else if (controls::nudge(g_cfocus, 1)) {
                    reflect_controls();
                }
                break;
            case Nav::Back:
                show(Screen::None);
                break;
            case Nav::Clear:
                if (r.kind == controls::RowKind::Binding) {
                    if (controls::clear(g_cfocus, g_cslot)) {
                        reflect_controls();
                    }
                    else {
                        set_controls_note("The menu keeps one key");
                    }
                }
                break;
        }
    }

    void click_row(Screen screen, int row) {
        switch (screen) {
            // THE BASELINE'S SECOND DEFECT: Debugging had no case here, so a click on a row's body
            // did nothing and only its arrows worked. Its rows are built like the others'.
            case Screen::Settings:
            case Screen::Graphics:
            case Screen::Gameplay:
            case Screen::Capture:
            case Screen::Photo:
            case Screen::Video:
            case Screen::Lighting:
            case Screen::Hud:
            case Screen::Debug:
            case Screen::Launch: {
                const int count = static_cast<int>(rows_for(screen).size());
                if (row < 0 || row >= count) {
                    return;
                }
                g_focus = row;
                apply_rows_focus(screen);
                rows_nav(screen, Nav::Accept);
                break;
            }
            case Screen::Browse:
                if (row < 0 || row >= static_cast<int>(g_browse.size())) {
                    return;
                }
                g_bfocus = row;
                apply_browse_focus();
                browse_nav(Nav::Accept);
                break;
            case Screen::Controls:
                if (row < 0 || row >= controls::row_count()) {
                    return;
                }
                g_cfocus = row;
                apply_controls_focus();
                controls_nav(Nav::Accept);
                break;
            default:
                break;
        }
    }

    void click_chrome(const std::string& id) {
        if (id == "win-min") {
            launch::request_minimize();
        }
        else if (id == "win-max") {
            launch::request_maximize();
        }
        else if (id == "win-close") {
            launch::request_quit();
        }
    }

    // ------------------------------------------------------------------------------------------
    // The startup hint
    // ------------------------------------------------------------------------------------------

    // THE TOAST, for anything worth one line over play. It was the startup hint's alone until
    // the quick keys needed it (the user, 2026-09-27: "can we make it show a small overlay,
    // saying State saved"). One surface, one set of timers, one fade: a second toast built
    // beside this one would be two things to keep agreeing.
    void show_toast(const std::string& lead, const std::string& note, double seconds) {
        if (g_hint_doc == nullptr) {
            return;
        }
        set_text(g_hint_doc, "hint-lead", lead);
        set_text(g_hint_doc, "hint-note", note);
        if (Rml::Element* line = g_hint_doc->GetElementById("hint-note")) {
            line->SetClass("gone", note.empty());
        }
        if (Rml::Element* toast = g_hint_doc->GetElementById("toast")) {
            toast->SetClass("gone", false);
        }
        g_hint_doc->Show();
        g_hint_visible = true;
        g_hint_fading = false;
        g_hint_since = Clock::now();
        g_hint_seconds = seconds;
        g_hint_wants_activity.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[ui] toast: %s%s%s\n", lead.c_str(), note.empty() ? "" : ". ", note.c_str());
    }

    void show_hint() {
        if (g_hint_doc == nullptr) {
            return;
        }
        std::string lead;
        std::string note;
        controls::menu_shortcut(lead, note);
        show_toast(lead, note, HINT_SECONDS);
        // What it last said, so a changed shortcut shows it again.
        g_hint_last = lead + "|" + note;
    }

    void dismiss_hint() {
        if (!g_hint_visible || g_hint_fading) {
            return;
        }
        if (Rml::Element* toast = g_hint_doc->GetElementById("toast")) {
            toast->SetClass("gone", true);
        }
        g_hint_fading = true;
        g_hint_since = Clock::now();
        g_hint_wants_activity.store(false, std::memory_order_relaxed);
    }

    void poll_hint() {
        if (!g_hint_visible || g_hint_doc == nullptr) {
            return;
        }
        if (!g_hint_fading) {
            if (seconds_since(g_hint_since) >= g_hint_seconds) {
                dismiss_hint();
            }
        }
        else if (seconds_since(g_hint_since) >= HINT_FADE_SECONDS) {
            g_hint_doc->Hide();
            g_hint_visible = false;
            g_hint_fading = false;
            say("hint gone");
        }
    }

    // ------------------------------------------------------------------------------------------
    // The game window's frame
    // ------------------------------------------------------------------------------------------

    void frame_show() {
        if (g_frame_doc == nullptr || g_frame_shown) {
            return;
        }
        g_frame_doc->Show();
        g_frame_doc->PushToBack();
        g_frame_shown = true;
        g_frame_since = Clock::now();
    }

    // Once per frame while the game runs: which parts of the frame are up right now.
    void poll_frame() {
        if (g_frame_doc == nullptr || !g_frame_shown) {
            return;
        }
        const bool fullscreen = g_fullscreen.load(std::memory_order_relaxed);
        const bool header = g_pointer_in_band ||
                            seconds_since(g_band_left_at) < HEADER_LINGER_SECONDS ||
                            seconds_since(g_frame_since) < HEADER_INTRO_SECONDS;
        const bool edge = !fullscreen;
        if (header != g_header_shown) {
            g_header_shown = header;
            if (Rml::Element* e = g_frame_doc->GetElementById("frame-bar")) {
                e->SetClass("hidden", !header);
            }
            // The frame rate counter keeps its distance from the bar (the user's note of
            // 2026-09-19): under it while it shows, up at the window's edge when it hides.
            if (g_counter_doc != nullptr) {
                if (Rml::Element* plate = g_counter_doc->GetElementById("counter")) {
                    plate->SetClass("under-bar", header);
                }
            }
            // The recorder's plate is the same piece of hardware in the opposite corner and moves
            // with it (the user's ask of 2026-09-23), so the two sit level in both states. What
            // makes that safe for something you have to CLICK is note_pointer above: the bar's
            // state is frozen while the pointer is on the plate, so it cannot move away from a
            // click that is on its way.
            if (g_recorder_doc != nullptr) {
                if (Rml::Element* plate = g_recorder_doc->GetElementById("recorder")) {
                    plate->SetClass("under-bar", header);
                }
            }
        }
        if (edge != g_edge_shown) {
            g_edge_shown = edge;
            if (Rml::Element* e = g_frame_doc->GetElementById("frame-edge")) {
                e->SetClass("hidden", !edge);
            }
        }
        // The document is shown only while the bar or the edge is, because being shown is what keeps
        // the interface layer drawing (showing()). Fullscreen with the bar up and gone again leaves
        // nothing of it on screen, and it must not keep the layer drawing for nothing. Both parts
        // hide at once (display: none), so hiding the document with them looks the same.
        const bool up = header || edge;
        if (up != g_frame_doc->IsVisible()) {
            if (up) {
                g_frame_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
                g_frame_doc->PushToBack();
            }
            else {
                g_frame_doc->Hide();
            }
        }
    }

    // The frame rate counter (the user's ask of 2026-09-19): the frames the render hook is
    // called for, counted, and the rate shown twice a second while the row is on and the game
    // is running. Presented frames are what the hook sees, so this is the rate the picture
    // actually changes at, interpolated frames included.
    bool g_counter_shown = false;
    int g_counter_frames = 0;
    Clock::time_point g_counter_since{};
    int g_counter_traces = 0;

    // THE LETTERBOX BARS (2026-09-24; the user: "the black bar at top and bottom of screen
    // animates in very very chappy rather than smooth when z targeting"). The game makes them by
    // scissoring the picture, and a display list carries one scissor per game update, so the bars
    // can only step twenty times a second however fast the picture is presented. The patch hands
    // the size it wants to the settings, which hold the easing (letterbox_now), and this draws
    // the pair over the picture at the rate the picture is presented.
    //
    // They are the WINDOW's full width rather than the picture's. Where the picture fills the
    // window, which is what expanding to the window does and what the person plays in, the two
    // are the same; where it does not, the bars run over ground that is already black.
    bool g_letterbox_shown = false;
    float g_letterbox_drawn = -1.0f;

    void poll_letterbox() {
        if (g_letterbox_doc == nullptr) {
            return;
        }
        const float share = g_game_started ? oot::ui::letterbox_now() : 0.0f;
        const bool on = share > 0.0005f;
        if (on != g_letterbox_shown) {
            g_letterbox_shown = on;
            if (on) {
                g_letterbox_doc->Show();
            }
            else {
                g_letterbox_doc->Hide();
                g_letterbox_drawn = -1.0f;
            }
        }
        if (!on) {
            return;
        }
        // A tenth of a pixel is under what any display can show, and skipping the write keeps the
        // document from being laid out again on a frame where nothing moved.
        const float pixels = share * static_cast<float>(g_height);
        if (g_letterbox_drawn >= 0.0f && std::fabs(pixels - g_letterbox_drawn) < 0.1f) {
            return;
        }
        g_letterbox_drawn = pixels;
        char height[32];
        std::snprintf(height, sizeof(height), "%.2fpx", static_cast<double>(pixels));
        if (Rml::Element* bar = g_letterbox_doc->GetElementById("letterbox-top")) {
            bar->SetProperty("height", height);
        }
        if (Rml::Element* bar = g_letterbox_doc->GetElementById("letterbox-bottom")) {
            bar->SetProperty("height", height);
        }
    }

    void poll_counter() {
        const bool on = g_game_started && g_counter_doc != nullptr && oot::ui::settings().counter == 1;
        if (on != g_counter_shown) {
            g_counter_shown = on;
            if (on) {
                g_counter_doc->Show();
                g_counter_frames = 0;
                g_counter_since = Clock::now();
                set_text(g_counter_doc, "counter-value", "0");
                if (Rml::Element* plate = g_counter_doc->GetElementById("counter")) {
                    plate->SetClass("under-bar", g_header_shown);
                }
            }
            else {
                g_counter_doc->Hide();
            }
        }
        if (!on) {
            return;
        }
        g_counter_frames++;
        const Clock::time_point now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - g_counter_since).count();
        if (elapsed >= 0.5) {
            const int fps = static_cast<int>(static_cast<double>(g_counter_frames) / elapsed + 0.5);
            set_text(g_counter_doc, "counter-value", std::to_string(fps));
            g_counter_frames = 0;
            g_counter_since = now;
            // A trace line every ten seconds or so, for the harness.
            if (g_counter_traces++ % 20 == 0) {
                std::fprintf(stderr, "[ui] counter %d fps\n", fps);
            }
        }
    }

    // Is the pointer on the recorder's plate, wherever the plate currently is? Asked of RmlUi
    // rather than computed from the tokens, so it stays true if the plate is ever restyled.
    bool pointer_on_plate(int x, int y) {
        if (g_recorder_doc == nullptr || !g_recorder_shown) {
            return false;
        }
        Rml::Element* plate = g_recorder_doc->GetElementById("recorder");
        return plate != nullptr &&
               plate->IsPointWithinElement(Rml::Vector2f(static_cast<float>(x), static_cast<float>(y)));
    }

    void note_pointer(int x, int y) {
        // WHILE THE POINTER IS ON THE RECORDER'S PLATE, THE BAR DOES NOT CHANGE STATE. That one
        // rule is what lets the plate move with the bar AND still be clickable, and it is worth
        // the paragraph because the two look incompatible.
        //
        // The bar appears when the pointer enters the top band of the window, and the plate sits
        // under the bar when it shows and at the window's edge when it does not. So a pointer
        // traveling up toward the plate raises the bar, which drops the plate to where the
        // cursor has just been: the plate slides out from under the click every time. That was
        // real, not theoretical, and it is why the plate was pinned in place for a day.
        //
        // Freezing the state while the pointer is ON it settles both positions at once. Reach the
        // plate with the bar hidden and it stays up; reach it with the bar showing and it stays
        // down. Either way it stops moving the moment you are on it, and everywhere else the bar
        // behaves exactly as it always did.
        if (pointer_on_plate(x, y)) {
            return;
        }
        const bool in_band = y < HEADER_BAND;
        if (g_pointer_in_band && !in_band) {
            g_band_left_at = Clock::now();
        }
        g_pointer_in_band = in_band;
    }

    // ------------------------------------------------------------------------------------------
    // The debug menu: every warp, nested by location (the user's ask of 2026-09-22)
    // ------------------------------------------------------------------------------------------

    int g_wfocus = 0;

    // ------------------------------------------------------------------------------------------
    // The time of day, sitting above the regions in the debug menu (the user's ask of 2026-09-23:
    // "This would help test the different theories of, you know, when something goes worse in one
    // time of day than another").
    // ------------------------------------------------------------------------------------------

    struct TimePreset {
        const char* label;
        int clock;      // the game's own 16 bit day time
    };

    // THE EIGHT ARE CHOSEN AROUND THE GAME'S OWN TWO BOUNDARIES, not spread evenly round the
    // clock. The game calls it night above 18:00 or below 06:30 (z_play.c), so 06:00 is NIGHT
    // and 18:00 is DAY, which is exactly the kind of thing a fault that "works differently
    // depending on whether it's day or night" will sit on. Four each side, with a pair either
    // side of each boundary, so both can be crossed in one step.
    //
    // The numbers are the decomp's own CLOCK_TIME(hr, min), which is (hr * 60 + min) * 65536 /
    // 1440, worked out here because a stylesheet of a macro cannot be included in native code.
    // Each is written with the hour it came from so it can be checked by eye.
    constexpr TimePreset TIMES[] = {
        { "00:00 night",  0 },        // midnight
        { "06:00 night",  16384 },    // before the 06:30 boundary, so still night
        { "08:00 day",    21845 },
        { "12:00 day",    32768 },    // noon
        { "15:00 day",    40960 },
        { "18:00 day",    49152 },    // the boundary itself, which the game counts as day
        { "18:30 night",  50517 },
        { "21:00 night",  57344 },
    };
    constexpr int TIME_COUNT = static_cast<int>(sizeof(TIMES) / sizeof(TIMES[0]));

    int g_time_index = 3;   // noon, which is the least surprising place to start

    // The control is offered at the top level only. Inside a region the list is places, and a
    // row that appears and disappears as you walk in and out would be a worse menu than one that
    // keeps it in one place.
    // THE TIME ROW HAS LEFT THIS DOCUMENT (the user, 2026-09-30): it is the second row of the
    // Debugging list now, right under Warps, because reaching the one control he uses most while
    // testing lighting should not mean opening the warps document first. Kept as a function
    // returning false rather than deleted along with everything that counts rows around it: the
    // offsets below are written in terms of it and are correct at zero.
    bool warps_has_time_row() {
        return false;
    }

    int warps_time_rows() {
        return warps_has_time_row() ? 1 : 0;
    }

    // How many rows the document shows: the warp rows, plus the time row when it is offered.
    int warps_row_total() {
        return warps::row_count() + warps_time_rows();
    }

    // A document row index to a warp index, or -1 for the time row.
    int warps_index_for_row(int row) {
        const int offset = warps_time_rows();
        return (row < offset) ? -1 : row - offset;
    }

    // ONE STEPPER FOR BOTH SURFACES (2026-09-30). The time row moved out of the warps document
    // into the Debugging list, and while it briefly lived in both there were two copies of this
    // arithmetic. It wraps, so walking round the clock is one direction rather than a return trip.
    void apply_time();

    const char* time_label() {
        return TIMES[g_time_index].label;
    }

    // A painted element's switch, round and round: Shown, Paint taken out, Hidden.
    void nudge_piece(int index, int direction) {
        const std::vector<oot::room_pieces::Piece> pieces = oot::room_pieces::list();
        if (index < 0 || index >= static_cast<int>(pieces.size())) {
            return;
        }
        const int count = oot::room_pieces::STATE_COUNT;
        const int next = (oot::room_pieces::state_of(pieces[index]) + direction + count) % count;
        oot::room_pieces::set_state(pieces[index], next);
    }

    void nudge_time(int by) {
        g_time_index += by;
        if (g_time_index < 0) {
            g_time_index = TIME_COUNT - 1;
        }
        if (g_time_index >= TIME_COUNT) {
            g_time_index = 0;
        }
        apply_time();
    }

    void apply_time() {
        oot::game_state::set_pending_day_time(TIMES[g_time_index].clock);
        std::fprintf(stderr, "[time] asked for %s (0x%04X)\n",
                     TIMES[g_time_index].label, TIMES[g_time_index].clock);
    }

    // The game's own clock as a person reads it. dayTime counts 0 to 0x10000 across a day, so a
    // minute is 65536 / 1440.
    std::string clock_text(unsigned int day_time, int night_flag) {
        const int minutes = static_cast<int>(static_cast<unsigned long long>(day_time) * 1440ULL / 65536ULL);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "the game says %02d:%02d, %s",
                      minutes / 60, minutes % 60, night_flag ? "night" : "day");
        return buffer;
    }

    void apply_warps_focus() {
        if (g_warps_doc == nullptr) {
            return;
        }
        for (int i = 0; i < warps_row_total(); ++i) {
            if (Rml::Element* e = g_warps_doc->GetElementById("wrow-" + std::to_string(i))) {
                e->SetClass("on", i == g_wfocus);
                if (i == g_wfocus) {
                    e->ScrollIntoView(Rml::ScrollIntoViewOptions{ Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest, list_scroll() });
                }
            }
        }
    }

    // Built whole on every change, exactly as the controls list is: at a few hundred rows this
    // costs nothing at the rate a person presses keys, and it keeps one path rather than a fast
    // one and a slow one that have to agree.
    // `keep_focus` is for the time row. Rebuilding normally takes the focus from the WARP
    // list, which is right when the level has just changed and wrong when the only thing that
    // happened was the clock being stepped: the focus would jump off the time row onto the first
    // region after every press, so a second press stepped nothing and the third walked into a
    // region instead.
    // ------------------------------------------------------------------------------------------
    // Moments (phase 77): the saved moments as a list of slots, with Save here, Resume and Delete.
    // ------------------------------------------------------------------------------------------

    // An element id cannot carry a minus sign comfortably, so the autosaves are "a0" to "a4"
    // (newest first), the reserved "r1" to "r20", and the player's slots keep their own numbers.
    std::string moment_row_id(int slot) {
        if (oot::moments::is_reserved(slot)) {
            return "r" + std::to_string(slot - oot::moments::RESERVED_FIRST + 1);
        }
        return oot::moments::is_autosave(slot)
            ? ("a" + std::to_string(oot::moments::AUTOSAVE_SLOT - slot))
            : std::to_string(slot);
    }

    int moment_slot_of_row(const std::string& id) {
        if (!id.empty() && id[0] == 'a') {
            return oot::moments::AUTOSAVE_SLOT - std::atoi(id.c_str() + 1);
        }
        if (!id.empty() && id[0] == 'r') {
            return oot::moments::RESERVED_FIRST + std::atoi(id.c_str() + 1) - 1;
        }
        return std::atoi(id.c_str());
    }

    std::string moment_slot_name(int slot) {
        if (oot::moments::is_reserved(slot)) {
            return "Reserved " + std::to_string(slot - oot::moments::RESERVED_FIRST + 1);
        }
        if (!oot::moments::is_autosave(slot)) {
            return "Slot " + std::to_string(slot);
        }
        const int age = oot::moments::AUTOSAVE_SLOT - slot;
        return age == 0 ? std::string("Autosave, the newest") : ("Autosave, " + std::to_string(age + 1) + " back");
    }

    // Whether the reserve button can move the chosen moment: into the reserved while fewer than
    // twenty hold one, or out of them while one of your eight is empty.
    bool moment_can_move() {
        if (!moment_present(g_mfocus)) {
            return false;
        }
        if (!oot::moments::is_reserved(g_mfocus)) {
            return oot::moments::reserved_count() < oot::moments::RESERVED_COUNT;
        }
        for (int slot = 1; slot <= oot::moments::SLOT_COUNT; ++slot) {
            if (!moment_present(slot)) {
                return true;
            }
        }
        return false;
    }

    // WHICH BUTTONS THIS STATE OFFERS, in the order they sit on screen. Cancel is always there,
    // because the user asked for a way out from the buttons themselves; Delete and Resume need a
    // file; Save here needs the game running and never writes over an autosave, which the
    // autosave owns, or a reserved moment, which nothing writes over; the reserve button needs a
    // file and somewhere to move it.
    std::vector<std::string> moment_buttons() {
        std::vector<std::string> ids;
        if (moment_present(g_mfocus)) {
            ids.push_back("moments-delete");
        }
        if (moment_can_move()) {
            ids.push_back("moments-reserve");
        }
        if (g_game_started && !oot::moments::is_autosave(g_mfocus) && !oot::moments::is_reserved(g_mfocus)) {
            ids.push_back("moments-save");
        }
        if (moment_present(g_mfocus)) {
            ids.push_back("moments-load");
        }
        ids.push_back("moments-cancel");
        return ids;
    }

    void apply_moments_focus() {
        if (g_moments_doc == nullptr) {
            return;
        }
        for (int slot : moment_order()) {
            if (Rml::Element* e = g_moments_doc->GetElementById("mrow-" + moment_row_id(slot))) {
                // The chosen row keeps its mark while the cursor is on the buttons, so a person
                // can always see what Load would load; only the CURSOR moves down.
                e->SetClass("on", slot == g_mfocus);
                e->SetClass("cursor", (slot == g_mfocus) && (g_mzone == MomentZone::Rows));
                if (slot == g_mfocus) {
                    e->ScrollIntoView(Rml::ScrollIntoViewOptions{ Rml::ScrollAlignment::Nearest,
                                                                  Rml::ScrollAlignment::Nearest, list_scroll() });
                }
            }
        }

        const std::vector<std::string> buttons = moment_buttons();
        if (g_mbutton < 0 || g_mbutton >= static_cast<int>(buttons.size())) {
            g_mbutton = static_cast<int>(buttons.size()) - 1;   // Cancel, which is always offered
        }
        for (const char* id : { "moments-delete", "moments-reserve", "moments-save", "moments-load", "moments-cancel" }) {
            if (Rml::Element* e = g_moments_doc->GetElementById(id)) {
                const bool offered = std::find(buttons.begin(), buttons.end(), std::string(id)) != buttons.end();
                e->SetClass("off", !offered);
                e->SetClass("on", (g_mzone == MomentZone::Buttons) && offered &&
                                  (buttons[static_cast<size_t>(g_mbutton)] == id));
            }
        }
        // The buttons say what they will do to THIS slot. Save here is the game's alone (the
        // launcher has no frame to keep) and never the autosave's; Resume and Delete need a file.
        const bool present = moment_present(g_mfocus);
        const bool reserved = oot::moments::is_reserved(g_mfocus);
        set_text(g_moments_doc, "moments-save-label",
                 reserved ? "Reserved, kept as it is"
                 : !g_game_started ? "Save in play"
                 : (oot::moments::is_autosave(g_mfocus) ? "The autosave's own" : (present ? "Save over" : "Save here")));
        // The reserve button says where the moment would go, and why it cannot when it cannot.
        set_text(g_moments_doc, "moments-reserve-label",
                 reserved ? (moment_can_move() ? "Unreserve" : "Your slots are full")
                 : (present && !moment_can_move()) ? "Reserved is full"
                 : "Mark as reserved");
        set_text(g_moments_doc, "moments-load-label", present ? "Load" : "Nothing here");
        set_text(g_moments_doc, "moments-delete-label", present ? (g_moment_delete_armed ? "Yes, delete it" : "Delete") : "Empty");
        if (Rml::Element* e = g_moments_doc->GetElementById("moments-delete")) {
            e->SetClass("armed", g_moment_delete_armed);
        }
    }

    void reflect_moments() {
        if (g_moments_doc == nullptr) {
            return;
        }
        Rml::Element* rows = g_moments_doc->GetElementById("moment-rows");
        if (rows == nullptr) {
            return;
        }
        std::string rml;
        bool divided = false;
        bool divided_reserved = false;
        const std::vector<int> order = moment_order();
        for (int slot : order) {
            // THE DIVIDER the user asked for, once, where the autosaves end and their own slots
            // begin. It is not a row: nothing focuses it and nothing can be done to it.
            if (!divided && !oot::moments::is_autosave(slot)) {
                divided = true;
                rml += "<div class=\"divider\"><span class=\"divider-label\">Your slots</span></div>";
            }
            // And the reserved's, with how many of the twenty are taken.
            if (!divided_reserved && oot::moments::is_reserved(slot)) {
                divided_reserved = true;
                const int taken = static_cast<int>(std::count_if(order.begin(), order.end(),
                                                                 [](int s) { return oot::moments::is_reserved(s); }));
                rml += "<div class=\"divider\"><span class=\"divider-label\">Reserved, " + std::to_string(taken) + " of " +
                       std::to_string(oot::moments::RESERVED_COUNT) + "</span></div>";
            }
            oot::moments::Header header{};
            std::vector<uint32_t> save_words;
            std::vector<uint32_t> context_words;
            std::string why;
            const std::filesystem::path file = oot::moments::slot_file(slot, ".moment");
            std::error_code ec;
            const bool exists = std::filesystem::is_regular_file(file, ec);
            const bool ok = exists && oot::moments::read_slot(file, header, save_words, context_words, why);
            g_moment_present[moment_index(slot)] = ok;

            rml += "<div class=\"row mrow\" id=\"mrow-" + moment_row_id(slot) + "\">";
            const std::filesystem::path thumb = oot::moments::slot_file(slot, ".rgba");
            // A moment from before the raw picture was written beside its PNG has only the PNG,
            // which this interface cannot draw: the raw one is made from it once, here.
            if (ok && !std::filesystem::is_regular_file(thumb, ec)) {
                const std::filesystem::path png = oot::moments::slot_file(slot, ".png");
                if (std::filesystem::is_regular_file(png, ec) && oot::recorder::raw_from_png(png)) {
                    std::fprintf(stderr, "[ui] moment %d's picture made from its PNG\n", slot);
                }
            }
            if (ok && std::filesystem::is_regular_file(thumb, ec)) {
                // Forward slashes, because the document's own path is joined in front otherwise.
                std::string src = thumb.string();
                for (char& c : src) { if (c == '\\') c = '/'; }
                // AND ITS VERSION, its time and size, so a picture written again under the same
                // name (a slot saved over, the autosaves stepping back, a moment reserved and
                // another saved where it was) is a new picture to the interface's cache, which
                // otherwise keeps the first one it loaded under a name, or a failed load, for good.
                const auto stamp = std::filesystem::last_write_time(thumb, ec).time_since_epoch().count();
                src += "?v=" + std::to_string(stamp) + "-" + std::to_string(std::filesystem::file_size(thumb, ec));
                rml += "<img class=\"thumb\" src=\"" + escape(src) + "\"/>";
            }
            else {
                rml += "<div class=\"thumb empty\"></div>";
            }
            rml += "<span class=\"label\">";
            // WHERE IT WAS TAKEN, BY NAME (the user, 2026-10-13: "a user friendly location along with
            // it: Fire Temple, Forest Temple, Inside Jabu-Jabu, Lake Hylia, Temple of Time, Mask Shop").
            // New moments are named for their place when taken; an older one still named "Scene 52 at
            // 12:00" is shown by its place the same way, from the entrance it kept.
            const oot::warps::Named where = ok ? oot::warps::place_of(static_cast<int>(header.entrance)) : oot::warps::Named{};
            std::string title = ok ? std::string(header.name) : std::string();
            const std::string old_name = "Scene " + std::to_string(header.scene);
            if (ok && where.place != nullptr && title.compare(0, old_name.size(), old_name) == 0) {
                title = std::string(where.place) + title.substr(old_name.size());
            }
            if (ok) {
                rml += escape(title) + "<span class=\"slot\">" + escape(moment_slot_name(slot)) + "</span>";
            }
            else if (exists) {
                rml += escape(moment_slot_name(slot)) + "<span class=\"slot\">" + escape(why) + "</span>";
            }
            else {
                rml += escape(moment_slot_name(slot)) + "<span class=\"slot\">Empty</span>";
            }
            rml += "</span>";
            rml += "<span class=\"value\">";
            if (ok) {
                // The wall clock the moment was taken at, as the machine shows dates.
                char when[32] = {};
                const std::time_t t = static_cast<std::time_t>(header.captured_at);
                std::tm tm{};
                localtime_s(&tm, &t);
                std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
                // The region the place sits in, beside the date; the scene's number where the
                // catalog has no place for it.
                const std::string area = (where.region != nullptr) ? std::string(where.region)
                                                                   : (std::string("Scene ") + std::to_string(header.scene));
                rml += escape(area + " \xC2\xB7 " + when);
            }
            rml += "</span></div>";
        }
        replace_rows(g_moments_doc, rows, rml,
                     [&]() { cap_long_rows(g_moments_doc, "moment-rows", rows_cap(Screen::Moments)); });
        if (std::find(order.begin(), order.end(), g_mfocus) == order.end()) {
            // A reserved moment deleted or moved out takes its row with it: the choice goes to the
            // row just above where it was, the last of your slots when it was the first reserved.
            int above = oot::moments::SLOT_COUNT;
            for (int slot : order) {
                if (oot::moments::is_reserved(slot) && slot < g_mfocus) {
                    above = slot;
                }
            }
            g_mfocus = oot::moments::is_reserved(g_mfocus) ? above : oot::moments::AUTOSAVE_SLOT;
        }
        apply_moments_focus();
    }

    void moments_leave() {
        g_moment_delete_armed = false;
        g_mzone = MomentZone::Rows;
        // Moments is a section of its own (phase 96): leaving it closes the menu in the game.
        show(Screen::None);
    }

    void moments_save_here() {
        if (!g_game_started || oot::moments::is_autosave(g_mfocus)) {
            return;
        }
        // The capture happens on the game's next safe frame, and this document is closed first so
        // the frame kept is the game, not the menu over it.
        oot::moments::request_capture(g_mfocus, 60, "");
        g_moment_delete_armed = false;
        show(Screen::None);
    }

    void moments_resume() {
        // Every slot loads, the autosaves included (the user, 2026-09-25: "we also need to allow
        // loading any of them even the autosaves"). Nothing here has ever refused one; what was
        // missing was a way to reach the button at all.
        if (!moment_present(g_mfocus)) {
            return;
        }
        if (oot::moments::request_load(g_mfocus)) {
            g_moment_delete_armed = false;
            if (g_game_started) {
                show(Screen::None);
            }
            else {
                g_launch_note = "Resumes " + moment_slot_name(g_mfocus) + " once the game has loaded a file";
                show(Screen::Launch);
            }
        }
    }

    // MARK AS RESERVED, AND BACK (the user, 2026-10-13). The moment moves, and the choice and the
    // cursor go with it, so the button under the cursor now reads Unreserve and a second press puts
    // it back. A move the list did not allow (another window filled the last place) says so.
    void moments_reserve() {
        if (!moment_present(g_mfocus)) {
            return;
        }
        const bool out = oot::moments::is_reserved(g_mfocus);
        int to = oot::moments::NO_SLOT;
        const oot::moments::Move moved = out ? oot::moments::unreserve(g_mfocus, to) : oot::moments::reserve(g_mfocus, to);
        g_moment_delete_armed = false;
        switch (moved) {
            case oot::moments::Move::Done:
                g_mfocus = to;
                oot::moments::notice(out ? "Back in your slots" : "Reserved: nothing will write over it");
                break;
            case oot::moments::Move::NoRoom:
                oot::moments::notice(out ? "Your slots are full: delete one first"
                                         : "All 20 reserved are taken: delete or unreserve one first");
                break;
            case oot::moments::Move::NothingThere:
            case oot::moments::Move::Failed:
                oot::moments::notice("The moment could not be moved");
                break;
        }
        reflect_moments();
        // The cursor stays on the reserve button where it is still offered, so a second press undoes it.
        const std::vector<std::string> buttons = moment_buttons();
        const auto at = std::find(buttons.begin(), buttons.end(), std::string("moments-reserve"));
        if (at != buttons.end()) {
            g_mbutton = static_cast<int>(at - buttons.begin());
            apply_moments_focus();
        }
    }

    void moments_delete() {
        if (!moment_present(g_mfocus)) {
            return;
        }
        if (!g_moment_delete_armed) {
            g_moment_delete_armed = true;
            apply_moments_focus();
            return;
        }
        std::error_code ec;
        std::filesystem::remove(oot::moments::slot_file(g_mfocus, ".moment"), ec);
        std::filesystem::remove(oot::moments::slot_file(g_mfocus, ".png"), ec);
        std::filesystem::remove(oot::moments::slot_file(g_mfocus, ".rgba"), ec);
        std::fprintf(stderr, "MOMENT_DELETED %d\n", g_mfocus);
        g_moment_delete_armed = false;
        reflect_moments();
    }

    // One press on a button, whatever pressed it. Kept apart from the click handler so the pad
    // and the keyboard go through exactly the same door as the mouse.
    void moments_press(const std::string& id) {
        if (id == "moments-save") {
            moments_save_here();
        }
        else if (id == "moments-load") {
            moments_resume();
        }
        else if (id == "moments-delete") {
            moments_delete();
        }
        else if (id == "moments-reserve") {
            moments_reserve();
        }
        else if (id == "moments-cancel") {
            moments_leave();
        }
    }

    // A ROW TAKES ONE PRESS AND HANDS THE CURSOR TO THE BUTTONS (2026-09-25, the user: "i want
    // the ability to press any one of them once and it makes that the selection and then moves
    // the cursor/focus to the buttons to either load, or cancel"). Before this a row only moved
    // a highlight and the buttons could be reached by the pad's Left and Right alone, which is
    // why a moment could be chosen and never loaded.
    void moments_choose(int slot) {
        if (!oot::moments::slot_exists(slot)) {
            return;
        }
        g_mfocus = slot;
        g_moment_delete_armed = false;
        g_mzone = MomentZone::Buttons;
        // On the button a person most likely wants: Load where there is something to load, and
        // Save here on an empty slot in play. Cancel otherwise, which is always last.
        const std::vector<std::string> buttons = moment_buttons();
        const std::string wanted = moment_present(slot) ? "moments-load" : "moments-save";
        const auto at = std::find(buttons.begin(), buttons.end(), wanted);
        g_mbutton = (at != buttons.end()) ? static_cast<int>(at - buttons.begin())
                                          : static_cast<int>(buttons.size()) - 1;
        apply_moments_focus();
    }

    void click_moments(const std::string& id) {
        if (id.compare(0, 5, "mrow-") == 0 && id.size() > 5) {
            moments_choose(moment_slot_of_row(id.substr(5)));
            return;
        }
        // A click on a button acts on it and moves the cursor there, so the keyboard and the pad
        // carry on from where the mouse left off rather than somewhere else.
        const std::vector<std::string> buttons = moment_buttons();
        const auto at = std::find(buttons.begin(), buttons.end(), id);
        if (at != buttons.end()) {
            g_mzone = MomentZone::Buttons;
            g_mbutton = static_cast<int>(at - buttons.begin());
        }
        moments_press(id);
    }

    // WHERE EACH BUTTON SITS (moments.rml, .actions.pairs, two to a row): Delete and Mark as
    // reserved on the top row, Save over and Load in the middle, Cancel alone across the bottom. A
    // button not offered keeps its place on screen.
    void moment_button_cell(const std::string& id, int& row, int& col) {
        row = (id == "moments-cancel") ? 2 : (id == "moments-save" || id == "moments-load") ? 1 : 0;
        col = (id == "moments-reserve" || id == "moments-load") ? 1 : 0;
    }

    // THE BUTTON IN A DIRECTION, BY WHERE THE BUTTONS ARE DRAWN (the user, 2026-10-12: "if i select
    // one, then Load is the focused button. so then pushing up on left stick SHOULD land on Delete, but
    // instead it unselects. same for if i press Right to select/focus the cancel button and then press
    // up to select save over, it doesn't move the focus to the save over button"). A row up or down
    // keeps the column where that button is offered and otherwise takes the other one of that row; left
    // and right cross to the other button of the same row. The index into `buttons`, or -1 when nothing
    // offered lies that way.
    int moment_button_toward(const std::vector<std::string>& buttons, int drow, int dcol) {
        const int count = static_cast<int>(buttons.size());
        if (g_mbutton < 0 || g_mbutton >= count) {
            return -1;
        }
        int row = 0;
        int col = 0;
        moment_button_cell(buttons[static_cast<size_t>(g_mbutton)], row, col);
        const int want_row = row + drow;
        const int want_col = col + dcol;
        if (want_row < 0 || want_row > 2 || want_col < 0 || want_col > 1) {
            return -1;
        }
        int other_in_row = -1;
        for (int i = 0; i < count; ++i) {
            int r = 0;
            int c = 0;
            moment_button_cell(buttons[static_cast<size_t>(i)], r, c);
            if (r != want_row) {
                continue;
            }
            if (c == want_col) {
                return i;
            }
            if (drow != 0) {
                other_in_row = i;
            }
        }
        return other_in_row;
    }

    void moments_nav(Nav nav) {
        const std::vector<int> order = moment_order();
        const std::vector<std::string> buttons = moment_buttons();
        const int count = static_cast<int>(order.size());
        const auto here = std::find(order.begin(), order.end(), g_mfocus);
        const int at = (here != order.end()) ? static_cast<int>(here - order.begin()) : 0;

        switch (nav) {
            // IN THE ROWS, up and down walk the list. ON THE BUTTONS the cursor moves through the
            // grid as it is drawn (moment_button_toward): up from the lower pair to the button above
            // it, up from the upper pair back to the list; down the other way, and no further than
            // the lower pair, so the buttons cannot be left downward by accident.
            case Nav::Up:
                if (g_mzone == MomentZone::Buttons) {
                    const int above = moment_button_toward(buttons, -1, 0);
                    if (above >= 0) {
                        g_mbutton = above;
                    }
                    else {
                        g_mzone = MomentZone::Rows;
                    }
                }
                else {
                    g_mfocus = order[static_cast<size_t>((at + count - 1) % count)];
                }
                g_moment_delete_armed = false;
                apply_moments_focus();
                break;
            case Nav::Down:
                if (g_mzone == MomentZone::Rows) {
                    g_mfocus = order[static_cast<size_t>((at + 1) % count)];
                }
                else {
                    const int below = moment_button_toward(buttons, 1, 0);
                    if (below >= 0) {
                        g_mbutton = below;
                    }
                }
                g_moment_delete_armed = false;
                apply_moments_focus();
                break;
            // Left and right cross to the other button of the same pair, and do nothing in the rows:
            // a row has no sideways.
            case Nav::Left:
            case Nav::Right:
                if (g_mzone == MomentZone::Buttons) {
                    const int beside = moment_button_toward(buttons, 0, (nav == Nav::Right) ? 1 : -1);
                    if (beside >= 0) {
                        g_mbutton = beside;
                        g_moment_delete_armed = false;
                        apply_moments_focus();
                    }
                }
                break;
            // The one press the user asked for: on a row it chooses and drops to the buttons, on
            // a button it acts.
            case Nav::Accept:
                if (g_mzone == MomentZone::Rows) {
                    moments_choose(g_mfocus);
                }
                else if (!buttons.empty()) {
                    moments_press(buttons[static_cast<size_t>(g_mbutton)]);
                }
                break;
            // Back climbs out one zone at a time, so a person on the buttons returns to the list
            // rather than losing the screen.
            case Nav::Back:
                if (g_mzone == MomentZone::Buttons) {
                    g_mzone = MomentZone::Rows;
                    g_moment_delete_armed = false;
                    apply_moments_focus();
                }
                else {
                    moments_leave();
                }
                break;
            case Nav::Clear:
                moments_delete();
                break;
            default:
                break;
        }
    }

    void reflect_warps(bool keep_focus = false) {
        if (g_warps_doc == nullptr) {
            return;
        }
        Rml::Element* rows = g_warps_doc->GetElementById("warp-rows");
        if (rows == nullptr) {
            return;
        }

        warps::refresh_here();

        std::string rml;

        // THE TIME OF DAY FIRST, at the top level only. Its own step ids rather than the
        // settings list's `prev-`/`next-`, because those are read as indices into the settings
        // table and this row is not in it: sharing them would send a click here into
        // nudge_setting with a row number that means something else entirely.
        if (warps_has_time_row()) {
            rml += "<div class=\"row\" id=\"wrow-0\">";
            rml += "<span class=\"label\">Time of day</span>";
            rml += "<span class=\"value\">";
            rml += "<span class=\"step\" id=\"wprev-0\">&#8249;</span>";
            rml += "<span class=\"shown\">" + escape(TIMES[g_time_index].label) + "</span>";
            rml += "<span class=\"step\" id=\"wnext-0\">&#8250;</span>";
            rml += "</span></div>";
        }

        for (int i = 0; i < warps::row_count(); ++i) {
            const std::string n = std::to_string(i + warps_time_rows());
            // The row the game is standing in is marked, because "which of these am I in" is
            // otherwise guesswork the moment you have warped once.
            const char* here = warps::row_is_here(i) ? " here" : "";
            rml += "<div class=\"row" + std::string(here) + "\" id=\"wrow-" + n + "\">";
            rml += "<span class=\"label\">" + escape(warps::row_label(i)) + "</span>";
            rml += "<span class=\"value\">" + escape(warps::row_value(i)) + "</span>";
            rml += "</div>";
        }
        replace_rows(g_warps_doc, rows, rml,
                     [&]() { cap_long_rows(g_warps_doc, "warp-rows", rows_cap(Screen::Warps)); });

        set_text(g_warps_doc, "warp-trail", warps::trail());

        // The foot says what the level is, which is the thing a breadcrumb alone does not: at the
        // top it is regions, inside a region it is places, inside a place it is the doors.
        const char* what = "";
        switch (warps::level()) {
            case warps::Level::Regions:   what = "Where in the world"; break;
            case warps::Level::Places:    what = "Places here"; break;
            case warps::Level::Entrances: what = "Ways in"; break;
        }
        set_text(g_warps_doc, "warp-note", what);

        if (!keep_focus) {
            g_wfocus = warps::focus() + warps_time_rows();
        }
        apply_warps_focus();
    }

    // Enter, or a click. A warp closes the menu, because the point of asking for a place is to
    // see it rather than to be left looking at a list.
    void take_warp_row(int row) {
        const int index = warps_index_for_row(row);
        if (index < 0) {
            // The time row. Enter re-applies whatever it is showing, which is the way to put the
            // clock back after the game has moved it on its own; that is the whole point of
            // having it here rather than in the settings.
            apply_time();
            return;
        }
        if (index >= warps::row_count()) {
            return;
        }
        warps::set_focus(index);
        switch (warps::activate(index)) {
            case warps::Action::Descended:
                reflect_warps();
                break;
            case warps::Action::Warped:
                show(Screen::None);
                break;
            case warps::Action::Nothing:
                break;
        }
    }

    void click_warp_row(int row) {
        g_wfocus = row;
        apply_warps_focus();
        take_warp_row(row);
    }

    // A chevron either side of the time row. Its own handler rather than click_step's, for the
    // reason given where the markup is written.
    void click_warp_step(const std::string& id) {
        if (!warps_has_time_row()) {
            return;
        }
        g_time_index += (id.compare(0, 6, "wprev-") == 0) ? -1 : 1;
        if (g_time_index < 0) {
            g_time_index = TIME_COUNT - 1;
        }
        if (g_time_index >= TIME_COUNT) {
            g_time_index = 0;
        }
        g_wfocus = 0;
        apply_time();
        reflect_warps(true);
    }

    // THE GAME'S OWN CLOCK, READ EVERY FRAME WHILE THE DEBUG MENU IS OPEN. The control above
    // sets the time; this says what the time actually IS, and those are different questions the
    // moment something is moving the clock without being asked. One of the three symptoms in the
    // Kakariko report is exactly that, and it is the only one of the three that can be watched as
    // a number rather than inferred from the color of the sky.
    void poll_warps_clock() {
        if (g_warps_doc == nullptr || g_screen != Screen::Warps) {
            return;
        }
        const oot::game_state::Snapshot now = oot::game_state::read();
        set_text(g_warps_doc, "warp-foot",
                 now.valid ? clock_text(now.day_time, now.night_flag) : std::string("no game running"));
    }

    void warps_nav(Nav nav) {
        const int count = warps_row_total();
        if (count <= 0) {
            if (nav == Nav::Back) {
                show(Screen::Debug);   // Warps is a page of Debugging (phase 96)
            }
            return;
        }
        if (g_wfocus < 0 || g_wfocus >= count) {
            g_wfocus = 0;
        }
        switch (nav) {
            case Nav::Default: case Nav::SectionPrev: case Nav::SectionNext:
                return;   // the shell's (phase 96); these older screens have none
            // The remembered focus is the WARP list's, so the time row does not write to it: a
            // -1 stored there would be clamped back to the first region and lose the place the
            // person was at before they reached up to change the clock.
            case Nav::Up:
                g_wfocus = (g_wfocus + count - 1) % count;
                if (warps_index_for_row(g_wfocus) >= 0) {
                    warps::set_focus(warps_index_for_row(g_wfocus));
                }
                apply_warps_focus();
                break;
            case Nav::Down:
                g_wfocus = (g_wfocus + 1) % count;
                if (warps_index_for_row(g_wfocus) >= 0) {
                    warps::set_focus(warps_index_for_row(g_wfocus));
                }
                apply_warps_focus();
                break;
            // ON THE TIME ROW, LEFT AND RIGHT STEP IT, which is what left and right do on every
            // other row of this interface that holds a value. Everywhere else in this list Right
            // goes in and Left comes back out, the same pair as Enter and Escape.
            case Nav::Right:
                if (warps_index_for_row(g_wfocus) < 0) {
                    click_warp_step("wnext-0");
                    break;
                }
                take_warp_row(g_wfocus);
                break;
            case Nav::Accept:
                take_warp_row(g_wfocus);
                break;
            case Nav::Left:
                if (warps_index_for_row(g_wfocus) < 0) {
                    click_warp_step("wprev-0");
                    break;
                }
                [[fallthrough]];
            case Nav::Back:
                // Back climbs one level, and only closes the menu once there is nowhere left to
                // climb, so Escape from three levels down walks out the way it walked in rather
                // than dropping the whole menu on the first press.
                if (warps::ascend()) {
                    reflect_warps();
                }
                else {
                    show(Screen::Debug);   // Warps is a page of Debugging (phase 96)
                }
                break;
            case Nav::Clear:
                break;
        }
    }

    // ------------------------------------------------------------------------------------------
    // The frame recorder's plate (the user's ask of 2026-09-22)
    //
    // The frame rate counter's twin: same plate, opposite corner, shown and hidden by its own
    // settings row. The one difference is that this one is a button, so it also carries the state
    // of a recording and what a click will do next.
    // ------------------------------------------------------------------------------------------

    void click_recorder() {
        // While a video is going the plate is the video's (poll_recorder), and a click on it does
        // nothing: the video has its own two controls, and a frame recording started under it
        // would cost both.
        if (oot::video::status().state != oot::video::State::Idle) {
            return;
        }
        // The length is the settings row's, read at the moment of the click rather than held, so
        // changing the row between recordings takes effect without anything having to notice.
        oot::recorder::toggle(oot::recorder::length_seconds(oot::ui::settings().record_length));
    }

    std::string minutes_and_seconds(int seconds) {
        const int m = seconds / 60;
        const int s = seconds % 60;
        return std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
    }

    // ------------------------------------------------------------------------------------------
    // The update plate (the user's ask of 2026-09-23): a plate at the bottom left while there is
    // news from the updater, which is the newer version being fetched in the background, the
    // fetched file verified and waiting for the next start, or a download that failed
    // verification. It says nothing otherwise, and it is a button while the file is ready.
    // ------------------------------------------------------------------------------------------

    void click_update() {
        if (oot::updates::state() == oot::updates::State::Ready && oot::updates::install()) {
            launch::request_quit();
        }
    }

    void poll_update() {
        // The updater's clock: the first check when the row is switched on mid session, and the
        // re-check every four hours. A clock read per frame, nothing more.
        oot::updates::tick();
        std::string value;
        std::string note;
        const bool on = g_game_started && g_update_doc != nullptr && oot::updates::plate(value, note);
        if (on != g_update_shown) {
            g_update_shown = on;
            if (on) {
                g_update_doc->Show();
            }
            else {
                g_update_doc->Hide();
            }
        }
        if (!on) {
            return;
        }
        static std::string shown_value;
        static std::string shown_note;
        if (value != shown_value) {
            shown_value = value;
            set_text(g_update_doc, "update-value", value);
        }
        if (note != shown_note) {
            shown_note = note;
            set_text(g_update_doc, "update-note", note);
        }
    }

    // THE VIDEO ON THE SAME PLATE (2026-10-09, main/video.h): while a video is recording, paused or
    // saving, the plate shows it whether or not the frame recorder's row is on, so a person always
    // knows the camera is running. The plate is the program's own interface, which is never in the
    // video.
    void show_video_on_plate(const oot::video::Status& v) {
        Rml::Element* plate = g_recorder_doc->GetElementById("recorder");
        if (plate != nullptr) {
            plate->SetClass("on", v.state == oot::video::State::Recording);
            plate->SetClass("saving", v.state == oot::video::State::Saving || v.state == oot::video::State::Paused);
            plate->SetClass("failed", false);
        }
        const std::string time = minutes_and_seconds(static_cast<int>(v.seconds));
        switch (v.state) {
            case oot::video::State::Starting:
                set_text(g_recorder_doc, "record-text", "Video");
                set_text(g_recorder_doc, "record-note", "starting");
                break;
            case oot::video::State::Recording:
                set_text(g_recorder_doc, "record-text", time);
                set_text(g_recorder_doc, "record-note", "video");
                break;
            case oot::video::State::Paused:
                set_text(g_recorder_doc, "record-text", "Paused");
                set_text(g_recorder_doc, "record-note", time);
                break;
            case oot::video::State::Saving:
            case oot::video::State::Idle:
                set_text(g_recorder_doc, "record-text", "Saving");
                set_text(g_recorder_doc, "record-note", "video");
                break;
        }
    }

    void poll_recorder() {
        const oot::video::Status video = oot::video::status();
        const bool video_on = video.state != oot::video::State::Idle;
        const bool on = g_game_started && g_recorder_doc != nullptr && (oot::ui::settings().recorder == 1 || video_on);
        if (on != g_recorder_shown) {
            g_recorder_shown = on;
            if (on) {
                g_recorder_doc->Show();
                if (Rml::Element* plate = g_recorder_doc->GetElementById("recorder")) {
                    plate->SetClass("under-bar", g_header_shown);
                }
            }
            else {
                g_recorder_doc->Hide();
            }
        }
        if (!on) {
            return;
        }
        if (video_on) {
            show_video_on_plate(video);
            return;
        }

        const oot::recorder::Status st = oot::recorder::status();

        Rml::Element* plate = g_recorder_doc->GetElementById("recorder");
        if (plate != nullptr) {
            plate->SetClass("on", st.state == oot::recorder::State::Recording);
            plate->SetClass("saving", st.state == oot::recorder::State::Draining);
            plate->SetClass("failed", st.failed);
        }

        switch (st.state) {
            case oot::recorder::State::Recording: {
                // The clock first, because while it runs that is the only number anyone reads.
                const int left = st.seconds_total - static_cast<int>(st.seconds_elapsed);
                set_text(g_recorder_doc, "record-text", minutes_and_seconds(left > 0 ? left : 0));
                // The frame count, and the stalls beside it when there are any: a recording that
                // had to hold the picture up is one whose timings are partly the recorder's own
                // fault, and saying so on the plate is more honest than only writing it into a
                // file nobody opens until afterward.
                std::string note = std::to_string(st.frames_taken) + " frames";
                if (st.stalls > 0) {
                    note += ", " + std::to_string(st.stalls) + " held";
                }
                set_text(g_recorder_doc, "record-note", note);
                break;
            }
            case oot::recorder::State::Draining:
                set_text(g_recorder_doc, "record-text", "Saving");
                set_text(g_recorder_doc, "record-note",
                         std::to_string(st.frames_written) + " of " + std::to_string(st.frames_taken));
                break;
            case oot::recorder::State::Idle:
                set_text(g_recorder_doc, "record-text", st.failed ? "Failed" : "Record");
                // At rest the plate says how long a click will record for, so the length is
                // visible without opening the settings to check it.
                set_text(g_recorder_doc, "record-note",
                         st.failed ? "see the log"
                                   : minutes_and_seconds(oot::recorder::length_seconds(
                                         oot::ui::settings().record_length)));
                break;
        }
    }

    // ------------------------------------------------------------------------------------------
    // Screens
    // ------------------------------------------------------------------------------------------

    // The version in every fixed foot (the user's ask at the 0.1.0 hand over): read from the
    // build, never typed into a document, so a bump in CMake reaches every surface at once.
    void stamp_versions() {
        const std::string v = std::string("v") + oot::build_info::version + " \xC2\xB7 ";
        set_text(g_settings_doc, "settings-foot", v + "F1 closes this, F2 is about, F3 is controls");
        set_text(g_controls_doc, "controls-foot", v + "Saved as you change it. F3 closes this");
        // The launcher's foot is one row: hints on the left, and the version joined to the
        // signature on the right, which is the one thing there that can be clicked.
        set_text(g_launch_doc, "launch-foot", "F2 is about, F3 is controls");
        set_text(g_launch_doc, "launch-signed", v + g_signature.status);
        set_text(g_browse_doc, "browse-foot", v + "Enter opens a folder or takes a ROM, B goes up, or drop the file here");
    }

    const char* screen_name(Screen screen) {
        switch (screen) {
            case Screen::Settings: return "settings";
            case Screen::About:    return "about";
            case Screen::Controls: return "controls";
            case Screen::Warps:    return "debug";
            case Screen::Launch:   return "launch";
            case Screen::Browse:   return "browse";
            case Screen::Files:    return "files";
            case Screen::Moments:  return "moments";
            case Screen::Lighting: return "lighting";
            case Screen::Ask:      return "ask";
            case Screen::Debug:    return "debug";
            case Screen::Hud:      return "hud";
            case Screen::Graphics: return "graphics";
            case Screen::Gameplay: return "gameplay";
            case Screen::Capture:  return "capture";
            case Screen::Photo:    return "photo";
            case Screen::Video:    return "video";
            default:               return "none";
        }
    }

    // The launcher's defaults for a fresh install, chosen once when there is no settings file:
    // the console's own picture, fullscreen (2026-10-08), for the person to raise.
    void choose_machine_defaults() {
        if (g_defaults_done || oot::ui::settings_file_present()) {
            return;
        }
        g_defaults_done = true;
        const oot::ui::Settings d = oot::ui::machine_defaults();
        oot::ui::apply(d);
        g_launch_note = "Starting as the console played it (" + oot::ui::render::device_name() + "). Raise any of them";
        std::fprintf(stderr, "[ui] machine defaults chosen: antialiasing %d for %s\n", d.antialiasing,
                     oot::ui::render::device_name().c_str());
    }

    // ------------------------------------------------------------------------------------------
    // THE ONE MENU (the menus plan, phase 96)
    //
    // shell.rml draws what is around the page: the head with the logo, the dial of sections, the
    // current one's name, the prompts and the held badge. The router (src/ui/shell/shell.h)
    // decides which section is up and what Back does; every screen of the old interface is a
    // section, a page inside one, or a layer over one, and its document is laid into the box the
    // shell leaves for the page (place_page) until the section moves into the shell itself.
    // ------------------------------------------------------------------------------------------
    namespace menu = oot::ui::menu;
    namespace shell_model = oot::ui::shell_model;
    using Device = oot::ui::input::Device;

    Rml::ElementDocument* g_shell_doc = nullptr;
    menu::Router g_router{ menu::Mode::Launcher };
    // The screen whose page is in the box: the screen itself, or, while a dialog is up, the page
    // the dialog was raised over.
    Screen g_page = Screen::None;
    // The page's box as last applied, so a page document is moved only when the box moves.
    Rml::Vector2f g_host_at{ -1.0f, -1.0f };
    Rml::Vector2f g_host_size{ -1.0f, -1.0f };
    Rml::ElementDocument* g_host_doc = nullptr;
    Device g_prompt_device = Device::Keyboard;

    struct SectionInfo {
        menu::Section section;
        const char* id;
        const char* name;
        const char* glyph;   // an icon under assets/icons/
        Screen screen;       // the section's own page
    };
    constexpr SectionInfo SECTIONS[] = {
        { menu::Section::Play,      "play",      "Play",      "play",      Screen::Launch },
        { menu::Section::Settings,  "settings",  "Settings",  "settings",  Screen::Settings },
        { menu::Section::Graphics,  "graphics",  "Graphics",  "monitor",   Screen::Graphics },
        { menu::Section::Gameplay,  "gameplay",  "Gameplay",  "sliders-horizontal", Screen::Gameplay },
        { menu::Section::Hud,       "hud",       "HUD",       "layers",    Screen::Hud },
        { menu::Section::Controls,  "controls",  "Controls",  "gamepad-2", Screen::Controls },
        { menu::Section::Capture,   "capture",   "Capture",   "video",     Screen::Capture },
        { menu::Section::Moments,   "moments",   "Moments",   "aperture",  Screen::Moments },
        { menu::Section::Debugging, "debugging", "Debugging", "bug",       Screen::Debug },
        { menu::Section::About,     "about",     "About",     "info",      Screen::About },
    };

    const SectionInfo& info_of(menu::Section section) {
        for (const SectionInfo& s : SECTIONS) {
            if (s.section == section) {
                return s;
            }
        }
        return SECTIONS[1];
    }

    Screen section_root_screen() {
        return info_of(g_router.section()).screen;
    }

    // The section a screen belongs to, and the page it is inside it. Browse is the Play section's
    // ROM browser and Warps the Debugging section's; Your files belongs to whichever section it was
    // opened from. False for the screens that are not pages (None, a dialog).
    bool section_of(Screen screen, menu::Section& section, menu::Page& page) {
        page = menu::Page::Root;
        switch (screen) {
            case Screen::Browse:
                section = menu::Section::Play;
                page = menu::Page::Browse;
                return true;
            case Screen::Warps:
                section = menu::Section::Debugging;
                page = menu::Page::Warps;
                return true;
            case Screen::Lighting:
                section = menu::Section::Graphics;
                page = menu::Page::Lighting;
                return true;
            case Screen::Photo:
                section = menu::Section::Capture;
                page = menu::Page::Photo;
                return true;
            case Screen::Video:
                section = menu::Section::Capture;
                page = menu::Page::Video;
                return true;
            case Screen::Files:
                section = g_router.open() ? g_router.section() : menu::Section::Settings;
                page = menu::Page::Files;
                return true;
            case Screen::None:
            case Screen::Ask:
                return false;
            default:
                for (const SectionInfo& s : SECTIONS) {
                    if (s.screen == screen) {
                        section = s.section;
                        return true;
                    }
                }
                return false;
        }
    }

    // The document that is a screen's page.
    Rml::ElementDocument* page_doc(Screen screen) {
        switch (screen) {
            case Screen::Launch:   return g_launch_doc;
            case Screen::Browse:   return g_browse_doc;
            case Screen::Files:    return g_files_doc;
            case Screen::Warps:    return g_warps_doc;
            case Screen::Settings:
            case Screen::Graphics:
            case Screen::Gameplay:
            case Screen::Capture:
            case Screen::Photo:
            case Screen::Video:    return g_settings_doc;
            case Screen::Lighting: return g_lighting_doc;
            case Screen::Hud:      return g_hud_doc;
            case Screen::Debug:    return g_debug_doc;
            case Screen::Controls: return g_controls_doc;
            case Screen::Moments:  return g_moments_doc;
            case Screen::About:    return g_about_doc;
            default:               return nullptr;
        }
    }

    // THE PROMPTS FOLLOW THE DEVICE IN USE (the user, 2026-10-12 21:37: "must detect in real time
    // so if I switch mid menu navigation, it switched the UI to show the proper controls"): the
    // device of the last input, read every frame, and nothing for the mouse, which needs none.
    // Face buttons carry their controllers' pastels (the user, 23:35); the shoulders and keys are
    // words on plates.
    // Whether the focused element is a value that Left and Right change: a stepped setting, a
    // lighting row, the time of day or a room's piece, on the pages built from the rows (phase 109).
    bool focused_is_value() {
        if (!settings_like(g_screen) && g_screen != Screen::Lighting && g_screen != Screen::Hud &&
            g_screen != Screen::Debug) {
            return false;
        }
        const std::vector<RowSpec>& specs = rows_for(g_screen);
        if (g_focus < 0 || g_focus >= static_cast<int>(specs.size())) {
            return false;
        }
        const RowSpec& spec = specs[static_cast<size_t>(g_focus)];
        return spec.kind == RowKind::Lighting || spec.kind == RowKind::TimeOfDay || spec.kind == RowKind::Piece ||
               (spec.kind == RowKind::Setting && !is_check_setting(spec.setting));
    }

    // A key's name as SDL gives it, for a prompt's plate.
    std::string key_name_of(int code) {
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
        return std::string(name != nullptr ? name : "");
    }

    const oot::input::InputBindings& pad_bindings_in_use(const oot::input::Bindings& all);
    int pad_slot_in_use();

    // A NOTE NAMES THE BINDING IN FORCE (the user, 2026-10-13, a standing rule: a description "goes
    // along with" the behavior, and it names what a control is bound to now, never a fixed default).
    // Each "{bind:<input>}" in a note (the input's key in input_bindings, "photo_mode") becomes that
    // control's working keys and, with a pad connected, its working pad buttons, read from the live
    // bindings as the rows are drawn: "F10, or Left stick press on the pad". Nothing bound says so.
    std::string with_bindings(const std::string& note) {
        namespace pg = oot::ui::prompts::photo_guide;
        namespace dev = oot::input::devices;
        std::string out;
        size_t at = 0;
        const std::shared_ptr<const oot::input::Bindings> all = oot::input::live();
        while (true) {
            const size_t open = note.find("{bind:", at);
            if (open == std::string::npos) {
                out += note.substr(at);
                return out;
            }
            const size_t close = note.find('}', open);
            if (close == std::string::npos) {
                out += note.substr(at);
                return out;
            }
            out += note.substr(at, open - at);
            at = close + 1;
            oot::input::GameInput input;
            if (!oot::input::input_from_key(note.substr(open + 6, close - open - 6), input)) {
                continue;
            }
            const auto words = [&](const oot::input::InputBindings& bound, int pad_slot) {
                const pg::Lookup lookup = [&bound](oot::input::GameInput i) { return bound[static_cast<size_t>(i)]; };
                std::string joined;
                for (const oot::input::Field& f : pg::working(lookup, input)) {
                    joined += (joined.empty() ? "" : " or ") + dev::field_name(f, pad_slot);
                }
                return joined;
            };
            std::string said = words(all->keyboard, -1);
            if (const int slot = pad_slot_in_use(); slot >= 0) {
                const std::string pad = words(pad_bindings_in_use(*all), slot);
                if (!pad.empty()) {
                    said += (said.empty() ? "" : ", or ") + pad + " on the pad";
                }
            }
            out += said.empty() ? std::string("unbound; bind it in Controls") : said;
        }
    }

    // THE PROMPTS FOLLOW THE DEVICE IN USE (phase 109, src/ui/prompts/): the family of the last
    // input's device, read every frame, and what the focused element and its place allow.
    std::vector<shell_model::Prompt> prompts_for(Device device) {
        namespace pr = oot::ui::prompts;
        pr::Here here;
        here.back = g_router.mode() == menu::Mode::Game || g_router.depth() > 0 || g_screen == Screen::Ask;
        here.change = focused_is_value();
        here.close = g_router.mode() == menu::Mode::Game && g_router.open();
        here.typing = g_kb_open_flag();
        here.type_path = g_screen == Screen::Browse && !here.typing;
        std::vector<shell_model::Prompt> out;
        const pr::Family family = pr::family_of(device);
        for (pr::Prompt p : pr::footer(family, here)) {
            // CLOSE IS THE MENU BINDING IN FORCE (the user, 2026-10-13: "mine says options but i have
            // it set to my touch pad"). The menu opens and closes on the Menu binding (the key path
            // and handle_pad_edges in boot.cpp), so its prompt is drawn from what the device in use
            // has bound now, read every frame, so a rebind shows at once. A pad with nothing bound
            // has no button to name, so no prompt; on the keys F1 always opens the menu too.
            // Both, when both are bound (the user: "show both of them but only in the event that both
            // of them actually work"): Menu is acted on first, so every Menu binding works here.
            if (p.action == "close") {
                const std::shared_ptr<const oot::input::Bindings> all = oot::input::live();
                const bool keys = family == pr::Family::Keyboard;
                const oot::input::InputBindings& bound = keys ? all->keyboard : pad_bindings_in_use(*all);
                const pr::photo_guide::Lookup lookup = [&bound](oot::input::GameInput input) {
                    return bound[static_cast<size_t>(input)];
                };
                const std::vector<pr::Glyph> glyphs =
                    pr::photo_guide::working_glyphs(family, lookup, oot::input::GameInput::Menu, key_name_of);
                if (glyphs.empty() && !keys) {
                    continue;
                }
                for (size_t i = 0; i + 1 < glyphs.size(); ++i) {
                    out.push_back({ glyphs[i].kind, glyphs[i].key, glyphs[i].shape, glyphs[i].tint, "", "close-also" });
                }
                if (!glyphs.empty()) {
                    p.glyph = glyphs.back();
                }
            }
            out.push_back({ p.glyph.kind, p.glyph.key, p.glyph.shape, p.glyph.tint, p.label, p.action });
        }
        return out;
    }

    // The router's answer, written into the shell's model, and the shell shown or hidden with it.
    // MOTION (the menus plan, phase 114): nothing in the menus jumps. A page eases in when it
    // changes, the menu eases in as it opens over the game and out as it closes, each over the
    // motion tokens' durations with the easing token's curve (quartic in and out). A document
    // fading out stays shown until its fade ends, then is hidden and put back to full opacity.
    //
    // A FADING DOCUMENT CARRIES is-fading, because RmlUi's opacity is inherited rather than
    // multiplied: an element with an opacity of its own (the medallion's glint, animated to 0.85)
    // ignores its document's fade and stood out bright over a panel that was going. The class
    // hides it for the fade's length (medallion.rcss).
    struct Fading {
        Rml::ElementDocument* doc;
        std::chrono::steady_clock::time_point until;
        bool out;
    };
    std::vector<Fading> g_fading;

    const Rml::Tween EASE{ Rml::Tween::Quartic, Rml::Tween::InOut };

    void stop_fading(Rml::ElementDocument* doc) {
        g_fading.erase(std::remove_if(g_fading.begin(), g_fading.end(), [doc](const Fading& f) { return f.doc == doc; }),
                       g_fading.end());
    }

    void fade_in(Rml::ElementDocument* doc, float seconds) {
        if (doc == nullptr) {
            return;
        }
        stop_fading(doc);
        const Rml::Property from(0.0f, Rml::Unit::NUMBER);
        doc->Animate("opacity", Rml::Property(1.0f, Rml::Unit::NUMBER), seconds, EASE, 1, false, 0.0f, &from);
        doc->SetClass("is-fading", true);
        g_fading.push_back({ doc, std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(static_cast<int>(seconds * 1000.0f)), false });
    }

    void fade_out(Rml::ElementDocument* doc, float seconds) {
        if (doc == nullptr || !doc->IsVisible()) {
            return;
        }
        stop_fading(doc);
        doc->Animate("opacity", Rml::Property(0.0f, Rml::Unit::NUMBER), seconds, EASE, 1, false);
        doc->SetClass("is-fading", true);
        g_fading.push_back({ doc, std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(static_cast<int>(seconds * 1000.0f)), true });
    }

    // PHOTO MODE'S BUTTON GUIDE (the user, 2026-10-13; ui/prompts/photo_guide.h): photo mode's
    // controls on the device in use, as that device is bound. The pad numbers it names without SDL
    // must be SDL's.
    static_assert(static_cast<int>(oot::ui::prompts::photo_guide::pad::A) == static_cast<int>(SDL_CONTROLLER_BUTTON_A) &&
                  static_cast<int>(oot::ui::prompts::photo_guide::pad::RightShoulder) == static_cast<int>(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
                  static_cast<int>(oot::ui::prompts::photo_guide::pad::DUp) == static_cast<int>(SDL_CONTROLLER_BUTTON_DPAD_UP) &&
                  static_cast<int>(oot::ui::prompts::photo_guide::pad::Touchpad) == static_cast<int>(SDL_CONTROLLER_BUTTON_TOUCHPAD) &&
                  static_cast<int>(oot::ui::prompts::photo_guide::pad::RightY) == static_cast<int>(SDL_CONTROLLER_AXIS_RIGHTY) &&
                  static_cast<int>(oot::ui::prompts::photo_guide::pad::TriggerRight) == static_cast<int>(SDL_CONTROLLER_AXIS_TRIGGERRIGHT),
                  "photo_guide.h's pad numbers are SDL's standard layout");

    // The bindings of the pad in use: its profile, found by the pad that pressed something last
    // (or the first pad), or the pad defaults when it has none yet.
    // The pad last used, or the first connected; -1 with none.
    int pad_slot_in_use() {
        namespace dev = oot::input::devices;
        const int instance = oot::ui::input::last_pad_instance();
        int slot_in_use = -1;
        for (int slot = 0; slot < dev::pad_count(); ++slot) {
            dev::UiPad pad;
            if (dev::ui_pad(slot, pad) && (slot_in_use < 0 || pad.instance == instance)) {
                slot_in_use = slot;
                if (pad.instance == instance) {
                    break;
                }
            }
        }
        return slot_in_use;
    }

    const oot::input::InputBindings& pad_bindings_in_use(const oot::input::Bindings& all) {
        static const oot::input::InputBindings defaults = [] {
            oot::input::InputBindings b{};
            oot::input::controller_defaults(b);
            return b;
        }();
        namespace dev = oot::input::devices;
        const int slot_in_use = pad_slot_in_use();
        dev::PadInfo info;
        if (slot_in_use >= 0 && dev::pad_info(slot_in_use, info)) {
            for (const oot::input::Profile& p : all.profiles) {
                if (p.guid == info.guid) {
                    return p.bindings;
                }
            }
        }
        return defaults;
    }

    std::vector<shell_model::Prompt> guide_prompts() {
        namespace pr = oot::ui::prompts;
        namespace pg = oot::ui::prompts::photo_guide;
        const pr::Family family = pr::family_of(g_prompt_device);
        const std::shared_ptr<const oot::input::Bindings> all = oot::input::live();
        const oot::input::InputBindings& bound = family == pr::Family::Keyboard ? all->keyboard : pad_bindings_in_use(*all);
        const pg::Lookup lookup = [&bound](oot::input::GameInput input) { return bound[static_cast<size_t>(input)]; };
        const pg::KeyName names = key_name_of;
        std::vector<shell_model::Prompt> out;
        for (const pr::Prompt& p : pg::rows(family, lookup, names, pg::Here{ oot::photo::hud_hidden(), oot::photo::hud_dimmed() })) {
            out.push_back({ p.glyph.kind, p.glyph.key, p.glyph.shape, p.glyph.tint, p.label, p.action });
        }
        return out;
    }

    // Up while photo mode holds the game, no menu is open and its binding has not hidden it; it
    // eases in and out like the menu does, and its prompts keep the footer's size (shell_model.h).
    void poll_guide() {
        if (g_guide_doc == nullptr) {
            return;
        }
        const bool want = g_game_started && oot::photo::active() && !oot::photo::guide_hidden() && g_screen == Screen::None;
        const bool going = std::any_of(g_fading.begin(), g_fading.end(),
                                       [](const Fading& f) { return f.doc == g_guide_doc && f.out; });
        if (want) {
            shell_model::set_guide(guide_prompts());
            shell_model::hold_prompt_size(g_guide_doc, g_context->GetDensityIndependentPixelRatio());
            if (!g_guide_doc->IsVisible() || going) {
                g_guide_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
                fade_in(g_guide_doc, oot::ui::tokens::duration_base);
            }
        }
        else if (g_guide_doc->IsVisible() && !going) {
            fade_out(g_guide_doc, oot::ui::tokens::duration_base);
        }
    }

    // Hides what has finished fading; once a frame.
    void poll_fading() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = g_fading.begin(); it != g_fading.end();) {
            if (now >= it->until) {
                it->doc->SetClass("is-fading", false);
                if (it->out) {
                    it->doc->Hide();
                    it->doc->SetProperty(Rml::PropertyId::Opacity, Rml::Property(1.0f, Rml::Unit::NUMBER));
                }
                it = g_fading.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void refresh_shell() {
        if (g_shell_doc == nullptr) {
            return;
        }
        shell_model::State state;
        state.mode = g_router.mode() == menu::Mode::Launcher ? "launcher" : "game";
        switch (menu::placement_for(g_router.mode(), oot::ui::settings().menu_side)) {
            case menu::Placement::Launcher:    state.placement = "launcher"; break;
            case menu::Placement::DockedLeft:  state.placement = "left"; break;
            case menu::Placement::DockedRight: state.placement = "right"; break;
            case menu::Placement::Centered:    state.placement = "center"; break;
        }
        state.title = info_of(g_router.section()).name;
        state.version = oot::build_info::version;
        state.held = g_router.mode() == menu::Mode::Game && g_router.open();
        const std::vector<menu::Section> order = g_router.sections();
        const int n = static_cast<int>(order.size());
        int current = 0;
        for (int i = 0; i < n; ++i) {
            if (order[static_cast<size_t>(i)] == g_router.section()) {
                current = i;
            }
        }
        for (int i = 0; i < n; ++i) {
            const SectionInfo& info = info_of(order[static_cast<size_t>(i)]);
            // Where this section stands from the current one, the shorter way round the dial.
            int d = ((i - current) % n + n) % n;
            if (d > n / 2) {
                d -= n;
            }
            state.sections.push_back({ info.id, info.name, info.glyph, (d >= -2 && d <= 2) ? d : 99, d == 0 });
        }
        state.prompts = prompts_for(g_prompt_device);
        shell_model::set(state);

        const bool up = g_screen != Screen::None;
        if (up && (!g_shell_doc->IsVisible() || !g_fading.empty())) {
            const bool opening = !g_shell_doc->IsVisible() || std::any_of(g_fading.begin(), g_fading.end(),
                                                                          [](const Fading& f) { return f.doc == g_shell_doc && f.out; });
            g_shell_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            if (opening) {
                fade_in(g_shell_doc, oot::ui::tokens::duration_base);
            }
        }
        else if (!up && g_shell_doc->IsVisible()) {
            fade_out(g_shell_doc, oot::ui::tokens::duration_base);
        }
        if (up) {
            g_shell_doc->PushToBack();
        }
    }

    // Lays the page's document into the shell's page box. Called after the context's update, when
    // the box has its size; true when it moved something, so the caller updates once more.
    bool place_page() {
        if (g_shell_doc == nullptr || !g_shell_doc->IsVisible()) {
            return false;
        }
        Rml::ElementDocument* doc = page_doc(g_page);
        Rml::Element* host = g_shell_doc->GetElementById("sh-host");
        if (doc == nullptr || host == nullptr) {
            return false;
        }
        // CENTERED, THE PAGE'S BOX ENDS WHERE THE FOOTER BEGINS (phase 116): the footer takes a
        // second line when the panel is too narrow for its prompts (phase 113), and a box ending at
        // the one line height then ran under it, the footer drawn over the page's last row.
        {
            static float s_bottom = -1.0f;
            Rml::Element* prompts = g_shell_doc->GetElementById("sh-prompts");
            const bool center = shell_model::get().placement == "center";
            const float wanted = (center && prompts != nullptr) ? prompts->GetBox().GetSize(Rml::BoxArea::Border).y : -1.0f;
            if (std::fabs(wanted - s_bottom) > 0.5f) {
                s_bottom = wanted;
                if (wanted >= 0.0f) {
                    host->SetProperty(Rml::PropertyId::Bottom, Rml::Property(wanted, Rml::Unit::PX));
                }
                else {
                    host->RemoveProperty(Rml::PropertyId::Bottom);
                }
                g_context->Update();
            }
        }
        const Rml::Vector2f at = host->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = host->GetBox().GetSize(Rml::BoxArea::Border);
        if (doc == g_host_doc && at == g_host_at && size == g_host_size) {
            return false;
        }
        doc->SetProperty(Rml::PropertyId::Position, Rml::Property(Rml::Style::Position::Absolute));
        doc->SetProperty(Rml::PropertyId::Left, Rml::Property(at.x, Rml::Unit::PX));
        doc->SetProperty(Rml::PropertyId::Top, Rml::Property(at.y, Rml::Unit::PX));
        doc->SetProperty(Rml::PropertyId::Width, Rml::Property(size.x, Rml::Unit::PX));
        doc->SetProperty(Rml::PropertyId::Height, Rml::Property(size.y, Rml::Unit::PX));
        g_host_doc = doc;
        g_host_at = at;
        g_host_size = size;
        // Where the page went, and where the prompts sit under it, once per move: the harness
        // reads this to tell a page laid out of its box from one that is merely drawn wrong.
        Rml::Element* prompts = g_shell_doc->GetElementById("sh-prompts");
        std::fprintf(stderr, "[ui] page %s in %.0f,%.0f %.0fx%.0f; prompts at %.0f, %.0f high\n", screen_name(g_page),
                     at.x, at.y, size.x, size.y,
                     prompts != nullptr ? prompts->GetAbsoluteOffset(Rml::BoxArea::Border).y : -1.0f,
                     prompts != nullptr ? prompts->GetBox().GetSize(Rml::BoxArea::Border).y : -1.0f);
        return true;
    }

    // The router follows whatever was opened, so every way of opening a screen (a row, a key, a
    // dialog's answer, a click on the dial) leaves the dial saying where you are.
    void sync_router(Screen screen) {
        if (screen == Screen::None) {
            g_router.close();
            return;
        }
        menu::Section section{};
        menu::Page page{};
        if (!section_of(screen, section, page)) {
            return;
        }
        if (section == menu::Section::Debugging) {
            g_router.unlock_debugging();
        }
        if (!g_router.open() || g_router.section() != section || (page == menu::Page::Root && g_router.depth() > 0)) {
            g_router.open_on(section);
        }
        if (page != menu::Page::Root && g_router.page() != page) {
            g_router.push(page);
        }
    }

    // Turns the dial and opens the section it lands on. A dialog holds the dial still.
    void turn_dial(int step) {
        if (g_screen == Screen::Ask || g_screen == Screen::None) {
            return;
        }
        if (step > 0) {
            g_router.next();
        }
        else {
            g_router.prev();
        }
        show(section_root_screen());
    }

    // A click on a medallion of the dial or on the rail.
    void click_section(const std::string& id) {
        for (const SectionInfo& s : SECTIONS) {
            if (id == std::string("sec-") + s.id && g_router.available(s.section) && g_screen != Screen::Ask) {
                show(s.screen);
                return;
            }
        }
    }

    // A click on a prompt does what its button does.
    void click_prompt(const std::string& id) {
        const std::string action = id.substr(7);
        if (action == "accept") {
            handle_nav(Nav::Accept);
        }
        else if (action == "back") {
            handle_nav(Nav::Back);
        }
        else if (action == "dial-prev" || action == "dial-next") {
            turn_dial(action == "dial-next" ? 1 : -1);
        }
        else if (action == "change") {
            handle_nav(Nav::Right);
        }
        else if (action == "close" || action == "close-also") {
            show(Screen::None);
        }
        else if (action == "type") {
            open_keyboard();
        }
    }

    void show(Screen screen) {
        // Until the game has been started, nothing open means the launcher.
        if (screen == Screen::None && !g_game_started) {
            screen = Screen::Launch;
        }
        const Screen previous = g_screen;
        if (screen == Screen::Ask && previous != Screen::Ask) {
            g_ask_yes = false;
        }
        // The menu opening over the game and closing again (phase 111).
        if (previous == Screen::None && screen != Screen::None) {
            oot::ui::sound::play(oot::ui::sound::Cue::Open);
        }
        else if (previous != Screen::None && screen == Screen::None) {
            oot::ui::sound::play(oot::ui::sound::Cue::Close);
        }

        if (previous == Screen::Controls && screen != Screen::Controls) {
            devices::cancel_capture();
            end_capture_panel();
        }
        if (previous == Screen::Browse && screen != Screen::Browse) {
            blur_text_entry();
            close_keyboard();
        }

        const auto set = [](Rml::ElementDocument* doc, bool on) {
            if (doc == nullptr) {
                return;
            }
            if (on) {
                doc->Show();
            }
            else {
                doc->Hide();
            }
        };
        // EVERY SCREEN IS A PAGE OF THE ONE MENU (phase 96), so one page is up at a time, in the
        // shell's box; a dialog is drawn over the page it was raised from, which stays up beneath.
        const Screen old_page = g_page;
        if (screen != Screen::Ask) {
            g_page = screen;
        }
        else if (page_doc(g_page) == nullptr) {
            g_page = g_game_started ? Screen::Settings : Screen::Launch;
        }
        const Screen page = g_page;
        set(g_settings_doc, settings_like(page));
        set(g_lighting_doc, page == Screen::Lighting);
        set(g_debug_doc, page == Screen::Debug);
        set(g_hud_doc, page == Screen::Hud);
        // A DEVICE THAT CANNOT TRACE SAYS SO ON THE SCREEN THAT OFFERS IT (2026-09-26, the user on
        // an Intel Iris Xe laptop: "Does ray tracing just not work at all on some devices?" and
        // then, looking at a panel reading DirectX 12 and Shader Model 6.7, "I feel like I should
        // actually be able to use it though!??"). It said so only in the log, so every row in this
        // menu could be moved and nothing on screen would ever change. Set here rather than in
        // stamp_versions because the capability is not known until the renderer exists, and the
        // launcher's foot is stamped long before that.
        if (screen == Screen::Lighting) {
            set_text(g_lighting_doc, "lighting-foot",
                     oot::rt_state::supported()
                         ? "Each part switches on its own; the tools for inspecting a single pass are under Debugging"
                         : "This graphics device cannot trace rays, so these rows change nothing and the game "
                           "draws its own lighting and shadows. It needs a card that supports DirectX Raytracing");
        }
        set(g_about_doc, page == Screen::About);
        set(g_controls_doc, page == Screen::Controls);
        set(g_launch_doc, page == Screen::Launch);
        set(g_browse_doc, page == Screen::Browse);
        set(g_files_doc, page == Screen::Files);
        set(g_ask_doc, screen == Screen::Ask);
        set(g_warps_doc, page == Screen::Warps);
        set(g_moments_doc, page == Screen::Moments);
        // The page eases in when it changes, and the page the menu closed on eases out with it.
        if (page != old_page || previous == Screen::None) {
            if (Rml::ElementDocument* doc = page_doc(page)) {
                fade_in(doc, oot::ui::tokens::duration_base);
            }
        }
        if (screen == Screen::None && previous != Screen::None) {
            if (Rml::ElementDocument* doc = page_doc(old_page)) {
                doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
                fade_out(doc, oot::ui::tokens::duration_base);
            }
        }

        g_screen = screen;
        if (g_capturing.exchange(screen != Screen::None, std::memory_order_relaxed) && screen == Screen::None) {
            g_capture_releases.fetch_add(1, std::memory_order_release);
        }
        sync_router(screen);
        // THE MENU HOLDS THE GAME while it is open in the game (phase 97), through photo mode's
        // freeze, the sound left playing.
        oot::shots::set_menu_hold(g_router.holds());
        refresh_shell();
        if (g_ask_doc != nullptr && screen == Screen::Ask) {
            g_ask_doc->PullToFront();
        }
        // The picture comes back to the whole window when no menu is up, and moves aside when a
        // side panel is. Every menu screen takes the placement now, so it is applied here once for
        // all of them (and again below for the ones that rebuild their rows).
        apply_menu_placement();
        if (previous != screen) {
            std::fprintf(stderr, "[ui] screen %s\n", screen_name(screen));
        }

        if (settings_like(screen)) {
            if (previous != screen) {
                g_focus = 0;
                // The system's microphones read again as the settings open, so one plugged in
                // since shows on the Microphone device row (main/microphone.h).
                oot::microphone::refresh();
            }
            apply_menu_placement();
            refresh_rows(screen);
            apply_rows_focus(screen);
        }
        else if (screen == Screen::Lighting) {
            if (previous != Screen::Lighting) {
                g_focus = 0;
            }
            apply_menu_placement();
            refresh_rows(screen);
            apply_rows_focus(screen);
        }
        else if (screen == Screen::Debug) {
            if (previous != Screen::Debug) {
                g_focus = 0;
            }
            apply_menu_placement();
            refresh_rows(screen);
            apply_rows_focus(screen);
        }
        else if (screen == Screen::Hud) {
            if (previous != Screen::Hud) {
                g_focus = 0;
            }
            apply_menu_placement();
            refresh_rows(screen);
            apply_rows_focus(screen);
        }
        else if (screen == Screen::Launch) {
            // THE FIRST RUN'S DEFAULTS COME BEFORE THE FIRST RUN'S QUESTIONS (fixed 2026-10-08).
            // They were chosen after them, and answering a question saves the settings file, so
            // by the time this ran the file existed and it stood down: since the update question
            // arrived on 2026-09-20, no fresh install got its defaults, only the bare rows. Found
            // by a first run check of the new defaults. Choosing them acts on nothing but the
            // rows, so it need not wait. (The console's own picture IS the default now, on the
            // user's word; what this decides is that the defaults are the ones written down in
            // machine_defaults, fullscreen included, rather than whatever the struct happens to
            // hold.)
            choose_machine_defaults();
            // THE FIRST RUN QUESTIONS, raised here because this is the moment the launcher is
            // about to be seen and every one of them has to be answered before anything acts on
            // its own. Asking from here rather than from startup also means the ROM question is
            // raised the first time the launcher is shown WITH a ROM accepted, which is usually
            // the run after the one that chose it.
            if (previous != Screen::Ask && ask_next_question()) {
                show(Screen::Ask);
                return;
            }
            // Back to the launcher's own length, in case a shorter document shrank the window.
            launch::request_window_rows(static_cast<int>(rows_for(Screen::Launch).size()) - 2);
            if (previous != Screen::Launch && previous != Screen::Browse && previous != Screen::Controls) {
                // PLAY IS WHAT THE LAUNCHER IS FOR, so it is what is selected when it opens (the
                // user's ask of 2026-09-23). Found by kind rather than by a row number, which
                // would go quietly wrong the next time a row is added above it.
                const std::vector<RowSpec>& specs = rows_for(Screen::Launch);
                g_focus = 0;
                for (size_t i = 0; i < specs.size(); ++i) {
                    if (specs[i].kind == RowKind::Play) {
                        g_focus = static_cast<int>(i);
                        break;
                    }
                }
            }
            refresh_rows(screen);
            apply_rows_focus(screen);
            g_rom_seen = rom_value() + "|" + rom_note();
        }
        else if (screen == Screen::Browse) {
            browse_load(g_browse_dir.empty() && previous != Screen::Browse ? launch::executable_directory() : g_browse_dir);
        }
        else if (screen == Screen::About) {
            reflect_about();
        }
        else if (screen == Screen::Ask) {
            reflect_ask();
            // Two short paragraphs and two buttons: shorter than any list, so the window follows.
            // ONLY FOR THE FIRST RUN QUESTIONS, which are the only thing on screen when they are
            // asked. A confirmation raised during play sits over the picture in a window that is
            // already the size the person chose, and shrinking it to five rows would crop the very
            // warning they are being asked to read.
            if (!g_confirm_open) {
                launch::request_window_rows(5);
            }
        }
        else if (screen == Screen::Files) {
            // Armed state never survives leaving the screen: coming back to it must not find a
            // destructive button already half pressed from last time.
            if (previous != Screen::Files) {
                g_remove_armed = false;
                g_ffocus = 0;
            }
            reflect_files();
            // This document shares the launcher's window and has a shorter list than it, so the
            // window follows the document rather than being sized once for the longest. The one
            // extra row is the mode line above the list.
            launch::request_window_rows(static_cast<int>(g_file_rows.size()) + 1);
        }
        else if (screen == Screen::Moments) {
            if (previous != Screen::Moments) {
                g_moment_delete_armed = false;
            }
            reflect_moments();
            launch::request_window_rows(oot::moments::SLOT_COUNT + 2);
        }
        else if (screen == Screen::Warps) {
            // ALWAYS BACK AT THE TOP. A menu that reopens three levels down, inside a branch the
            // person has forgotten they were in, reads as broken rather than as helpful; the
            // remembered focus is for climbing back out within one visit.
            if (previous != Screen::Warps) {
                warps::reset();
            }
            reflect_warps();
        }
        else if (screen == Screen::Controls) {
            controls::refresh();
            g_pads_seen = devices::pad_count();
            // On the way in only: the device in use is selected, and the focus starts at the top.
            // Shown again while already open (a refresh), the person's selection and place stay.
            if (previous != Screen::Controls) {
                controls::select_in_use(oot::ui::input::last_pad_instance());
                g_cfocus = 0;
                g_cslot = 0;
            }
            reflect_controls();
        }

        if (previous == Screen::Controls && screen == Screen::None) {
            // The shortcut may have been rebound in there. Say so the same way as at start.
            std::string lead;
            std::string note;
            controls::menu_shortcut(lead, note);
            if (lead + "|" + note != g_hint_last) {
                show_hint();
            }
        }
    }

    void start_splash() {
        g_splash_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        g_splash_doc->PullToFront();
        g_splash = SplashState::Showing;
        g_splash_since = Clock::now();
        g_splash_frames = 0;
        g_capturing.store(true, std::memory_order_relaxed);
        say("splash");
    }

    void poll_splash() {
        if (g_splash == SplashState::Off || g_splash_doc == nullptr) {
            return;
        }
        const double t = seconds_since(g_splash_since);
        if (g_splash == SplashState::Showing) {
            // THE GOLD POURS ONCE THE SLAB HAS BEEN DRAWN (the user, 2026-10-13: "the splash starts
            // already filled"). A transition needs a first value to start from, and this waited a
            // fixed 0.05 s for it; the first frame comes later than that while the window and the
            // renderer come up, so the class landed before the bar had ever been drawn at zero and
            // it appeared full. It now waits for three frames drawn WITH THE WINDOW ON SCREEN (the
            // window is held hidden until the first frame, launch.h; the slab's own textures reach the
            // card on the second), and the splash's time counts from the pour, so it is all seen.
            //
            // NO TIME LIMIT ON THAT WAIT (the user, 2026-10-13, after the first fix: "nope still gold
            // when it loaded. it starts as a black rectangle before anything shows"). The window is
            // shown before the renderer is up, and no frame is drawn while it starts; a fallback that
            // poured after 1.5 s counted that black stretch, fired on the first frame after it, and
            // the bar was full again. This only runs when a frame is drawn, so it cannot wait forever.
            if (!g_splash_poured && g_splash_frames >= 3) {
                g_splash_poured = true;
                std::fprintf(stderr, "[ui] splash on screen %.2f s after it was set up; the pour begins\n", t);
                g_splash_since = Clock::now();
                return;
            }
            if (!g_splash_poured) {
                return;
            }
            // THE POUR'S WIDTH, FRAME BY FRAME (the user, 2026-10-13: "still not starting grey and
            // filling"): a transition on a class never animated, so the program eases it itself.
            if (Rml::Element* pour = g_splash_doc->GetElementById("splash-pour")) {
                // As loading reads (the user, 2026-10-13): slowly to the pause, a hold, then the rest.
                namespace tk = oot::ui::tokens;
                const float at = static_cast<float>(t);
                float share = 1.0f;
                if (at < tk::splash_pour_first) {
                    share = tk::splash_pour_pause_at * quartic_in_out(at / tk::splash_pour_first);
                }
                else if (at < tk::splash_pour_first + tk::splash_pour_hold) {
                    share = tk::splash_pour_pause_at;
                }
                else {
                    const float rest = (at - tk::splash_pour_first - tk::splash_pour_hold) / tk::splash_pour_rest;
                    share = tk::splash_pour_pause_at + (1.0f - tk::splash_pour_pause_at) * quartic_in_out(rest);
                }
                // Along the name, not the slab: the share is of the name itself.
                const float width = tk::splash_name_from + share * (tk::splash_name_to - tk::splash_name_from);
                pour->SetProperty(Rml::PropertyId::Width, Rml::Property(width, Rml::Unit::DP));
            }
            if (t >= SPLASH_MIN_SECONDS || (g_splash_skip && t >= SPLASH_SKIP_AFTER_SECONDS)) {
                g_splash = SplashState::Fading;
                g_splash_since = Clock::now();
                g_splash_doc->SetClass("gone", true);
                show(Screen::None);   // the launcher, beneath the fading splash
                launch::request_maximize_launcher();
                g_splash_doc->PullToFront();
                say("splash gone");
            }
        }
        else if (t >= SPLASH_FADE_SECONDS) {
            g_splash_doc->Hide();
            g_splash = SplashState::Off;
        }
    }

    void drain() {
        std::deque<Command> local;
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            local.swap(g_queue);
        }

        for (const Command& c : local) {
            switch (c.kind) {
                case Command::Kind::Open:
                    show(c.screen);
                    break;

                case Command::Kind::Close:
                    show(Screen::None);
                    break;

                case Command::Kind::Toggle:
                    // F1, F2, F3 and the menu key OPEN THE MENU ON THEIR SECTION (phase 96): from
                    // a closed menu, or from another section, they go there; pressed on their own
                    // section they close the menu in the game and go back to Play before it. A
                    // question being asked holds.
                    if (g_screen == Screen::Ask) {
                        break;
                    }
                    if (g_screen == c.screen) {
                        show(Screen::None);
                    }
                    else {
                        show(c.screen);
                    }
                    break;

                case Command::Kind::Activity:
                    dismiss_hint();
                    break;

                case Command::Kind::MouseMove: {
                    note_pointer(c.x, c.y);
                    if (g_context != nullptr) {
                        g_context->ProcessMouseMove(c.x, c.y, 0);
                    }
                    break;
                }

                case Command::Kind::MouseButton:
                    if (g_splash == SplashState::Showing && c.down) {
                        g_splash_skip = true;
                        break;
                    }
                    if (g_context != nullptr) {
                        if (c.down) {
                            g_context->ProcessMouseButtonDown(c.x, 0);
                        }
                        else {
                            g_context->ProcessMouseButtonUp(c.x, 0);
                        }
                    }
                    break;

                case Command::Kind::MouseWheel:
                    if (g_context != nullptr) {
                        g_context->ProcessMouseWheel(c.delta, 0);
                    }
                    break;

                case Command::Kind::Nav:
                    handle_nav(c.nav);
                    break;

                case Command::Kind::Text:
                    g_context->ProcessTextInput(c.text);
                    break;

                case Command::Kind::Key: {
                    const Rml::Input::KeyIdentifier key = rml_key(c.x);
                    if (key != Rml::Input::KI_UNKNOWN) {
                        const int mods = (c.down ? Rml::Input::KM_CTRL : 0) | (c.shift ? Rml::Input::KM_SHIFT : 0);
                        g_context->ProcessKeyDown(key, mods);
                        g_context->ProcessKeyUp(key, mods);
                    }
                    break;
                }

                case Command::Kind::TextAccept:
                    use_typed_path();
                    break;

                case Command::Kind::TextEscape:
                    blur_text_entry();
                    break;

                case Command::Kind::BrowsePath:
                    if (Rml::ElementFormControl* input = browse_input()) {
                        input->SetValue(c.text);
                    }
                    break;
            }
        }
    }

    // A navigation on whatever is open: from the keys and the pad through the queue, and from
    // a modal's own close button (Back).
    void handle_nav_now(Nav nav);

    // What a move does to bring a page's contents back, without moving: the rows rebuilt and the
    // focus applied and followed into view; any other page shown again as it is.
    void refresh_after_resize() {
        switch (g_screen) {
            case Screen::Settings:
            case Screen::Graphics:
            case Screen::Gameplay:
            case Screen::Capture:
            case Screen::Photo:
            case Screen::Video:
            case Screen::Launch:
            case Screen::Lighting:
            case Screen::Hud:
            case Screen::Debug:
                refresh_rows(g_screen);
                apply_rows_focus(g_screen);
                break;
            default:
                show(g_screen);
                break;
        }
        std::fprintf(stderr, "[ui] refreshed %s after a resize\n", screen_name(g_screen));
    }

    // THE MENU'S SOUNDS (phase 111): each press makes its sound after it has acted, so a change
    // to the sound rows themselves is heard at the new volume, and turning them On is heard.
    void handle_nav(Nav nav) {
        namespace snd = oot::ui::sound;
        const bool splash = g_splash == SplashState::Showing;
        const Screen before = g_screen;
        const bool value = focused_is_value();
        handle_nav_now(nav);
        // A press that closed the menu has made the closing sound already (show).
        if (splash || before == Screen::None || g_screen == Screen::None) {
            return;
        }
        switch (nav) {
            case Nav::Up:
            case Nav::Down:
            case Nav::SectionPrev:
            case Nav::SectionNext:
                snd::play(snd::Cue::Move);
                break;
            case Nav::Left:
            case Nav::Right:
                snd::play(value ? snd::Cue::Toggle : snd::Cue::Move);
                break;
            case Nav::Accept:
                snd::play(snd::Cue::Select);
                break;
            case Nav::Back:
                // Back on the launcher's first page goes nowhere: the refusal, not a step back.
                snd::play(before == Screen::Launch && g_screen == Screen::Launch ? snd::Cue::Refuse : snd::Cue::Back);
                break;
            default:
                snd::play(snd::Cue::Toggle);
                break;
        }
    }

    void handle_nav_now(Nav nav) {
        // The splash takes any press as a wish to get on (phase 99).
        if (g_splash == SplashState::Showing) {
            g_splash_skip = true;
            return;
        }
        // The on-screen keyboard holds every press while it is up (phase 110).
        if (g_kb_open) {
            keyboard_nav(nav);
            return;
        }
        // THE SHOULDERS, Q AND E TURN THE DIAL (phase 96), from any section and any page in it,
        // except while a question or the capture panel holds the focus.
        if (nav == Nav::SectionPrev || nav == Nav::SectionNext) {
            if (!(g_screen == Screen::Controls && (g_capture_open || devices::capture_status() == devices::CaptureStatus::Waiting))) {
                turn_dial(nav == Nav::SectionNext ? 1 : -1);
            }
            return;
        }
        switch (g_screen) {
            case Screen::None:
                break;
            case Screen::Controls:
                controls_nav(nav);
                break;
            case Screen::Warps:
                warps_nav(nav);
                break;
            case Screen::Moments:
                moments_nav(nav);
                break;
            case Screen::Lighting:
                rows_nav(Screen::Lighting, nav);
                break;
            case Screen::Debug:
                rows_nav(Screen::Debug, nav);
                break;
            case Screen::Hud:
                rows_nav(Screen::Hud, nav);
                break;
            // The first run questions take an answer, and Back is not one of them: there is no
            // way past this that does not decide, because the whole point of asking is that
            // nothing acts until somebody has. Left and Right move between the two answers the
            // way they move a setting, and Enter takes the one under the focus.
            //
            // THE BASELINE'S FIRST DEFECT (the menus plan, baseline.md): Left used to answer No at
            // once, against the sentence above, and Right to answer Yes. They move the focus now,
            // No first (the safe answer, never a destructive one by default), and Enter, the pad's
            // A or a click takes the answer.
            case Screen::Ask:
                if (nav == Nav::Left || nav == Nav::Right) {
                    g_ask_yes = nav == Nav::Right;
                    ask_focus_draw();
                }
                else if (nav == Nav::Accept) {
                    click_ask(g_ask_yes);
                }
                else if (nav == Nav::Back && g_confirm_open) {
                    // Escape and the pad's B decline. The first run questions still refuse Back,
                    // for the reason above; a confirmation raised later has somewhere to go back
                    // to, and refusing would trap a person in a warning they had already read.
                    click_ask(false);
                }
                break;
            // The files surface: the rows and the two buttons walk with the pad and the keyboard
            // (2026-09-24), and Back leaves it with an armed removal disarmed on the way out.
            case Screen::Files:
                files_nav(nav);
                break;
            case Screen::About:
                if (nav == Nav::Back) {
                    show(Screen::None);
                }
                else if (nav == Nav::Up || nav == Nav::Down) {
                    Rml::Element* body = g_about_doc != nullptr ? g_about_doc->GetElementById("about-body") : nullptr;
                    if (body != nullptr) {
                        body->ScrollTo(Rml::Vector2f(body->GetScrollLeft(),
                                                     body->GetScrollTop() + (nav == Nav::Down ? ABOUT_SCROLL_STEP : -ABOUT_SCROLL_STEP)),
                                       list_scroll());
                    }
                }
                break;
            case Screen::Browse:
                browse_nav(nav);
                break;
            case Screen::Settings:
            case Screen::Graphics:
            case Screen::Gameplay:
            case Screen::Capture:
            case Screen::Photo:
            case Screen::Video:
            case Screen::Launch:
                rows_nav(g_screen, nav);
                break;
        }
    }

    bool start() {
        Rml::SetSystemInterface(&g_system);

        if (!Rml::Initialise()) {   // RmlUi's own name, spelled as its authors spell it (lang-ok)
            say("RmlUi would not initialize, so there will be no interface");
            return false;
        }

        oot::ui::fonts::load(g_assets_dir);
        // The menu's own sounds (phase 111): their device, or one line saying they are silent.
        oot::ui::sound::start();

        g_context = Rml::CreateContext("main", Rml::Vector2i(g_width, g_height));
        if (g_context == nullptr) {
            say("no RmlUi context, so there will be no interface");
            return false;
        }
        // The rows' data model (phase 95), before the documents that bind it.
        if (!oot::ui::rows::create(g_context)) {
            say("the rows' data model would not start, so the menus will have no rows");
        }
        // The shell's (phase 96), and the shell itself: the one menu every page is laid into.
        if (!shell_model::create(g_context)) {
            say("the shell's data model would not start, so the menu will have no dial");
        }
        g_shell_doc = load("shell.rml");
        g_shell_doc_for_inset = g_shell_doc;
        g_splash_doc = load("splash.rml");
        g_keyboard_doc = load("keyboard.rml");

        g_settings_doc = load("settings.rml");
        g_about_doc = load("about.rml");
        g_controls_doc = load("controls.rml");
        g_hint_doc = load("hint.rml");
        g_counter_doc = load("counter.rml");
        g_guide_doc = load("photo-guide.rml");
        g_letterbox_doc = load("letterbox.rml");
        g_recorder_doc = load("recorder.rml");
        g_update_doc = load("update.rml");
        g_warps_doc = load("warps.rml");
        g_moments_doc = load("moments.rml");
        g_launch_doc = load("launch.rml");
        g_browse_doc = load("browse.rml");
        g_files_doc = load("files.rml");
        g_lighting_doc = load("lighting.rml");
        g_debug_doc = load("debug.rml");
        g_hud_doc = load("hud.rml");
        g_ask_doc = load("ask.rml");
        g_frame_doc = load("frame.rml");

        // THE FOCUS RING (the user, 2026-10-13): every action button gets an element of its own
        // for the ring the stylesheet draws a few dp outside it while it has the focus
        // (case.rcss, .focus-ring).
        for (Rml::ElementDocument* doc : { g_ask_doc, g_files_doc, g_launch_doc, g_moments_doc, g_settings_doc }) {
            if (doc == nullptr) {
                continue;
            }
            Rml::ElementList buttons;
            doc->QuerySelectorAll(buttons, ".act-btn");
            for (Rml::Element* button : buttons) {
                Rml::ElementPtr ring = doc->CreateElement("div");
                ring->SetClass("focus-ring", true);
                button->AppendChild(std::move(ring));
            }
        }

        // Every rows document draws its rows through the one template, bound to its own list.
        for (const Screen screen : { Screen::Launch, Screen::Settings, Screen::Lighting, Screen::Hud, Screen::Debug }) {
            if (Rml::ElementDocument* doc = rows_doc(screen)) {
                oot::ui::rows::install(doc->GetElementById(rows_container(screen)), rows_list(screen));
            }
        }

        if (g_settings_doc == nullptr && g_about_doc == nullptr) {
            say("no documents loaded, so there is nothing to show");
            return false;
        }

        // The recorder's plate listens too, because unlike the frame rate counter it is a
        // button: a click anywhere on it starts or ends a recording.
        // The mouse UP rather than the click, for the reason the listener gives: a press must
        // never be dropped because the library refused a focus or lost the pressed element.
        for (Rml::ElementDocument* doc : { g_settings_doc, g_about_doc, g_controls_doc, g_launch_doc,
                                          g_browse_doc, g_files_doc, g_ask_doc, g_frame_doc,
                                          g_warps_doc, g_recorder_doc, g_update_doc, g_moments_doc,
                                          g_lighting_doc, g_debug_doc, g_hud_doc, g_shell_doc, g_keyboard_doc }) {
            if (doc != nullptr) {
                doc->AddEventListener("mouseup", &g_clicks);
            }
        }
        // Every page is drawn inside the shell's box, without its own lid, foot or scrim.
        for (const Screen screen : { Screen::Launch, Screen::Browse, Screen::Files, Screen::Warps, Screen::Settings,
                                     Screen::Lighting, Screen::Hud, Screen::Debug, Screen::Controls, Screen::Moments,
                                     Screen::About }) {
            if (Rml::ElementDocument* doc = page_doc(screen)) {
                doc->SetClass("in-shell", true);
            }
        }

        g_signature = oot::signature::read_self();
        load_changelog();
        stamp_versions();
        g_game_started = launch::game_started();
        g_router.set_mode(g_game_started ? menu::Mode::Game : menu::Mode::Launcher);
        // Before Play the splash comes first, and the launcher when it goes (poll_splash).
        if (!g_game_started && g_splash_doc != nullptr) {
            start_splash();
        }
        else {
            show(Screen::None);
            if (!g_game_started) {
                launch::request_maximize_launcher();
            }
        }
        return true;
    }

} // namespace

namespace oot::ui::shell {

    // THE ROWS DUMP (the menus plan, phase 95): every rows document's rows, under the defaults and
    // under the choices that bring rows in and take them away, one line a row. Taken before and
    // after a change to how rows are drawn, the two files must be identical. Reads the tables
    // only: no window, no renderer, nothing written but the file named.
    int dump_rows(const std::string& path) {
        struct State {
            const char* name;
            void (*set)(oot::ui::Settings&);
        };
        const State states[] = {
            { "defaults", [](oot::ui::Settings&) {} },
            { "screenshots, video and microphone on, a pack chosen", [](oot::ui::Settings& s) {
                s.screenshots = 1; s.video_recording = 1; s.microphone = 1; s.texture_pack = 1; } },
            { "the HUD fades when idle, to clear, each part its own size, custom place", [](oot::ui::Settings& s) {
                s.hud_fade = 1; s.hud_fade_to = 0; s.hud_sizes = 1; s.hud_ratio = 3; } },
            { "the HUD fades always, to a color", [](oot::ui::Settings& s) {
                s.hud_fade = 2; s.hud_fade_to = 1; } },
        };
        const std::pair<Screen, const char*> screens[] = {
            { Screen::Launch, "launcher" }, { Screen::Settings, "settings" }, { Screen::Graphics, "graphics" },
            { Screen::Lighting, "lighting" }, { Screen::Gameplay, "gameplay" }, { Screen::Capture, "capture" },
            { Screen::Photo, "photo" }, { Screen::Video, "video" }, { Screen::Hud, "hud" }, { Screen::Debug, "debugging" },
        };
        std::FILE* out = std::fopen(path.c_str(), "wb");
        if (out == nullptr) {
            std::printf("ROWS_DUMP_FAIL cannot write %s\n", path.c_str());
            return 1;
        }
        int lines = 0;
        for (const State& state : states) {
            oot::ui::Settings s{};
            state.set(s);
            oot::ui::clamp(s);
            for (const auto& [screen, name] : screens) {
                std::fprintf(out, "== %s / %s\n", state.name, name);
                for (const oot::ui::rows::Row& row : row_views(screen, s, oot::ui::lighting::defaults())) {
                    std::fprintf(out, "%s\n", oot::ui::rows::dump_line(row).c_str());
                    ++lines;
                }
            }
        }
        std::fclose(out);
        std::printf("ROWS_DUMP_OK %d rows to %s\n", lines, path.c_str());
        return 0;
    }

    void configure(const std::string& assets_dir, const std::string& settings_path) {
        g_assets_dir = assets_dir;
        g_settings_path = settings_path;
    }

    void on_renderer_ready(Rml::RenderInterface* renderer) {
        if (renderer == nullptr || g_started || g_failed) {
            return;
        }
        Rml::SetRenderInterface(renderer);
        // The context needs a size before it can lay anything out, and the first hook call has
        // not happened yet. A sane default now, corrected on the first draw.
        if (g_width <= 0 || g_height <= 0) {
            g_width = 1280;
            g_height = 960;
        }
        if (start()) {
            g_started = true;
            say("interface ready");
            if (g_game_started) {
                frame_show();
                show_hint();
            }
        }
        else {
            g_failed = true;
            say("interface unavailable; the game is unaffected");
        }
    }

    void on_renderer_gone() {
        if (!g_started) {
            return;
        }
        oot::ui::rows::release();
        shell_model::release();
        g_shell_doc = nullptr;
        g_splash_doc = nullptr;
        g_splash = SplashState::Off;
        g_keyboard_doc = nullptr;
        g_kb_open = false;
        g_shell_doc_for_inset = nullptr;
        g_host_doc = nullptr;
        g_host_at = Rml::Vector2f(-1.0f, -1.0f);
        g_host_size = Rml::Vector2f(-1.0f, -1.0f);
        g_settings_doc = nullptr;
        g_about_doc = nullptr;
        g_controls_doc = nullptr;
        g_hint_doc = nullptr;
        g_launch_doc = nullptr;
        g_browse_doc = nullptr;
        g_frame_doc = nullptr;
        g_warps_doc = nullptr;
        g_moments_doc = nullptr;
        g_recorder_doc = nullptr;
        g_recorder_shown = false;
        g_update_doc = nullptr;
        g_update_shown = false;
        g_frame_shown = false;
        g_header_shown = false;
        g_edge_shown = false;
        g_hint_visible = false;
        g_hint_fading = false;
        g_capture_open = false;
        g_capture_settling = false;
        g_context = nullptr;
        oot::ui::sound::stop();
        Rml::Shutdown();
        g_started = false;
    }

    // THE LAYER DRAWS WHILE ANY DOCUMENT IS SHOWN, asked of the documents themselves (the user,
    // 2026-10-14: "we shouldn't have any like hand-maintained anything"). It used to draw while one of
    // a list showed: a page, the toast, the frame, the counter, the recorder plate, the splash. Photo
    // mode's guide and the letterbox bars were never put on that list, so each stayed on screen only
    // while something on it kept the layer drawing: the guide went with the toast, at the first camera
    // move or a few seconds in, and with the header bar, and never while the counter or the recorder
    // showed. Every document added from here on counts by being shown. RmlUi's Show and Hide update
    // the document at once, so this is right on the frame a document comes or goes.
    bool showing() {
        if (g_context == nullptr) {
            return false;
        }
        for (int i = 0; i < g_context->GetNumDocuments(); ++i) {
            if (const Rml::ElementDocument* doc = g_context->GetDocument(i); doc != nullptr && doc->IsVisible()) {
                return true;
            }
        }
        return false;
    }

    bool wants_draw() {
        return g_started && showing();
    }

    void pump(int width, int height) {
        if (!g_started || g_context == nullptr) {
            return;
        }

        // Every call is one presented frame: the frame rate counter counts them.
        poll_counter();
        poll_guide();
        poll_letterbox();
        poll_recorder();
        poll_update();
        poll_warps_clock();
        poll_text_entry();

        if (width != g_width || height != g_height) {
            g_width = width;
            g_height = height;
            g_resized = true;
            g_context->SetDimensions(Rml::Vector2i(width, height));
            if (g_screen == Screen::Controls) {
                size_long_rows(g_controls_doc, "controls-rows");
            }
            else if (g_screen == Screen::Browse) {
                size_long_rows(g_browse_doc, "browse-rows");
            }
            else if (g_screen == Screen::About) {
                size_long_rows(g_about_doc, "about-body", ABOUT_SHARE);
            }
            else if (g_screen == Screen::Warps) {
                cap_long_rows(g_warps_doc, "warp-rows", rows_cap(Screen::Warps));
            }
            else if (settings_like(g_screen) || g_screen == Screen::Launch) {
                cap_long_rows(rows_doc(g_screen), rows_container(g_screen), rows_cap(g_screen));
            }
        }

        // THE INTERFACE SIZE (phase 113): the scale the row and the window ask for, applied the frame
        // it changes; the shell reflows and place_page moves the page into its new box.
        {
            // The row changed in the launcher: the launcher takes the new scale (launch.cpp, on the
            // main thread), and a window that is not maximized takes the new size with it.
            static int shaped_for = -1;
            const int size_row = oot::ui::settings().interface_size;
            if (!g_game_started && size_row != shaped_for) {
                if (shaped_for >= 0) {
                    launch::request_window_rows(1);
                }
                shaped_for = size_row;
            }
            int display_w = 1920;
            int display_h = 1080;
            launch::display_size(display_w, display_h);
            const float dp = g_game_started ? oot::ui::interface_scale(size_row, g_width, g_height, display_w, display_h)
                                            : launch::launcher_scale_now();
            if (std::fabs(dp - g_context->GetDensityIndependentPixelRatio()) > 0.001f) {
                g_context->SetDensityIndependentPixelRatio(dp);
                g_resized = true;
                // RmlUi recomputes the sizes written in dp but not the ones a child inherits from
                // them: a row's label (the body's dp size, inherited) stayed at 100% beside a value
                // at 150%. Each document's stylesheet applied again recomputes everything.
                // Every document in the context, so one added later is never left out.
                for (int i = 0; i < g_context->GetNumDocuments(); ++i) {
                    Rml::ElementDocument* doc = g_context->GetDocument(i);
                    if (doc != nullptr && oot::ui::settings().scale_reload != 0) {
                        doc->ReloadStyleSheet();
                    }
                }
                std::fprintf(stderr, "[ui] interface size %.0f%%\n", dp * 100.0f);
                if (settings_like(g_screen) || g_screen == Screen::Launch) {
                    cap_long_rows(rows_doc(g_screen), rows_container(g_screen), rows_cap(g_screen));
                }
            }
            // The footer's prompts keep their 100% size below 100% (shell_model.h).
            oot::ui::shell_model::hold_prompt_size(g_shell_doc, g_context->GetDensityIndependentPixelRatio());
        }

        // THE PAGE AFTER A RESIZE (the user, 2026-10-13: "changing window size makes all options
        // disappear until i begin moving around the menu again"): refreshed and its focus put back
        // in view, which is what a move did that brought them back. A Debugging toggle while it is
        // a candidate (ui_settings.h).
        if (g_resized) {
            g_resized = false;
            if (oot::ui::settings().resize_refresh != 0 && g_screen != Screen::None && g_screen != Screen::Ask) {
                refresh_after_resize();
            }
        }

        drain();
        poll_fading();
        poll_inset();
        poll_splash();
        // A LINE LEFT BY THE GAME THREAD, taken here because this is already the place that owns
        // the toast. The moments module says what happened and knows nothing about RmlUi; this
        // decides how it is shown. A quick save arrives when the file is actually written rather
        // than when the key was pressed, which is the honest moment: the capture waits for a
        // frame it is safe to take.
        std::string said;
        if (oot::moments::take_notice(said)) {
            show_toast(said, "", NOTICE_SECONDS);
        }

        poll_hint();

        // Play started the game: the launcher goes, the frame comes, and the hint says how to
        // get the menu back.
        if (!g_game_started && launch::game_started()) {
            g_game_started = true;
            g_launch_note.clear();
            g_router.set_mode(menu::Mode::Game);
            show(Screen::None);
            frame_show();
            show_hint();
        }
        if (g_game_started) {
            poll_frame();
        }

        // The prompts follow the device in use from one input to the next (phase 96, 109).
        const Device device = oot::ui::input::last_device();
        if (device != g_prompt_device) {
            g_prompt_device = device;
            refresh_shell();
        }

        if (g_screen == Screen::Launch) {
            // A ROM validated on the main thread since the rows were last written.
            const std::string now = rom_value() + "|" + rom_note();
            if (now != g_rom_seen) {
                g_rom_seen = now;
                g_launch_note.clear();
                refresh_rows(Screen::Launch);
            }
            // The update check answers on its own thread, whenever it answers. It replaces the
            // key hints on the left of the foot and says nothing at all in every other case:
            // a check that found nothing, or failed, is not news.
            const std::string news = oot::updates::line();
            if (news != g_update_seen) {
                g_update_seen = news;
                set_text(g_launch_doc, "launch-foot",
                         news.empty() ? "F2 is about, F3 is controls" : news);
            }
        }
        else if (g_screen == Screen::Controls) {
            // A pad plugged in or pulled out while the document is open.
            const int pads = devices::pad_count();
            if (pads != g_pads_seen) {
                g_pads_seen = pads;
                controls::refresh();
                reflect_controls();
            }
            poll_capture();
        }
    }

    void draw(int width, int height) {
        (void)width;
        (void)height;
        if (!wants_draw()) {
            return;
        }

        g_context->Update();
        // The page goes into the shell's box once the box has its size, and again whenever it
        // moves (a placement, a window size); that is one more update, on that frame only.
        if (place_page()) {
            g_context->Update();
            apply_picture_inset(g_screen);
        }
        follow_rows_focus();
        g_context->Render();
        // The window is shown once something has been drawn in it (launch.h), and the splash counts
        // the frames drawn while it is actually on screen, for the pour.
        launch::request_show_window();
        if (g_splash == SplashState::Showing && launch::window_shown()) {
            ++g_splash_frames;
        }
    }

    void render_again() {
        if (wants_draw()) {
            g_context->Render();
        }
    }

    void post_open(Screen screen) {
        Command c;
        c.kind = Command::Kind::Open;
        c.screen = screen;
        post(c);
    }

    void post_close() {
        Command c;
        c.kind = Command::Kind::Close;
        post(c);
    }

    void post_toggle(Screen screen) {
        Command c;
        c.kind = Command::Kind::Toggle;
        c.screen = screen;
        post(c);
    }

    void post_toggle_settings() {
        post_toggle(Screen::Settings);
    }

    bool post_nav(Nav nav) {
        if (!g_capturing.load(std::memory_order_relaxed)) {
            return false;
        }
        Command c;
        c.kind = Command::Kind::Nav;
        c.nav = nav;
        post(c);
        return true;
    }

    bool text_entry_active() {
        return g_text_entry.load(std::memory_order_relaxed);
    }

    void post_text(const std::string& utf8) {
        Command c{ Command::Kind::Text };
        c.text = utf8;
        post(c);
    }

    void post_key(int sdl_scancode, bool ctrl, bool shift) {
        Command c{ Command::Kind::Key };
        c.x = sdl_scancode;
        c.down = ctrl;
        c.shift = shift;
        post(c);
    }

    void post_text_accept() {
        post(Command{ Command::Kind::TextAccept });
    }

    void post_text_escape() {
        post(Command{ Command::Kind::TextEscape });
    }

    void post_browse_path(const std::string& utf8) {
        Command c{ Command::Kind::BrowsePath };
        c.text = utf8;
        post(c);
    }

    void post_activity() {
        if (!g_hint_wants_activity.load(std::memory_order_relaxed)) {
            return;
        }
        Command c;
        c.kind = Command::Kind::Activity;
        post(c);
    }

    void post_mouse_move(int x, int y) {
        Command c;
        c.kind = Command::Kind::MouseMove;
        c.x = x;
        c.y = y;
        post(c);
    }

    void post_mouse_button(int button, bool down) {
        Command c;
        c.kind = Command::Kind::MouseButton;
        c.x = button;
        c.down = down;
        post(c);
    }

    void post_mouse_wheel(float delta) {
        Command c;
        c.kind = Command::Kind::MouseWheel;
        c.delta = delta;
        post(c);
    }

    void set_fullscreen(bool fullscreen) {
        g_fullscreen.store(fullscreen, std::memory_order_relaxed);
    }

    bool capturing_input() {
        return g_capturing.load(std::memory_order_relaxed);
    }

    uint32_t capture_releases() {
        return g_capture_releases.load(std::memory_order_acquire);
    }

} // namespace oot::ui::shell
