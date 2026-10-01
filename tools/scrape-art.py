#!/usr/bin/env python3
"""Fill in missing box art from libretro's thumbnail collection.

    tools/scrape-art.py [--roms DIR] [--dry-run] [--force] [--system NAME]

Runs on the host against a ROM tree - the working library, or a card mounted
at /Volumes/TORTOS/Roms. Writes <System>/.media/<name>.png, which is where the
launcher already looks.

No account, no API key, no developer registration. That is the whole reason
this source was chosen: ScreenScraper refuses every call without a devid it
issues by hand, and TheGamesDB now refuses keyless requests outright.

MATCHING IS AGAINST THE DIRECTORY INDEX, NOT BY GUESSING FILENAMES, and the
difference is not small. Measured over 178 ROMs on 2026-08-29:

    exact filename only                       148/178   83%
    plus stripping (Translated) and friends   158/178   88%
    fetch the index, match normalized titles  174/178   97%

Guessing cannot find what it does not know to guess. Master System sat at 9/20
under every variant scheme because libretro carries `Sonic The Hedgehog (USA,
Europe, Brazil) (En)` and the card has the same game without the `(En)`. One
index fetch shows that immediately, and costs nine requests for the whole
library rather than one probe per ROM per guess.

The four it still misses are fan translations. Those are romhacks, absent from
any No-Intro-derived database under those names, so no provider has them and
they want art supplied by hand.
"""
import argparse
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = "https://thumbnails.libretro.com"
UA = "TortOS/1.0 (+box art fetch)"

# systems.cfg is the authority on which systems exist; this maps its folder
# names onto libretro's, which is a fact about someone else's server and has
# no business in a config the device reads.
LIBRETRO = {
    # A shelf can map to MORE THAN ONE collection: libretro files a machine's
    # disc games separately from its cartridges. TurboGrafx-16 is the case that
    # exposed it - 23 of one card's 27 missing covers were sitting in
    # "NEC - PC Engine CD - TurboGrafx-CD", 946 entries neither scraper had
    # ever looked at. Sega CD and the Famicom Disk System are the same shape.
    "Arcade": ["FBNeo - Arcade Games"],
    "Neo Geo": ["SNK - Neo Geo"],
    "NES": ["Nintendo - Nintendo Entertainment System",
            "Nintendo - Family Computer Disk System"],
    "SNES": ["Nintendo - Super Nintendo Entertainment System"],
    "PlayStation": ["Sony - PlayStation"],
    "Game Boy": ["Nintendo - Game Boy"],
    "Game Boy Color": ["Nintendo - Game Boy Color"],
    "Game Boy Advance": ["Nintendo - Game Boy Advance"],
    "Genesis": ["Sega - Mega Drive - Genesis",
                "Sega - Mega-CD - Sega CD"],
    "Master System": ["Sega - Master System - Mark III"],
    "Game Gear": ["Sega - Game Gear"],
    "TurboGrafx-16": ["NEC - PC Engine - TurboGrafx 16",
                      "NEC - PC Engine CD - TurboGrafx-CD"],
    # The two Pocket shelves, which this tool never had at all while the C port
    # did. Two machines, two catalogs: the mono Pocket's art is NOT in the
    # Color repo - checked, it 404s.
    "Neo Geo Pocket": ["SNK - Neo Geo Pocket"],
    "Neo Geo Pocket Color": ["SNK - Neo Geo Pocket Color"],
}


def systems_from_cfg(path):
    """(folder, extensions) per system, in shelf order, out of systems.cfg.

    The extensions matter: a ROM folder also holds .media/, and may hold a
    stray text file or a disc folder. Treating every file as a ROM makes the
    tool report missing art for things that are not games."""
    out = []
    try:
        for line in open(path, encoding="utf-8"):
            f = line.rstrip("\n").split("|")
            if len(f) > 7 and f[0] == "sys":
                exts = {e.strip().lower() for e in f[7].split(",") if e.strip()}
                out.append((f[2], exts))
    except OSError as e:
        sys.exit(f"scrape-art: cannot read {path}: {e}")
    return out


def norm(s):
    """A title with every parenthesized tag removed, for comparison only.

    Region, language, revision and dump tags are exactly what differs between
    two catalogs of the same game, and they are never what distinguishes two
    different games."""
    s = re.sub(r"\([^)]*\)", " ", s)
    s = re.sub(r"[^a-z0-9]+", " ", s.lower())
    return " ".join(s.split())


def get(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    return urllib.request.urlopen(req, timeout=timeout)


def index(remote):
    """Every box art filename libretro has for one system."""
    url = f"{BASE}/{urllib.parse.quote(remote)}/Named_Boxarts/"
    html = get(url).read().decode("utf-8", "replace")
    return [urllib.parse.unquote(m)[:-4]
            for m in re.findall(r'href="([^"]+\.png)"', html)]


def fetch(remote, name, dest):
    """Download one image, written through a temp file so an interrupted run
    never leaves a half-written PNG that later looks like cached art."""
    url = f"{BASE}/{urllib.parse.quote(remote)}/Named_Boxarts/{urllib.parse.quote(name + '.png')}"
    data = get(url).read()
    if not data.startswith(b"\x89PNG"):
        raise ValueError("not a PNG")
    tmp = dest + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dest)
    return len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", default=os.path.join(ROOT, "TortOS-Test-Set", "Roms"),
                    help="ROM tree to fill in (default: the working library)")
    ap.add_argument("--system", help="only this system folder")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="refetch art that is already present")
    a = ap.parse_args()

    if not os.path.isdir(a.roms):
        sys.exit(f"scrape-art: no such ROM tree: {a.roms}")

    wanted = systems_from_cfg(os.path.join(ROOT, "config", "systems.cfg"))
    if a.system:
        wanted = [t for t in wanted if t[0] == a.system]
        if not wanted:
            sys.exit(f"scrape-art: {a.system} is not in systems.cfg")

    got = missing = skipped = 0
    unmapped = []

    for folder, exts in wanted:
        d = os.path.join(a.roms, folder)
        if not os.path.isdir(d):
            continue
        remotes = LIBRETRO.get(folder)
        if not remotes:
            # Loudly. A system silently skipped looks exactly like a system
            # with complete art, and systems.cfg gains entries over time.
            unmapped.append(folder)
            continue

        roms = []
        for f in sorted(os.listdir(d)):
            if f.startswith(".") or not os.path.isfile(os.path.join(d, f)):
                continue
            stem, ext = os.path.splitext(f)
            if exts and ext[1:].lower() not in exts:
                continue
            roms.append(stem)
        if not roms:
            continue

        # Every collection for this shelf, merged, remembering which one each
        # name came from so the download asks the right catalog. The C port
        # fetches the second only when the first left something unmatched;
        # here they are always fetched, because one extra index on a host is
        # not worth a second pass to avoid.
        names, origin = [], {}
        failed = False
        for remote in remotes:
            try:
                got = index(remote)
            except (urllib.error.URLError, OSError) as e:
                # Distinguished on purpose: a TLS or network failure is not
                # "this game has no art", and reporting it as one is how a
                # certificate change gets mistaken for a missing game.
                print(f"  {folder:<20} INDEX FAILED ({remote}): {e}")
                failed = True
                break
            for n in got:
                origin.setdefault(n, remote)
            names.extend(got)
        if failed:
            continue

        exact = set(names)
        by = {}
        for n in names:
            by.setdefault(norm(n), []).append(n)

        media = os.path.join(d, ".media")
        n_got = n_miss = n_skip = 0
        for base in roms:
            dest = os.path.join(media, base + ".png")
            if os.path.exists(dest) and not a.force:
                n_skip += 1
                continue
            hit = base if base in exact else (by.get(norm(base)) or [None])[0]
            if not hit:
                n_miss += 1
                print(f"    no art: {folder}/{base}")
                continue
            if a.dry_run:
                n_got += 1
                continue
            try:
                os.makedirs(media, exist_ok=True)
                fetch(origin[hit], hit, dest)
                n_got += 1
            except Exception as e:
                n_miss += 1
                print(f"    FAILED: {folder}/{base}: {e}")

        got += n_got; missing += n_miss; skipped += n_skip
        print(f"  {folder:<20} {n_got:>3} fetched, {n_miss:>2} missing, "
              f"{n_skip:>3} already there   (index {len(names)})")

    print(f"\n  {'TOTAL':<20} {got:>3} fetched, {missing:>2} missing, "
          f"{skipped:>3} already there")
    if unmapped:
        print(f"\n  NO LIBRETRO MAPPING for: {', '.join(unmapped)}")
        print("  Add them to LIBRETRO in this file, or their art will never fill in.")
    if a.dry_run:
        print("  (dry run: nothing was written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
