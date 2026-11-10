#!/usr/bin/env python3
"""Create an ignored, persistent play profile from the user's opening save."""

import argparse
import shutil
from pathlib import Path

from tools.make_macos_playable_test_save import patch_opening_cutscene


SETTINGS = """# Conservative local Metal raster play preset; keep fullscreen and packs off.
window = 0
resolution = 5
downsampling = 0
aspect = 1
antialiasing = 0
framerate = 0
counter = 0
texturepack = 0
texturedetail = 0
"""

NATIVE_RT_SETTINGS = """# Native drawable output, original NTSC rate, no texture replacements.
window = 1
resolution = 2
downsampling = 0
aspect = 1
antialiasing = 0
framerate = 0
counter = 0
texturepack = 0
texturedetail = 0
"""


def initialize_profile(source_save: Path, profile: Path, *, native_rt: bool = False) -> None:
    """Make a new profile once; never replace its later gameplay save."""
    patched = patch_opening_cutscene(source_save.read_bytes(), skip_saria_greeting=True)
    profile.mkdir(parents=True, exist_ok=False)
    try:
        saves = profile / "saves"
        saves.mkdir()
        (saves / "oot.n64.us.1.0.bin").write_bytes(patched)
        (profile / "settings.txt").write_text(NATIVE_RT_SETTINGS if native_rt else SETTINGS)
        if native_rt:
            (profile / "lighting.txt").write_text("# Two AO rays; shadows and local lights stay enabled.\nocclusionquality = 3\n")
    except Exception:
        shutil.rmtree(profile)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-rt", action="store_true",
                        help="initialize the separate native-resolution ray tracing play profile")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = Path.home() / "Library/Application Support/OoT Recompiled/saves/oot.n64.us.1.0.bin"
    profile = root / ("data/native-rt-profile" if args.native_rt else "data/play-profile")
    if profile.exists():
        print(f"PLAY_PROFILE_EXISTS {profile} (existing progress preserved)")
        return 0
    initialize_profile(source, profile, native_rt=args.native_rt)
    print(f"PLAY_PROFILE_READY {profile}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
