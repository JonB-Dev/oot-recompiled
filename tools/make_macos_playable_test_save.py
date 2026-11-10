#!/usr/bin/env python3
"""Make an ignored test profile that skips only OoT's opening cutscene.

The user's original save is read-only. Offsets and checksum match the pinned
zeldaret/oot include/save.h and src/code/z_sram.c for NTSC-U 1.0.
"""

import argparse
import shutil
from pathlib import Path


SRAM_SIZE = 0x8000
SAVE_SIZE = 0x1354
CHECKSUM_OFFSET = 0x1352
SLOT_OFFSETS = (0x20, 0x3D10)
OPENING_CUTSCENE = 0xFFF1
NO_CUTSCENE = 0
LINKS_HOUSE_ENTRANCE = 0xBB
# SaveInfo.infTable[0] at Save offset 0x1C + 0x0EDC. Bit 0 marks
# INFTABLE_SARIA_GREETED_LINK; see pinned z_en_sa.c and save.h.
SARIA_INF_TABLE_OFFSET = 0xEF8


def save_checksum(save: bytes | bytearray) -> int:
    if len(save) != SAVE_SIZE:
        raise ValueError("unexpected OoT save size")
    data = bytearray(save)
    data[CHECKSUM_OFFSET:CHECKSUM_OFFSET + 2] = bytes(2)
    return sum(int.from_bytes(data[i:i + 2], "big")
               for i in range(0, SAVE_SIZE, 2)) & 0xFFFF


def patch_opening_cutscene(sram: bytes | bytearray, skip_saria_greeting: bool = False) -> bytes:
    if len(sram) != SRAM_SIZE:
        raise ValueError("expected a 32 KiB OoT SRAM file")
    result = bytearray(sram)
    saves = [bytes(result[offset:offset + SAVE_SIZE]) for offset in SLOT_OFFSETS]
    if saves[0] != saves[1]:
        raise ValueError("file 1 and its backup differ")
    for offset in SLOT_OFFSETS:
        save = bytes(result[offset:offset + SAVE_SIZE])
        if int.from_bytes(save[CHECKSUM_OFFSET:CHECKSUM_OFFSET + 2], "big") != save_checksum(save):
            raise ValueError("file 1 checksum is invalid")
        if (int.from_bytes(save[0:4], "big") != LINKS_HOUSE_ENTRANCE or
                int.from_bytes(save[4:8], "big") != 1 or
                int.from_bytes(save[8:12], "big") != OPENING_CUTSCENE):
            raise ValueError("file 1 is not the expected child opening save")
        result[offset + 8:offset + 12] = NO_CUTSCENE.to_bytes(4, "big")
        if skip_saria_greeting:
            flag_at = offset + SARIA_INF_TABLE_OFFSET
            flags = int.from_bytes(result[flag_at:flag_at + 2], "big")
            result[flag_at:flag_at + 2] = (flags | 1).to_bytes(2, "big")
        result[offset + CHECKSUM_OFFSET:offset + CHECKSUM_OFFSET + 2] = save_checksum(
            result[offset:offset + SAVE_SIZE]).to_bytes(2, "big")
    return bytes(result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_save", type=Path)
    parser.add_argument("profile", type=Path,
                        help="new ignored profile under this repository's build directory")
    parser.add_argument("--settings", type=Path,
                        help="optional settings.txt to copy into the test profile")
    parser.add_argument("--skip-saria-greeting", action="store_true",
                        help="mark Saria's first greeting as seen in the copied profile")
    args = parser.parse_args()
    build_root = (Path(__file__).resolve().parent.parent / "build").resolve()
    profile = args.profile.resolve()
    if not profile.is_relative_to(build_root) or profile == build_root:
        parser.error("profile must be a new directory under build/")
    if profile.exists():
        parser.error("profile already exists; choose a new name to preserve its saves")
    original = args.source_save.read_bytes()
    patched = patch_opening_cutscene(original, skip_saria_greeting=args.skip_saria_greeting)
    (profile / "saves").mkdir(parents=True)
    (profile / "saves" / "oot.n64.us.1.0.bin").write_bytes(patched)
    if args.settings:
        shutil.copyfile(args.settings, profile / "settings.txt")
    print(f"PLAYABLE_TEST_PROFILE_OK {profile}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
