#pragma once

// SCREENSHOTS, AND THE BEFORE AND AFTER PAIR (the user, 2026-10-09: "need to be able to enable
// screencaps in a settings option and ability to bind screencap to a controller or keyboard
// shortcut which just captures one screenshot ... in the current window size with the users
// currently enabled settings. this being enabled should dynamically then show an option for
// comparison mode. which will freeze the game at its exat point, screencap once with all the users
// curent settings, and then turn all settings to be 4:3 original res, no downsample, and all
// lighting and ambience off. and screencap one more before re-enabled all user's settings and
// releasing the freeze. this is used for making before and after shots").
//
// THE SCREENSHOT is the next presented frame at the window's size, as a PNG, through the recorder's
// snapshot (main/recorder.h), into the captures folder under the day, beside the recordings:
// `HH-MM-SS-oot-screenshot.png`. Only while the Screenshots row is On; the Screenshot binding
// (Controls, F12 by default) asks for it. The Screenshots show row (2026-10-09, the user, of photo
// mode: "We need the same thing" as Video shows) says what it holds: the game alone (the default;
// the picture drawn again without this program's interface, and whole even with the settings panel
// open), or the whole window, our own interface included, exactly what is on the screen.
//
// THE COMPARISON, while the Debugging menu's Comparison shots row is also On, in place of the one
// shot:
//   1. the game is held (main/shots.h, set_hold): it draws its frame over and over and does not
//      update, its frame counter and random numbers pinned, so every frame from here is the same;
//   2. a few frames later, the first shot, with every one of the person's settings
//      (`...-oot-compare-yours.png`);
//   3. the renderer is given the original look, the person's rows and files untouched: the
//      console's picture in full (ui_settings.h, set_original_look: 4:3, the original resolution,
//      no downsampling, no antialiasing, the interface at its own ratio, the game's own draw
//      distance, no texture pack; the user, the same day: "original N64 style and quality versus
//      basically maxed out settings"), and the traced lighting off, which takes every traced
//      pass, the dust, the shafts and the glow with it (ui_lighting, as the sweep puts a value to
//      the renderer);
//   4. after the picture has settled, the second shot (`...-oot-compare-original.png`);
//   5. the person's settings back to the renderer, and the hold released.
// The toast says so once both files are written, so it is in neither picture.
//
// Ticked once per presented frame from the renderer's update, like the sweep; nothing here blocks.
// A request while one is under way, a sweep is running or a snapshot is already waiting is ignored
// with a line in the log.
namespace oot::screenshots {

    // The Screenshot binding was pressed. Safe from any thread; the work happens in tick.
    void request();

    // One presented frame has gone by. Cheap when nothing is under way.
    void tick();

    bool busy();

} // namespace oot::screenshots
