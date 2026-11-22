#!/usr/bin/env python3
"""Produce the warp catalog the debug menu reads, from the decompilation's own tables.

    python tools/gen_warps.py --decomp upstream/oot --out "src/game/warp_table.ntsc-1.0.h"
    python tools/gen_warps.py --decomp upstream/oot --check    # fail if the committed file is stale

WHY THIS IS GENERATED RATHER THAN TYPED. The user asked for EVERY place a player can warp to,
nested by location. "Every" is the whole requirement: a list that is merely long looks identical
to a list that is complete, and nobody discovers the difference until the one scene they wanted is
missing. So the list is derived from `include/tables/entrance_table.h`, which is the table the game
itself indexes, and this tool REFUSES to write anything if a single scene in that table has no
place in the region map below. Completeness is then a property of the build rather than a claim in
a comment.

WHAT AN ENTRY IS. The entrance table has 1556 rows, but they are not 1556 destinations. Rows are
grouped into scene LAYERS (child day, child night, adult day, adult night, and for a few places
many more), and the decomp's own header says it plainly:

    Only the first entrance within a group of layers is expected to be referenced in code.
    The entrance system will apply the offset on its own to access the correct entrance
    for a given layer.

So the canonical destination is the FIRST row of each group, and the groups are separated by blank
lines in that file. That yields 357 destinations across 102 scenes. Using any row but the first
would warp to a layer directly and skip the game's own age and time of day selection, which is a
subtly wrong scene rather than an obvious failure.

THE ENTRANCES WITHIN A PLACE ARE NUMBERED, NOT NAMED, AND THAT IS A DELIBERATE LIMIT. A spawn
number identifies a door, but which door is in the scene's own spawn list, not in either of these
tables. Naming them from memory would be inventing, which this project forbids outright. So a place
with one way in is one row, and a place with several offers its numbered doors and their entrance
indices, which is exactly what the harness's --warp already takes.

VERSION LOCK. Everything here is NTSC-U 1.0: the indices, the scene set, the layer groupings. The
output file carries the version in its name so a second revision could never be mistaken for it.
"""

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

# ----------------------------------------------------------------------------------------------
# The region map: the one hand written thing in this tool, and the one that decides the shape of
# the menu.
#
# THE NESTING IS THE USER'S OWN EXAMPLE, taken literally. They asked for "kakariko village and
# nested would be windmill, cemetary holes etc", so a region holds the places that a person would
# say are IN it, whatever the game's file layout does. The windmill, the graveyard, the tomb and
# the two grave holes are separate scenes in the ROM and they are all Kakariko here, because that
# is where somebody looking for them would look.
#
# Dungeons sit under the region they are entered from rather than in a dungeon list of their own,
# for the same reason: the Fire Temple is up Death Mountain, and a person hunting a problem in the
# crater wants the crater and the temple side by side.
#
# EVERY SCENE IN THE ENTRANCE TABLE MUST APPEAR HERE EXACTLY ONCE. The tool checks both directions
# and refuses to write if either fails, so a decomp update that adds or renames a scene stops the
# build instead of quietly dropping a destination.
# ----------------------------------------------------------------------------------------------
REGIONS = [
    ("Kokiri Forest", [
        ("SCENE_KOKIRI_FOREST",          "Kokiri Forest"),
        ("SCENE_LINKS_HOUSE",            "Link's house"),
        ("SCENE_MIDOS_HOUSE",            "Mido's house"),
        ("SCENE_SARIAS_HOUSE",           "Saria's house"),
        ("SCENE_KOKIRI_SHOP",            "Kokiri shop"),
        ("SCENE_KNOW_IT_ALL_BROS_HOUSE", "Know-It-All Brothers' house"),
        ("SCENE_TWINS_HOUSE",            "The twins' house"),
        ("SCENE_DEKU_TREE",              "Inside the Deku Tree"),
        ("SCENE_DEKU_TREE_BOSS",         "Inside the Deku Tree: boss room"),
    ]),
    ("Lost Woods", [
        ("SCENE_LOST_WOODS",             "Lost Woods"),
        ("SCENE_SACRED_FOREST_MEADOW",   "Sacred Forest Meadow"),
        ("SCENE_FOREST_TEMPLE",          "Forest Temple"),
        ("SCENE_FOREST_TEMPLE_BOSS",     "Forest Temple: boss room"),
    ]),
    ("Hyrule Field", [
        ("SCENE_HYRULE_FIELD",           "Hyrule Field"),
    ]),
    ("Lon Lon Ranch", [
        ("SCENE_LON_LON_RANCH",          "Lon Lon Ranch"),
        ("SCENE_LON_LON_BUILDINGS",      "The house and the tower"),
        ("SCENE_STABLE",                 "The stable"),
    ]),
    ("Market", [
        ("SCENE_MARKET_ENTRANCE_DAY",    "Market entrance"),
        ("SCENE_MARKET_DAY",             "Market"),
        ("SCENE_BACK_ALLEY_DAY",         "Back alley"),
        ("SCENE_BACK_ALLEY_HOUSE",       "Back alley house"),
        ("SCENE_BAZAAR",                 "Bazaar"),
        ("SCENE_POTION_SHOP_MARKET",     "Potion shop"),
        ("SCENE_BOMBCHU_SHOP",           "Bombchu shop"),
        ("SCENE_BOMBCHU_BOWLING_ALLEY",  "Bombchu bowling alley"),
        ("SCENE_HAPPY_MASK_SHOP",        "Mask shop"),
        ("SCENE_TREASURE_BOX_SHOP",      "Treasure box shop"),
        ("SCENE_SHOOTING_GALLERY",       "Shooting gallery"),
        ("SCENE_DOG_LADY_HOUSE",         "The dog lady's house"),
        ("SCENE_MARKET_GUARD_HOUSE",     "Guard house"),
        ("SCENE_TEMPLE_OF_TIME_EXTERIOR_DAY", "Temple of Time grounds"),
        ("SCENE_TEMPLE_OF_TIME",         "Temple of Time"),
    ]),
    ("Hyrule Castle", [
        ("SCENE_HYRULE_CASTLE",              "Castle grounds"),
        ("SCENE_CASTLE_COURTYARD_GUARDS_DAY", "Courtyard: the guards"),
        ("SCENE_CASTLE_COURTYARD_ZELDA",     "Courtyard: the inner garden"),
    ]),
    ("Kakariko Village", [
        ("SCENE_KAKARIKO_VILLAGE",           "Kakariko Village"),
        ("SCENE_IMPAS_HOUSE",                "Impa's house"),
        ("SCENE_KAKARIKO_CENTER_GUEST_HOUSE", "The guest house"),
        ("SCENE_HOUSE_OF_SKULLTULA",         "House of Skulltula"),
        ("SCENE_POTION_SHOP_KAKARIKO",       "Potion shop"),
        ("SCENE_POTION_SHOP_GRANNY",         "Potion shop back room"),
        ("SCENE_WINDMILL_AND_DAMPES_GRAVE",  "The windmill and the grave below it"),
        ("SCENE_GRAVEYARD",                  "Graveyard"),
        ("SCENE_GRAVEKEEPERS_HUT",           "Gravekeeper's hut"),
        ("SCENE_ROYAL_FAMILYS_TOMB",         "Royal Family's tomb"),
        ("SCENE_REDEAD_GRAVE",               "The grave with the ReDeads"),
        ("SCENE_GRAVE_WITH_FAIRYS_FOUNTAIN", "The grave with the fairy's fountain"),
        ("SCENE_BOTTOM_OF_THE_WELL",         "Bottom of the Well"),
        ("SCENE_SHADOW_TEMPLE",              "Shadow Temple"),
        ("SCENE_SHADOW_TEMPLE_BOSS",         "Shadow Temple: boss room"),
    ]),
    ("Death Mountain", [
        ("SCENE_DEATH_MOUNTAIN_TRAIL",   "Death Mountain Trail"),
        ("SCENE_DEATH_MOUNTAIN_CRATER",  "Death Mountain Crater"),
        ("SCENE_GORON_CITY",             "Goron City"),
        ("SCENE_GORON_SHOP",             "Goron shop"),
        ("SCENE_DODONGOS_CAVERN",        "Dodongo's Cavern"),
        ("SCENE_DODONGOS_CAVERN_BOSS",   "Dodongo's Cavern: boss room"),
        ("SCENE_FIRE_TEMPLE",            "Fire Temple"),
        ("SCENE_FIRE_TEMPLE_BOSS",       "Fire Temple: boss room"),
    ]),
    ("Zora's River and Domain", [
        ("SCENE_ZORAS_RIVER",            "Zora's River"),
        ("SCENE_ZORAS_DOMAIN",           "Zora's Domain"),
        ("SCENE_ZORA_SHOP",              "Zora shop"),
        ("SCENE_ZORAS_FOUNTAIN",         "Zora's Fountain"),
        ("SCENE_JABU_JABU",              "Inside Jabu-Jabu"),
        ("SCENE_JABU_JABU_BOSS",         "Inside Jabu-Jabu: boss room"),
        ("SCENE_ICE_CAVERN",             "Ice Cavern"),
    ]),
    ("Lake Hylia", [
        ("SCENE_LAKE_HYLIA",             "Lake Hylia"),
        ("SCENE_LAKESIDE_LABORATORY",    "Lakeside laboratory"),
        ("SCENE_FISHING_POND",           "Fishing pond"),
        ("SCENE_WATER_TEMPLE",           "Water Temple"),
        ("SCENE_WATER_TEMPLE_BOSS",      "Water Temple: boss room"),
    ]),
    ("Gerudo Valley and the desert", [
        ("SCENE_GERUDO_VALLEY",          "Gerudo Valley"),
        ("SCENE_CARPENTERS_TENT",        "The carpenters' tent"),
        ("SCENE_GERUDOS_FORTRESS",       "Gerudo's Fortress"),
        ("SCENE_THIEVES_HIDEOUT",        "Thieves' Hideout"),
        ("SCENE_GERUDO_TRAINING_GROUND", "Gerudo Training Ground"),
        ("SCENE_HAUNTED_WASTELAND",      "Haunted Wasteland"),
        ("SCENE_DESERT_COLOSSUS",        "Desert Colossus"),
        ("SCENE_SPIRIT_TEMPLE",          "Spirit Temple"),
        ("SCENE_SPIRIT_TEMPLE_BOSS",     "Spirit Temple: boss room"),
    ]),
    ("Ganon's Castle", [
        ("SCENE_GANONS_TOWER",                    "Ganon's Tower"),
        ("SCENE_INSIDE_GANONS_CASTLE",            "Inside the castle"),
        ("SCENE_INSIDE_GANONS_CASTLE_COLLAPSE",   "Inside the castle, collapsing"),
        ("SCENE_GANONS_TOWER_COLLAPSE_INTERIOR",  "The tower collapsing: inside"),
        ("SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR",  "The tower collapsing: outside"),
        ("SCENE_GANONDORF_BOSS",                  "Ganondorf's chamber"),
        ("SCENE_GANON_BOSS",                      "The last fight"),
    ]),
    ("Fountains and grottos", [
        ("SCENE_GROTTOS",                       "Grottos"),
        ("SCENE_FAIRYS_FOUNTAIN",               "Fairy's fountain"),
        ("SCENE_GREAT_FAIRYS_FOUNTAIN_MAGIC",   "Great Fairy's fountain: magic"),
        ("SCENE_GREAT_FAIRYS_FOUNTAIN_SPELLS",  "Great Fairy's fountain: spells"),
    ]),
    ("Sacred Realm and cutscenes", [
        ("SCENE_CHAMBER_OF_THE_SAGES",   "Chamber of the Sages"),
        ("SCENE_CUTSCENE_MAP",           "Cutscene map"),
    ]),
    # The maps the developers left in. They are reachable through the entrance table like anything
    # else and several of them crash or render oddly, which is exactly why a debug menu should be
    # able to get to them rather than pretend they are not there.
    ("Unused and test maps", [
        ("SCENE_UNUSED_6E",              "Unused map 6E"),
        ("SCENE_HAIRAL_NIWA2",           "Unused castle courtyard"),
        ("SCENE_SASATEST",               "Test map: sasatest"),
        ("SCENE_SYOTES",                 "Test map: syotes"),
        ("SCENE_SYOTES2",                "Test map: syotes2"),
        ("SCENE_TESTROOM",               "Test map: testroom"),
        ("SCENE_SUTARU",                 "Test map: sutaru"),
        ("SCENE_TEST01",                 "Test map: test01"),
        ("SCENE_DEPTH_TEST",             "Test map: depth test"),
        ("SCENE_BESITU",                 "Test map: besitu"),
    ]),
]

# A row of the entrance table. The trailing arguments are transition types and are not captured,
# because some of them are macro calls with their own parentheses (TRANS_TYPE_CIRCLE(...)) and a
# regex that tried to take them would silently skip those rows. Skipping rows here would merge two
# layer groups into one and lose a destination, which is the failure this whole tool exists to
# prevent, so the pattern deliberately stops at the spawn number.
ROW = re.compile(r"^/\* (0x[0-9A-Fa-f]+) \*/ DEFINE_ENTRANCE\((\w+),\s*(\w+),\s*(\d+),")


def read_entrances(table: Path):
    """Return the first row of each layer group: [(index, enum name, scene, spawn), ...].

    Groups are separated by blank lines, which is the decomp's own convention and is documented in
    the header of the file being read. A group can be four rows (the usual age and time of day
    set) or sixteen (the Temple of Time, which has a layer for every state of the story), and the
    size is never assumed.
    """
    groups = []
    current = []
    for line in table.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        match = ROW.match(stripped)
        if match:
            current.append((int(match.group(1), 16), match.group(2), match.group(3), int(match.group(4))))
        elif not stripped and current:
            groups.append(current)
            current = []
    if current:
        groups.append(current)

    if not groups:
        sys.exit(f"no entrances parsed from {table}; has the table's format changed?")

    rows = sum(len(g) for g in groups)
    return [g[0] for g in groups], rows


def check_coverage(entrances):
    """Every scene in the table has a place, and every place in the map has a scene."""
    in_table = {e[2] for e in entrances}
    in_map = []
    for _, places in REGIONS:
        in_map.extend(scene for scene, _ in places)

    problems = []

    duplicates = {s for s in in_map if in_map.count(s) > 1}
    if duplicates:
        problems.append("listed in more than one region: " + ", ".join(sorted(duplicates)))

    missing = sorted(in_table - set(in_map))
    if missing:
        problems.append(
            "in the entrance table with nowhere to go (add each to REGIONS):\n    "
            + "\n    ".join(missing))

    extra = sorted(set(in_map) - in_table)
    if extra:
        problems.append(
            "in the region map but not in the entrance table (renamed or removed upstream):\n    "
            + "\n    ".join(extra))

    if problems:
        sys.exit("the region map and the entrance table disagree.\n\nScenes " +
                 "\n\nScenes ".join(problems))


def escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


def render(entrances, group_rows: int) -> str:
    by_scene = defaultdict(list)
    for index, enum, scene, spawn in entrances:
        by_scene[scene].append((index, spawn, enum))
    for rows in by_scene.values():
        rows.sort(key=lambda r: r[1])

    out = []
    w = out.append
    w("// GENERATED by tools/gen_warps.py. Do not edit; edit the tool's region map and run it.")
    w("//")
    w("// Every place a player can warp to in NTSC-U 1.0, nested by location: a region, the places")
    w("// in it, and where a place has more than one way in, its numbered entrances.")
    w("//")
    w("// The numbers are entrance indices, the same ones --warp takes on the command line and the")
    w("// same ones the game writes into nextEntranceIndex. Each is the FIRST row of its layer")
    w("// group, so the game still chooses the age and time of day layer itself.")
    w("//")
    w(f"// {len(REGIONS)} regions, {len(by_scene)} places, {len(entrances)} entrances,")
    w(f"// from {group_rows} rows of the decompilation's entrance table.")
    w("")
    w("#pragma once")
    w("")
    w("namespace oot::warps {")
    w("")
    w("    struct Entrance {")
    w("        const char* label;   // \"Entrance 4\", or the place's own name where there is only one")
    w("        int index;           // what goes to set_pending_warp")
    w("    };")
    w("")
    w("    struct Place {")
    w("        const char* name;")
    w("        const Entrance* entrances;")
    w("        int count;")
    w("    };")
    w("")
    w("    struct Region {")
    w("        const char* name;")
    w("        const Place* places;")
    w("        int count;")
    w("    };")
    w("")

    # One entrance array per place, named by region and place ordinal so nothing collides.
    for r, (region_name, places) in enumerate(REGIONS):
        for p, (scene, place_name) in enumerate(places):
            rows = by_scene[scene]
            w(f"    // {region_name} / {place_name} ({scene})")
            w(f"    inline constexpr Entrance ENTRANCES_{r}_{p}[] = {{")
            for index, spawn, enum in rows:
                if len(rows) == 1:
                    label = place_name
                else:
                    label = f"Entrance {spawn}"
                w(f'        {{ "{escape(label)}", 0x{index:03X} }},   // {enum}')
            w("    };")
            w("")

    for r, (region_name, places) in enumerate(REGIONS):
        w(f"    inline constexpr Place PLACES_{r}[] = {{")
        for p, (scene, place_name) in enumerate(places):
            count = len(by_scene[scene])
            w(f'        {{ "{escape(place_name)}", ENTRANCES_{r}_{p}, {count} }},')
        w("    };")
        w("")

    w("    inline constexpr Region REGIONS[] = {")
    for r, (region_name, places) in enumerate(REGIONS):
        w(f'        {{ "{escape(region_name)}", PLACES_{r}, {len(places)} }},')
    w("    };")
    w("")
    w(f"    inline constexpr int REGION_COUNT = {len(REGIONS)};")
    w(f"    inline constexpr int PLACE_COUNT = {len(by_scene)};")
    w(f"    inline constexpr int ENTRANCE_COUNT = {len(entrances)};")
    w("")
    w("} // namespace oot::warps")
    w("")
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--decomp", type=Path, default=Path("upstream/oot"),
                        help="the zeldaret/oot clone (default: upstream/oot)")
    parser.add_argument("--out", type=Path, default=Path("src/game/warp_table.ntsc-1.0.h"))
    parser.add_argument("--check", action="store_true",
                        help="do not write; exit non-zero if the file on disk is not what this "
                             "tool would produce")
    args = parser.parse_args()

    table = args.decomp / "include" / "tables" / "entrance_table.h"
    if not table.is_file():
        sys.exit(f"{table} not found. Point --decomp at a zeldaret/oot clone.")

    entrances, group_rows = read_entrances(table)
    check_coverage(entrances)
    text = render(entrances, group_rows)

    if args.check:
        if not args.out.is_file():
            sys.exit(f"{args.out} does not exist; run without --check to write it.")
        if args.out.read_text(encoding="utf-8") != text:
            sys.exit(f"{args.out} is stale; run tools/gen_warps.py to regenerate it.")
        print(f"{args.out} is up to date ({len(entrances)} entrances).")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    print(f"wrote {args.out}: {len(REGIONS)} regions, "
          f"{len({e[2] for e in entrances})} places, {len(entrances)} entrances "
          f"(from {group_rows} table rows).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
