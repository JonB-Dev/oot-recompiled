#include "ui/ui_shell.h"
#include "ui/ui_warps.h"
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

#include "main/input.h"
#include "main/launch.h"
#include "main/places.h"
#include "main/install.h"
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
#include <SDL_scancode.h>

#include "build_info.h"

#include <fstream>
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
    constexpr size_t FILES_PATH_CHARS = 58;
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
    // Indexed by slot, which now runs from AUTOSAVE_OLDEST (negative) to SLOT_COUNT, so every
    // read goes through moment_index() rather than the slot itself.
    bool g_moment_present[oot::moments::SLOT_COUNT - oot::moments::AUTOSAVE_OLDEST + 1] = {};
    bool g_moment_delete_armed = false;

    // THE SCREEN HAS TWO ZONES (2026-09-25, the user could not select a moment and load it).
    // The rows are one, the buttons beneath them the other. A press on a row makes it the
    // selection and moves the cursor to the buttons, where the next press is Load or Cancel;
    // Back from the buttons returns to the rows with the selection kept. Mouse, keyboard and pad
    // all drive the same two zones, so none of them can reach a state the others cannot.
    enum class MomentZone { Rows, Buttons };
    MomentZone g_mzone = MomentZone::Rows;
    int g_mbutton = 0;   // an index into the buttons the current state offers

    // The rows in the order they are shown: the autosaves newest first, then the player's.
    std::vector<int> moment_order() {
        std::vector<int> order;
        for (int slot = oot::moments::AUTOSAVE_SLOT; slot >= oot::moments::AUTOSAVE_OLDEST; --slot) {
            order.push_back(slot);
        }
        for (int slot = 1; slot <= oot::moments::SLOT_COUNT; ++slot) {
            order.push_back(slot);
        }
        return order;
    }

    int moment_index(int slot) {
        return slot - oot::moments::AUTOSAVE_OLDEST;
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
    void load_fonts() {
        const std::string ours = g_assets_dir + "/fonts/";
        const std::vector<std::string> preferred = {
            ours + "display.ttf",
            ours + "display-bold.ttf",
            ours + "body.ttf",
            ours + "body-bold.ttf",
            ours + "mono.ttf",
        };

        int loaded = 0;
        for (const std::string& path : preferred) {
            if (std::filesystem::exists(path) && Rml::LoadFontFace(path)) {
                ++loaded;
            }
        }

        if (loaded > 0) {
            std::fprintf(stderr, "[ui] %d of our own font files loaded\n", loaded);
            return;
        }

        // The design decision's own fallback stacks name a serif for display and the system face
        // for reading, so this is the documented second choice rather than an improvisation.
        const std::vector<std::string> system_faces = {
            "C:/Windows/Fonts/georgia.ttf",
            "C:/Windows/Fonts/georgiab.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/segoeuib.ttf",
            "C:/Windows/Fonts/consola.ttf",
        };
        for (const std::string& path : system_faces) {
            if (std::filesystem::exists(path) && Rml::LoadFontFace(path)) {
                ++loaded;
            }
        }

        if (loaded == 0) {
            say("no font could be loaded, so the interface will have no text at all");
        }
        else {
            std::fprintf(stderr, "[ui] no bundled faces present, using %d system fallbacks\n", loaded);
        }
    }

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
    void click_files(const std::string& id);
    void click_moments(const std::string& id);
    void reflect_moments();
    void moments_nav(Nav nav);
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
    // Takes a row out of a list entirely. Only the files surface needs this now: the settings and
    // launcher lists build their rows from the model, so a row that should not be drawn is simply
    // not emitted, and the default prefix that used to serve them has gone with it.
    void hide_row(Rml::ElementDocument* doc, int row, const char* prefix);
    void show(Screen screen);
    void handle_nav(Nav nav);
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
            for (Rml::Element* e = event.GetTargetElement(); e != nullptr; e = e->GetParentNode()) {
                const std::string id = e->GetId();
                if (id.empty()) {
                    continue;
                }
                std::fprintf(stderr, "[ui] press on \"%s\"\n", id.c_str());
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
    enum class RowKind { Setting, Rom, Controls, Files, Moments, Debug, Play, Quit, Lighting, LightingMenu, LightingReset, LightingSweep, Warps, TimeOfDay, Piece, HudMenu };

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
        return (row >= 0) && (row == recorder || row == length || row == comparison);
    }

    // COMPARISON SHOTS SHOWS ONLY WHILE SCREENSHOTS IS ON (the user, 2026-10-09: "this being enabled
    // should dynamically then show an option for comparison mode"). The list is rebuilt on every
    // step, so turning Screenshots on brings the row at once.
    // THE HUD'S ROWS (2026-10-09, src/game/hud.h) belong to the HUD screen alone: every key that
    // begins "hud", so a row added to the HUD menu later is kept out of the main list without a
    // range to widen. "hud" itself, HUD sits, was the main list's Interface ratio until stage
    // three of the HUD brought it here, where the rest of where the HUD sits is.
    bool is_hud_setting(int row) {
        return std::strncmp(oot::ui::row_key(row), "hud", 3) == 0;
    }

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
        if (key == "hudwaketarget") {
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
        return nullptr;
    }

    bool is_hidden_debug_setting(int row) {
        static const int comparison = oot::ui::row_index("comparisonshots");
        return (row >= 0) && (row == comparison) && (oot::ui::settings().screenshots == 0);
    }

    // THE TEXTURE DETAIL ROW SHOWS ONLY WHILE A PACK IS CHOSEN (the user, 2026-10-07: "when you
    // enable it, then it adds an additional option right underneath it"). With no pack it would
    // be a row that does nothing, which this file's own notes call worse than no row. The list is
    // rebuilt on every step, so moving the pack row on or off brings it or takes it away at once,
    // and it sits directly under the pack row because it is the next row in the table.
    bool is_hidden_setting(int row) {
        static const int detail = oot::ui::row_index("texturedetail");
        return (row >= 0) && (row == detail) && (oot::ui::settings().texture_pack == 0);
    }

    bool is_debug_lighting(int row) {
        const std::string key = oot::ui::lighting::row_key(row);
        return key == "inspect" || key == "experiment" || key.compare(0, 6, "window") == 0 ||
               key == "lightstone" || key == "glowtest" ||   // the candidates of 2026-10-08
               key == "drawnbeam";                           // the game's beam beside the shafts, 2026-10-09
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

    std::vector<RowSpec> rows_for(Screen screen) {
        std::vector<RowSpec> rows;
        if (screen == Screen::Hud) {
            const oot::ui::Settings& s = oot::ui::settings();
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
                    add("hudwakeany");
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
                if (is_debug_setting(i) && !is_hidden_debug_setting(i)) {
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
        if (screen == Screen::Launch) {
            rows.push_back(RowSpec{ RowKind::Rom, 0 });
        }
        // THE TEXT AND THE HEART ROWS SIT WITH THEIR KIND (2026-10-08): appended to the table so
        // nothing renumbers, shown under Camera drift (the two text rows) and under Volume (the
        // beep), by key.
        static const int text_speed = oot::ui::row_index("textspeed");
        static const int text_skip = oot::ui::row_index("textskip");
        static const int health_beep = oot::ui::row_index("lowhealthbeep");
        static const int camera_drift = oot::ui::row_index("cameradrift");
        static const int volume = oot::ui::row_index("volume");
        // Screenshots sits under Menu placement (2026-10-09), placed the same way.
        static const int screenshots = oot::ui::row_index("screenshots");
        static const int menu_side = oot::ui::row_index("menu");
        // The video recorder's rows under Screenshots (2026-10-09), its size, quality and sound
        // only while it is On, the way Texture detail follows the pack.
        static const int video = oot::ui::row_index("videorecording");
        static const int video_size = oot::ui::row_index("videosize");
        static const int video_quality = oot::ui::row_index("videoquality");
        static const int video_sound = oot::ui::row_index("videosound");
        static const int video_mode = oot::ui::row_index("videomode");
        static const int microphone = oot::ui::row_index("microphone");
        static const int mic_device = oot::ui::row_index("micdevice");
        static const int mic_level = oot::ui::row_index("miclevel");
        // Photo mode under Screenshots too (2026-10-09), ahead of the video rows.
        static const int photo_mode = oot::ui::row_index("photomode");
        static const int video_shows = oot::ui::row_index("videoshows");
        static const int screenshot_shows = oot::ui::row_index("screenshotshows");
        for (int i = 0; i < oot::ui::row_count(); ++i) {
            if (is_debug_setting(i) || is_hidden_setting(i) || is_hud_setting(i) || i == text_speed || i == text_skip || i == health_beep ||
                i == screenshots || i == video || i == video_size || i == video_quality || i == video_sound ||
                i == video_mode || i == photo_mode || i == video_shows || i == screenshot_shows || i == microphone ||
                i == mic_device || i == mic_level) {
                continue;
            }
            rows.push_back(RowSpec{ RowKind::Setting, i });
            if (i == camera_drift) {
                rows.push_back(RowSpec{ RowKind::Setting, text_speed });
                rows.push_back(RowSpec{ RowKind::Setting, text_skip });
            }
            if (i == menu_side) {
                rows.push_back(RowSpec{ RowKind::Setting, screenshots });
                if (oot::ui::settings().screenshots != 0) {
                    rows.push_back(RowSpec{ RowKind::Setting, screenshot_shows });
                }
                rows.push_back(RowSpec{ RowKind::Setting, photo_mode });
                rows.push_back(RowSpec{ RowKind::Setting, video });
                if (oot::ui::settings().video_recording != 0) {
                    rows.push_back(RowSpec{ RowKind::Setting, video_shows });
                    rows.push_back(RowSpec{ RowKind::Setting, video_size });
                    rows.push_back(RowSpec{ RowKind::Setting, video_quality });
                    rows.push_back(RowSpec{ RowKind::Setting, video_sound });
                    rows.push_back(RowSpec{ RowKind::Setting, microphone });
                    if (oot::ui::settings().microphone != 0) {
                        rows.push_back(RowSpec{ RowKind::Setting, mic_device });
                        rows.push_back(RowSpec{ RowKind::Setting, mic_level });
                    }
                    rows.push_back(RowSpec{ RowKind::Setting, video_mode });
                }
            }
            if (i == volume) {
                rows.push_back(RowSpec{ RowKind::Setting, health_beep });
            }
        }
        rows.push_back(RowSpec{ RowKind::LightingMenu, 0 });
        rows.push_back(RowSpec{ RowKind::HudMenu, 0 });
        rows.push_back(RowSpec{ RowKind::Controls, 0 });
        rows.push_back(RowSpec{ RowKind::Files, 0 });
        // Saved moments (phase 77): in play, save anywhere and resume; from the launcher, resume
        // only, applied once a file is loaded.
        rows.push_back(RowSpec{ RowKind::Moments, 0 });
        // The debug menu, and IN PLAY ONLY. Every row in it asks the game to go somewhere, and
        // there is no game to send anywhere from the launcher: offering it there would be a menu
        // whose every row does nothing, which this project's own settings note calls worse than
        // no menu at all.
        if (screen != Screen::Launch) {
            rows.push_back(RowSpec{ RowKind::Debug, 0 });
        }
        if (screen == Screen::Launch) {
            rows.push_back(RowSpec{ RowKind::Play, 0 });
            rows.push_back(RowSpec{ RowKind::Quit, 0 });
        }
        else {
            // QUIT IN PLAY TOO. When Play and Quit moved out of the list into the launcher's
            // action bar, the in-game menu was left with no way out of the program at all (the
            // user, 2026-09-23: "The quit button is actually missing inside the game"), which
            // matters most with the title bar off, where this menu IS the way out. Play has no
            // place here; Quit does, in the same bar, on its own.
            rows.push_back(RowSpec{ RowKind::Quit, 0 });
        }
        return rows;
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
            case Screen::Settings: return g_settings_doc;
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

    void apply_picture_inset(Screen screen) {
        const int side = oot::ui::settings().menu_side;
        const bool panel = (side != 0) && (panel_doc(screen) != nullptr);
        int pixels = 0;
        if (panel) {
            Rml::ElementDocument* doc = panel_doc(screen);
            Rml::Element* box = (doc != nullptr) ? doc->QuerySelector(".case") : nullptr;
            float width = (box != nullptr) ? box->GetBox().GetSize(Rml::BoxArea::Margin).x : 0.0f;
            if (width <= 0.0f) {
                width = 620.0f * ((g_context != nullptr) ? g_context->GetDensityIndependentPixelRatio() : 1.0f);
            }
            pixels = std::min(static_cast<int>(std::lround(width)), g_width / 2);
        }
        oot::renderer::set_picture_inset(side == 2 ? pixels : 0, side == 1 ? pixels : 0);
    }

    // Where the two menus sit: over the picture, as a panel on the right, or as a panel on the
    // left (the Menu placement row). The classes go on each document's scrim; the stylesheet
    // does the rest, and the picture moves aside with it.
    void apply_menu_placement() {
        const int placement = oot::ui::settings().menu_side;
        const bool side = placement != 0;
        const bool left = placement == 2;
        for (Screen screen : { Screen::Settings, Screen::Lighting, Screen::Debug, Screen::Hud, Screen::Warps, Screen::Moments,
                               Screen::Controls, Screen::About, Screen::Ask }) {
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

    // The value column for one row, as markup.
    //
    // A SETTING GETS THE TWO STEPS either side of its value. A click used to move a setting one
    // way only, and a choice stops at its ends on purpose, so the last option was a dead end for
    // anyone using the mouse (the user, 2026-09-20: "it toggled up to 240 and then just stays
    // there and I have no way to get back down"). These do exactly what Left and Right do, and
    // being on screen is the other half of the fix: the keys worked and nothing said so.
    std::string stepper_html(size_t row, const std::string& text, bool low, bool high) {
        const std::string n = std::to_string(row);
        return "<span class=\"step" + std::string(low ? " spent" : "") + "\" id=\"prev-" + n + "\">&#8249;</span>"
               "<span class=\"shown\">" + escape(text) + "</span>"
               "<span class=\"step" + std::string(high ? " spent" : "") + "\" id=\"next-" + n + "\">&#8250;</span>";
    }

    // A box to tick where a stepper's value would sit, its right edge where the values' is, filled
    // while the row is on. It carries the row's next- id, so a press on it switches the row as a
    // press on the row does (nudge_setting toggles a box whichever way it is stepped).
    std::string check_html(size_t row, bool on) {
        const std::string n = std::to_string(row);
        return "<span class=\"shown\"><span class=\"check" + std::string(on ? " ticked" : "") + "\" id=\"next-" + n +
               "\"><span class=\"tick\"></span></span></span><span class=\"step-gap\"></span>";
    }

    // An action row says nothing at rest (the user's note of 2026-09-19: three rows reading
    // "Enter" were confusing); the text is there in a span the stylesheet shows only while the
    // row is focused or under the pointer.
    std::string action_html(const std::string& text) {
        return "<span class=\"act\">" + escape(text) + "</span>";
    }

    // Put the live values into a rows document.
    //
    // THE LIST IS BUILT WHOLE FROM THE MODEL, and the document holds no rows of its own. It used
    // to fill a fixed run of row elements typed into the .rml, and that arrangement failed the way
    // it was always going to: on 2026-09-22 two new settings pushed Controls, Your files and Debug
    // past the last slot, and a row with no element simply does not appear. No error, no gap, a
    // menu that looked entirely normal and was missing three entries. The user's answer was the
    // right one, and it is what the controls document and the debug menu already did: build the
    // rows, and let the container scroll.
    //
    // Rebuilt whole on every change, which at the rate a person presses keys costs nothing and
    // keeps one path rather than a fast one and a slow one that have to agree.
    void reflect_rows(Screen screen) {
        Rml::ElementDocument* doc = rows_doc(screen);
        if (doc == nullptr) {
            return;
        }
        Rml::Element* container = doc->GetElementById(rows_container(screen));
        if (container == nullptr) {
            return;
        }

        const oot::ui::Settings& s = oot::ui::settings();
        const std::vector<RowSpec> rows = rows_for(screen);

        std::string rml;
        for (size_t i = 0; i < rows.size(); ++i) {
            const std::string n = std::to_string(i);
            std::string label;
            std::string value;
            // What the chosen option means, under the row, for a row that has one (2026-10-09).
            const char* note = nullptr;

            switch (rows[i].kind) {
                case RowKind::Setting: {
                    const int row = rows[i].setting;
                    label = oot::ui::row_label(row);
                    const int number = oot::ui::get_row(s, row);
                    const char* option = oot::ui::option_label(row, number);
                    // A null label means the row is a number rather than a choice, which is how
                    // the volume row says so without needing a type tag.
                    const std::string shown = option != nullptr ? std::string(option)
                                                                : std::to_string(number) + "%";
                    value = is_check_setting(row) ? check_html(i, number != 0)
                                                  : stepper_html(i, shown, at_low(row, number), at_high(row, number));
                    note = oot::ui::option_note(row, number);
                    break;
                }
                case RowKind::Lighting: {
                    const int row = rows[i].setting;
                    label = oot::ui::lighting::row_label(row);
                    const int number = oot::ui::lighting::get_row(oot::ui::lighting::current(), row);
                    value = stepper_html(i, oot::ui::lighting::option_label(row, number), number <= 0,
                                         number >= oot::ui::lighting::option_count(row) - 1);
                    break;
                }
                case RowKind::LightingMenu:
                    label = "Lighting";
                    value = action_html("Switch each part of it on or off, and tune it");
                    break;
                case RowKind::HudMenu:
                    label = "HUD";
                    value = action_html("Fading, size and place");
                    break;
                case RowKind::LightingSweep:
                    label = "Sweep";
                    value = action_html(oot::sweep::running() ? "Running; the frames and the log go where recordings go"
                                                              : "Capture one frame per option of every row, then put yours back");
                    break;
                case RowKind::LightingReset:
                    label = "Reset";
                    value = action_html("Put the low starting values back");
                    break;
                case RowKind::Rom:
                    label = "ROM";
                    value = escape(rom_value());
                    break;
                case RowKind::Controls:
                    label = "Controls";
                    value = action_html("Click or press Enter to open");
                    break;
                case RowKind::Debug:
                    label = "Debugging";
                    value = action_html("Tools for development, not for playing");
                    break;
                case RowKind::TimeOfDay:
                    label = "Time of day";
                    // Never spent at either end: the list wraps, so a person can walk round the
                    // clock in one direction rather than back up the whole row.
                    value = stepper_html(i, time_label(), false, false);
                    break;
                case RowKind::Warps:
                    label = "Warps";
                    value = action_html("Go anywhere in the game");
                    break;
                case RowKind::Piece: {
                    // Named by the list's place in the room's file, which is the name the
                    // decompilation's extracted files carry (DL_03xxxxxx), so a piece found by eye
                    // here can be looked up there.
                    const std::vector<oot::room_pieces::Piece> pieces = oot::room_pieces::list();
                    const int at = rows[i].setting;
                    if (at < 0 || at >= static_cast<int>(pieces.size())) {
                        continue;
                    }
                    const oot::room_pieces::Piece& piece = pieces[at];
                    char name[96];
                    std::snprintf(name, sizeof(name), "Room %d, %08X (%d of %d white%s)", piece.room, 0x03000000u | piece.offset,
                                  piece.white, piece.vertices, piece.translucent ? ", see-through" : "");
                    label = name;
                    value = stepper_html(i, oot::room_pieces::state_label(oot::room_pieces::state_of(piece)), false, false);
                    break;
                }
                // Where everything actually is, and the way to take it all off the machine. The
                // answer to "I cannot find my saves", which is the real objection to a program
                // putting a person's files anywhere they did not pick themselves.
                case RowKind::Files:
                    label = "Your files";
                    value = action_html("Click or press Enter to open");
                    break;
                case RowKind::Moments:
                    label = "Moments";
                    value = action_html(g_game_started ? "Save anywhere, resume there" : "Resume a saved moment");
                    break;
                // Play and Quit are ACTIONS and are drawn in the action bar below the list (the
                // user, twice, most recently 2026-09-20: "I asked for the play and quit buttons to
                // be outside of this main list"). So they are NOT EMITTED here at all, where the
                // slotted version had to emit an empty row and then hide it. They keep their place
                // in the row order, so everything that walks the rows still walks them, and
                // apply_rows_focus puts the focus on the button instead.
                case RowKind::Play:
                    set_text(doc, "act-play-label", launch::rom_ready() ? "Play" : "Needs a ROM");
                    continue;
                case RowKind::Quit:
                    set_text(doc, "act-quit-label", "Quit");
                    continue;
            }

            if (const char* section = section_before(screen, rows[i])) {
                rml += "<div class=\"divider\"><span class=\"divider-label\">" + escape(section) + "</span></div>";
            }
            rml += std::string("<div class=\"row") + (note != nullptr ? " noted" : "") + "\" id=\"row-" + n + "\">";
            rml += "<span class=\"label\" id=\"label-" + n + "\">" + escape(label) + "</span>";
            rml += "<span class=\"value\" id=\"value-" + n + "\">" + value + "</span>";
            if (note != nullptr) {
                rml += "<div class=\"row-note\">" + escape(note) + "</div>";
            }
            rml += "</div>";
        }

        replace_rows(doc, container, rml, [&]() { cap_long_rows(doc, rows_container(screen), rows_cap(screen)); });
        apply_rows_focus(screen);
    }

    // A row whose content lives somewhere else (Play and Quit, which are buttons now) is
    // taken out of the list entirely rather than left as an empty stripe.
    // A click on Play or Quit runs exactly what Enter on them runs, by finding their own place
    // in the row order rather than duplicating what they do.
    // A step does exactly what Left and Right do on the focused row, so there is one behavior
    // and one place it is written.
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
        reflect_rows(g_screen);
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

    void apply_rows_focus(Screen screen) {
        Rml::ElementDocument* doc = rows_doc(screen);
        if (doc == nullptr) {
            return;
        }
        const std::vector<RowSpec>& specs = rows_for(screen);
        const int count = static_cast<int>(specs.size());
        for (int row = 0; row < count; ++row) {
            if (Rml::Element* e = doc->GetElementById("row-" + std::to_string(row))) {
                e->SetClass("on", row == g_focus);
                // THE LIST SCROLLS NOW, so walking off the bottom of the case has to bring the
                // row with it. Without this the focus carries on down a list nobody can see,
                // which is a worse version of the bug the scrolling was meant to fix.
                //
                // MEASURED HERE RATHER THAN ASKED OF ScrollIntoView (2026-09-24): its nearest
                // alignment moved the list on rows that were already in full view, so changing
                // a value scrolled the menu. The list moves only when the row is actually
                // outside it, and then by exactly the amount that brings it back.
                if (row == g_focus) {
                    if (Rml::Element* list = doc->GetElementById(rows_container(screen))) {
                        const float scroll = list->GetScrollTop();
                        const float top = e->GetAbsoluteOffset().y - list->GetAbsoluteOffset().y + scroll;
                        const float bottom = top + e->GetOffsetHeight();
                        const float view = list->GetClientHeight();
                        const float slack = 2.0f;   // a pixel of rounding is not a reason to move
                        if (top < scroll - slack) {
                            list->SetScrollTop(top);
                        }
                        else if (bottom > scroll + view + slack) {
                            list->SetScrollTop(bottom - view);
                        }
                    }
                }
            }
            // The two actions are drawn outside the list, so the focus has to follow them there
            // or walking down the screen with a pad appears to stop at the last setting.
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
                    reflect_rows(screen);
                }
                else if (spec.kind == RowKind::Lighting) {
                    nudge_lighting(spec.setting, nav == Nav::Right ? 1 : -1);
                    reflect_rows(screen);
                }
                else if (spec.kind == RowKind::TimeOfDay) {
                    nudge_time(nav == Nav::Right ? 1 : -1);
                    reflect_rows(screen);
                }
                else if (spec.kind == RowKind::Piece) {
                    nudge_piece(spec.setting, nav == Nav::Right ? 1 : -1);
                    reflect_rows(screen);
                }
                break;
            case Nav::Accept:
                switch (spec.kind) {
                    case RowKind::Setting:
                        nudge_setting(spec.setting, 1);
                        reflect_rows(screen);
                        break;
                    case RowKind::Lighting:
                        nudge_lighting(spec.setting, 1);
                        reflect_rows(screen);
                        break;
                    case RowKind::TimeOfDay:
                        nudge_time(1);
                        reflect_rows(screen);
                        break;
                    case RowKind::Piece:
                        nudge_piece(spec.setting, 1);
                        reflect_rows(screen);
                        break;
                    case RowKind::LightingMenu:
                        show(Screen::Lighting);
                        break;
                    case RowKind::HudMenu:
                        show(Screen::Hud);
                        break;
                    case RowKind::LightingReset:
                        oot::ui::lighting::apply(oot::ui::lighting::defaults());
                        reflect_rows(screen);
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
                            reflect_rows(screen);
                            launch::request_play();
                        }
                        else {
                            g_launch_note = "Choose a ROM first: drop it on the window, or Enter on the ROM row";
                            reflect_rows(screen);
                        }
                        break;
                    case RowKind::Quit:
                        launch::request_quit();
                        break;
                }
                break;
            case Nav::Back:
                if (screen == Screen::Settings) {
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
                else if (screen == Screen::Lighting || screen == Screen::Debug || screen == Screen::Hud) {
                    show(g_game_started ? Screen::Settings : Screen::Launch);
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
        set_text(g_files_doc, "files-left-label", g_remove_armed ? "No, keep everything" : "Close");
        if (oot::places::mode() == oot::places::Mode::Installed) {
            set_text(g_files_doc, "files-right-label",
                     g_remove_armed ? "Yes, remove it all" : "Remove everything");
            set_text(g_files_doc, "files-foot",
                     g_remove_armed
                         ? "Remove everything this program installed? The Everything folder above is deleted, with the saves and mods in it. Your copy of the game is moved to your Downloads folder first, never deleted"
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
                show(g_game_started ? Screen::Settings : Screen::Launch);
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
            const std::string said = oot::install::remove_everything();
            std::fprintf(stderr, "[install] %s\n", said.c_str());
            // Nothing is left to run from, so this is the end. The message goes to the log,
            // which is inside the folder that was just removed, so it also goes to stderr where
            // a harness run can read it.
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

    // The pad and the keyboard on the files surface: up and down walk the rows and then the two
    // buttons, Accept opens the focused folder or presses the focused button, Back leaves (and
    // disarms a removal on the way, so a half pressed destructive button never waits for the next
    // visit).
    void files_nav(Nav nav) {
        const int rows = static_cast<int>(g_file_rows.size());
        const int count = rows + 2;
        switch (nav) {
            case Nav::Up:
                g_ffocus = (g_ffocus + count - 1) % count;
                apply_files_focus();
                break;
            case Nav::Down:
                g_ffocus = (g_ffocus + 1) % count;
                apply_files_focus();
                break;
            case Nav::Left:
                g_ffocus = rows;
                apply_files_focus();
                break;
            case Nav::Right:
                g_ffocus = rows + 1;
                apply_files_focus();
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
                show(g_game_started ? Screen::Settings : Screen::Launch);
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

    void reflect_ask() {
        if (g_ask_doc == nullptr) {
            return;
        }
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
            set_text(g_ask_doc, "ask-detail",
                     "Once each time it opens, it would ask the release page whether a newer "
                     "version exists, and tell you if there is one. That is the only thing this "
                     "program ever sends over the network, and it is off until you say otherwise. "
                     "Nothing about you, your machine or your copy of the game is sent.");
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
                    e->ScrollIntoView(Rml::ScrollIntoViewOptions{ Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest });
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
                    e->ScrollIntoView(Rml::ScrollIntoViewOptions{ Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest });
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
            case Screen::Settings:
            case Screen::Lighting:
            case Screen::Hud:
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

    bool frame_wants_draw() {
        return g_frame_shown && (g_header_shown || g_edge_shown);
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
                    e->ScrollIntoView(Rml::ScrollIntoViewOptions{ Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest });
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
    // (newest first) and the player's slots keep their own numbers.
    std::string moment_row_id(int slot) {
        return oot::moments::is_autosave(slot)
            ? ("a" + std::to_string(oot::moments::AUTOSAVE_SLOT - slot))
            : std::to_string(slot);
    }

    int moment_slot_of_row(const std::string& id) {
        if (!id.empty() && id[0] == 'a') {
            return oot::moments::AUTOSAVE_SLOT - std::atoi(id.c_str() + 1);
        }
        return std::atoi(id.c_str());
    }

    std::string moment_slot_name(int slot) {
        if (!oot::moments::is_autosave(slot)) {
            return "Slot " + std::to_string(slot);
        }
        const int age = oot::moments::AUTOSAVE_SLOT - slot;
        return age == 0 ? std::string("Autosave, the newest") : ("Autosave, " + std::to_string(age + 1) + " back");
    }

    // WHICH BUTTONS THIS STATE OFFERS, in the order they sit on screen. Cancel is always there,
    // because the user asked for a way out from the buttons themselves; Delete and Resume need a
    // file; Save here needs the game running and never writes over an autosave, which the
    // autosave owns.
    std::vector<std::string> moment_buttons() {
        std::vector<std::string> ids;
        if (moment_present(g_mfocus)) {
            ids.push_back("moments-delete");
        }
        if (g_game_started && !oot::moments::is_autosave(g_mfocus)) {
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
                                                                  Rml::ScrollAlignment::Nearest });
                }
            }
        }

        const std::vector<std::string> buttons = moment_buttons();
        if (g_mbutton < 0 || g_mbutton >= static_cast<int>(buttons.size())) {
            g_mbutton = static_cast<int>(buttons.size()) - 1;   // Cancel, which is always offered
        }
        for (const char* id : { "moments-delete", "moments-save", "moments-load", "moments-cancel" }) {
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
        set_text(g_moments_doc, "moments-save-label",
                 !g_game_started ? "Save in play" : (oot::moments::is_autosave(g_mfocus) ? "The autosave's own" : (present ? "Save over" : "Save here")));
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
        for (int slot : moment_order()) {
            // THE DIVIDER the user asked for, once, where the autosaves end and their own slots
            // begin. It is not a row: nothing focuses it and nothing can be done to it.
            if (!divided && !oot::moments::is_autosave(slot)) {
                divided = true;
                rml += "<div class=\"divider\"><span class=\"divider-label\">Your slots</span></div>";
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
            if (ok && std::filesystem::is_regular_file(thumb, ec)) {
                // Forward slashes, because the document's own path is joined in front otherwise.
                std::string src = thumb.string();
                for (char& c : src) { if (c == '\\') c = '/'; }
                rml += "<img class=\"thumb\" src=\"" + escape(src) + "\"/>";
            }
            else {
                rml += "<div class=\"thumb empty\"></div>";
            }
            rml += "<span class=\"label\">";
            if (ok) {
                rml += escape(std::string(header.name)) + "<span class=\"slot\">" + escape(moment_slot_name(slot)) + "</span>";
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
                rml += escape(std::string("Scene ") + std::to_string(header.scene) + " \xC2\xB7 " + when);
            }
            rml += "</span></div>";
        }
        replace_rows(g_moments_doc, rows, rml,
                     [&]() { cap_long_rows(g_moments_doc, "moment-rows", rows_cap(Screen::Moments)); });
        if (!oot::moments::slot_exists(g_mfocus)) {
            g_mfocus = oot::moments::AUTOSAVE_SLOT;
        }
        apply_moments_focus();
    }

    void moments_leave() {
        g_moment_delete_armed = false;
        g_mzone = MomentZone::Rows;
        show(g_game_started ? Screen::Settings : Screen::Launch);
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

    void moments_nav(Nav nav) {
        const std::vector<int> order = moment_order();
        const std::vector<std::string> buttons = moment_buttons();
        const int count = static_cast<int>(order.size());
        const auto here = std::find(order.begin(), order.end(), g_mfocus);
        const int at = (here != order.end()) ? static_cast<int>(here - order.begin()) : 0;

        switch (nav) {
            // IN THE ROWS, up and down walk the list. ON THE BUTTONS, up returns to the rows and
            // down does nothing, so the two zones cannot be left by accident.
            case Nav::Up:
                if (g_mzone == MomentZone::Buttons) {
                    g_mzone = MomentZone::Rows;
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
                g_moment_delete_armed = false;
                apply_moments_focus();
                break;
            // Left and right walk the buttons, and do nothing in the rows: a row has no sideways.
            case Nav::Left:
            case Nav::Right:
                if ((g_mzone == MomentZone::Buttons) && !buttons.empty()) {
                    const int step = (nav == Nav::Right) ? 1 : static_cast<int>(buttons.size()) - 1;
                    g_mbutton = (g_mbutton + step) % static_cast<int>(buttons.size());
                    apply_moments_focus();
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
                show(Screen::Settings);
            }
            return;
        }
        if (g_wfocus < 0 || g_wfocus >= count) {
            g_wfocus = 0;
        }
        switch (nav) {
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
                    show(Screen::Settings);
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

    void show(Screen screen) {
        // Until the game has been started, nothing open means the launcher.
        if (screen == Screen::None && !g_game_started) {
            screen = Screen::Launch;
        }
        const Screen previous = g_screen;

        if (previous == Screen::Controls && screen != Screen::Controls) {
            devices::cancel_capture();
            end_capture_panel();
        }
        if (previous == Screen::Browse && screen != Screen::Browse) {
            blur_text_entry();
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
        // Before the game has started, a document opened from the launcher is a modal over it:
        // the launcher stays drawn beneath, pushed to the back, so the document's translucent
        // scrim dims it the way it dims the game in play, rather than the runtime's empty
        // frames showing black behind the case.
        const bool modal_over_launcher = !g_game_started &&
            (screen == Screen::Settings || screen == Screen::About || screen == Screen::Controls ||
             screen == Screen::Lighting || screen == Screen::Hud || screen == Screen::Ask);
        set(g_settings_doc, screen == Screen::Settings);
        set(g_lighting_doc, screen == Screen::Lighting);
        set(g_debug_doc, screen == Screen::Debug);
        set(g_hud_doc, screen == Screen::Hud);
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
        set(g_about_doc, screen == Screen::About);
        set(g_controls_doc, screen == Screen::Controls);
        set(g_launch_doc, screen == Screen::Launch || modal_over_launcher);
        set(g_browse_doc, screen == Screen::Browse);
        set(g_files_doc, screen == Screen::Files);
        set(g_ask_doc, screen == Screen::Ask);
        set(g_warps_doc, screen == Screen::Warps);
        set(g_moments_doc, screen == Screen::Moments);
        if (modal_over_launcher && g_launch_doc != nullptr) {
            g_launch_doc->PushToBack();
        }

        g_screen = screen;
        g_capturing.store(screen != Screen::None, std::memory_order_relaxed);
        // The picture comes back to the whole window when no menu is up, and moves aside when a
        // side panel is. Every menu screen takes the placement now, so it is applied here once for
        // all of them (and again below for the ones that rebuild their rows).
        apply_menu_placement();
        if (previous != screen) {
            std::fprintf(stderr, "[ui] screen %s\n", screen_name(screen));
        }

        if (screen == Screen::Settings) {
            if (previous != Screen::Settings) {
                g_focus = 0;
                // The system's microphones read again as the settings open, so one plugged in
                // since shows on the Microphone device row (main/microphone.h).
                oot::microphone::refresh();
            }
            apply_menu_placement();
            reflect_rows(screen);
            apply_rows_focus(screen);
        }
        else if (screen == Screen::Lighting) {
            if (previous != Screen::Lighting) {
                g_focus = 0;
            }
            apply_menu_placement();
            reflect_rows(screen);
            apply_rows_focus(screen);
        }
        else if (screen == Screen::Debug) {
            if (previous != Screen::Debug) {
                g_focus = 0;
            }
            apply_menu_placement();
            reflect_rows(screen);
            apply_rows_focus(screen);
        }
        else if (screen == Screen::Hud) {
            if (previous != Screen::Hud) {
                g_focus = 0;
            }
            apply_menu_placement();
            reflect_rows(screen);
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
            reflect_rows(screen);
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
            g_cfocus = 0;
            g_cslot = 0;
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
                    // Opens the screen when nothing is open and closes whatever is open
                    // otherwise. The launcher counts as nothing open, except that it already
                    // holds the settings, so the settings toggle does nothing there.
                    if (g_screen == Screen::None || g_screen == Screen::Launch) {
                        if (!(g_screen == Screen::Launch && c.screen == Screen::Settings)) {
                            show(c.screen);
                        }
                    }
                    else {
                        show(Screen::None);
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
    void handle_nav(Nav nav) {
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
            case Screen::Ask:
                if (nav == Nav::Left) {
                    click_ask(false);
                }
                else if (nav == Nav::Right || nav == Nav::Accept) {
                    click_ask(true);
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
                        body->SetScrollTop(body->GetScrollTop() + (nav == Nav::Down ? ABOUT_SCROLL_STEP : -ABOUT_SCROLL_STEP));
                    }
                }
                break;
            case Screen::Browse:
                browse_nav(nav);
                break;
            case Screen::Settings:
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

        load_fonts();

        g_context = Rml::CreateContext("main", Rml::Vector2i(g_width, g_height));
        if (g_context == nullptr) {
            say("no RmlUi context, so there will be no interface");
            return false;
        }

        g_settings_doc = load("settings.rml");
        g_about_doc = load("about.rml");
        g_controls_doc = load("controls.rml");
        g_hint_doc = load("hint.rml");
        g_counter_doc = load("counter.rml");
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
                                          g_lighting_doc, g_debug_doc, g_hud_doc }) {
            if (doc != nullptr) {
                doc->AddEventListener("mouseup", &g_clicks);
            }
        }

        g_signature = oot::signature::read_self();
        load_changelog();
        stamp_versions();
        g_game_started = launch::game_started();
        show(Screen::None);
        return true;
    }

} // namespace

namespace oot::ui::shell {

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
        Rml::Shutdown();
        g_started = false;
    }

    bool wants_draw() {
        return g_started && (g_screen != Screen::None || g_hint_visible || frame_wants_draw() ||
                             g_counter_shown || g_recorder_shown);
    }

    void pump(int width, int height) {
        if (!g_started || g_context == nullptr) {
            return;
        }

        // Every call is one presented frame: the frame rate counter counts them.
        poll_counter();
        poll_letterbox();
        poll_recorder();
        poll_update();
        poll_warps_clock();
        poll_text_entry();

        if (width != g_width || height != g_height) {
            g_width = width;
            g_height = height;
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
            else if (g_screen == Screen::Settings || g_screen == Screen::Launch) {
                cap_long_rows(rows_doc(g_screen), rows_container(g_screen), rows_cap(g_screen));
            }
        }

        drain();
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
            show(Screen::None);
            frame_show();
            show_hint();
        }
        if (g_game_started) {
            poll_frame();
        }

        if (g_screen == Screen::Launch) {
            // A ROM validated on the main thread since the rows were last written.
            const std::string now = rom_value() + "|" + rom_note();
            if (now != g_rom_seen) {
                g_rom_seen = now;
                g_launch_note.clear();
                reflect_rows(Screen::Launch);
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
        if (!g_started || g_context == nullptr ||
            (g_screen == Screen::None && !g_hint_visible && !frame_wants_draw() &&
             !g_counter_shown && !g_recorder_shown)) {
            return;
        }

        g_context->Update();
        g_context->Render();
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

} // namespace oot::ui::shell
