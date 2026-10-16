#pragma once

#include <cstdint>
#include <string>

namespace Rml {
    class RenderInterface;
}

// The interface's lifecycle and the only place that talks to RmlUi.
//
// EVERYTHING RmlUi TOUCHES HAPPENS ON RT64's RENDER THREAD. The library is not thread safe, and
// the draw has to happen on the thread holding the command list, so the simplest arrangement that
// is actually correct is to put the whole of it there: the context is created, documents are
// loaded, input is applied and the frame is drawn, all inside the render hook.
//
// The game's thread therefore never calls into RmlUi. It posts small commands (open the settings,
// a key went down) into a queue, and the hook drains them. That queue is the entire boundary.
namespace oot::ui::shell {

    // Called from the main thread before the runtime starts. Records where the documents and
    // fonts live; nothing is loaded yet, because there is no device until RT64 makes one.
    void configure(const std::string& assets_dir, const std::string& settings_path);

    // Writes every rows document's rows, under the defaults and under the choices that bring rows
    // in and out, one line a row (--ui-rows-dump, the menus plan, phase 95). Prints ROWS_DUMP_OK
    // or ROWS_DUMP_FAIL and returns the exit code.
    int dump_rows(const std::string& path);

    // Called by the render hook once the device side is up, on the render thread.
    void on_renderer_ready(Rml::RenderInterface* renderer);
    void on_renderer_gone();

    // Called from the render hook EVERY frame, before anything else and whether or not a screen
    // is open. This is where the command queue is drained, which is why it cannot live inside
    // `draw`: the command that opens a screen would then be waiting on the screen being open.
    void pump(int width, int height);

    // Is there anything to draw? Checked after the pump and before the framebuffer is bound: whether
    // any document is shown, asked of the documents themselves, so a closed interface costs a look
    // at a couple of dozen flags per frame and nothing has to be added to a list to be drawn.
    bool wants_draw();

    // Called inside the render hook with the framebuffer bound.
    void draw(int width, int height);

    // The interface drawn again exactly as `draw` left it, nothing updated, laid out or moved: for a
    // picture of the window at a size of its own, into whatever the render hook has bound
    // (ui_render.cpp). Called inside the render hook, after `draw`.
    void render_again();

    // ------------------------------------------------------------------------------------------
    // Posted from the game's thread. None of these touch RmlUi.
    // ------------------------------------------------------------------------------------------

    // Launch is the launcher (play milestone): the ROM, every setting, the controls, Play. It
    // is what None means until the game has been started; Browse is its ROM browser.
    //
    // Files is the answer to "where is this actually keeping my things": every resolved path,
    // openable, plus the removal of everything this program installed. One surface rather than
    // two, because the objection to putting somebody's files where they did not choose and the
    // wish to be able to take it all away again are the same worry asked twice.
    // Ask is the first run, and only the first run: one question at a time, asked once, with the
    // answer becoming an ordinary setting afterward. Nothing here has a default that acts before
    // it is answered, which is the point of asking.
    //
    // Warps is the debug menu (the user's ask of 2026-09-22): every place a player can warp to,
    // nested by location, walked a level at a time. Reached from a row in the in-game settings and
    // from nowhere else, because warping into a scene needs a game to be running.
    // Debug is the one menu that gathers everything meant for development rather than for playing:
    // the frame recorder and its length, the warps, the lighting sweep, and the two lighting rows
    // that replace the picture with one pass of it. It is reached through a confirmation, never
    // straight from a row (the user, 2026-09-26).
    // Graphics, Gameplay, Capture and its Photo and Video pages are the settings split out of the
    // one long list (2026-10-13); they are drawn by the settings document.
    enum class Screen { None, Settings, About, Controls, Launch, Browse, Files, Ask, Warps, Moments, Lighting, Debug, Hud,
                        Graphics, Gameplay, Capture, Photo, Video };

    void post_open(Screen screen);
    void post_close();

    // The menu key and the pad's menu button: opens the screen when nothing is open, and closes
    // whatever is open otherwise, so one press always gets back to the game.
    void post_toggle(Screen screen);
    void post_toggle_settings();

    // A key or a pad button, already translated to something the interface understands. Returns
    // true if the interface wanted it, in which case the game should not also act on it.
    // Default (restore a row's default) and the section changes arrive from the input stream
    // (ui/input/, the menus plan, phase 90); the screens that predate the shell ignore them.
    enum class Nav { Up, Down, Left, Right, Accept, Back, Clear, Default, SectionPrev, SectionNext };
    bool post_nav(Nav nav);

    // Any press at all, from the pump. The startup hint goes on the first one.
    void post_activity();

    // The mouse, from the pump, while a document is up: it hovers and clicks rows, works the
    // launcher's own minimize and close, and scrolls a long list. Window coordinates; RmlUi's
    // button numbering (0 left, 1 right, 2 middle); the wheel's delta positive down the page.
    void post_mouse_move(int x, int y);
    void post_mouse_button(int button, bool down);
    void post_mouse_wheel(float delta);

    // Whether the window is fullscreen, from the pump each tick: the game window's frame draws
    // its edge only in a window, and its header on the pointer either way.
    void set_fullscreen(bool fullscreen);

    // The ROM browser's path field (2026-09-19). While it has the focus the pump routes the
    // keyboard here instead of to the navigation: typed text, the editing keys (SDL scancodes,
    // with the control and shift state; the shell maps them for RmlUi), Enter (the path is
    // taken as the ROM) and Escape (the field lets go of the focus). The file picker, run by
    // the main thread, puts the chosen path into the field with post_browse_path.
    bool text_entry_active();
    void post_text(const std::string& utf8);
    void post_key(int sdl_scancode, bool ctrl, bool shift);
    void post_text_accept();
    void post_text_escape();
    void post_browse_path(const std::string& utf8);

    // True while a document is open, so the game's input can be held back. The hint is not a
    // document in this sense: it takes nothing.
    bool capturing_input();

    // How many times a document has let the input go: a count, so the game's input can tell a menu
    // closed even when it read nothing while the menu was open (the game is held then).
    uint32_t capture_releases();

} // namespace oot::ui::shell
