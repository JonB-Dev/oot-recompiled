"""Generate the website's roadmap from what has actually shipped and what is actually planned.

    python tools/roadmap_doc.py [--out <path>]

TWO HALVES, FROM TWO DIFFERENT PLACES, and neither is typed into the website:

  DONE      read from `assets/changelog.json`, which is the program's own release notes and is
            already written for the person using it. Anything that shipped is in there by
            definition, because a release without an entry is not shippable in this project.

  PLANNED   read from the phase titles in `.scaffold/upgrades/phases.md`, which is the real plan
            the work follows, paired with a plain description written here. The phase numbers are
            the link between the two: if a phase is added to the plan and nothing is written for
            it here, this refuses to generate rather than quietly leaving a gap on the page.

WHY NOT TYPE THE ROADMAP. A roadmap typed into a website is a promise nobody re-reads. This one
cannot claim something shipped that did not, because that half is the changelog; and it cannot
forget something planned, because that half is the plan.

WHAT IS DELIBERATELY NOT SHOWN: phase numbers, internal names, and anything about how the work is
done. A reader wants to know what the program will be able to do, not how the project is organized.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHANGELOG = ROOT / "assets" / "changelog.json"
PHASES = ROOT / ".scaffold" / "upgrades" / "phases.md"
DEFAULT_OUT = ROOT.parent / "oot-recompiled-site" / "src" / "data" / "roadmap.json"

PHASE_RE = re.compile(r"^### Phase (\d+): (.+)$", re.M)

# ----------------------------------------------------------------------------------------------
# What each planned phase means to somebody who wants to play the game. Written here because the
# plan's own titles are for whoever is building it ("acceleration structures from the frame" tells
# a player nothing), and grouped because a reader cares about the outcome, not the steps.
#
# `phases` lists which phases make up each entry, so a plan change that touches an undescribed
# phase is caught. `status` is one of: building, next, later.
# ----------------------------------------------------------------------------------------------
PLANNED = [
    dict(
        title="Upgraded textures, drawn from the game's own art",
        status="building",
        phases=[59, 60, 61, 62, 63],
        description=(
            "The game's textures are read out of your own copy, sorted into what they are, and "
            "rebuilt at a higher resolution. They load as a pack you can switch off, and nothing "
            "is ever redistributed: the work happens on your machine, from your file."
        ),
    ),
    dict(
        title="Faithful enlargement rather than reinvention",
        status="next",
        phases=[66, 67, 69, 70],
        description=(
            "Several different methods of enlarging a texture, compared against each other on the "
            "same images, with the results side by side so the one that keeps the original's "
            "character wins rather than the one that invents the most detail. Measured, not judged "
            "by eye alone."
        ),
    ),
    dict(
        title="Redrawn detail where enlargement is not enough",
        status="later",
        phases=[68, 71],
        description=(
            "Some things do not survive being made larger: icons, maps and menu backgrounds were "
            "drawn small on purpose. Those are redrawn to match, rather than stretched."
        ),
    ),
    dict(
        title="The game's own text, sharp at any size",
        status="next",
        phases=[64, 65],
        description=(
            "The interface and the game's text re-rendered from a fitted typeface and vector "
            "artwork, so menus and dialogue stay crisp however large the picture is, instead of "
            "being small images scaled up."
        ),
    ),
    dict(
        title="Better lighting on cards that cannot trace rays",
        status="next",
        phases=[75, 76, 77, 78, 79, 80, 81, 82],
        description=(
            "Ray traced lighting needs a graphics card that can trace rays, and a card that "
            "cannot is handed the game's original lighting and a settings screen where every row "
            "does nothing. The same four steps gain a second way of working: shadows cast by the "
            "sun and by a couple of nearby lights, and light worked out for every pixel rather "
            "than every corner of a shape. It is aimed at built-in graphics at sixty frames a "
            "second, and it can be chosen on a card that can trace as well, for anyone who would "
            "rather have the frames."
        ),
    ),
    dict(
        title="Everything checked in the game before it ships",
        status="later",
        phases=[72, 73],
        description=(
            "Every pack verified in the running game rather than in a viewer, and the whole "
            "process made repeatable, so a later version can be rebuilt and compared against this "
            "one."
        ),
    ),
    dict(
        title="Rounded geometry, behind a switch",
        status="later",
        phases=[74],
        description=(
            "The hard edges of the original models smoothed out, as an option you can turn off. "
            "It changes the shape of things, so it will never be on by default."
        ),
    ),
]

# Phases that are already built and therefore have nothing to promise. Listed so the completeness
# check below knows they were considered rather than forgotten.
ALREADY_BUILT = set(range(47, 59))


def read_planned() -> list[dict]:
    text = PHASES.read_text(encoding="utf-8")
    titles = {int(n): t.strip() for n, t in PHASE_RE.findall(text)}
    described = {p for entry in PLANNED for p in entry["phases"]}

    missing = sorted(set(titles) - described - ALREADY_BUILT)
    if missing:
        raise SystemExit(
            "these phases are in the plan and nothing on the roadmap describes them: "
            + ", ".join(f"{p} ({titles[p]})" for p in missing)
            + "\nAdd them to PLANNED in tools/roadmap_doc.py, or to ALREADY_BUILT if they have shipped."
        )
    unknown = sorted(described - set(titles))
    if unknown:
        raise SystemExit(f"the roadmap describes phases that are not in the plan: {unknown}")

    return [
        {"title": e["title"], "status": e["status"], "description": e["description"]}
        for e in PLANNED
    ]


def read_done() -> list[dict]:
    entries = json.loads(CHANGELOG.read_text(encoding="utf-8"))
    out = []
    for entry in entries:
        items: list[str] = []
        for key in ("added", "changed", "fixed"):
            items.extend(entry.get("sections", {}).get(key, []))
        out.append({
            "version": entry["version"],
            "date": entry.get("date"),
            "title": entry.get("title"),
            "summary": entry.get("summary"),
            "items": items,
        })
    return out


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    args = parser.parse_args()

    document = {
        "note": "Generated by tools/roadmap_doc.py from the changelog and the plan. Do not edit by hand.",
        "done": read_done(),
        "planned": read_planned(),
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    shipped = sum(len(r["items"]) for r in document["done"])
    print(f"wrote {out}")
    print(f"  {len(document['done'])} releases, {shipped} changes in them")
    print(f"  {len(document['planned'])} planned entries")

    # THE NOTES GO WITH IT, because keeping them by hand has already failed twice (2026-09-27:
    # 0.3.4 and 0.3.5 both shipped while the website still showed 0.3.3, because the site's copy
    # of the changelog was a duplicate somebody had to remember to update). The roadmap is
    # generated from this file, so the file itself travels the same way and the two can no longer
    # disagree. The website reads a copy rather than reaching into this repository, so a site
    # build on a machine without it still works.
    notes = out.parent / "changelog.json"
    notes.write_text(CHANGELOG.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
    print(f"wrote {notes}")


if __name__ == "__main__":
    main()
