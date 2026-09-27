#!/usr/bin/env python3
"""Import EmulationStation-style gamelist.xml / miyoogamelist.xml metadata into
TortOS's library.db, for a card that already has this metadata from elsewhere
and should not need a ScreenScraper account or a live lookup on-device.

    tools/gamelist-import.py [--roms DIR] [--system NAME] --out FILE [--dry-run]

This writes the tab-separated format `tortos --meta` already reads
(src/main.c ~line 10165: "the scrape runs on a computer for now... so the
rows travel as a file"). Emitting that file is this script's whole job -
getting it onto the card and applying it is the same unchanged second step a
ScreenScraper dump already uses:

    adb push out.meta /tmp/x.meta && adb shell tortos --meta /tmp/x.meta

FIELD MAPPING (ES gamelist -> TortOS game_meta, src/db.h):

    <desc>                      -> synopsis          direct
    <releasedate> YYYYMMDDT...  -> year               first 4 digits
    <developer>                 -> developer          direct
    <publisher>                 -> publisher          direct
    <genre>                     -> genres             direct
    <players>                   -> players            direct (already "1"/"1+"/"2")
    <rating> 0.0-1.0            -> note (out of 20)    x20, rounded; see note_from_rating
    (no ESRB field in ES)       -> esrb                left blank
    <path>                      -> file                basename only
    parent directory            -> folder              must be a Roms/<folder>
                                                        TortOS's own systems.cfg names

NOT MAPPED, ON PURPOSE:

    <name>       TortOS never reads a title from the db - lib_title() derives
                 the shelf title from the ROM filename always (src/library.c
                 ~line 112). Making a cleaned-up <name> appear on the shelf
                 needs a separate schema+UI change; out of scope here.
    <playcount>, <lastplayed>, <gametime>
                 TortOS's own Play Time (src/stats.c) lives in the `settings`
                 table under "sess.<start>.<clock>.<tag>" keys, not the games
                 table this writes - confirmed by reading GAMES_SCHEMA
                 (src/db.c) and stats.h, not assumed. There is no column here
                 for these fields to land in even by accident.

The `scraped` column (src/db.c db_game_set) is stamped with the current time
by the C side on every write, same as a live ScreenScraper hit - so a later
re-scrape pass correctly treats an imported row as already done rather than
re-fetching it.
"""
import argparse
import os
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMELIST_NAMES = ("gamelist.xml", "miyoogamelist.xml")


def systems_from_cfg(path):
    """Known Roms/ folder names, out of systems.cfg - so a gamelist.xml sitting
    under a typo'd, renamed, or unsupported folder is reported rather than
    silently written under a folder TortOS's own shelf will never scan."""
    out = set()
    try:
        for line in open(path, encoding="utf-8"):
            f = line.rstrip("\n").split("|")
            if len(f) > 2 and f[0] == "sys":
                out.add(f[2])
    except OSError as e:
        sys.exit(f"gamelist-import: cannot read {path}: {e}")
    return out


def year_from_releasedate(s):
    """First four digits of an ES releasedate (YYYYMMDDT000000), or "" - any
    other shape is a game with no known year, not a parse error."""
    if s and len(s) >= 4 and s[:4].isdigit():
        return s[:4]
    return ""


def note_from_rating(s):
    """ES stores 0.0-1.0; TortOS's note is the source's own score out of 20
    (src/db.h). 0.0 is ES's sentinel for "never rated" - the same "blank means
    unscored" convention a real ScreenScraper miss already uses (src/ss.h) -
    so a literal 0/20 would read as "rated the worst possible score" when the
    truth is nobody rated it. Left blank rather than written as 0."""
    try:
        v = float(s)
    except (TypeError, ValueError):
        return ""
    if v <= 0.0:
        return ""
    return str(round(v * 20))


def clean(s):
    """A tab or newline would corrupt the tab-separated header line these
    fields sit on - --meta's own doc comment is why the synopsis alone is
    byte-counted instead of line-delimited. Neither character is expected in
    a title/publisher/genre string in practice, but a stray one silently
    shifting every field after it is worse than a space."""
    return (s or "").replace("\t", " ").replace("\n", " ").replace("\r", " ").strip()


def parse_gamelist(path):
    """One gamelist.xml/miyoogamelist.xml -> a list of (file, fields, synopsis).
    Malformed XML is reported and skipped, not fatal - one bad file should not
    block every other system's import."""
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as e:
        print(f"gamelist-import: {path}: {e}", file=sys.stderr)
        return []

    rows = []
    for g in root.findall("game"):
        p = g.findtext("path")
        if not p:
            continue
        file = clean(os.path.basename(p))
        if not file:
            continue
        fields = {
            "year": year_from_releasedate(clean(g.findtext("releasedate"))),
            "publisher": clean(g.findtext("publisher")),
            "developer": clean(g.findtext("developer")),
            "players": clean(g.findtext("players")),
            "genres": clean(g.findtext("genre")),
            "esrb": "",
            "note": note_from_rating(g.findtext("rating")),
        }
        rows.append((file, fields, g.findtext("desc") or ""))
    return rows


def write_meta(path, records):
    """The exact wire shape src/main.c's --meta reader expects: nine
    tab-separated fields, a byte count, a newline, that many raw synopsis
    bytes, then one more newline. Written in binary so the byte count always
    matches what was actually written, regardless of platform line-ending
    translation."""
    with open(path, "wb") as out:
        for folder, file, f, synopsis in records:
            body = synopsis.encode("utf-8")
            head = "\t".join([folder, file, f["year"], f["publisher"],
                               f["developer"], f["players"], f["genres"],
                               f["esrb"], f["note"], str(len(body))])
            out.write(head.encode("utf-8") + b"\n")
            out.write(body)
            out.write(b"\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", default=os.path.join(ROOT, "TortOS-Test-Set", "Roms"),
                    help="ROM tree to read gamelist.xml/miyoogamelist.xml from "
                         "(default: the working library)")
    ap.add_argument("--system", help="only this system folder")
    ap.add_argument("--out", help="write the --meta file here (required unless --dry-run)")
    ap.add_argument("--dry-run", action="store_true",
                    help="parse and report; write nothing")
    a = ap.parse_args()

    if not a.dry_run and not a.out:
        sys.exit("gamelist-import: --out is required (or pass --dry-run)")
    if not os.path.isdir(a.roms):
        sys.exit(f"gamelist-import: no such ROM tree: {a.roms}")

    known = systems_from_cfg(os.path.join(ROOT, "config", "systems.cfg"))
    if a.system and a.system not in known:
        sys.exit(f"gamelist-import: {a.system} is not in systems.cfg")

    records = []
    unmapped = []
    for folder in sorted(os.listdir(a.roms)):
        if a.system and folder != a.system:
            continue
        d = os.path.join(a.roms, folder)
        if not os.path.isdir(d):
            continue
        gamelist = next((p for n in GAMELIST_NAMES
                          if os.path.isfile(p := os.path.join(d, n))), None)
        if not gamelist:
            continue
        if folder not in known:
            # Loudly, the same reasoning scrape-art.py's "unmapped" list uses:
            # a gamelist silently skipped looks exactly like a gamelist with
            # nothing worth importing.
            unmapped.append(folder)
            continue
        for file, fields, synopsis in parse_gamelist(gamelist):
            records.append((folder, file, fields, synopsis))

    if unmapped:
        print("gamelist-import: skipped, not in systems.cfg: "
              + ", ".join(unmapped), file=sys.stderr)

    if a.dry_run:
        for folder, file, f, synopsis in records:
            print(f"{folder}/{file}: {f['year'] or '?'} "
                  f"{f['publisher']} / {f['developer']} "
                  f"({len(synopsis.encode('utf-8'))}b synopsis)")
        print(f"{len(records)} game(s) would be written", file=sys.stderr)
        return

    write_meta(a.out, records)
    print(f"{len(records)} game(s) written to {a.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
