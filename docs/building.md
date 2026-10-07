# Building TortOS

Everything cross-compiles in TortOS's own toolchain image - a stock Debian
cross-compiler pinned by digest, built by `mk/toolchain.Dockerfile`, linking
against the device's own SDL2 in `sysroot/`. So the only host requirements are
Docker and (for regenerating art) Python with Pillow, plus ffmpeg.

```sh
make toolchain  # the cross-compiler      -> tortos-toolchain   (once)
mk/fetch-sysroot.sh  # the device's SDL2  -> sysroot/           (once, needs adb)
make            # the launcher            -> build/tortos.elf
make vendor     # five libretro cores     -> vendor/
mk/build-mgba-bridge.sh  # the sixth      -> vendor/            (see below)
make payload    # the installable card    -> out/sd/ and out/plorpOS-brick-v$(VERSION).zip
make native     # host build of the launcher, for working on how it looks
make boot       # regenerate the boot animation
make check      # every check below, offline, in a second or two
```

**mGBA is built rather than fetched, and is meant to stop being.** Every MBC2
Game Boy cartridge segfaults on the buildbot core - Kirby's Pinball Land, Wave
Race, Golf, X, both Final Fantasy Legends - a regression reported as
mgba-emu/mgba#3859 and fixed upstream the same day, but in no downloadable core
because libretro builds mGBA from a fork that has not synced.
`mk/build-mgba-bridge.sh` builds that same pinned tree with only upstream's own
fix applied, hardened to match the official build. Without it `make payload`
refuses the card rather than shipping one whose Game Boy, Game Boy Color and
Game Boy Advance shelves open onto nothing. Restore the fetch and delete the
script the day that fork syncs.

`make payload` needs a built diatom binary (`DIATOM_ELF`, defaulting to a
sibling checkout).

**The other ports** build the same way with `PLATFORM=` on every target, into
`build/<platform>/` against a sysroot of their own:

```sh
mk/fetch-gkd-sysroot.sh        # GKD 350H Ultra (ROCKNIX)      -> sysroot-gkd/
make PLATFORM=gkd && make PLATFORM=gkd payload
mk/fetch-h700-sysroot.sh       # Anbernic H700 (BaseOS)        -> sysroot-h700/
make PLATFORM=h700 && make PLATFORM=h700 payload
```

BaseOS has no SDL2 or FFmpeg of its own, so `mk/fetch-h700-sysroot.sh` builds
both from source (about ten minutes) and `make PLATFORM=h700 payload` ships
those libraries in `TortOS/lib/`. Its FFmpeg is configured with
`--disable-everything` and a short filter list: a filter Muse starts using
must be added to that list, or every track fails to open on the H700 alone.

`make check` runs before every commit. Each part can be run alone, and each
exists because something once broke in a way nothing noticed:

| | |
|---|---|
| `check-menus` | what each menu CONTAINS in a given state, with no renderer and no device |
| `check-audioout` | where sound goes given a cable, a headset and a setting - all eight combinations |
| `check-idle` | the idle clock behind Auto Sleep and Auto Off, including the charger case and the counter wrapping |
| `check-cheevos` | the achievement half: parsing and filtering |
| `check-rahash` | the C and Python ROM hashers agree, over the whole library |
| `check-raset` | the C and Python set converters agree (needs `RA_USER`/`RA_PASS`) |
| `check-artscrape` | two name normalizers agree on every candidate |
| `check-artrun` | whole Box Art runs against a fake network: Replace keeps a cover it cannot better, and a checksum names the ROM in a zip, not the readme beside it |
| `check-hare` | nothing on the file server is reachable without the PIN |
| `check-httpd` | request parsing, including the malformed ones |
| `check-xfer` | upload paths cannot escape the directory they were aimed at |
| `check-db` | a shipped default never overwrites a choice, and the two scopes stay apart |
| `check-stats` | play time is recorded, and a LAUNCH still writes nothing |
| `check-sort` | a shelf sorts the way it says it does, and the same shelf twice the same way |
| `check-bt` | a device name from the air is only ever data; an address is validated |
| `check-backlog` | the backlog still says what is left, and has been swept recently |

They are offline and need no device. A screen's rows are a pure function of
its state precisely so the first two can exist - see `docs/decisions/`.

The host build renders exactly what the handheld renders, and can be asked for
a single frame:

```sh
TORTOS_ROOT=… TORTOS_ROMS=… TORTOS_FONT=res/fonts/menu.ttf \
  build-native/tortos --shot /tmp/shelf.png --screen games
```

`--menu [row]` draws the TortOS menu over that shelf, and `--slots <n>
[aspect]` draws one frame of the save/load carousel over synthetic game
frames - the two screens that otherwise need a game running on a device before
they can be looked at. `--jump <n>` applies n letter-jumps first (negative for
up), so where the d-pad lands on a real library can be checked without a hand
on the device. Every shot names the screen and the focused item on stderr, so
a sequence of them reads back as a list of what was actually drawn.

## On device

```sh
make adb          # push everything over USB
make adb-elf      # just the launcher
make adb-restart  # kill the launcher so launch.sh picks the new one up
make adb-log
```

**Never `kill` `launch.sh` itself.** The boot hook's failsafe powers the device
off when the launch loop exits.

## Layout

```
src/            the launcher (MIT)
docs/           the guide, how it works, configuration and building; why the
                code is shaped the way it is, in docs/decisions/; and the
                rules a menu follows, in docs/menus.md
mk/             cross build, payload, deployment
tools/          the boot-animation and card generators, setbright, the
                achievement fetcher, and the checks
res/            the boot animation, the system cards, the font, Over The
                Hare's page, and the marks and screenshots the README shows
config/         systems.cfg as shipped; the rest is compiled in
sd/             the boot hook and launch.sh as they land on the card
sysroot/        fetched: the device's own SDL2, for linking (mk/fetch-sysroot.sh)
vendor/         the libretro cores: five fetched and hash-pinned, mGBA
                built by mk/build-mgba-bridge.sh
```
