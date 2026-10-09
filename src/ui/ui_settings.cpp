#include "ui/ui_settings.h"
#include "game/texture_packs.h"
#include "main/microphone.h"

#include <cstring>
#include <algorithm>
#include <iterator>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

#include "ultramodern/config.hpp"

#include "main/keyvalue.h"
#include "game/render.h"

namespace {

    oot::ui::Settings g_live;
    std::string g_path;

    // Read on the audio thread once per buffer. A float written whole is fine here: the worst a
    // torn read could do is apply last frame's gain, and there is no lock to take on that path.
    std::atomic<float> g_gain{ 1.0f };

    // Asked once, on the first run, and never again. Kept beside the rows rather than as rows:
    // the question has an answer (the `updates` row, and whether a copy of the ROM was taken) and
    // a control whose options included "not yet asked" would be a control with a meaningless
    // state in it.
    bool g_update_choice_made = false;
    bool g_rom_choice_made = false;


    struct Row {
        const char* key;        // what it is called in the file
        const char* label;      // what it is called on screen
        // TWELVE, which is the recording length row's count (five seconds to a minute, in fives).
        // It was ten, which was exactly the frame rate row's count and therefore exactly one row
        // away from being a silent overrun: a longer row would have written past the end of the
        // struct with nothing to say so.
        const char* options[12];
        int count;
    };

    // The fixed rates the frame rate row offers after Original and Match the display, in the
    // row's order (the user's ask of 2026-09-19); the runtime's manual rate carries them.
    constexpr int FIXED_RATES[] = { 30, 60, 75, 90, 120, 144, 165, 240 };
    constexpr int FIXED_RATE_COUNT = static_cast<int>(sizeof(FIXED_RATES) / sizeof(FIXED_RATES[0]));

    // The order here IS the order on screen and the order in the file. Adding a row means adding
    // it here, in get_row, set_row and clamp, and in the documents' row slots.
    const Row ROWS[] = {
        { "window",       "Window",             { "Windowed", "Fullscreen" },                    2 },
        // The fixed sizes and ratios come AFTER the choices that were there (2026-09-24), so a
        // file written before them still means what it meant: Match the window is 2 in every
        // file that exists.
        { "resolution",   "Resolution",         { "Original", "Double", "Match the window", "720p", "1080p", "1440p", "4K" }, 7 },
        { "downsampling", "Downsampling",       { "Off", "2x", "4x" },                           3 },
        { "aspect",       "Aspect ratio",       { "Original 4:3", "Expand to the window", "16:9", "16:10", "21:9", "32:9" }, 6 },
        // WHERE THE HUD SITS (the user, 2026-10-09: "choosing whether it sits specific to a ratio,
        // the current window size, or custom positioning only up to the window size so stuff never
        // gets hidden when scaling"). The Interface ratio row it was, its first three values kept
        // so a settings file reads the same, with Custom added; shown in the HUD menu since.
        { "hud",          "HUD sits",           { "Original 4:3", "Within 16:9", "Window edges", "Custom" }, 4 },
        { "antialiasing", "Antialiasing",       { "Off", "2x", "4x", "8x" },                     4 },
        // How far ahead the game draws, as a multiple of what each actor asks for, clamped to
        // the scene's own fog. The labels say what they cost rather than only what they do,
        // because the user asked for this row so the cost could be theirs to spend and the
        // numbers were measured rather than guessed (ui_settings.h has the table).
        { "renderdistance", "Render distance",  { "Original", "2x", "3x", "4x", "6x", "8x" },    6 },
        { "framerate",    "Frame rate",         { "Original", "Match the display", "30", "60", "75", "90", "120", "144", "165", "240" }, 2 + FIXED_RATE_COUNT },
        { "counter",      "Frame rate counter", { "Off", "On" },                                 2 },
        // The frame recorder (the user's ask of 2026-09-22). Its plate sits at the top LEFT of
        // the picture, mirroring the frame rate counter's at the top right, and clicking it
        // records every presented frame as its own PNG for the length below. Read back from the
        // file like every other row (2026-09-24, the user's word; see load_settings).
        { "recorder",     "Frame recorder",     { "Off", "On" },                                 2 },
        // "any incriment of 5 seconds every 5 seconds up to 1 min", in the user's own words. The
        // seconds are (index + 1) * 5, which recorder.h's length_seconds does; this is the one
        // place the twelve are written down.
        //
        // THE DEFAULT IS TEN SECONDS rather than the minute the row goes up to, because the cost
        // is files: a minute at sixty frames a second is 3600 of them and several gigabytes. Ten
        // is long enough to catch something that has just been noticed, and the row is right here
        // for when it is not.
        { "recordlength", "Recording length",   { "5 seconds", "10 seconds", "15 seconds",
                                                  "20 seconds", "25 seconds", "30 seconds",
                                                  "35 seconds", "40 seconds", "45 seconds",
                                                  "50 seconds", "55 seconds", "1 minute" },      12 },
        // NOT HERE YET, deliberately. The top bar row is written and the field exists, and it is
        // held out of the table until the work that makes it DO something lands, because a row
        // that appears and changes nothing is worse than no row. It goes in with the in-game Quit
        // button, which it must ship with: with the bar hidden, that button is the way out.
        //   { "topbar", "Top bar", { "Show near the pointer", "Always hidden" }, 2 },
        // The one network connection this program makes, and it is off until somebody says
        // otherwise. The row exists so the answer can be changed at any time afterward, which
        // was the user's second condition when they asked for the updater.
        { "updates",      "Check for updates",  { "Off", "On" },                                 2 },
        // The autosave (phase 78): a saved moment into its own slot on an interval, at a safe
        // moment only, never over a slot the player picked.
        { "autosave",     "Autosave",           { "Off", "Every 5 minutes", "Every 10 minutes", "Every 15 minutes" }, 4 },
        { "saveonquit",   "Save on quit",       { "Off", "On" },                                 2 },
        { "resumeonstart", "Resume on start",   { "Off", "On" },                                 2 },
        // Where the settings and the lighting menus sit (2026-09-24, the user: "the panel option
        // to affix to the right side of the screen like planned before"): over the picture as a
        // modal, or as a panel against the window's right edge with the picture left showing,
        // so a lighting control can be changed while watching what it does.
        // THE FREE CAMERA (upgrades phase 79; the user, 2026-09-24: "build out the free aim
        // camera for right stick (being sure to exclude from the static camera scenes)"): the
        // right stick turns the camera wherever the game's own camera is free to turn, and its
        // C button bindings fall silent while it does (except with the ocarina out). The axes
        // row inverts either direction or both.
        { "camera",       "Free camera",        { "Off", "Right stick" },                        2 },
        { "cameraaxes",   "Camera axes",        { "As the stick moves", "Invert left and right", "Invert up and down", "Invert both" }, 4 },
        // The first person look and the aiming have their own (my note, 2026-09-24: "Separate
        // up and down ... for first person and/or the slingshot versus just normal free camera").
        // One per stick, since a person who wants the aim inverted usually wants it on one of
        // them (the user, 2026-09-24). The right stick is the one this program adds to aiming;
        // the left is the game's own, and its row works with the free camera off.
        { "aimaxes",      "Aiming, right stick", { "As the stick moves", "Invert left and right", "Invert up and down", "Invert both" }, 4 },
        { "aimaxesleft",  "Aiming, left stick",  { "As the stick moves", "Invert left and right", "Invert up and down", "Invert both" }, 4 },
        // How far the camera stands from Link, every player camera and not only the free one.
        // NEAR IS THE GAME'S OWN DISTANCE (the user, 2026-09-24), Original a fifth further out,
        // Medium three tenths and Far two fifths. The step below the game's own is gone: it put
        // the eye inside him in tight places. Four steps is the most the row can carry, since the
        // patch reads it out of two bits.
        // THE WORD "ORIGINAL" NOW MEANS THE GAME'S OWN DISTANCE, which it did not (2026-09-26,
        // my note: "one of the camera settings marks 'original' in menu but isn't actually what's
        // in the original game so this is a lie").
        //
        // It was { "Near", "Original", "Medium", "Far" }, and Near was the console's own distance
        // while the option called Original sat a fifth further out (patches/free_camera.c,
        // sDistanceScale = { 1.0, 1.2, 1.3, 1.4 }). That happened honestly: the row once had a
        // step BELOW the game's own, so "Original" sat in the middle and meant it. The nearer step
        // was removed for putting the eye inside him in tight places, and the labels were not
        // revisited, so a word that had been true became false without anybody touching it.
        //
        // The website had grown a paragraph explaining the discrepancy, which was the wrong answer:
        // a row should not need a warning to be read correctly.
        //
        // THE STORED VALUE IS AN INDEX AND THE SCALES ARE UNCHANGED, so nobody's camera moves. A
        // person who chose the second step keeps exactly the distance they had; it is now called
        // what it always was.
        { "cameradistance", "Camera distance",  { "Original", "A little further", "Further", "Furthest" }, 4 },
        // The game's own camera eases its distance OUT as he runs and back IN as he stops, which
        // reads as the camera pulling away (my note, 2026-09-24: "the slight pull away with
        // movement or as link gets closer can be an option to disable or enable"). On is the
        // game's own and the default. Off holds one distance. It governs the game's own camera
        // alone: while the right stick is holding the view the distance is held anyway ("drift
        // also doesn't need to be in the free mode. only in original cam mode").
        { "cameradrift",  "Camera drift",       { "Off", "On" },                                 2 },
        // NO LETTERBOX ROW, DELIBERATELY, AND THE FIELD BELOW STAYS AT THE GAME'S OWN. The bars
        // drawn by the program were smooth and covered the interface, because the game's bars are
        // a scissor on the three dimensional view alone and the interface is drawn afterward at
        // the full picture, so in the game the hearts sit on top of the black. The row went in,
        // the user chose Smooth, the choice was SAVED, and turning the default back to the game's
        // own did nothing for them: "bars still cover". A row whose only other choice is wrong is
        // a trap, so it is gone until the renderer can apply the inset to the perspective
        // projections alone. The patch and the document are still here for that work.
        { "menu",         "Menu placement",     { "Over the picture", "Panel on the right", "Panel on the left" }, 3 },
        { "volume",       "Volume",             { nullptr },                                     0 },
        // THE TEXTURE PACK ROW, and it is LAST for a reason worth stating. Every switch below
        // maps a row by its literal index, and this file already carries the scar of a row
        // inserted above a hard coded number (see the note on the debug list). Appending
        // cannot renumber anything. Its options are not in this table: they are whatever
        // packs are in the folder, so option_count and option_label answer for this row from
        // the scan. "Off" is here so the array is never empty if the scan has not run.
        { "texturepack",  "Texture pack",       { "Off" },                                       1 },
        // THE DETAIL A PACK IS READ AT (the user, 2026-10-07: "maybe have a 1080p version, a 2K
        // version and a 4K version ... when you enable it, then it adds an additional option
        // right underneath it for the resolution"). Shown only while a pack is chosen (rows_for
        // in ui_shell.cpp), and appended for the same reason the pack row was.
        //
        // NAMED BY HOW MUCH OF THE PACK, NOT BY A SCREEN SIZE. "4K" is what this pack was made
        // for, not what every pack is, so a label naming a resolution would be true of one pack
        // and wrong for the next. Each step is the pack's own width and height times that much,
        // applied as each texture loads: nothing in the person's folder is rewritten, and the
        // video memory a texture takes falls with the square, so Half holds a quarter as much.
        //
        // THREE STEPS, NOT FOUR (2026-10-07, the same day). A DDS pack is read at a lower detail
        // by starting its own mip chain lower (renderer patch 0026), and a chain only has halves:
        // "Three quarters" would have been Full again for a DDS pack, a step that does nothing.
        // Full, Half and Quarter mean the same thing for PNG and DDS alike.
        //
        // LIGHTEST FIRST (the user, 2026-10-08: "it needs to start with quarter first end on
        // full"): the row starts where it starts, on Quarter, and steps up toward Full. Reversed
        // in place the same day it became the default; it had shipped to nobody, so a file
        // written the day before simply reads the other way round.
        { "texturedetail", "Texture detail",    { "Quarter", "Half", "Full" },                   3 },
        // THE TEXT AND THE HEART ROWS (2026-10-08), appended for the reason the pack row was and
        // SHOWN elsewhere: the two text rows under Camera drift and the beep under Volume
        // (rows_for in ui_shell.cpp places them by key), so nothing renumbers. Each starts on the
        // game's own. See the fields in ui_settings.h.
        { "textspeed",    "Text speed",         { "Original", "Fast", "Faster", "Instant" },     4 },
        { "textskip",     "Skip text",          { "As the game", "A or B, always" },             2 },
        { "lowhealthbeep", "Low health beep",   { "On", "Off" },                                 2 },
        // SCREENSHOTS (the user, 2026-10-09: "need to be able to enable screencaps in a settings
        // option and ability to bind screencap to a controller or keyboard shortcut which just
        // captures one screenshot ... in the current window size with the users currently enabled
        // settings"; "the normal screencap is a normal setting while the compare is debug only").
        // The Screenshot binding (Controls) takes one PNG of the picture as presented while this is
        // On (main/screenshots.h). Shown under Menu placement. Comparison shots is the Debugging
        // menu's, shown only while Screenshots is On: the binding then takes a before and after
        // pair instead.
        { "screenshots",  "Screenshots",        { "Off", "On" },                                 2 },
        { "comparisonshots", "Comparison shots", { "Off", "On" },                                2 },
        // THE VIDEO RECORDER (the user, 2026-10-09: "record full gameplay to MP4 ... the ability to
        // control quality via a setting ... choose the output resolution ... a setting to enable or
        // disable sound"). Appended, and shown under Screenshots (rows_for in ui_shell.cpp); the
        // three below it only while it is On. The Record video and Pause video bindings
        // (Controls, F7 and F8) do nothing while it is Off. Video size is a height at the window's
        // shape, and may be larger than the window: the picture is drawn again at that size
        // from what the Resolution row renders (main/video.h).
        { "videorecording", "Video recording",  { "Off", "On" },                                 2 },
        { "videosize",    "Video size",         { "Match the window", "720p", "1080p", "1440p", "4K" }, 5 },
        { "videoquality", "Video quality",      { "Small file", "Standard", "High", "Best" },    4 },
        { "videosound",   "Video sound",        { "Recorded", "Off" },                           2 },
        // How the video is written (the user, 2026-10-09: "a compatibility mode versus a
        // reliability mode"), each choice described under the row (OPTION_NOTES below).
        { "videomode",    "Video mode",         { "Reliable", "Compatible" },                    2 },
        // PHOTO MODE (the user, 2026-10-09: "Does not need to be a debug setting ... called
        // something like photo mode Have a description for it"). Shown under Screenshots, its
        // description under it (OPTION_NOTES). main/photo.h has the whole of it.
        { "photomode",    "Photo mode",         { "Off", "On" },                                 2 },
        // WHAT A VIDEO RECORDS (the user, 2026-10-09: "allow recording gameplay only, or whether it
        // also allows recording the menu ... just recording the window as a whole, as opposed to
        // recording only the game itself"). Shown under Video recording, described under the row.
        { "videoshows",   "Video shows",        { "The game alone", "The whole window" },        2 },
        // WHAT A SCREENSHOT SHOWS (the user, 2026-10-09, pointing at Video shows: "We need the same
        // thing for photo mode"). Photo mode's pictures are the Screenshot control's, so this is
        // the Screenshots row's, under it while it is On, described under the row.
        { "screenshotshows", "Screenshots show", { "The game alone", "The whole window" },       2 },
        // THE HUD'S FADING (the user, 2026-10-09: "auto-dim/auto-hide hud when not in use (undims
        // when using z targeting, using the c buttons, or talking), dim at all times, or hide/dim
        // hud on mapped key or button. and if dimming, allow setting opacity and saturation", and,
        // asked how hiding should be shaped: "More things to bring it back, Which parts hide, Fade
        // speed, Delay before hiding, any other useful controls ... more modern-like"). Shown in
        // the HUD menu alone (rows_for in ui_shell.cpp), each only when it applies; game/hud.h.
        { "hudfade",      "HUD fades",          { "Never", "When idle", "Always", "By button" }, 4 },
        { "hudfadeto",    "Fades to",           { "Dim", "Hidden" },                             2 },
        { "hudopacity",   "Dim opacity",        { "20%", "35%", "50%", "65%", "80%" },           5 },
        { "hudcolor",     "Dim color",          { "Full", "75%", "50%", "25%", "Gray" },         5 },
        { "huddelay",     "Idle delay",         { "1 second", "2 seconds", "3 seconds", "5 seconds", "8 seconds", "12 seconds" }, 6 },
        // Z TARGETING, the first of what brings the HUD back. This place held one Comes back for
        // row whose steps each added more, until the user's word of 2026-10-09: "'comes back for'
        // needs to be all separate options as just checkbox toggles. since users need to be able
        // to pick and choose things that come back or not on action". The rest follow the sizes.
        { "hudwaketarget", "Z targeting",       { "Off", "On" },                                 2 },
        { "hudspeed",     "Fade speed",         { "Instant", "Quick", "Smooth", "Slow" },        4 },
        { "hudfadehearts",  "Hearts",           { "Fade", "Stay shown" },                        2 },
        { "hudfademagic",   "Magic",            { "Fade", "Stay shown" },                        2 },
        { "hudfaderupees",  "Rupees and keys",  { "Fade", "Stay shown" },                        2 },
        { "hudfadebuttons", "Buttons",          { "Fade", "Stay shown" },                        2 },
        { "hudfademap",     "Map",              { "Fade", "Stay shown" },                        2 },
        // THE MICROPHONE IN A VIDEO (the user, 2026-10-09: "did you not add the voice recording and
        // device detection/selection for selecting the mic to use?"). Under Video sound while Video
        // recording is On; the device and the level only while Microphone is On. The device row's
        // options are the system's capture devices (main/microphone.h), so this table holds only
        // the first, as the texture pack row's does.
        { "microphone",   "Microphone",         { "Off", "On" },                                 2 },
        { "micdevice",    "Microphone device",  { "System default" },                            1 },
        { "miclevel",     "Microphone level",   { "50%", "75%", "100%", "150%", "200%", "300%" }, 6 },
        // THE HUD'S SIZE (the user, 2026-10-09: "control hud sizeing per item. like scale hearts and
        // scale c button both down and make map independently larger", and "ability to scale all at
        // once or independent"). The HUD menu's, after the fading rows.
        { "hudsizes",     "Sizes",              { "One for all", "Each its own" },               2 },
        { "hudsize",      "HUD size",           { "50%", "60%", "70%", "80%", "90%", "100%", "115%", "130%", "150%", "175%", "200%" }, 11 },
        { "hudsizehearts",  "Hearts size",      { "50%", "60%", "70%", "80%", "90%", "100%", "115%", "130%", "150%", "175%", "200%" }, 11 },
        { "hudsizemagic",   "Magic size",       { "50%", "60%", "70%", "80%", "90%", "100%", "115%", "130%", "150%", "175%", "200%" }, 11 },
        { "hudsizerupees",  "Rupees and keys size", { "50%", "60%", "70%", "80%", "90%", "100%", "115%", "130%", "150%", "175%", "200%" }, 11 },
        { "hudsizebuttons", "Buttons size",     { "50%", "60%", "70%", "80%", "90%", "100%", "115%", "130%", "150%", "175%", "200%" }, 11 },
        { "hudsizemap",     "Map size",         { "50%", "60%", "70%", "80%", "90%", "100%", "115%", "130%", "150%", "175%", "200%" }, 11 },
        // WHAT ELSE BRINGS THE HUD BACK, one box each, after Z targeting (row 43) in the HUD menu,
        // under its Comes back for divider (ui_shell.cpp draws these rows as boxes to tick).
        { "hudwakec",      "C buttons",                      { "Off", "On" },                    2 },
        { "hudwaketalk",   "Talking",                        { "Off", "On" },                    2 },
        { "hudwakehealth", "Health or magic changing",       { "Off", "On" },                    2 },
        { "hudwakelow",    "Hearts at low health",           { "Off", "On" },                    2 },
        { "hudwakeitems",  "Rupees, keys or items changing", { "Off", "On" },                    2 },
        { "hudwakepause",  "The pause menu",                 { "Off", "On" },                    2 },
        { "hudwakeany",    "Any button",                     { "Off", "On" },                    2 },
        // WHERE THE HUD SITS, CUSTOM (the user, 2026-10-09: "the user can set a percentage away from
        // the edge of the window so when the window does scale down and it starts to shrink, the
        // user can choose its distance from where it actually sits, from all edges of the screen").
        // Each corner of the HUD keeps these distances from the two edges it sits against, as a
        // share of the frame, so they shrink with it; shown while HUD sits is Custom.
        { "hudleft",       "From the left",   { "0%", "1%", "2%", "3%", "4%", "5%", "6%", "8%", "10%", "15%", "20%", "25%" }, 12 },
        { "hudright",      "From the right",  { "0%", "1%", "2%", "3%", "4%", "5%", "6%", "8%", "10%", "15%", "20%", "25%" }, 12 },
        { "hudtop",        "From the top",    { "0%", "1%", "2%", "3%", "4%", "5%", "6%", "8%", "10%", "15%", "20%", "25%" }, 12 },
        { "hudbottom",     "From the bottom", { "0%", "1%", "2%", "3%", "4%", "5%", "6%", "8%", "10%", "15%", "20%", "25%" }, 12 },
    };

    // WHAT A CHOICE MEANS, shown under its row (the user, 2026-10-09: "it should have a description
    // underneath them when they're toggled that says what they do ... list the downsides and the
    // upsides"). By key, one line per option in the row's order; a row not here shows nothing.
    struct OptionNotes {
        const char* key;
        const char* notes[4];
    };

    constexpr OptionNotes OPTION_NOTES[] = {
        { "videomode", {
            "Upside: saved as it records, so a crash or a forced close keeps the video up to its last moments, and a "
            "finished video is an ordinary MP4. Downside: stopping takes a few seconds longer and needs room for a "
            "second copy while it finishes, and a video cut short by a crash may not open in some editors, though "
            "players and video sites take it.",
            "Upside: an ordinary MP4 from the first second, which every player and editor opens, and stopping is "
            "instant. Downside: if the game crashes or is closed before you stop, the whole video is lost.",
        } },
        { "videoshows", {
            "Only the game's picture: none of this program's menus, toasts or the corner plate are in the video, "
            "whatever is open while it records.",
            "Everything in the window: the game with every menu, toast and plate over it, so using photo mode or "
            "the settings can be recorded. Video size still sets the video's size, the window scaled to it.",
        } },
        { "screenshotshows", {
            "Only the game's picture, at the window's size: none of this program's menus, toasts or plate, and "
            "the whole picture even while the settings panel is open, so in photo mode a shot can be taken at "
            "each setting with the menu still up.",
            "Everything in the window, exactly as you see it: the game with every menu, toast and plate over it.",
        } },
        { "hud", {
            "The console's own place: the HUD stays within the middle 4:3 of a wider picture.",
            "The HUD spreads toward the edges, but no wider than a 16:9 screen, so on an ultrawide one "
            "it stays within easy sight.",
            "The HUD sits at the very edges of the picture, as wide as it is, at the console's own "
            "distances from them.",
            "The HUD keeps the distances below from each edge, as a share of the picture, so they "
            "shrink and grow with the window, and it never leaves the picture.",
        } },
        { "hudfade", {
            "The HUD is always the game's own.",
            "The HUD fades once nothing has needed it for the idle delay, and comes back for what is ticked under "
            "Comes back for. The HUD button (Controls) brings it back at once.",
            "The HUD stays faded. The HUD button (Controls) shows it, and again fades it.",
            "The HUD button (Controls) fades it and brings it back.",
        } },
        { "microphone", {
            "No microphone: the video has the game's sound alone, if Video sound records it.",
            "Your voice from the microphone below, mixed with the game's sound. It is listened to only while a "
            "video records, and goes only into that video.",
        } },
        { "photomode", {
            "Turn this on to freeze the game with the Photo mode control (F10, or the key or button you bind) and "
            "frame a picture.",
            "In play, the Photo mode control (F10, or the key or button you bind) freezes the game where it stands "
            "and pauses its sound; press it again to carry on. While frozen, every setting you change shows on the "
            "same frame, and the camera moves with the Photo mode section of the controls screen: by default the "
            "stick moves it, the right stick turns it, the shoulders raise and lower it, the D-pad brings it closer "
            "or further, B puts it back, and H or the top face button hides and shows the game's HUD.",
        } },
    };

    // The share of the pack's own size each Texture detail step reads it at, in percent.
    constexpr int TEXTURE_DETAIL_PERCENT[] = { 25, 50, 100 };

    constexpr int ROW_COUNT = static_cast<int>(sizeof(ROWS) / sizeof(ROWS[0]));

    // WHICH ROW THE TEXTURE PACKS BELONG TO, found by KEY rather than written as a number.
    // Every other row in the two switches is a literal index, which is how this file once
    // sent a first run question to the wrong row, and the answer there was the same as the
    // answer here: name the thing. Looked up once.
    int texture_pack_row() {
        // Qualified because this sits in the file's own unnamed namespace, above the point where
        // oot::ui's own declarations come into scope unqualified.
        static const int row = oot::ui::row_index("texturepack");
        return row;
    }

    int clamp_int(int value, int low, int high) {
        return std::max(low, std::min(high, value));
    }

    // Push the graphics settings at the runtime, which owns the renderer's configuration and
    // hands it to RT64. Going through the runtime rather than at RT64 directly is what keeps the
    // change surviving a device reset.
    bool g_window_mode_held = false;

    // Set from the game thread once a frame, read here. See set_prerendered_room in ui_settings.h
    // for what it is and why the user's rows are left alone.
    std::atomic<bool> g_prerendered_room{ false };

    // GUARDS THE LIVE SETTINGS WHILE THEY ARE BEING TURNED INTO A RENDERER CONFIGURATION.
    //
    // Until 2026-09-20 every caller of push_to_runtime was on the main thread, which also owns
    // every write to g_live, so the rows could be read without a lock. The prerendered room
    // override broke that: it is decided on the GAME thread, once a frame, and pushes from there.
    // A push that read the rows while the user was moving one in the settings document would be
    // a plain data race. The lock is held only across the read of the rows and the write of the
    // settings, never across anything that waits.
    std::mutex g_live_mutex;

    // THE ORIGINAL LOOK, FOR THE COMPARISON SHOTS (2026-10-09, main/screenshots.h): while set, the
    // push gives the renderer the console's picture whatever the rows say, the way the prerendered
    // room override does; the rows and the file are never touched, so clearing it puts the
    // person's own picture straight back. The console's picture in full (the user, the same day:
    // "the comparison to be original N64 style and quality versus basically maxed out settings ...
    // disabling texture pack and pretty much reverting all settings to be what they exactly would
    // look like on an N64"): 4:3, the original resolution, no downsampling, no antialiasing beyond
    // the console's own, the interface at its own ratio, the game's own draw distance, and no
    // texture pack. The rows that change how the game MOVES (the camera's distance and drift, the
    // free camera) cannot change a held frame and are left alone.
    std::atomic<bool> g_original_look{ false };

    // THE ROWS AS THE RUNTIME'S CONFIGURATION, and the picture's fixed size and ratio beside it.
    // One mapping for both of its readers: the push below, and the Downsampling row's labels,
    // which plan each of its options from exactly what that option would push (2026-10-07).
    void to_runtime(const oot::ui::Settings& s, ultramodern::renderer::GraphicsConfig& config,
                    oot::renderer::PictureValues& picture) {
        using namespace ultramodern::renderer;

        // The launcher's window stays a window whatever the setting says, until Play.
        config.wm_option = (s.window_mode == 1 && !g_window_mode_held) ? WindowMode::Fullscreen
                                                                       : WindowMode::Windowed;

        switch (s.resolution) {
            case 1:  config.res_option = Resolution::Original2x; break;
            case 2:  config.res_option = Resolution::Auto;       break;
            // A fixed height is the runtime's Original with the height beside it, carried to the
            // render context on its own (game/render.h, set_picture), since the runtime's own
            // option knows nothing of it.
            case 3: case 4: case 5: case 6: config.res_option = Resolution::Original; break;
            default: config.res_option = Resolution::Original;   break;
        }
        static constexpr int FIXED_HEIGHTS[] = { 720, 1080, 1440, 2160 };
        static constexpr double FIXED_ASPECTS[] = { 16.0 / 9.0, 16.0 / 10.0, 21.0 / 9.0, 32.0 / 9.0 };
        picture = oot::renderer::PictureValues{};
        picture.fixed_height = (s.resolution >= 3) ? FIXED_HEIGHTS[clamp_int(s.resolution - 3, 0, 3)] : 0;
        picture.aspect = (s.aspect >= 2) ? FIXED_ASPECTS[clamp_int(s.aspect - 2, 0, 3)] : 0.0;
        // The picture is drawn at this multiple of the size shown and shrunk to it, at every
        // resolution, Match the window included since 2026-10-07; the render context's plan
        // steps it down when the card cannot hold it (game/render.h, plan_resolution).
        config.ds_option = (s.downsampling == 2) ? 4 : (s.downsampling == 1) ? 2 : 1;

        // Expand is the one that actually widens the view rather than stretching it. What it
        // cannot do on its own is tell the game's own 3D code that the view is wider, which is
        // what transform tagging is for; until then Expand shows more at the edges and some
        // effects that assume 4:3 will show their seams.
        // A prerendered room overrides both of these to the console's own ratio. The rows keep
        // their values and the file is not written; only what reaches the runtime changes.
        const bool prerendered = g_prerendered_room.load(std::memory_order_relaxed);

        // A fixed ratio rides on Expand as far as the runtime knows; the render context turns it
        // into the renderer's manual ratio with the target beside it. A prerendered room forces
        // the console's own whatever the row says.
        config.ar_option = (s.aspect >= 1 && !prerendered) ? AspectRatio::Expand : AspectRatio::Original;

        if (prerendered) {
            config.hr_option = HUDRatioMode::Original;
        }
        else {
            switch (s.hud_ratio) {
                case 1:  config.hr_option = HUDRatioMode::Clamp16x9; break;
                case 2:  config.hr_option = HUDRatioMode::Full;      break;
                // Custom measures its distances against the whole frame (game/hud.h).
                case 3:  config.hr_option = HUDRatioMode::Full;      break;
                default: config.hr_option = HUDRatioMode::Original;  break;
            }
        }

        switch (s.antialiasing) {
            case 1:  config.msaa_option = Antialiasing::MSAA2X; break;
            case 2:  config.msaa_option = Antialiasing::MSAA4X; break;
            case 3:  config.msaa_option = Antialiasing::MSAA8X; break;
            default: config.msaa_option = Antialiasing::None;   break;
        }

        // Display asks the renderer to present at the display's rate, interpolating whatever the
        // game has tagged between its own frames; a fixed rate asks for that many through the
        // runtime's manual rate. The render context already maps both to RT64.
        if (s.frame_rate >= 2) {
            config.rr_option = RefreshRate::Manual;
            config.rr_manual_value = FIXED_RATES[clamp_int(s.frame_rate - 2, 0, FIXED_RATE_COUNT - 1)];
        }
        else {
            config.rr_option = (s.frame_rate == 1) ? RefreshRate::Display : RefreshRate::Original;
        }
    }

    void push_to_runtime() {
        using namespace ultramodern::renderer;

        GraphicsConfig config = get_graphics_config();
        oot::renderer::PictureValues picture;

        // Held across every read of the rows below, because this now runs on the game thread too.
        std::lock_guard<std::mutex> lock(g_live_mutex);
        if (g_original_look.load(std::memory_order_relaxed)) {
            oot::ui::Settings original = g_live;
            original.aspect = 0;          // Original 4:3
            original.resolution = 0;      // Original
            original.downsampling = 0;    // Off
            original.antialiasing = 0;    // Off: the console's own edges only
            original.hud_ratio = 0;       // Original
            to_runtime(original, config, picture);
        }
        else {
            to_runtime(g_live, config, picture);
        }

        // The picture's fixed size and ratio go first: the runtime's push reads them when it
        // applies the configuration.
        oot::renderer::set_picture(picture);
        set_graphics_config(config);

        g_gain.store(static_cast<float>(g_live.volume_percent) / 100.0f, std::memory_order_relaxed);

        // The chosen texture pack. Applied through the renderer, which ignores a repeat of the
        // same path, so calling it on every settings write costs nothing until the row moves.
        // No pack for the comparison's original look (above).
        const int pack = g_original_look.load(std::memory_order_relaxed) ? 0 : g_live.texture_pack;
        oot::renderer::apply_texture_pack(oot::packs::path_for(pack),
                                          TEXTURE_DETAIL_PERCENT[clamp_int(g_live.texture_detail, 0,
                                              static_cast<int>(std::size(TEXTURE_DETAIL_PERCENT)) - 1)]);
    }

    // The file grammar (`key = value`, garbage costs one value and never the program) lived here
    // from phase 24 until phase 47, when the bindings file needed the same reading and it moved
    // to main/keyvalue.h so that one parser serves both files.
    using oot::keyvalue::parse_line;
    using oot::keyvalue::as_int;

} // namespace

namespace oot::ui {

    bool Settings::operator==(const Settings& other) const {
        return window_mode == other.window_mode &&
               resolution == other.resolution &&
               aspect == other.aspect &&
               downsampling == other.downsampling &&
               hud_ratio == other.hud_ratio &&
               antialiasing == other.antialiasing &&
               render_distance == other.render_distance &&
               frame_rate == other.frame_rate &&
               counter == other.counter &&
               recorder == other.recorder &&
               record_length == other.record_length &&
               updates == other.updates &&
               autosave == other.autosave &&
               save_on_quit == other.save_on_quit &&
               resume_on_start == other.resume_on_start &&
               free_camera == other.free_camera &&
               camera_invert == other.camera_invert &&
               aim_invert == other.aim_invert &&
               aim_invert_left == other.aim_invert_left &&
               camera_distance == other.camera_distance &&
               camera_drift == other.camera_drift &&
               letterbox == other.letterbox &&
               top_bar == other.top_bar &&
               menu_side == other.menu_side &&
               volume_percent == other.volume_percent &&
               texture_pack == other.texture_pack &&
               texture_detail == other.texture_detail &&
               text_speed == other.text_speed &&
               text_skip == other.text_skip &&
               low_health_beep == other.low_health_beep &&
               screenshots == other.screenshots &&
               comparison_shots == other.comparison_shots &&
               video_recording == other.video_recording &&
               video_size == other.video_size &&
               video_quality == other.video_quality &&
               video_sound == other.video_sound &&
               video_mode == other.video_mode &&
               photo_mode == other.photo_mode &&
               video_source == other.video_source &&
               screenshot_source == other.screenshot_source &&
               hud_fade == other.hud_fade && hud_fade_to == other.hud_fade_to &&
               hud_opacity == other.hud_opacity && hud_color == other.hud_color &&
               hud_delay == other.hud_delay && hud_wake_target == other.hud_wake_target && hud_speed == other.hud_speed &&
               hud_fade_hearts == other.hud_fade_hearts && hud_fade_magic == other.hud_fade_magic &&
               hud_fade_rupees == other.hud_fade_rupees && hud_fade_buttons == other.hud_fade_buttons &&
               hud_fade_map == other.hud_fade_map &&
               microphone == other.microphone && mic_device == other.mic_device && mic_level == other.mic_level &&
               hud_sizes == other.hud_sizes && hud_size == other.hud_size &&
               hud_size_hearts == other.hud_size_hearts && hud_size_magic == other.hud_size_magic &&
               hud_size_rupees == other.hud_size_rupees && hud_size_buttons == other.hud_size_buttons &&
               hud_size_map == other.hud_size_map &&
               hud_wake_c == other.hud_wake_c && hud_wake_talk == other.hud_wake_talk &&
               hud_wake_health == other.hud_wake_health && hud_wake_low == other.hud_wake_low &&
               hud_wake_items == other.hud_wake_items && hud_wake_pause == other.hud_wake_pause &&
               hud_wake_any == other.hud_wake_any &&
               hud_left == other.hud_left && hud_right == other.hud_right &&
               hud_top == other.hud_top && hud_bottom == other.hud_bottom;
    }

    int row_count() {
        return ROW_COUNT;
    }

    const char* row_label(int row) {
        return (row >= 0 && row < ROW_COUNT) ? ROWS[row].label : "";
    }

    const char* row_key(int row) {
        return (row >= 0 && row < ROW_COUNT) ? ROWS[row].key : "";
    }

    int option_count(int row) {
        // The texture pack row's options are the packs in the person's folder, not a table.
        if (row == texture_pack_row()) {
            return oot::packs::option_count();
        }
        // The microphone device row's are the system's capture devices.
        static const int mic_device_row = row_index("micdevice");
        if (row == mic_device_row) {
            return oot::microphone::option_count();
        }
        return (row >= 0 && row < ROW_COUNT) ? ROWS[row].count : 0;
    }

    int row_index(const char* key) {
        for (int row = 0; row < ROW_COUNT; ++row) {
            if (std::strcmp(ROWS[row].key, key) == 0) {
                return row;
            }
        }
        return -1;
    }

    const char* option_note(int row, int value) {
        if (row < 0 || row >= ROW_COUNT || ROWS[row].count == 0) {
            return nullptr;
        }
        const int choice = clamp_int(value, 0, ROWS[row].count - 1);
        for (const OptionNotes& entry : OPTION_NOTES) {
            if (std::strcmp(entry.key, ROWS[row].key) == 0) {
                return (choice < 4) ? entry.notes[choice] : nullptr;
            }
        }
        return nullptr;
    }

    const char* option_label(int row, int value) {
        if (row < 0 || row >= ROW_COUNT) {
            return "";
        }
        if (row == texture_pack_row()) {
            return oot::packs::label_for(value);
        }
        static const int mic_device_row = row_index("micdevice");
        if (row == mic_device_row) {
            return oot::microphone::label_for(value);
        }
        if (ROWS[row].count == 0) {
            return nullptr;   // a number, not a choice; the caller formats it
        }
        const int choice = clamp_int(value, 0, ROWS[row].count - 1);
        static const int downsampling_row = row_index("downsampling");
        if (row == downsampling_row) {
            // EACH OPTION SAYS WHAT IT WILL ACTUALLY DRAW (the user's option 1, 2026-10-07):
            // "2x (4268x2400)", or "4x, only 2x fits (3840x2160)" when the card cannot hold
            // what was asked and the renderer will step down. Planned by the renderer's own
            // plan from the live rows with this option in place, so the label and the picture
            // can never disagree. One buffer per option and per thread, since the rows document
            // reads every option of a row before drawing it.
            thread_local std::string labels[3];
            Settings s;
            {
                std::lock_guard<std::mutex> lock(g_live_mutex);
                s = g_live;
            }
            s.downsampling = choice;
            ultramodern::renderer::GraphicsConfig config = ultramodern::renderer::get_graphics_config();
            oot::renderer::PictureValues picture;
            to_runtime(s, config, picture);
            const oot::renderer::ResolutionPlan plan = oot::renderer::plan_resolution(config, picture);
            std::string& label = labels[clamp_int(choice, 0, 2)];
            label = ROWS[row].options[choice];
            if ((plan.width > 0) && (plan.height > 0)) {
                if (plan.used == plan.asked) {
                    label += " (";
                }
                else if (plan.used > 1) {
                    label += ", only " + std::to_string(plan.used) + "x fits (";
                }
                else {
                    label += ", does not fit (";
                }
                label += std::to_string(plan.width) + "x" + std::to_string(plan.height) + ")";
            }
            return label.c_str();
        }
        return ROWS[row].options[choice];
    }

    int get_row(const Settings& s, int row) {
        switch (row) {
            case 0:  return s.window_mode;
            case 1:  return s.resolution;
            case 2:  return s.downsampling;
            case 3:  return s.aspect;
            case 4:  return s.hud_ratio;
            case 5:  return s.antialiasing;
            case 6:  return s.render_distance;
            case 7:  return s.frame_rate;
            case 8:  return s.counter;
            case 9:  return s.recorder;
            case 10: return s.record_length;
            case 11: return s.updates;
            case 12: return s.autosave;
            case 13: return s.save_on_quit;
            case 14: return s.resume_on_start;
            case 15: return s.free_camera;
            case 16: return s.camera_invert;
            case 17: return s.aim_invert;
            case 18: return s.aim_invert_left;
            case 19: return s.camera_distance;
            case 20: return s.camera_drift;
            case 21: return s.menu_side;
            case 22: return s.volume_percent;
            case 23: return s.texture_pack;
            case 24: return s.texture_detail;
            case 25: return s.text_speed;
            case 26: return s.text_skip;
            case 27: return s.low_health_beep;
            case 28: return s.screenshots;
            case 29: return s.comparison_shots;
            case 30: return s.video_recording;
            case 31: return s.video_size;
            case 32: return s.video_quality;
            case 33: return s.video_sound;
            case 34: return s.video_mode;
            case 35: return s.photo_mode;
            case 36: return s.video_source;
            case 37: return s.screenshot_source;
            case 38: return s.hud_fade;
            case 39: return s.hud_fade_to;
            case 40: return s.hud_opacity;
            case 41: return s.hud_color;
            case 42: return s.hud_delay;
            case 43: return s.hud_wake_target;
            case 44: return s.hud_speed;
            case 45: return s.hud_fade_hearts;
            case 46: return s.hud_fade_magic;
            case 47: return s.hud_fade_rupees;
            case 48: return s.hud_fade_buttons;
            case 49: return s.hud_fade_map;
            case 50: return s.microphone;
            case 51: return s.mic_device;
            case 52: return s.mic_level;
            case 53: return s.hud_sizes;
            case 54: return s.hud_size;
            case 55: return s.hud_size_hearts;
            case 56: return s.hud_size_magic;
            case 57: return s.hud_size_rupees;
            case 58: return s.hud_size_buttons;
            case 59: return s.hud_size_map;
            case 60: return s.hud_wake_c;
            case 61: return s.hud_wake_talk;
            case 62: return s.hud_wake_health;
            case 63: return s.hud_wake_low;
            case 64: return s.hud_wake_items;
            case 65: return s.hud_wake_pause;
            case 66: return s.hud_wake_any;
            case 67: return s.hud_left;
            case 68: return s.hud_right;
            case 69: return s.hud_top;
            case 70: return s.hud_bottom;
            default: return 0;
        }
    }

    // The field a row names, given a value as it stands: no clamping, so clamp itself can use it.
    void set_row_unclamped(Settings& s, int row, int value) {
        switch (row) {
            case 0: s.window_mode = value; break;
            case 1: s.resolution = value; break;
            case 2: s.downsampling = value; break;
            case 3: s.aspect = value; break;
            case 4: s.hud_ratio = value; break;
            case 5: s.antialiasing = value; break;
            case 6: s.render_distance = value; break;
            case 7: s.frame_rate = value; break;
            case 8: s.counter = value; break;
            case 9: s.recorder = value; break;
            case 10: s.record_length = value; break;
            case 11: s.updates = value; break;
            case 12: s.autosave = value; break;
            case 13: s.save_on_quit = value; break;
            case 14: s.resume_on_start = value; break;
            case 15: s.free_camera = value; break;
            case 16: s.camera_invert = value; break;
            case 17: s.aim_invert = value; break;
            case 18: s.aim_invert_left = value; break;
            case 19: s.camera_distance = value; break;
            case 20: s.camera_drift = value; break;
            case 21: s.menu_side = value; break;
            case 22: s.volume_percent = value; break;
            case 23: s.texture_pack = value; break;
            case 24: s.texture_detail = value; break;
            case 25: s.text_speed = value; break;
            case 26: s.text_skip = value; break;
            case 27: s.low_health_beep = value; break;
            case 28: s.screenshots = value; break;
            case 29: s.comparison_shots = value; break;
            case 30: s.video_recording = value; break;
            case 31: s.video_size = value; break;
            case 32: s.video_quality = value; break;
            case 33: s.video_sound = value; break;
            case 34: s.video_mode = value; break;
            case 35: s.photo_mode = value; break;
            case 36: s.video_source = value; break;
            case 37: s.screenshot_source = value; break;
            case 38: s.hud_fade = value; break;
            case 39: s.hud_fade_to = value; break;
            case 40: s.hud_opacity = value; break;
            case 41: s.hud_color = value; break;
            case 42: s.hud_delay = value; break;
            case 43: s.hud_wake_target = value; break;
            case 44: s.hud_speed = value; break;
            case 45: s.hud_fade_hearts = value; break;
            case 46: s.hud_fade_magic = value; break;
            case 47: s.hud_fade_rupees = value; break;
            case 48: s.hud_fade_buttons = value; break;
            case 49: s.hud_fade_map = value; break;
            case 50: s.microphone = value; break;
            case 51: s.mic_device = value; break;
            case 52: s.mic_level = value; break;
            case 53: s.hud_sizes = value; break;
            case 54: s.hud_size = value; break;
            case 55: s.hud_size_hearts = value; break;
            case 56: s.hud_size_magic = value; break;
            case 57: s.hud_size_rupees = value; break;
            case 58: s.hud_size_buttons = value; break;
            case 59: s.hud_size_map = value; break;
            case 60: s.hud_wake_c = value; break;
            case 61: s.hud_wake_talk = value; break;
            case 62: s.hud_wake_health = value; break;
            case 63: s.hud_wake_low = value; break;
            case 64: s.hud_wake_items = value; break;
            case 65: s.hud_wake_pause = value; break;
            case 66: s.hud_wake_any = value; break;
            case 67: s.hud_left = value; break;
            case 68: s.hud_right = value; break;
            case 69: s.hud_top = value; break;
            case 70: s.hud_bottom = value; break;
            default: break;
        }
    }

    void set_row(Settings& s, int row, int value) {
        set_row_unclamped(s, row, value);
        clamp(s);
    }

    void clamp(Settings& s) {
        s.window_mode   = clamp_int(s.window_mode, 0, option_count(0) - 1);
        s.resolution    = clamp_int(s.resolution, 0, option_count(1) - 1);
        s.downsampling  = clamp_int(s.downsampling, 0, option_count(2) - 1);
        s.aspect        = clamp_int(s.aspect, 0, option_count(3) - 1);
        s.hud_ratio     = clamp_int(s.hud_ratio, 0, option_count(4) - 1);
        s.antialiasing  = clamp_int(s.antialiasing, 0, option_count(5) - 1);
        s.render_distance = clamp_int(s.render_distance, 0, option_count(6) - 1);
        s.frame_rate    = clamp_int(s.frame_rate, 0, option_count(7) - 1);
        s.counter       = clamp_int(s.counter, 0, option_count(8) - 1);
        s.recorder      = clamp_int(s.recorder, 0, option_count(9) - 1);
        s.record_length = clamp_int(s.record_length, 0, option_count(10) - 1);
        s.top_bar       = clamp_int(s.top_bar, 0, 1);
        s.updates = clamp_int(s.updates, 0, option_count(11) - 1);
        s.autosave      = clamp_int(s.autosave, 0, option_count(12) - 1);
        s.save_on_quit  = clamp_int(s.save_on_quit, 0, option_count(13) - 1);
        s.resume_on_start = clamp_int(s.resume_on_start, 0, option_count(14) - 1);
        s.free_camera   = clamp_int(s.free_camera, 0, option_count(15) - 1);
        s.camera_invert = clamp_int(s.camera_invert, 0, option_count(16) - 1);
        s.aim_invert    = clamp_int(s.aim_invert, 0, option_count(17) - 1);
        s.aim_invert_left = clamp_int(s.aim_invert_left, 0, option_count(18) - 1);
        s.camera_distance = clamp_int(s.camera_distance, 0, option_count(19) - 1);
        s.camera_drift  = clamp_int(s.camera_drift, 0, option_count(20) - 1);
        s.letterbox     = 0;   // no row: the game's own scissor, always
        s.menu_side     = clamp_int(s.menu_side, 0, option_count(21) - 1);
        s.volume_percent = clamp_int(s.volume_percent, 0, 100);
        // Clamped against what the SCAN found, so a settings file naming the fourth pack on a
        // machine with two falls back to Off rather than to a row that cannot be drawn. The
        // scan therefore has to have run before settings are loaded; boot does that.
        s.texture_pack  = clamp_int(s.texture_pack, 0, oot::packs::option_count() - 1);
        s.texture_detail = clamp_int(s.texture_detail, 0, option_count(24) - 1);
        // The rows appended since 2026-10-08, which the file can hold out of range like any other.
        for (int row = 25; row <= 50; ++row) {
            set_row_unclamped(s, row, clamp_int(get_row(s, row), 0, option_count(row) - 1));
        }
        // The microphone's device is kept by its place in the list whatever the list holds now: a
        // device unplugged opens as the system's default and returns when it does.
        s.mic_device = clamp_int(s.mic_device, 0, 64);
        s.mic_level = clamp_int(s.mic_level, 0, option_count(52) - 1);
        // Every row from the sizes on is a plain choice from the table. A row added later whose
        // options are not the table's (as the microphone's device is not) is clamped on its own.
        for (int row = 53; row < ROW_COUNT; ++row) {
            set_row_unclamped(s, row, clamp_int(get_row(s, row), 0, option_count(row) - 1));
        }
    }

    bool update_choice_made() {
        return g_update_choice_made;
    }

    void mark_update_choice_made() {
        g_update_choice_made = true;
    }

    bool rom_choice_made() {
        return g_rom_choice_made;
    }

    void mark_rom_choice_made() {
        g_rom_choice_made = true;
    }

    const Settings& settings() {
        return g_live;
    }

    float volume_gain() {
        return g_gain.load(std::memory_order_relaxed);
    }

    unsigned int free_camera_mode() {
        const Settings& s = g_live;
        unsigned int bits = (s.free_camera != 0) ? 1u : 0u;
        if (s.camera_invert == 1 || s.camera_invert == 3) {
            bits |= 2u;
        }
        if (s.camera_invert == 2 || s.camera_invert == 3) {
            bits |= 4u;
        }
        // The aiming's own inversions, bits 8 and 16 (the first person look, the bow, the
        // slingshot, the hookshot, the boomerang).
        if (s.aim_invert == 1 || s.aim_invert == 3) {
            bits |= 8u;
        }
        if (s.aim_invert == 2 || s.aim_invert == 3) {
            bits |= 16u;
        }
        // Bits 5 and 6: the Camera distance row (0 the game's own, 1 a fifth further, 2 two
        // fifths further; the patch holds the three numbers).
        bits |= static_cast<unsigned int>(clamp_int(s.camera_distance, 0, 3)) << 5;
        // Bit 9: the Camera drift row, set when the row is OFF, so that an unset bit is the
        // game's own behavior like every other bit here.
        if (s.camera_drift == 0) {
            bits |= 512u;
        }
        // Bits 7 and 8: the LEFT stick's aiming inversions, the game's own stick.
        if (s.aim_invert_left == 1 || s.aim_invert_left == 3) {
            bits |= 128u;
        }
        if (s.aim_invert_left == 2 || s.aim_invert_left == 3) {
            bits |= 256u;
        }
        return bits;
    }

    unsigned int message_assist_mode() {
        const Settings& s = g_live;
        unsigned int bits = static_cast<unsigned int>(clamp_int(s.text_speed, 0, 3));
        if (s.text_skip != 0) {
            bits |= 4u;
        }
        return bits;
    }

    bool low_health_beep_enabled() {
        return g_live.low_health_beep == 0;
    }

    bool screenshots_enabled() {
        return g_live.screenshots != 0;
    }

    bool comparison_shots_enabled() {
        return (g_live.screenshots != 0) && (g_live.comparison_shots != 0);
    }

    bool video_recording_enabled() {
        return g_live.video_recording != 0;
    }

    bool photo_mode_enabled() {
        return g_live.photo_mode != 0;
    }

    bool screenshots_game_alone() {
        return g_live.screenshot_source == 0;
    }

    float mic_gain() {
        constexpr float GAIN[] = { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f };
        return GAIN[clamp_int(g_live.mic_level, 0, 5)];
    }

    void set_original_look(bool on) {
        g_original_look.store(on, std::memory_order_relaxed);
        push_to_runtime();
    }

    unsigned int render_distance_q16() {
        // The multiplier each option means, in the row's own order: Original, 2x, 3x, 4x, 6x, 8x.
        // It lives here rather than in the patch so the labels and the numbers cannot drift
        // apart, and it is a table rather than arithmetic because the options are not a
        // sequence: they skip 5x and 7x, which buy nothing a person can see.
        static constexpr unsigned int SCALES[] = { 1, 2, 3, 4, 6, 8 };
        static constexpr int COUNT = static_cast<int>(sizeof(SCALES) / sizeof(SCALES[0]));

        std::lock_guard<std::mutex> lock(g_live_mutex);
        // The game's own distance for the comparison's original look (g_original_look).
        const int distance = g_original_look.load(std::memory_order_relaxed) ? 0 : g_live.render_distance;
        return SCALES[clamp_int(distance, 0, COUNT - 1)] * 65536u;
    }

    bool prerendered_room() {
        return g_prerendered_room.load(std::memory_order_relaxed);
    }

    void set_prerendered_room(bool on, int cam_setting) {
        // THE ANSWER HAS TO HOLD BEFORE IT IS ACTED ON, and this is not caution for its own sake.
        // Changing the view's ratio makes the renderer rebuild, which costs a visible moment, and
        // the first build of this flickered: leaving Link's house produced "entered", "left",
        // "entered", "left" within a few frames, because a scene load passes through states where
        // the room's shape and the camera's setting briefly agree again. Acting on each of those
        // would rebuild the renderer four times over a doorway.
        //
        // So a change must be the same answer for this many consecutive game frames before it is
        // applied. At the twenty updates a second this game runs at, four frames is a fifth of a
        // second: far too short to see on a real room change (which happens behind a fade
        // anyway) and long enough to swallow every blip measured so far.
        constexpr int STEADY_FRAMES = 4;

        static bool applied = false;
        static bool pending = false;
        static int pending_frames = 0;

        if (on == applied) {
            pending_frames = 0;
            return;
        }
        if (on != pending) {
            pending = on;
            pending_frames = 0;
        }
        if (++pending_frames < STEADY_FRAMES) {
            return;
        }

        applied = on;
        pending_frames = 0;
        g_prerendered_room.store(on, std::memory_order_relaxed);
        std::fprintf(stderr, "[gfx] prerendered room %s (camera setting %d): the view is %s\n",
                     on ? "entered" : "left", cam_setting,
                     on ? "forced to the console's 4:3" : "back to the settings");
        push_to_runtime();
    }

    bool g_file_present = false;

    bool settings_file_present() {
        return g_file_present;
    }

    // THE LETTERBOX BARS. The game thread publishes the size it wants; the interface thread reads
    // it once per presented frame and eases toward it. One number each way, written whole, so
    // neither thread waits on the other and the worst a torn read could do is use last frame's
    // target, which is a frame of easing nobody can see.
    std::atomic<int> g_letterbox_target{ 0 };
    std::atomic<float> g_letterbox_now{ 0.0f };
    std::atomic<long long> g_letterbox_last_us{ 0 };

    bool letterbox_target(int size) {
        if (g_live.letterbox == 0) {
            g_letterbox_target.store(0, std::memory_order_relaxed);
            g_letterbox_now.store(0.0f, std::memory_order_relaxed);
            return false;
        }
        g_letterbox_target.store(std::max(0, std::min(size, 120)), std::memory_order_relaxed);
        return true;
    }

    float letterbox_now() {
        // The game's own speed, so the bars take exactly as long as they always did: ten console
        // pixels per game update, twenty updates a second, which is two hundred a second.
        constexpr float SPEED = 200.0f;
        const long long now = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count();
        const long long previous = g_letterbox_last_us.exchange(now, std::memory_order_relaxed);
        float seconds = (previous == 0) ? 0.0f : float(now - previous) / 1.0e6f;
        // A long gap is a load or a pause, not an animation: step straight there rather than
        // sliding across the picture when play comes back.
        if (seconds > 0.25f) {
            seconds = 0.25f;
        }
        const float target = float(g_letterbox_target.load(std::memory_order_relaxed));
        float value = g_letterbox_now.load(std::memory_order_relaxed);
        const float step = SPEED * seconds;
        if (value < target) {
            value = std::min(value + step, target);
        } else if (value > target) {
            value = std::max(value - step, target);
        }
        g_letterbox_now.store(value, std::memory_order_relaxed);
        return value / 240.0f;   // the console's picture height
    }

    void hold_window_mode(bool held) {
        if (g_window_mode_held == held) {
            return;
        }
        g_window_mode_held = held;
        push_to_runtime();
    }

    // A FIRST RUN LOOKS LIKE TURNING ON THE CONSOLE (the user, 2026-10-08: "when you first boot
    // up the game as a brand new user, you're looking at N64 defaults", "I don't want it to have
    // a 16 by 9 by default. I don't want it to have higher resolution by default ... frame rate
    // 20"). Every row that adds something starts on the console's own value and the person
    // raises it; the website carries the author's recommendations. The one exception is the
    // window, fullscreen, which they were fine with. Until that day this matched the monitor,
    // expanded to its shape, ran at the display's rate and chose antialiasing from the card.
    Settings machine_defaults() {
        Settings d;
        d.window_mode = 1;      // fullscreen, at the desktop's mode
        d.resolution = 0;       // the console's own 320 by 240
        d.downsampling = 0;
        d.aspect = 0;           // the console's 4:3
        d.hud_ratio = 0;        // the interface where the console put it
        d.antialiasing = 0;     // none, as the console had none
        d.render_distance = 0;  // the game's own distance
        d.frame_rate = 0;       // the game's own rate: 20 a second
        d.counter = 0;
        d.texture_pack = 0;     // the game's own textures
        d.volume_percent = 100;
        clamp(d);
        return d;
    }

    void load_settings(const std::string& path) {
        g_path = path;

        std::ifstream file(path);
        g_file_present = static_cast<bool>(file);
        if (file) {
            Settings loaded;   // from the defaults, so a half written file loses nothing else
            std::string line;
            while (std::getline(file, line)) {
                std::string key, value;
                if (!parse_line(line, key, value)) {
                    continue;
                }
                if (key == "updates_asked") {
                    g_update_choice_made = as_int(value, 0) != 0;
                    continue;
                }
                if (key == "rom_copy_asked") {
                    g_rom_choice_made = as_int(value, 0) != 0;
                    continue;
                }
                for (int row = 0; row < ROW_COUNT; ++row) {
                    if (key == ROWS[row].key) {
                        set_row(loaded, row, as_int(value, get_row(loaded, row)));
                        break;
                    }
                    // An unknown key is ignored on purpose, so a file written by a later version
                    // still loads here rather than being rejected wholesale.
                }
            }
            clamp(loaded);
            // THE RECORDER AND THE COUNTER ARE READ BACK LIKE EVERY OTHER ROW (the user,
            // 2026-09-24: "make both the fps counter and the recorder options retain setting
            // values cross session like before"). From 2026-09-23 to today both were forced Off at
            // every launch, after a release had shipped with the recorder on by default; the
            // defaults in the table are still Off, so a fresh install starts with neither, and a
            // person who switches one on keeps it until they switch it off.
            g_live = loaded;
        }
        // A missing file is not an error. The first run has none, and that is the common case.

        // PUSH WHAT WAS LOADED. Until the post-parity baseline was taken, this function stored
        // the file's values and stopped there, so a setting only reached the renderer when it was
        // changed through the interface in the same session. The file round-tripped, the rows
        // showed the right words, and none of it applied at startup: a 16:9 window with the
        // aspect rows set drew a 4:3 picture with black pillars, and the wrong reading ("the
        // renderer needs the extended GBI first") looked plausible for a phase. The runtime
        // stores the configuration and the renderer reads it when it is created, so pushing
        // here, before the renderer exists, is exactly right.
        push_to_runtime();
    }

    bool apply(const Settings& next) {
        Settings copy = next;
        clamp(copy);
        {
            // The write is under the same lock push_to_runtime reads under, and released before
            // the push so the two never nest.
            std::lock_guard<std::mutex> lock(g_live_mutex);
            g_live = copy;
        }

        push_to_runtime();

        if (g_path.empty()) {
            return false;
        }

        std::ofstream file(g_path, std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "[ui] settings could not be written to %s\n", g_path.c_str());
            return false;
        }
        g_file_present = true;

        file << "# OoT: Recompiled settings. Edit by hand if you like; every value is clamped when\n";
        file << "# it is read, and anything unrecognized is ignored.\n";
        for (int row = 0; row < ROW_COUNT; ++row) {
            file << ROWS[row].key << " = " << get_row(g_live, row) << "\n";
        }
        // Not rows, so written explicitly. They record that a question was ASKED, which is a
        // different fact from what the answer turned out to be.
        file << "updates_asked = " << (g_update_choice_made ? 1 : 0) << "\n";
        file << "rom_copy_asked = " << (g_rom_choice_made ? 1 : 0) << "\n";
        return file.good();
    }

} // namespace oot::ui
