#pragma once

#include <cstdint>
#include <string>

// The settings the interface can change.
//
// EVERY ONE OF THESE DOES SOMETHING. That is the rule this file is built around, and it is why
// there are six of them rather than a dozen. A control that is shown, moves, saves, and changes
// nothing is worse than no control: it teaches the person using it that the menu is decorative.
// Separate music and effects volumes, a stick dead zone and a rumble switch were all written and
// then removed for exactly that reason, because nothing behind them was implemented yet. They
// come back when they work.
//
// THE FILE IS HOSTILE INPUT, every time. It sits where the user can edit it, it can be truncated
// by a power cut mid-write, and it can be a file somebody else wrote. So every value is clamped on
// the way in, an unreadable file is a reason to use the defaults rather than an error, and nothing
// here may stop the program starting.
namespace oot::ui {

    // Indices rather than enums, because they are written to a text file and read back from one,
    // and a number out of range has to be survivable. The names each index maps to live beside the
    // option lists in ui_settings.cpp.
    struct Settings {
        int window_mode = 0;    // 0 windowed, 1 fullscreen
        int resolution = 0;     // 0 original, 1 double, 2 match the window
        // 0 off, 1 2x, 2 4x: the picture rendered at that multiple of the chosen resolution
        // and downsampled to it, the reference project's Downsampling Quality. Nothing at
        // Match the window, where the picture is already the window's size.
        int downsampling = 0;
        int aspect = 0;         // 0 original 4:3, 1 expand to the window
        int hud_ratio = 0;      // HUD sits: 0 original 4:3, 1 within 16:9, 2 the window's edges, 3 custom
        int antialiasing = 0;   // 0 none, 1 MSAA 2x, 2 MSAA 4x, 3 MSAA 8x
        // How far ahead the game draws, as a multiple of the distance each actor asks for,
        // clamped to the scene's own fog (patches/culling.c). 0 is the game's own distance and
        // the rest step up from it. The user's ask of 2026-09-20: the cost is theirs to spend,
        // since it depends on the machine and on the scene, and the effect is visible while
        // standing still in a field and changing it.
        //
        // MEASURED, in Hyrule Field, as actor groups handed to the renderer per frame and frames
        // actually presented per second: the game's own distance is 52 groups and a 0.02 percent
        // shortfall, 3x is 292 and 0.14, 8x is 500 and 0.45. 3x is where the trees reach the fog
        // and stop paying for more.
        // THE DEFAULT IS THE GAME'S OWN since 2026-10-08 (was 3x): a first run is the console's
        // own picture and a person raises it (the user: "you're looking at N64 defaults").
        int render_distance = 0;   // index into the row's options, 0 being the game's own
        // 0 the game's own rate, 1 match the display, 2 and up a fixed rate (30, 60, 75, 90,
        // 120, 144, 165, 240: the user's ask of 2026-09-19, through the runtime's manual rate).
        // The renderer interpolates the transforms the game tags between game frames, which
        // since the post-parity plan's tagging phases is the camera, every actor, the effects,
        // the environment and the pause menu. The default was the display (OPEN-2, settled at
        // phase 46 on the harness's measurements); since 2026-10-08 it is the game's own 20, as
        // the console ran (the user: "I want those defaults to just be N64 defaults. frame rate
        // 20"). A file that names the row keeps its own value.
        int frame_rate = 0;   // the game's own rate
        // 0 off, 1 on: the frames presented per second, shown on a small plate at the top
        // right of the picture beside the game's own interface (the user's ask of 2026-09-19).
        // Off by default, and kept across launches like every other row (the user, 2026-09-24).
        int counter = 0;
        // 0 off, 1 on: the frame recorder's plate, at the top LEFT of the picture, mirroring the
        // counter's at the top right (the user's ask of 2026-09-22: "an overlay element that sits
        // it top left just like the fps does in top right"). Clicking it records every presented
        // frame as its own PNG for the length below. Off by default, as the counter is: a plate
        // that records several gigabytes on a stray click is not something to leave switched on.
        // Kept across launches like every other row (the user, 2026-09-24; from 2026-09-23 until
        // then it was forced off at every launch).
        int recorder = 0;
        // 0 the menus over the picture as a modal, 1 as a panel against the window's right edge,
        // 2 against its left (2026-09-24). With a panel the picture is scaled, ratio kept, into
        // the width beside it rather than covered (the user: "menu doesn't push over game, it
        // covers it").
        int menu_side = 0;
        // Which length the recorder takes, as an index: 0 is five seconds and 11 is a minute, in
        // steps of five. The default is 10 seconds; see the row's note in ui_settings.cpp for why
        // it is not the minute the row goes up to.
        int record_length = 1;
        // 0 the bar appears while the pointer is at the top of the window, 1 it never appears.
        // The user's ask of 2026-09-20: with it off the picture is never covered, and the way
        // out of the program is then the Quit button in this menu rather than the bar's close.
        // Which is why the two shipped together.
        int top_bar = 0;
        // 0 off, 1 on: whether the program asks the release page, once at launch, whether a
        // newer version exists. THE DEFAULT IS OFF AND HAS TO BE. The user's condition when they
        // asked for this was that the program prompt on first open, and a program that checks
        // while it is asking permission to check has already done the thing it is asking about.
        // See .scaffold/security/edge-serverside.md for the whole threat model.
        int updates = 0;
        // 0 off, 1 every 5 minutes, 2 every 10, 3 every 15: a saved moment taken into the autosave
        // slot at a safe moment on that interval (phase 78, .scaffold/states/). Off by default:
        // the manual save is the game's own and stays the player's decision.
        int autosave = 0;
        // 0 off, 1 on: quitting (the menu's Quit, the window's close) first keeps a moment in the
        // autosave slot, waiting a few seconds for a safe frame; and, separately, starting resumes
        // the newest moment once a file is loaded. Both off by default. The user, 2026-09-23:
        // "so close and reopen is seamless".
        int save_on_quit = 0;
        int resume_on_start = 0;
        // THE FREE CAMERA (upgrades phase 79, the user's ask of 2026-09-24): 0 off, 1 the right
        // stick turns the camera around Link wherever the game's own camera is free to turn
        // (patches/free_camera.c; a fixed, scripted or Z target camera never sees it). While
        // it is on, the stick's C button bindings are silent except with the ocarina out.
        // Off by default: it takes the C buttons off the stick, which a person should choose.
        int free_camera = 0;
        // 0 as the stick moves, 1 left and right inverted, 2 up and down inverted, 3 both.
        int camera_invert = 0;
        // The same four, for the first person look and the aiming (the bow, the slingshot, the
        // hookshot, the boomerang), which the user wants inverted on their own (2026-09-24:
        // "Separate up and down ... for first person and/or the slingshot versus just normal
        // free camera").
        int aim_invert = 0;
        // The same four for the LEFT stick, which is the one the game itself reads in first
        // person and while aiming (the user, 2026-09-24: "for aiming please add an option to set
        // left or right stick inverts individually since some like left and some like right").
        // It applies whether or not the Free camera row is on, being the game's own stick.
        int aim_invert_left = 0;
        // How far the camera sits from Link, as a scale on the distance the game's own camera
        // keeps for the scene it is in. 0 NEAR IS THE GAME'S OWN, 1 a fifth further, 2 three
        // tenths, 3 two fifths (the user, 2026-09-24: "remove the near camera distance option
        // make the current original be the new near ... place new original in between them", then
        // "can you add a cam distance of medium in between original and far"). Near is the default,
        // so an untouched install stands exactly where the game stands. It applies to every
        // player camera, the free one and the game's own alike, including the one Z targeting
        // hands back to, because it is applied where they all clamp their distance
        // (patches/free_camera.c, Camera_ClampDist).
        int camera_distance = 0;
        // 0 off, 1 on: whether the camera's distance eases out as he runs and back in as he
        // stops, which is what the game itself does and the default here. Off holds one distance
        // (the user, 2026-09-24: "the slight pull away with movement or as link gets closer can
        // be an option to disable or enable"). The free camera holds its distance either way.
        int camera_drift = 1;
        // 0 the game's own scissor, 1 the program draws the bars itself. HELD AT 0 AND NOT ON A
        // ROW (2026-09-24): the drawn bars are smooth and cover the interface, because the game's
        // bars are a scissor on the three dimensional view alone and the interface is drawn
        // afterward at the full picture, so in the game the hearts sit on top of the black. The
        // row existed for one build, the user chose Smooth, the choice was saved, and turning the
        // default back did nothing for them ("bars still cover"), which is what a row whose other
        // choice is wrong does to somebody. It comes back when the renderer can apply the inset
        // to the perspective projections alone; clamp() holds it at 0 until then.
        int letterbox = 0;
        int volume_percent = 100;

        // The Texture pack row (2026-09-30): 0 none, else the one-based place of a pack in
        // the folder's own sorted list. An INDEX rather than a name because a setting is an
        // int here, with the consequence written down rather than discovered: remove a pack
        // and the packs after it shift up one, so the row may name a different pack than it
        // did. It can never name a pack that is gone, because the label falls back to Off.
        int texture_pack = 0;
        // The Texture detail row (2026-10-07): 0 a quarter of the pack's width and height, 1 a
        // half, 2 the pack as made (lightest first since 2026-10-08), applied as each texture
        // loads.
        // QUARTER UNTIL THE PERSON RAISES IT (the user, 2026-10-08: "make the texture pack start at
        // quarter quality so that users with low end pcs dont have it kill their pc before they
        // are able to change it"). The first load of a pack happens the moment the row names one,
        // often on the launcher before anything has been seen running, so it must be the light
        // one. A settings file that already holds the row keeps its value.
        int texture_detail = 0;
        // THE TEXT AND THE HEART ROWS (the user, 2026-10-08: "faster dialog typing so it doesn't
        // animate in so slowly, and the ability to always skip press the a or b to auto-finish the
        // typing and skip if needed rgardless if the game normally allows. then also add the
        // ability as a settings to turn off the low health beeps"). patches/message_assist.c and
        // patches/health_beep.c read them through recomp_api.cpp. Each starts on the game's own.
        // Text speed: 0 the game's pace, 1 twice it, 2 four times, 3 each box whole at once.
        int text_speed = 0;
        // Skip text: 0 the game's rule (B, where the message allows it), 1 always: A shows the
        // rest of the box, B skips on as the game's B does, in every message, and either ends a
        // box's timed wait.
        int text_skip = 0;
        // Low health beep: 0 the game's beep, 1 silent.
        int low_health_beep = 0;
        // SCREENSHOTS (2026-10-09, main/screenshots.h): 0 off, 1 the Screenshot binding takes one
        // PNG of the picture as presented. Comparison shots, a Debugging row shown only while
        // Screenshots is on: 1 makes the binding take a before and after pair instead.
        int screenshots = 0;
        int comparison_shots = 0;
        // THE VIDEO RECORDER (2026-10-09, main/video.h). Video recording: 0 off, 1 the Record video
        // binding starts and stops a recording and the Pause video binding pauses it. Video size:
        // 0 the window's own, then 720p, 1080p, 1440p and 4K at the window's shape. Video quality:
        // Small file, Standard, High, Best (the bit rate). Video sound: 0 the game's sound is
        // recorded, 1 the video is silent and no time is spent on sound.
        int video_recording = 0;
        int video_size = 2;
        int video_quality = 1;
        int video_sound = 0;
        // Video mode (2026-10-09): 0 Reliable, written in pieces that each play on their own and
        // made an ordinary MP4 at the stop; 1 Compatible, an ordinary MP4 from the start.
        int video_mode = 0;
        // Photo mode (2026-10-09, main/photo.h): 0 off, 1 the Photo mode binding freezes the game.
        int photo_mode = 0;
        // Video shows (2026-10-09, main/video.h): 0 the game alone, 1 the whole window, the
        // program's menus, launcher, toasts and plate included.
        int video_source = 0;
        // Screenshots show (2026-10-09, main/screenshots.h): 0 the game alone, 1 the whole window.
        int screenshot_source = 0;
        // THE HUD'S FADING (2026-10-09, game/hud.h, the HUD menu). HUD fades: 0 never, 1 when idle,
        // 2 always, 3 by button. Fades to: 0 dim, 1 hidden. Dim opacity and Dim color (its
        // saturation) by step. Idle delay by step. Z targeting: whether targeting brings it
        // back (the rest of what can is below, hud_wake_c onward). Fade speed: 0 instant to 3
        // slow. Each part's own: 0 fades with the HUD, 1 stays shown.
        int hud_fade = 0;
        int hud_fade_to = 0;
        int hud_opacity = 2;
        int hud_color = 0;
        int hud_delay = 3;
        int hud_wake_target = 1;
        int hud_speed = 2;
        int hud_fade_hearts = 0;
        int hud_fade_magic = 0;
        int hud_fade_rupees = 0;
        int hud_fade_buttons = 0;
        int hud_fade_map = 0;
        // THE MICROPHONE IN A VIDEO (2026-10-09, main/microphone.h): 0 off, 1 the voice mixed in.
        // Microphone device: 0 the system's default, then the system's devices by place in the
        // list (not clamped to the list as read, which changes as devices come and go). Microphone
        // level by step.
        int microphone = 0;
        int mic_device = 0;
        int mic_level = 2;
        // THE HUD'S SIZE (2026-10-09, game/hud.h, the HUD menu): Sizes 0 one for all, 1 each part
        // its own; HUD size and each part's by step, 5 being the game's own (100%).
        int hud_sizes = 0;
        int hud_size = 5;
        int hud_size_hearts = 5;
        int hud_size_magic = 5;
        int hud_size_rupees = 5;
        int hud_size_buttons = 5;
        int hud_size_map = 5;
        // WHAT BRINGS THE HUD BACK, each 0 off or 1 on (2026-10-09, game/hud.h, the HUD menu's
        // Comes back for boxes): Z targeting is hud_wake_target above; then the C buttons, talking,
        // health or magic changing, low health (which keeps the hearts alone shown), rupees, keys,
        // items or ammunition changing, the pause menu, and any button.
        int hud_wake_c = 1;
        int hud_wake_talk = 1;
        int hud_wake_health = 1;
        int hud_wake_low = 1;
        int hud_wake_items = 1;
        int hud_wake_pause = 1;
        int hud_wake_any = 0;
        // WHERE THE HUD SITS, CUSTOM (2026-10-09, game/hud.h, the HUD menu): each corner's distance
        // from the frame's left, right, top and bottom edges, as a share of the frame, by step.
        int hud_left = 4;
        int hud_right = 4;
        int hud_top = 4;
        int hud_bottom = 6;

        bool operator==(const Settings& other) const;
        bool operator!=(const Settings& other) const { return !(*this == other); }
    };

    // How many choices each index has, and what to show for the current one.
    int option_count(int row);
    const char* option_label(int row, int value);
    // What the chosen option of a row means, shown under the row while it is on screen
    // (2026-10-09), or nullptr for a row that says nothing more than its label.
    const char* option_note(int row, int value);
    const char* row_label(int row);
    // The row's key as the settings file names it ("hudfade", ...), or "" past the table.
    const char* row_key(int row);
    int row_count();
    // The index of the row with this key ("updates", "recorder", ...), or -1. Anything outside
    // the table that needs one row asks by KEY: a fixed number went stale the day two rows were
    // inserted above it, and the first run prompt then switched the recorder on for three days
    // while the update check stayed off (2026-09-23).
    int row_index(const char* key);

    // Read and write one row's value by index, so the interface needs no knowledge of the fields.
    int get_row(const Settings& s, int row);
    void set_row(Settings& s, int row, int value);

    const Settings& settings();

    // Has the person been asked about updates yet? Persisted beside the rows, but NOT a row: the
    // question is asked once and the answer is the `updates` row, so a third visible state would
    // be a control with a meaningless option in it. False on a fresh install, which is what makes
    // the first run prompt appear.
    bool update_choice_made();
    void mark_update_choice_made();

    // Has the person been asked whether to keep a copy of their ROM in the data folder? Same
    // shape, same reason, and asked in the same breath so first run asks once rather than twice.
    bool rom_choice_made();
    void mark_rom_choice_made();

    // Load, clamping everything. A missing or damaged file leaves the defaults and says so.
    void load_settings(const std::string& path);

    // Whether a file was there to load. A fresh install has none, and that is when the launcher
    // chooses defaults for the machine rather than the table's.
    bool settings_file_present();

    // While the launcher is up the window is the launcher's own small one, so the window mode
    // the settings hold (a fresh install's default is fullscreen) is held back from the runtime
    // until Play; releasing the hold pushes the real mode. Main thread.
    void hold_window_mode(bool held);

    // The launcher's defaults for a fresh install: the console's own picture (320 by 240 at 4:3,
    // the interface where it was, the game's own 20 frames a second and draw distance, no
    // antialiasing, no pack), fullscreen. Since 2026-10-08, on the user's word; until then they
    // matched the monitor and chose antialiasing from the card's memory. Applied once, when the
    // launcher first shows with no settings file.
    Settings machine_defaults();

    // Replace the live settings, push them at the runtime, and write them out. Returns false only
    // if the write failed; the change still took effect, because a disk that will not take a
    // settings file is not a reason to ignore what the user just asked for.
    bool apply(const Settings& next);

    void clamp(Settings& s);

    // The output gain the audio path should apply, 0.0 to 1.0. Read on the audio thread every
    // buffer, so it is a plain float updated whole rather than anything that can lock.
    float volume_gain();

    // The render distance row as a 16.16 fixed point multiplier, 65536 being the game's own
    // distance. Fixed point because this crosses into the MIPS patch (patches/culling.c) through
    // recomp_api.cpp, and a float has no register both sides of that bridge agree on; the same
    // reason recomp_wide_frame_scale_q16 is fixed point.
    //
    // Read once per frame by the patch and cached there, NOT once per actor: the culling test it
    // feeds runs for every actor of every frame.
    unsigned int render_distance_q16();

    // The free camera rows as bits for the patch (patches/free_camera.c, through
    // recomp_api.cpp): 1 the Free camera row on, 2 left and right inverted, 4 up and down
    // inverted; 8 and 16 the same two for the aiming (the Aiming axes row). Read once per game
    // frame.
    unsigned int free_camera_mode();

    // The text rows as bits for the patch (patches/message_assist.c, through recomp_api.cpp):
    // bits 0 and 1 the Text speed row, bit 2 the Skip text row's "always". Read once per frame.
    unsigned int message_assist_mode();

    // Whether the low health beep sounds (the Low health beep row). Read when it would sound.
    bool low_health_beep_enabled();

    // The Screenshots row, and the Debugging menu's Comparison shots row under it (main/screenshots.h).
    bool screenshots_enabled();
    bool comparison_shots_enabled();

    // The Video recording row (main/video.h): whether its two bindings do anything.
    bool video_recording_enabled();

    // The Photo mode row (main/photo.h): whether its binding does anything.
    bool photo_mode_enabled();

    // The Screenshots show row: the game's picture alone rather than the whole window.
    bool screenshots_game_alone();

    // The Microphone level row as a gain for the microphone's sound.
    float mic_gain();

    // The console's 4:3, original resolution and no downsampling to the renderer while on, whatever
    // the rows say; the rows and the file are untouched. For the comparison shots.
    void set_original_look(bool on);

    // FORCE THE CONSOLE'S 4:3 WHILE A PRERENDERED ROOM IS ON SCREEN, whatever the aspect and
    // interface rows say, and put them back when it is not.
    //
    // WHY. A handful of rooms (Link's house, the castle grounds, most shop and house interiors)
    // are drawn as a fixed camera over a background PICTURE made for the console's 320 by 240
    // frame, with only the actors and a little geometry drawn in 3D over it. Widen the view and
    // the two stop agreeing: the renderer stretches that 2D picture across the frame while the
    // 3D projection widens instead, so the world no longer sits where the backdrop says it does.
    // The user's report, 2026-09-20: "the larger aspect ratio is stretching it out and causing
    // the interactions and Link's positioning to be different. So it actually needs to force
    // these to maintain a four x three ratio."
    //
    // The user's rows are NOT touched and nothing is written to the settings file: this is an
    // override applied on the way to the runtime, so leaving such a room restores whatever the
    // user chose. The interface ratio is forced with it, because an interface spread across a
    // wide frame around a 4:3 picture is its own kind of wrong.
    //
    // Called from the game thread once a frame (patches/prerender_aspect.c) and cheap when
    // nothing changes: it pushes at the runtime only when the answer is different from last
    // frame's.
    // cam_setting is the active camera's setting, or -1 when there is no play state. It changes
    // nothing and is carried only so the trace names it when the view switches: this revision has
    // three prerender camera settings and the first version of the patch matched only one of
    // them, which made a working mechanism look like a broken one.
    void set_prerendered_room(bool on, int cam_setting);

    // Whether a prerendered room is up, as the debounced answer above settled it. The traced
    // lighting stands down in one: such a room is a PICTURE with a crude depth mesh under it,
    // so the rays have a few boxes to work with and the walls and towers in the picture are not
    // there at all (the user, 2026-09-28, with the castle courtyard beside its white debug pass:
    // "her light doesnt hit top portions of the fixed scenes ... there is nothing to interact
    // with ... we probably need to disable these effects in the fixed castle scenes").
    bool prerendered_room();

    // THE LETTERBOX BARS (2026-09-24; the user: "the black bar at top and bottom of screen
    // animates in very very chappy rather than smooth when z targeting"). The game makes them by
    // scissoring the picture, and that scissor can only change once per game update, twenty times
    // a second, so a bar coming in shows four positions however fast the picture is presented.
    // The patch publishes the size it wants here, in console pixels out of the picture's two
    // hundred and forty, and the interface eases its own bars to it every frame it draws.
    //
    // Returns true when the program is drawing them, which tells the patch to leave the game's
    // scissor at the full picture. False when the row is on Game's own, and then nothing here
    // draws and the scissor is the game's as it always was. Called from the game thread.
    bool letterbox_target(int size);

    // What the bars stand at right now, as a share of the picture's height (0 to 0.5 at each
    // edge), eased toward the target by the time since the last call. Called from the interface
    // once per presented frame; it is the one place the easing happens.
    float letterbox_now();

} // namespace oot::ui
