"""Generate the website's settings documentation from the program's own row tables.

    python tools/settings_doc.py [--out <path>]

WHY THIS IS GENERATED. Every row, its label and every one of its options already exist in exactly
one place: the ROWS tables in `src/ui/ui_settings.cpp` and `src/ui/ui_lighting.cpp`. Typing them
into a website by hand guarantees the two disagree by the next release, quietly, in the direction
that makes the site wrong. So the labels and options are READ FROM THE SOURCE, and the only thing
written here by hand is the part the source cannot supply: a plain description of what each row
does, and which value gives the original console picture.

THREE COLUMNS COME OUT OF THIS, which is what the site shows:

  Original     what to set to play it as the cartridge played it, with nothing added.
  Recommended  the author's own settings, read from the installed copy on this machine.
  Hardware     the rows whose right answer depends on the machine, called out separately so
               nobody copies a 4K setting onto a laptop and concludes the program is slow.

WHY THE ORIGINAL VALUE IS AN INDEX AND NOT A WORD. Until 2026-09-26 the Camera distance row had an
option LABELED "Original" that was NOT the game's own distance: the game's own was the step called
Near (patches/free_camera.c, sDistanceScale[0] = 1.0). Anyone assembling an original-experience list
by matching on the word got that row wrong. The row itself has since been fixed, because a menu
should not need a footnote to be read correctly, but the lesson stands and the indices stay: a label
is a thing somebody edits, and an index is a thing the program actually uses.
"""
from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SETTINGS_CPP = ROOT / "src" / "ui" / "ui_settings.cpp"
LIGHTING_CPP = ROOT / "src" / "ui" / "ui_lighting.cpp"
DEFAULT_OUT = ROOT.parent / "oot-recompiled-site" / "src" / "data" / "settings.json"


def strip_comments(text: str) -> str:
    """Remove // comments and /* */ blocks, leaving string literals alone.

    The row tables are buried in comment prose, and one of the rows is itself commented out (the
    top bar row, held back until it does something). Parsing without this would resurrect it.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            out.append(text[i : j + 1])
            i = j + 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i)
            i = n if j < 0 else j + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


ROW_RE = re.compile(
    r'\{\s*"(?P<key>[a-z]+)"\s*,\s*"(?P<label>[^"]*)"\s*,\s*\{(?P<options>.*?)\}\s*,\s*(?P<count>[^}]+?)\s*\}',
    re.S,
)
OPTION_RE = re.compile(r'"([^"]*)"')


def read_rows(path: Path) -> list[dict]:
    text = strip_comments(path.read_text(encoding="utf-8"))
    start = text.index("const Row ROWS[]")
    end = text.index("};", start)
    body = text[start:end]
    rows = []
    for m in ROW_RE.finditer(body):
        options = OPTION_RE.findall(m.group("options"))
        rows.append({"key": m.group("key"), "label": m.group("label"), "options": options})
    return rows


def read_values(path: Path) -> dict[str, int]:
    """One of the program's own settings files, as key to number."""
    values: dict[str, int] = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        try:
            values[key.strip()] = int(value.strip())
        except ValueError:
            continue
    return values


# ----------------------------------------------------------------------------------------------
# The hand written half: what each row does, and what the original picture wants.
#
# `original` is an INDEX into that row's options, or None where the row does not change what the
# game looks like and is simply the person's own preference.
# `why` is only written where the answer is not obvious from the label, which is where a reader
# would otherwise assume the wrong thing.
# ----------------------------------------------------------------------------------------------
SETTINGS_NOTES: dict[str, dict] = {
    "window": dict(group="Picture", original=None,
        description="Whether the program runs in a window or fills the screen. It changes nothing about how the game is drawn."),
    "resolution": dict(group="Picture", original=0, hardware=True,
        description="How many pixels the game is drawn at. Original is the console's own 320 by 240, which is what the art was drawn for. Every step above it renders the same scene at more detail. This is the setting that costs the most performance, so the right answer depends on your graphics card."),
    "downsampling": dict(group="Picture", original=0, hardware=True,
        description="Renders the picture larger than it is shown and shrinks it down, which removes jagged edges more thoroughly than antialiasing does. 4x means drawing sixteen times the pixels, so it is expensive. It works at every resolution, Match the window included, and never draws more than half of your graphics card's memory can hold: a setting that would not fit steps down by itself, and each option says the size it will actually draw."),
    "aspect": dict(group="Picture", original=0, hardware=True,
        description="The shape of the picture. Original 4:3 is the console's. The wider settings genuinely widen the view rather than stretching it, so you see more of the world at the sides. The right one is your screen's own shape."),
    "hud": dict(group="HUD", original=0,
        description="Where the HUD sits. Original 4:3 keeps it within the console's own 4:3 frame; Within 16:9 spreads it toward the edges of a wider picture but no wider than a 16:9 screen; Window edges puts it at the picture's very edges; Custom keeps distances you choose from each edge, as a share of the picture, at any shape, so it follows the window as it changes size and never leaves it."),
    "antialiasing": dict(group="Picture", original=0,
        description="Smooths the jagged steps along the edges of shapes. The console had nothing of the kind. Higher settings cost more."),
    "renderdistance": dict(group="Picture", original=0,
        description="How far into the distance the game keeps drawing things. The console dropped anything past a short distance to keep up, which is why scenery appears as you approach it. Raising this keeps it drawn, at a cost."),
    "framerate": dict(group="Picture", original=0,
        description="How many times a second the picture is presented. The game's own logic always runs at 20 steps a second, exactly as it did on the console; anything above Original fills the gaps between those steps with interpolated frames, so movement is smooth without the game running faster."),
    "counter": dict(group="Picture", original=0,
        description="Shows the number of frames being presented each second, on a small plate in the corner."),
    "updates": dict(group="Program", original=None,
        description="Whether the program checks the release page for a newer version: when it opens, and every four hours while it runs until one is found. On an installed copy a newer version downloads in the background, is verified, and installs the next time the program opens, or right away with F5. It is the only network connection the program makes, and nothing about you or your copy of the game is sent."),
    "autosave": dict(group="Saving", original=None,
        description="Saves a moment by itself on a timer, into its own slots, never over one you made. The five most recent are kept."),
    "saveonquit": dict(group="Saving", original=None,
        description="Saves a moment when you close the program, so you can pick up where you stopped."),
    "resumeonstart": dict(group="Saving", original=None,
        description="When you press Play, the game resumes from the most recent saved moment, an autosave or one of yours, instead of starting at the title."),
    "camera": dict(group="Camera", original=0,
        description="Lets the right stick turn the camera freely, wherever the game's own camera is able to turn. Off leaves the camera exactly as the game moves it."),
    "cameraaxes": dict(group="Camera", original=0,
        description="Which way the free camera moves when you push the stick. Only has an effect when the free camera is on."),
    "aimaxes": dict(group="Camera", original=0,
        description="Which way aiming moves when you push the right stick, in first person view and with the bow, the slingshot, the hookshot or the boomerang. The right stick aims only while the free camera is on."),
    "aimaxesleft": dict(group="Camera", original=0,
        description="The same for the left stick, which is the one the game itself uses for aiming."),
    "cameradistance": dict(group="Camera", original=0,
        description="How far the camera sits from Link, for every camera the game uses, not only the free one. Original is the console's own distance; each step above it pulls the camera back a little further."),
    "cameradrift": dict(group="Camera", original=1,
        why="On is the console's behavior. The game's own camera eases outward as he runs and back in as he stops.",
        description="Whether the camera drifts further away as Link runs and back in as he stops."),
    "textspeed": dict(group="Game", original=0,
        description="How fast a message's text types into its box. Fast and Faster are two and four times the original pace; Instant shows each box whole at once. A box still waits at its end for you to go on."),
    "textskip": dict(group="Game", original=0,
        description="As the game, B shows the rest of a message where the game allows it. Always lets A show the rest of the box you are reading and B skip on through the message in every message, and either button ends a box's timed wait."),
    "lowhealthbeep": dict(group="Game", original=0,
        description="Whether the alarm sounds while your health is low. The heart still beats and turns red either way."),
    "screenshots": dict(group="Program", original=None,
        description="Lets the Screenshot control (F12 by default; change it in Controls) save a picture at Screenshot size into your Pictures folder, in a folder for each day. Screenshots show decides whether it holds the game alone or the whole window. Photo mode's own picture control takes a picture whether this is on or not."),
    "comparisonshots": dict(group="Development", original=None, debug=True,
        description="Makes the Screenshot control save a before and after pair instead: the game holds still, one picture is taken with your settings, then one exactly as the original console showed it (4:3, original resolution, no antialiasing, no texture pack, no traced lighting), and everything is put back. For development only."),
    "screenshotshows": dict(group="Program", original=None,
        description="What a screenshot or a photo mode picture shows: the game alone, without any of this program's menus, toasts or plate, even with the menu open (so photo mode can take the same picture at every setting with the menu still up), or the whole window as you see it. Either way it is drawn again at Screenshot size, the whole window with its menus drawn at that size too rather than stretched."),
    "screenshotsize": dict(group="Program", original=None,
        description="The size a screenshot or a photo mode picture is saved at, at the shape of your window: the window's own size, or 720p, 1080p, 1440p or 4K. It can be larger than your screen, for the game alone or the whole window; for a sharp large picture, set Resolution at least as high, since the picture is drawn from what the game renders. The picture is saved in the background, so play never stalls for it."),
    "photohud": dict(group="Program", original=None,
        description="How the game's HUD starts when photo mode freezes the game: shown in full, dimmed to the Dimmed to share, or hidden for a clean picture. Photo mode's HUD control (H or the top face button by default; change it in Controls) then steps through the three: shown, dimmed, hidden. HUD fades has no say in photo mode."),
    "photohuddim": dict(group="Program", original=None,
        description="How far Dimmed takes the HUD in photo mode, from 20% to 80% of its full strength."),
    "photomode": dict(group="Program", original=None,
        description="Lets the Photo mode control (F10 or the left stick's press by default) freeze the game where it stands and pause its sound, so you can change any setting and see it on the same frame, move the camera around the frozen scene, show, dim or hide the game's HUD (it starts as HUD in photo mode says), and take the same picture at every setting. Take a picture (Space or the bottom face button by default) saves it to your Pictures folder at Screenshot size, with Screenshots on or off. A row along the bottom of the picture shows each control as it is bound on the keyboard or controller in use, and its own control (C or the right face button by default) hides it. Every one of these can be changed in Controls. Press the Photo mode control again to carry on."),
    "videorecording": dict(group="Program", original=None,
        description="Lets the Record video control (F7 by default; change it in Controls) start and stop a recording of what you play, saved as an MP4 ready to upload into your Videos folder. The Pause video control (F8 by default) pauses and resumes the same video, so a run of chosen moments ends up in one file. Video shows decides whether the program's menus are in it, and Video sound whether the game's sound is."),
    "videoshows": dict(group="Program", original=None,
        description="What a video records: the game alone, without any of this program's menus, or the whole window, the game with every menu, toast and plate over it, so using photo mode or the menus can be shown. Either way it is drawn again at Video size, the whole window with its menus drawn at that size too rather than stretched."),
    "videosize": dict(group="Program", original=None,
        description="The video's height, at the shape of your window: the window's own size, or 720p, 1080p, 1440p or 4K. It can be larger than your screen, for the game alone or the whole window; for a sharp large video, set Resolution at least as high, since the video is drawn from what the game renders."),
    "videoquality": dict(group="Program", original=None,
        description="How much detail the video keeps, which sets the file's size: Small file, Standard (about what video sites recommend), High, or Best for a video you will edit again."),
    "videosound": dict(group="Program", original=None,
        description="Whether the game's sound is recorded with the video. Off leaves the game's sound out and spends no time on it; your voice is still recorded if the Microphone row is on."),
    "videomode": dict(group="Program", original=None,
        description="How the video is written. Reliable saves it as it records, so a crash or a forced close keeps the video up to its last moments, and makes it an ordinary MP4 when you stop, which takes a few seconds longer and needs room for a second copy while it finishes. Compatible writes an ordinary MP4 from the first second and stops instantly, but a crash or a forced close before you stop loses the whole video."),
    "hudfade": dict(group="HUD", original=0,
        description="Whether the game's HUD fades: never, once it has not been needed for a while, always (the HUD control shows it until pressed again), or only when the HUD control is pressed. The HUD control is Tab or the right stick's press by default; change it in Controls. In photo mode it has no say: the HUD follows HUD in photo mode and photo mode's own control instead."),
    "hudfadeto": dict(group="HUD", original=None,
        description="Whether the HUD fades to dim, still there but quieter, or away entirely."),
    "hudopacity": dict(group="HUD", original=None,
        description="How much of the HUD shows while it is dimmed."),
    "hudcolor": dict(group="HUD", original=None,
        description="How much of its color the HUD keeps while it is dimmed, down to gray. Item icons keep their own colors."),
    "huddelay": dict(group="HUD", original=None,
        description="How long the HUD waits, unused, before it fades."),
    "hudwaketarget": dict(group="HUD", original=None,
        description="Whether targeting with Z brings the HUD back while it fades when idle. Each thing that can bring it back is its own box to tick."),
    "hudspeed": dict(group="HUD", original=None,
        description="How quickly the HUD fades out and back in."),
    "hudfadehearts": dict(group="HUD", original=None,
        description="Whether the hearts fade with the HUD or stay shown."),
    "hudfademagic": dict(group="HUD", original=None,
        description="Whether the magic meter fades with the HUD or stays shown."),
    "hudfaderupees": dict(group="HUD", original=None,
        description="Whether the rupee and key counters fade with the HUD or stay shown."),
    "hudfadebuttons": dict(group="HUD", original=None,
        description="Whether the A, B, C and Start buttons fade with the HUD or stay shown."),
    "hudfademap": dict(group="HUD", original=None,
        description="Whether the map fades with the HUD or stays shown."),
    "microphone": dict(group="Program", original=None,
        description="Whether your voice from a microphone is mixed into a video with the game's sound. The microphone is listened to only while a video records, and goes only into that video; nothing is sent anywhere."),
    "micdevice": dict(group="Program", original=None,
        options=["System default", "A microphone on your computer, by name"],
        description="Which microphone to record: the system's default, or any microphone your computer has, listed by name. One plugged in later shows up the next time the Video page opens."),
    "miclevel": dict(group="Program", original=None,
        description="How loud your voice is in the video, against the game's sound."),
    "hudsizes": dict(group="HUD", original=0,
        description="Whether one size applies to the whole HUD, or each part (hearts, magic, rupees and keys, the buttons, the map) has its own."),
    "hudsize": dict(group="HUD", original=5,
        description="The whole HUD's size, from half to twice the original, each part growing from the corner it sits in so nothing leaves the screen."),
    "hudsizehearts": dict(group="HUD", original=5,
        description="The hearts' size, growing from the top left corner."),
    "hudsizemagic": dict(group="HUD", original=5,
        description="The magic meter's size, growing from its left end."),
    "hudsizerupees": dict(group="HUD", original=5,
        description="The rupee and key counters' size, growing from the bottom left corner."),
    "hudsizebuttons": dict(group="HUD", original=5,
        description="The A, B, C and Start buttons' size, growing from the top right corner."),
    "hudsizemap": dict(group="HUD", original=5,
        description="The map's size, growing from the bottom right corner."),
    "hudwakec": dict(group="HUD", original=None,
        description="Whether using a C button brings the HUD back."),
    "hudwaketalk": dict(group="HUD", original=None,
        description="Whether talking, or any message on screen, brings the HUD back."),
    "hudwakehealth": dict(group="HUD", original=None,
        description="Whether your health or magic changing brings the HUD back."),
    "hudwakelow": dict(group="HUD", original=None,
        description="Whether the hearts stay shown while your health is low, when the HUD fades when idle or always. Only the hearts: the rest of the HUD fades as it would."),
    "hudwakeitems": dict(group="HUD", original=None,
        description="Whether your rupees, keys, the items on your buttons or any ammunition changing brings the HUD back."),
    "hudwakepause": dict(group="HUD", original=None,
        description="Whether opening the pause menu brings the HUD back."),
    "hudreticle": dict(group="HUD", original=0,
        description="Whether the arrows that close in and spin around what you have targeted with Z are drawn. The targeting itself works either way."),
    "hudmarker": dict(group="HUD", original=0,
        description="Whether the small arrow that floats over what Z would lock on to, enemy or not, is drawn. The targeting itself works either way."),
    "hudwakemap": dict(group="HUD", original=None,
        description="Whether showing or hiding the map brings the HUD back."),
    "hudwakeany": dict(group="HUD", original=None,
        description="Whether the press of any button brings the HUD back."),
    "hudleft": dict(group="HUD", original=None,
        description="With HUD sits on Custom: how far the hearts, the magic meter and the rupee counter sit from the picture's left edge, as a share of its width."),
    "hudright": dict(group="HUD", original=None,
        description="With HUD sits on Custom: how far the buttons and the map sit from the picture's right edge, as a share of its width."),
    "hudtop": dict(group="HUD", original=None,
        description="With HUD sits on Custom: how far the hearts and the buttons sit from the picture's top edge, as a share of its height."),
    "hudbottom": dict(group="HUD", original=None,
        description="With HUD sits on Custom: how far the rupee counter and the map sit from the picture's bottom edge, as a share of its height."),
    "menu": dict(group="Program", original=None,
        description="Where the menu sits in play: over the picture, or as a panel against the right or left edge, with the picture moved aside so the game stays in view. The launcher keeps its own layout."),
    "volume": dict(group="Program", original=None,
        description="How loud the game is, as a percentage of its own volume."),
    "menusounds": dict(group="Program", original=None,
        description="Whether the menus make a small sound as you move, choose, change a value, go back, and open or close them. In play they are the game's own menu sounds."),
    "menusoundvolume": dict(group="Program", original=None,
        description="With Menu sounds on: how loud they are, scaled by Volume as well."),
    "resizerefresh": dict(group="Development", original=None, debug=True,
        description="Temporary, while a fault is found: after the window or the interface size changes, the page's rows are refreshed and the focus put back in view. For development only."),
    "scalereload": dict(group="Development", original=None, debug=True,
        description="Temporary, while a fault is found: a change of interface size applies every stylesheet again, so every size follows it. For development only."),
    "smoothscroll": dict(group="Development", original=None, debug=True,
        description="Temporary, while a fault is found: lists scroll smoothly to the focus rather than at once. For development only."),
    "interfacesize": dict(group="Program", original=None,
        description="How large this program's menus are drawn. Automatic suits the screen: 100% on a 1920 by 1080 screen, smaller on a smaller one and larger on a larger one, in the launcher and the game alike. A chosen size is used whenever the window has room for it. The button prompts along the bottom never go smaller than 100%."),
    # THE PACK ROW'S OPTIONS ARE THE PERSON'S FOLDER (2026-10-08). The table carries only "Off";
    # every other option is whatever packs that person has dropped in, so the site names the kind
    # of option rather than a pack, and the author's own value (a place in their folder's list)
    # reads as that kind rather than falling out of range and showing nothing.
    "texturepack": dict(group="Picture", original=0,
        options=["Off", "A pack in your Texture packs folder"], folder=True,
        link=dict(href="https://github.com/GhostlyDark/OoT-Reloaded/releases",
                  label="Download OoT Reloaded, by GhostlyDark, from its author's releases page (the rt64 version)"),
        description="Replaces the game's own textures with a texture pack, a set of higher resolution images made by other people. The program comes with none and downloads none, but a pack can be downloaded: the one below is made for this renderer, and its rt64 version comes in parts, the first of which opens with 7-Zip. Put the .rtz file inside it in the Texture packs folder (Your files, in Settings, opens it), and it appears on this row by name the next time the program starts. Off is the game's own textures."),
    "texturedetail": dict(group="Picture", original=None, hardware=True,
        original_text="Not used: the original picture has no texture pack",
        description="How much of a texture pack's own detail is used: Quarter, Half or Full of its width and height. Which one depends on your graphics card, since the lower steps take less video memory. It starts on Quarter, so a pack loads light on any machine; raise it once the game runs well. Shown only while a pack is chosen."),
    "recorder": dict(group="Development", original=0, debug=True,
        description="Writes every frame the program shows to disk as a PNG file, for as long as the length below. It is meant for showing somebody exactly what went wrong. It fills a drive very quickly: at 1080p and 60 frames a second it is roughly 10 GB a minute, and the program asks you to confirm before it will switch on."),
    "recordlength": dict(group="Development", original=None, debug=True,
        description="How long a recording runs before it stops by itself."),
}

LIGHTING_NOTES: dict[str, dict] = {
    "raytracing": dict(group="Lighting", original=0,
        why="Off is the original picture. With it off the game lights itself exactly as the console did, and draws its own shadows under the characters.",
        description="The master switch for traced lighting. With it on, light is followed through the scene rather than painted on by the game, so shadows and bounced light are worked out from where the lights actually are. It needs a graphics card that supports DirectX Raytracing; on one that does not, these rows do nothing and the Lighting page says so. In a place the game draws as a painted backdrop, such as a shop, the game's own lighting is used either way."),
    "shadows": dict(group="Lighting", original=None,
        description="Whether the sun casts traced shadows, and indoors the room's own light. Torches, fairies and windows cast theirs either way. The game's painted shadows under characters stay hidden while Ray traced lighting is on, so with this off a character in the sun has no shadow at all."),
    "shadowsoftness": dict(group="Lighting", original=None,
        description="How sharp the edge of the sun's shadow is, and indoors of the room's own light. The sun casts a nearly hard edge in life; the higher settings spread it as though the light came from a wider source. Torches, fairies and windows keep their own softness."),
    "shadowrays": dict(group="Lighting", original=None,
        description="How many rays each lit pixel spends working out whether it is in shadow. More rays cost frame rate and buy a cleaner shadow edge; fewer leave visible grain, especially on anything that moves."),
    "casters": dict(group="Lighting", original=None,
        description="How much of the world behind and beside the camera is kept in the traced scene so its shadow can still reach the picture. The console's own culling threw away anything off screen, which is why a shadow can vanish as the camera turns."),
    "indoorlight": dict(group="Lighting", original=None,
        description="How dark the short shadows go indoors, where there is no sun: what a character or anything else casts on what is close beside it, by the room's own light. Off leaves indoor places without them. Rooms whose own settings carry no directional light get one supplied overhead so there is something to cast with. It needs Shadows on."),
    "darkness": dict(group="Lighting", original=None,
        description="How much of the game's own background lighting is taken away where no traced light reaches. Raising it means a room is genuinely dark except where a torch or a fairy lights it, which is what makes those lights read as real lights."),
    "indoordust": dict(group="Lighting", original=None,
        description="How much dust hangs in the air indoors, in temples, caverns and houses. The motes show only where light actually reaches them, in a window's light, a shaft of light or the glow of a torch, and never in shadow. Indoors all of that light is traced by Local lights, so the dust needs it on."),
    "indoordustvisibility": dict(group="Lighting", original=None,
        description="How bright an indoor mote is where light reaches it. Faint keeps the dust barely there; Bright makes the light in a room read as something you could put your hand into."),
    "indoordustsize": dict(group="Lighting", original=None,
        description="The typical size of an indoor mote. Every mote is its own size around this, and its own opacity and brightness too, so the dust reads as many faint specks with a few brighter ones rather than a grid of identical dots."),
    "outdoordustsize": dict(group="Lighting", original=None,
        description="The typical size of an outdoor mote, each one varying around it."),
    "indoordustdrift": dict(group="Lighting", original=None,
        description="How the indoor dust moves: hanging still, drifting slowly, or wandering about. Indoors there is no wind."),
    "outdoordust": dict(group="Lighting", original=None,
        description="How much dust is carried in the air outdoors. Like the indoor dust it shows only in the light, and it travels with the wind."),
    "outdoordustvisibility": dict(group="Lighting", original=None,
        description="How bright an outdoor mote is in the sun."),
    "wind": dict(group="Lighting", original=None,
        description="How fast the wind carries the outdoor dust. It never reaches indoors."),
    "windgusts": dict(group="Lighting", original=None,
        description="Gusts that rise, hold and die away over the wind's steady breeze, each a little up or down as well as across. Light gusts are weaker and further apart than Strong. With a set wind direction each gust comes from near it; with Varied, from any direction. There are none while Wind is Calm."),
    "winddirection": dict(group="Lighting", original=None,
        description="Which way the wind comes from, by the world's own compass. Varied lets it wander slowly round the compass on its own, and sends each gust from any direction."),
    "indoorambiencedistance": dict(group="Lighting", original=None,
        description="How far away the indoor dust can still be seen. It always thins gradually with distance rather than stopping at a line; this sets where it is finally gone."),
    "outdoorambiencedistance": dict(group="Lighting", original=None,
        description="How far away the outdoor dust can still be seen, the same way."),
    "lightbrightness": dict(group="Lighting", original=None,
        description="How far a traced light can lift a surface above the game's own lighting. On Normal a light mostly gives back what a shadow took; the higher settings let a torch, a fairy or daylight through a window light the ground brighter than the game ever drew it, up to a harsh, bright pool."),
    "occlusion": dict(group="Lighting", original=None,
        description="Darkens the creases where surfaces meet, the way dust and shadow gather in a corner. It is subtle and it is what stops objects looking as though they are floating."),
    "occlusionradius": dict(group="Lighting", original=None,
        description="How far from a surface the program looks for something that would shade it. Small values catch only tight corners; large ones darken under whole objects."),
    "occlusionquality": dict(group="Lighting", original=None,
        description="How many rays the shading in corners is worked out from. More is cleaner and costs more."),
    "lights": dict(group="Lighting", original=None,
        description="Whether the game's own lights are traced: torches and fairies, drawn shafts of light, and the daylight through the Temple of Time's windows. With this on they light what is around them and cast real shadows from whatever stands in the way. Off leaves those lights to the game, as the console showed them."),
    "lightreach": dict(group="Lighting", original=None,
        description="How far a torch's or a fairy's light carries, as a percentage of its normal reach. 100% is the game's own radius for a torch and twice the game's own for a fairy, whose light the game kept to a few steps."),
    "lightstrength": dict(group="Lighting", original=None,
        description="How bright the traced lights are, as a percentage of their normal strength: torches, fairies, shafts of light and window daylight alike. 100% is the game's own; 0% is the same as Local lights Off."),
    "bounce": dict(group="Lighting", original=None,
        description="Light that has already hit one surface and carried its color onto another. It is what makes a red wall tint the floor beside it."),
    "bouncerange": dict(group="Lighting", original=None,
        description="How far bounced light is followed before the program stops looking."),
    # REMOVED 2026-09-30 (the reflections came out), but their rows stay in the program's table so
    # nothing renumbers; the menus hide them by key. `removed` keeps them off the site the same way.
    "reflections": dict(group="Lighting", original=None, removed=True,
        description="Traced reflections in surfaces that should have them, such as water and polished floors."),
    "reflectionrange": dict(group="Lighting", original=None, removed=True,
        description="How far into the scene reflections are worked out."),
    "glow": dict(group="Lighting", original=None,
        description="The haze in the air immediately around a light, as though dust were catching it."),
    "glowsize": dict(group="Lighting", original=None,
        description="How large that halo is, relative to the light's own radius."),
    "haze": dict(group="Lighting", original=None,
        description="Shafts of sunlight in the air outdoors."),
    "hazerange": dict(group="Lighting", original=None,
        description="How far into the distance the sun's haze and the glow around lights are drawn. Both thin out from about a third of this distance and are gone by it."),
    "fogfade": dict(group="Lighting", original=None,
        description="Whether traced lighting fades into the game's own fog, so a lit surface far away does not stand out of the haze in front of it."),
    "distancefade": dict(group="Lighting", original=None,
        description="Fades the traced shadows and light back to the game's own lighting with distance, gradually, ending nearer the camera from Far to Near. It changes only the look: everything is still traced, so it saves no time."),
    "smoothing": dict(group="Lighting", original=None,
        description="How much each frame's lighting is averaged with the frames before it. Tracing a scene with a limited number of rays leaves grain; averaging across frames removes it, at the cost of a faint trail behind fast movement."),
    "filter": dict(group="Lighting", original=None,
        description="A final blur across the traced lighting, which removes what is left of the grain."),
    "inspect": dict(group="Development", original=None, debug=True,
        description="Replaces the picture with a single stage of how it was drawn, such as the shadows alone or how many frames of history each pixel has. It is a tool for working out why the lighting looks wrong, and it makes the game unplayable while it is on."),
    "experiment": dict(group="Development", original=None, debug=True,
        description="Undoes one piece of the lighting math at a time, so a particular look can be traced to the step that causes it. For development only."),
    "windowshafts": dict(group="Lighting", original=None,
        description="Shafts of daylight in the air below a window, brightest at the glass and fading as the light falls, lit by the window's own traced light so a pillar standing in one cuts a gap through it. Only in the Temple of Time, the one place whose windows give light. The game's own painted beam is still drawn beside them."),
    "shaftlength": dict(group="Lighting", original=None,
        description="How far down toward the floor a window's shaft of light carries before it fades away, measured on each window's own fall, so a high window's shaft reaches as far down as a low one's."),
    "lightstone": dict(group="Development", original=None, debug=True,
        description="Whether a torch's or a fairy's traced light stops at the stone around it or reaches a little way through, so a flame set into a wall can be compared both ways. For development only."),
    "glowtest": dict(group="Development", original=None, debug=True,
        description="How the round glow around a torch or a fairy is hidden when something stands in front of it: all at once when its center is covered, as the original game did, or a pixel at a time. For development only."),
    "windowaim": dict(group="Development", original=None, debug=True,
        description="Which window's traced light the aim rows below move: every window, or one by its number. For development only."),
    "windowaimfrom": dict(group="Development", original=None, debug=True,
        description="Whether that light falls at its window's own slope or the way the game's drawn beam fell. For development only."),
    "windowtilt": dict(group="Development", original=None, debug=True,
        description="Makes that light fall more steeply or more shallowly, a degree at a time, which moves where it lands nearer or further. For development only."),
    "windowturn": dict(group="Development", original=None, debug=True,
        description="Turns that light left or right, a degree at a time. For development only."),
    "windowmovex": dict(group="Development", original=None, debug=True,
        description="Moves that light along the world's x axis. For development only."),
    "windowmovey": dict(group="Development", original=None, debug=True,
        description="Moves that light up or down. For development only."),
    "windowmovez": dict(group="Development", original=None, debug=True,
        description="Moves that light along the world's z axis. For development only."),
}


def build(rows: list[dict], notes: dict[str, dict], live: dict[str, int]) -> list[dict]:
    out = []
    for row in rows:
        note = notes.get(row["key"])
        if note is None:
            raise SystemExit(
                f"the row '{row['key']}' ({row['label']}) has no description in tools/settings_doc.py.\n"
                "A row was added to the program and the site would have shown it undocumented."
            )
        if note.get("removed"):
            continue
        # A note may name the options itself, for a row whose real options are not in the table.
        options = note.get("options", row["options"])
        count = len(options)
        original = note.get("original")
        if original is not None and not (0 <= original < count):
            raise SystemExit(f"the original value for '{row['key']}' is {original}, outside its {count} options")

        value = live.get(row["key"])
        if note.get("folder") and value is not None and value >= count:
            value = count - 1   # any place in the folder's list reads as "a pack in the folder"
        recommended = value if (value is not None and 0 <= value < count) else None
        # NO RECOMMENDATION WHERE THE ANSWER IS THE MACHINE'S (the user, 2026-10-08: resolution,
        # downsampling, aspect ratio and texture pack quality "depend on hardware ... don't
        # recommend a specific setting for that"). Every other row is the author's own value.
        if note.get("hardware"):
            recommended = None

        out.append({
            "key": row["key"],
            "label": row["label"],
            "group": note["group"],
            "description": note["description"],
            "why": note.get("why"),
            "options": options,
            "original": original,
            "originalLabel": options[original] if original is not None else None,
            "recommended": recommended,
            "recommendedLabel": options[recommended] if recommended is not None else None,
            "hardware": bool(note.get("hardware")),
            "debug": bool(note.get("debug")),
            "link": note.get("link"),
            "originalText": note.get("original_text"),
        })
    return out


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    args = parser.parse_args()

    appdata = Path(os.environ.get("LOCALAPPDATA", "")) / "OoT Recompiled"
    live_settings = read_values(appdata / "settings.txt")
    live_lighting = read_values(appdata / "lighting.txt")

    # THE AUTHOR'S OWN COPY IS THE RECOMMENDATION, which is what the user asked for, with two
    # corrections. Both are development leftovers rather than recommendations: the frame recorder
    # was left on from a debugging session, and Inspect is out of range entirely in that file.
    if live_settings.get("recorder"):
        live_settings["recorder"] = 0
    live_lighting["inspect"] = 0
    live_lighting["experiment"] = 0
    # The windows' aim (2026-10-08) is a tool for lining a light up, not a setting.
    for key, start in (("windowaim", 0), ("windowaimfrom", 0), ("windowtilt", 15), ("windowturn", 15),
                       ("windowmovex", 9), ("windowmovey", 9), ("windowmovez", 9)):
        live_lighting[key] = start

    settings = build(read_rows(SETTINGS_CPP), SETTINGS_NOTES, live_settings)
    lighting = build(read_rows(LIGHTING_CPP), LIGHTING_NOTES, live_lighting)

    version = re.search(r"project\(OoTRecompiled VERSION (\d+\.\d+\.\d+)", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))

    document = {
        "note": "Generated by tools/settings_doc.py from the program's own row tables. Do not edit by hand.",
        "version": version.group(1) if version else None,
        "readSettingsFrom": str(appdata) if live_settings else None,
        "settings": settings,
        "lighting": lighting,
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {out}")
    print(f"  {len(settings)} settings rows, {len(lighting)} lighting rows")
    print(f"  {sum(1 for r in settings + lighting if r['debug'])} marked development")
    print(f"  {sum(1 for r in settings + lighting if r['hardware'])} marked hardware dependent")
    missing = [r["key"] for r in settings + lighting if r["recommended"] is None and not r["debug"]]
    if missing:
        print(f"  no recommended value read for: {', '.join(missing)}")


if __name__ == "__main__":
    main()
