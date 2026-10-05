# Configuration

**Settings live in a database, not in files.** Two of them, and the split is
deliberate:

| | holds | why it is separate |
|---|---|---|
| `.userdata/<platform>/tortos.db` | volume, brightness, Auto Sleep, Suspend Timeout, Auto Off, audio output, Wi-Fi, Bluetooth, display mode per system, and the RetroAchievements and ScreenScraper accounts | per handheld. A card moved to another device should not carry the first one's screen and speaker settings, or its account token |
| `.userdata/shared/.tortos/library.db` | timezone, startup system, turbo maps, core options | per card. It travels with the library, the same way favorites and earned achievements do |

The shipped defaults are **compiled into the launcher** and seed whichever
database is missing them, so there is no config file to ship, none to drift from
the code that reads it, and a deleted database comes back working.

## Reading it

Nothing on the device can open a database - there is no `sqlite3` binary - so
the launcher prints it:

```
tortos.elf --dump
```

That is deliberately read-only. Settings are not hand-edited any more; every one
of them is reachable from a menu.

## What the boot script reads

`launch.sh` needs five values before `tortos.elf` exists - the panel brightness
for before the boot animation, the timezone, and whether each radio should come
up. It is POSIX shell and cannot read a database, so the launcher exports
`.userdata/<platform>/boot.env` and the script sources it.

That file is **derived, never authoritative**. Delete it and the next boot runs
at the shipped defaults, then the next settings change rewrites it. It replaced
six `sed` invocations across four files, each one a fork, so the boot path got
shorter rather than longer.

## `systems.cfg`, which is still a file

`TortOS/systems.cfg` is the one that did not move, because it is **build input
rather than a setting**. `mk/payload.sh` reads it twice on the host: once to
refuse a card whose `systems.cfg` names cores that `vendor/` does not have -
which is how a card was nearly built with four of nine systems dead - and once
to create the ROM folders. Neither can wait for a database that only exists on
the device.

```
sys | display name | Roms/ folder | core | tag | card art | accent | extensions | disc bios
```

The **tag** keys the per-system display mode and the favorites list. It is not
what saves and states hang off - states are keyed on the folder and `.srm` files
are named after the ROM, in `Saves/<folder>/`. `system_cfg` declares `tag[8]`, so up
to seven characters, and changing a tag orphans that system's display mode and
favorites.

## Core options and turbo

Both are entries in the library database rather than files, seeded from the
values compiled into the launcher.

**Core options** are keyed `coreopt.<tag>.<option>`, with an empty tag for a
global - so `coreopt..mgba_sgb_borders` applies everywhere and
`coreopt.GB.mgba_gb_model` to Game Boy alone. A tagged entry overrides a global
of the same name. A core that does not declare a key ignores it, so a key meant
for one core is harmless everywhere else. They are sent **before** the game
loads, because a core reads its `(Restart)` options during load and one set
afterwards does nothing until the next launch.

**Game Boy palettes** are per game rather than per system: the in-game menu's
Palette row stores `palette.GB.<rom file>` (the label, e.g. `GB Pocket`), and a
game with no row plays **Auto** - what a Game Boy Color does with nothing held,
its own colours for the 144 games in its boot ROM and Dark Green for the rest.
Every Game Boy launch sends the game's `mgba_gb_colors_preset` and
`mgba_gb_colors` after the core options above, so a `coreopt.GB.mgba_gb_colors`
stored on an older card is overridden. A save state does not carry the palette.

**Multi-disc games** are a folder holding the discs and an `.m3u` that lists
them, one file name per line (`Roms/PlayStation/Policenauts/Policenauts.m3u`);
the folder shows on the shelf as one game. A game launched from an `.m3u` gets
a **Disc** row in the in-game menu, under Palette, whenever the core reports
more than one disc. Picking a disc opens the virtual tray, swaps, and closes it
after a second of play, which is when a game waiting for "insert disc 2" notices.
The choice is stored as `disc.<TAG>.<m3u file>` (0-based) and sent at the next
launch, so a game resumed from its auto-state finds the disc it was saved on.
Every disc shares the one memory card, named after the `.m3u`.

One trap worth repeating: **a resume state beats these.** A save state carries
the machine it was made on, so changing an option that selects hardware will not
appear to work on a game you have already played. Test on a game that has never
been launched, or delete its `.auto.state`.

**Turbo** is keyed `turbo.<tag>`, and [turbo.md](turbo.md) carries the
reasoning: which nine systems get it, why MD and SFC do not, and why PC Engine
is on the list despite its core having a turbo of its own.

## What the launcher still writes as files

Two things, each for a reason:

| | |
|---|---|
| `cheevos-active.set` | Diatom reads it, handed over as a path on RUN under its ADR-0026. Moving it would mean diatom linking sqlite and learning the schema |
| `systems.cfg` | build input rather than a setting, as above |

Everything else is in one of the two databases. Favorites are rows keyed
`fav.<tag>\t<file>` in the library, earned achievements are `chv.<game>.<id>`,
and the RetroAchievements account is `ra.user` and `ra.token` in the device
database.
