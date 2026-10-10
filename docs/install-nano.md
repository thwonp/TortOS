# Installing on an Anbernic RG Nano

plorpOS nano is a super stripped down plorp for the **RG Nano**, it has a simple UX and music can keep playing over a game.

- **Minimal UI**: your consoles, their games, Recently Played,
  Favorites, and Music
- **Muse** music player, works in the background
- **Fully updated PicoArch cores** for all platforms via ([plorpOS's fork](https://github.com/thwonp/picoarch/tree/plorpos-nano)
  of [DrUm78's](https://github.com/DrUm78/picoarch), with a Music page in its
  menu
- **Native PICO-8** support
- **Simple** folder structure - BIOS and Saves are easy to find

It is a rebundled fork of [DrUm78's FunKey OS build](https://github.com/DrUm78/FunKey-OS/releases/tag/fps-classics)
for the RG Nano with plorpOS installed in place of FunKey's menus, the
updated cores, and no extra junk on the card. FunKey's volume and brightness
keys, its game menu and its power key work as before.

## 1. Flash the card

Download **`plorpOS-nano-v<version>-sdcard.zip`**, unzip the image, and flash it to a microSD
card with [Rufus](https://rufus.ie/en/) or any other image tool. Flashing **wipes the card**.

Put the card in the Nano and turn it on. The first start sets the card up -
FunKey's six steps on the screen, under a minute - and then the shelf comes
up.

## 2. Copy your files

Power off the Nano (hold the power button) and put the card in the computer -
or leave it in and connect the Nano by USB: it starts as a USB drive (mount button is in settings). The
card's big partition holds `plorpOS`, `Music`, `Bios`, `Roms` (one folder per
console) and `FunKey`. **Do not format the other partitions if Windows asks you to.**

## What goes where on the card

Put music in `Music/`, one folder per artist and one per album inside it
(`Music/<artist>/<album>/<tracks>`). MP3, FLAC, AAC/M4A, Ogg and Opus play.

Games go in `Roms/`, one folder per console, named as on the other plorpOS
devices. plorpOS shows a console once its folder has a game in it.

| Folder in `Roms/` | Shown as | Files |
|---|---|---|
| `Game Boy` | Game Boy | `.gb` `.dmg` `.zip` |
| `Game Boy Color` | Game Boy Color | `.gbc` `.zip` |
| `Game Boy Advance` | Game Boy Advance | `.gba` `.agb` `.gbz` `.bin` `.zip` |
| `NES` | NES | `.nes` `.fds` `.unf` `.unif` `.zip` |
| `SNES` | Super NES | `.smc` `.sfc` `.fig` `.swc` `.gd3` `.gd7` `.dx2` `.bsx` `.zip` |
| `Genesis` | Genesis | `.md` `.gen` `.smd` `.bin` `.32x` `.cue` `.iso` `.chd` `.cso` `.m3u` `.68k` `.sgd` `.pco` `.zip` |
| `Master System` | Master System | `.sms` `.gg` `.sg` `.sc` `.bin` `.zip` |
| `Game Gear` | Game Gear | `.gg` `.zip` |
| `TurboGrafx-16` | PC Engine | `.pce` `.sgx` `.cue` `.ccd` `.chd` `.toc` `.m3u` `.zip` |
| `Neo Geo Pocket` | Neo Geo Pocket | `.ngp` `.ngc` `.ngpc` `.npc` `.zip` |
| `WonderSwan` | WonderSwan | `.ws` `.wsc` `.pc2` `.zip` |
| `Atari Lynx` | Atari Lynx | `.lnx` `.lyx` `.o` `.zip` |
| `Pokemon Mini` | Pokemon Mini | `.min` `.zip` |
| `Pico-8` | PICO-8 | `.p8` `.png` `.zip` |
| `PlayStation` | PlayStation | `.cue` `.bin` `.chd` `.pbp` `.m3u` `.iso` `.img` `.mdf` `.toc` `.cbn` |
| `Arcade (MAME 2000)` | Arcade (MAME) | `.zip` |
| `Arcade (FBA 2012)` | Arcade (FBA) | `.zip` |

Arcade games keep their set names (`mslug.zip`); the lists show their titles
(Metal Slug: Super Vehicle-001), from each core's own romset list.
Vertical arcade games come up upright; PicoArch's **Screen rotation** (in the
game menu's settings) turns the picture further if you hold the Nano sideways.

**BIOS files** go loose in `Bios/`. None is required:

| System | File | Without it |
|---|---|---|
| Game Boy Advance | `gba_bios.bin` | a built-in BIOS; a few games misbehave |
| PlayStation | `scph1001.bin` (or `scph5501.bin`, `scph7001.bin`) | a built-in BIOS; less compatible |
| PC Engine CD | `syscard3.pce` | CD games don't start (cards play) |
| Pokemon Mini | `bios.min` | a built-in BIOS |
| Atari Lynx | `lynxboot.img` | none needed - FunKey ships it and plorpOS puts it there |
| PICO-8 | `pico8_dyn` and `pico8.dat`, from your native PICO-8's Raspberry Pi download | the fake-08 core plays carts |

**Saves** - battery saves and save states, the power key's too - go in
`Saves/<console folder>/`, made at the first play, named after the game.
PicoArch's settings stay in `FunKey/.picoarch/`; keep that folder.

**Native PICO-8.** With `pico8_dyn` and `pico8.dat` in `Bios/`, the PICO-8
list's footer shows **R fake08** or **R pico8**: R switches the engine for
PICO-8 games, and the footer shows the one in use. Native PICO-8 has no save
states; its cartridge data is in `Saves/Pico-8/native/`. A power tap in it
opens the Nano's menu (music, volume, brightness, exit).

## Using it

| Button | On the shelf | On Now Playing |
|---|---|---|
| D-pad | move; left/right a page | left/right seek 10 s |
| A | open, play | pause / play |
| B | back | back |
| X | Now Playing | the queue |
| Y | a game: Favorites on/off | play mode (in order, repeat all, repeat one, shuffle) |
| L / R | R in the PICO-8 list: fake08 / native PICO-8 | previous / next track |
| START | pause / play, anywhere | pause / play |
| Power (tap) | dim to Now Playing | brightness back |

**Settings > Controls** shows the same, a page per place (Left/Right).

**FN is the SELECT button.** Volume and brightness are FunKey's keys
everywhere: **SELECT + A / Y** and **SELECT + X / B**; **SELECT + Up** saves a
screenshot to `Screenshots/`.

**In a game**, tap the **power** button: that is the Nano's MENU. The first
page is **MUSIC**: the track playing, A to pause or play, left/right for the
previous or next track. Up/Down change pages, B closes the menu. While music
plays the game is silent; pause the music and the game's sound comes back. The
rest of the menu is FunKey's: volume, brightness, save, load, aspect ratio,
exit.

**Favorites:** Y on a game adds it or takes it off; Favorites appears on the
shelf, A-Z, under Recently Played, once it has a game.

**Dimmed Music Mode** A power tap outside a game dims the screen to Now Playing (the
lowest backlight - the Nano's screen cannot go fully dark) and the music goes
on. Tap again for the brightness you had. While dimmed, B and X do nothing; with
**Dimmed button lock** on, nothing but the power tap does.

**Settings** (last on the shelf; the title shows the version):

- **Output**, **Library** - where the music plays and how many tracks it found.
- **Mount card on computer** - the Nano's card appears on the computer as a
  USB drive; the music stops. Eject it on the computer, then press B. Shown
  only when the Nano started as a USB drive.
- **USB at start** - USB drive, Network (ssh) or adb, after a restart (see
  Connecting).
- **Inactive shutdown** - 1, 2, 5, 10 or 30 minutes, or Never (default 5): the
  Nano turns off after that long with no button pressed and no music playing,
  anywhere outside a game. Time in a game does not count.
- **Dimmed button lock** - On (default): while dimmed, only a power tap does
  anything.
- **Restart**, **Power off** - press A twice.
- **Controls** - the button guide.

Left/Right change a setting's choice; A steps to the next one.

The header shows the battery (`+` while charging) and, beside it, whether music
is playing.

## USB DAC

Plug it into the Nano's USB-C port. The music moves to it within two seconds,
and a game started while it is plugged in plays through it too. Pull it out and
the music stops, paused, on the speaker.

The Nano has one USB port, so a DAC and a computer cannot be connected at the
same time.

## Updating

Download **`plorpOS-nano-v<version>-UPDATE.zip`**, unzip it, and copy the `plorpOS`
folder inside over the one on the card (USB drive, or the card in the
computer). At the next start the Nano installs it - FunKey's message
**INSTALLING PLORPOS** - and restarts once by itself. Your settings, saves and
lists stay. If an install fails the Nano starts the version it had and leaves
`plorpOS/update-failed.log` on the card.

**From plorpOS nano v0.2**: games moved into `Roms/`, and some console folders
were renamed (table above). Flash this image, or move the console folders
into `Roms/` by hand and rename `Sega Genesis`, `Sega Master System`,
`PCE-TurboGrafx`, `PS1`, `MAME 2000` and `Final Burn Alpha 2012` - in
`Saves/` too (`PICO-8` and `Atari lynx` work as they are: the card ignores
case). Recently Played and Favorites forget the games of renamed folders.

**From plorpOS nano v0.1** (the adb install): flash this image once. Flashing
wipes the card, so first copy off your music, games, BIOS files and
`FunKey/.picoarch/` (your saves are in its `data/` folders). Afterwards put
BIOS files in `Bios/` and copy the `.srm` / `.sav` / `.st*` files of each game
into `Saves/<console folder>/`; their names already match.

## Connecting

A shell is not needed to install or update, only for poking around. FunKey's
USB modes are chosen in plorpOS's **Settings > USB at start**, after a
restart, or by a file at the root of the card:

| File at the root of the drive | The Nano is |
|---|---|
| none | a USB drive (copying files) |
| `usbnet` | a network device: `ssh root@192.168.137.2`, password `funkey`, after giving the computer's side `192.168.137.1/24` |
| `adb` | an Android Debug Bridge device: `adb shell` ([platform-tools](https://developer.android.com/tools/releases/platform-tools)) |

## Notes

- **What it keeps where.** plorpOS: `/mnt/plorpOS/` (`recent.txt`,
  `favorites.txt`, `settings.txt`, and `queue.txt` - the queue and place, kept
  over a restart, stopped until you press play). Saves: `/mnt/Saves/<console folder>/`. BIOS
  files: `/mnt/Bios/`. Screenshots: `/mnt/Screenshots/`. PicoArch settings: `/mnt/FunKey/.picoarch/`. Logs: `/tmp/nanoshelf.log`, gone at restart.
- **Full speed with music playing** was measured for Game Boy, Game Boy Advance
  and Genesis. Heavier systems may slow down while music decodes.
- **Resume.** When a game exits, FunKey saves its place, and the next start of
  that game asks: **RESUME GAME** (A, at once) or **NEW GAME** (asks to be
  sure); B goes back to the shelf with the saved place kept.
- **Power.** Holding power in a game saves and turns off, as before. The next
  start goes straight back into that game - through plorpOS, so the MUSIC page
  works - and the shelf comes up when you leave it. Holding power on the shelf
  turns off; **Power off** in Settings does the same, cleanly.

## Known issues

- After a USB DAC is unplugged, the computer connection (adb, network) may not
  come back until a restart; once, the Nano restarted by itself.
- The speaker can pop on pause/play and when a DAC is plugged in or out.

## Removing plorpOS

Flash DrUm78's FunKey-OS image
([`FunKey-sdcard-DrUm78_RG_Nano.img`](https://github.com/DrUm78/FunKey-OS/releases/tag/fps-classics))
in its place. That wipes the card; copy off what you want to keep first.

## Sources and licences

- The card image is DrUm78's FunKey-OS `fps-classics` image for the RG Nano
  (FunKey-OS 2.3.0; GPL and other licences, source:
  [DrUm78/FunKey-OS](https://github.com/DrUm78/FunKey-OS/tree/fps-classics),
  mirrored at [thwonp/FunKey-OS](https://github.com/thwonp/FunKey-OS/tree/fps-classics)),
  with plorpOS installed by `mk/nano-image.sh` in this repository, which says
  every change.
- Its kernel is DrUm78's ([DrUm78/linux `v1.0-rg-nano`](https://github.com/DrUm78/linux/tree/v1.0-rg-nano),
  mirrored at [thwonp/linux](https://github.com/thwonp/linux/tree/v1.0-rg-nano);
  GPL-2.0) built unchanged with his configuration, except the boot logo:
  `mk/build-nano-kernel.sh` and `res/nano/bootlogo.png`.
- plorpOS's own programs, PicoArch, the cores, FFmpeg and the PICO-8 runtime:
  `plorpOS/src/SOURCE.txt` and `/usr/local/plorpos/` on the card, with their
  licences.
