# Installing on an Anbernic RG Nano

plorpOS nano is a super stripped down plorpOS for the **RG Nano**, where music comes first:
the shelf plays your albums, and they keep playing while you play a game. It is
not the full plorpOS - the Nano has 64 MB of memory and one slow core - but
three things in one folder:

- **nanoshelf UI**, the menu: your consoles, their games, Recently Played,
  Favorites, and Music (Now Playing, artists > albums > tracks, the queue);
- **Muse**, plorpOS's music player, in the background;
- **PicoArch** for the games ([plorpOS's fork](https://github.com/thwonp/picoarch/tree/plorpos-nano)
  of [DrUm78's](https://github.com/DrUm78/picoarch), with a MUSIC page in its
  menu), using the cores already on the Nano.

It runs on [DrUm78's FunKey OS Build](https://github.com/DrUm78/FunKey-OS/releases/tag/fps-classics), using its games folders, cores, saves,
volume and brightness keys; it does not replace them.

## 1. Flash DrUm78's FunKey-OS

plorpOS nano installs on top of **DrUm78's FunKey-OS for the RG Nano**. If your
Nano doesn't run it yet, or for a fresh card:

1. Download **`FunKey-sdcard-DrUm78_RG_Nano.img`** from DrUm78's
   [FunKey-OS release](https://github.com/DrUm78/FunKey-OS/releases/tag/fps-classics).
   Flashing it **wipes the card**.
2. Write it to the microSD card with an image tool such as
   [balenaEtcher](https://etcher.balena.io/) or
   [Win32 Disk Imager](https://win32-disk-imager.en.uptodown.com), following
   the release notes there.
3. Put the card in the Nano and turn it on. The first start finishes the
   install and resizes the card's partition - let it finish, until FunKey's
   menu shows.

## 2. Copy plorpOS to the card

Power off the Nano (hold the power button) and put its microSD card in the
computer. The card's big partition - the one with `Game Boy/`, `Music/` and
the rest - is the one to copy to. **Do not reformat the other partitions if Windows asks you to**. 

Download **`plorpOS-nano-v<version>.zip`** and unzip it. It holds one folder,
`plorpOS-nano-v<version>`; copy **what is inside it** - the `plorpOS` folder and
the empty file `adb` - to the root of that partition.

The `adb` file makes the Nano start with a shell for step 3. The install
deletes it, so afterwards the Nano is a USB drive again.

**NOTE**: If you want to clean up your card, see [Tidying your card](#tidying-your-card). Leaving the default folders is harmless.

Copy your media according to [What goes where on the card](#what-goes-where-on-the-card).

## 3. Make plorpOS the Nano's menu

This makes plorpOS the menu the Nano starts into, in place of FunKey's
RetroFE and GMenu2X, which it removes. Everything it changes or removes in the
Nano's system is kept first in `plorpOS/backup/`, so it can be undone (see
Removing).

Eject the card, put it back in the Nano, turn the Nano on and connect it to
the computer by USB. With [adb](#connecting) on the computer, first check that
the Nano is there:

```
adb devices
```

It should list one device, followed by `device`. If the list is empty, check
the cable and that `adb` is at the root of the card, then restart the Nano. 
If you're having connection troubles, a USB A-C cable is more reliable.

Then:

```
adb shell sh /mnt/plorpOS/install-root.sh
adb reboot
```

It:

- copies plorpOS's programs to `/usr/local/plorpos/` and runs them from there,
  so the card can be lent to a computer while plorpOS is running;
- changes `/usr/local/sbin/frontend`, FunKey's menu loop, to start plorpOS (and
  start it again if it ever stops);
- changes `/etc/asound.conf`, the speaker mix, to half level per channel so
  music is not clipped on the mono speaker;
- removes RetroFE and GMenu2X. FunKey's cores, PicoArch's menu, the apps and
  your saves stay;
- makes plorpOS resume a game saved by the power key (in `/root/.profile`,
  FunKey's start script), so the music comes back with it;
- deletes the `adb` / `usbnet` file, so the Nano starts as a USB drive again.
  To keep a shell, choose **Settings > USB at start** after the restart.

FunKey's own USB-audio handling stays: it sets a DAC's level when it is
plugged in and points the volume keys at it.

**Updating:** copy the new `plorpOS` folder over the old one, and `adb` too,
then run the same two `adb` commands. Your settings stay, and the Nano is a
USB drive again after the restart.

## What goes where on the card

Put music in `Music/`, one folder per artist and one per album inside it
(`Music/<artist>/<album>/<tracks>`). MP3, FLAC, AAC/M4A, Ogg and Opus play.

Games go in FunKey's folders, at the root of the card. plorpOS shows a console
once its folder has a game in it.

| Folder | Shown as | Files |
|---|---|---|
| `Game Boy` | Game Boy | `.gb` `.dmg` `.zip` |
| `Game Boy Color` | Game Boy Color | `.gbc` `.zip` |
| `Game Boy Advance` | Game Boy Advance | `.gba` `.agb` `.gbz` `.bin` `.zip` |
| `NES` | NES | `.nes` `.fds` `.unf` `.unif` `.zip` |
| `SNES` | Super NES | `.smc` `.sfc` `.fig` `.swc` `.gd3` `.gd7` `.dx2` `.bsx` `.zip` |
| `Sega Genesis` | Genesis | `.md` `.gen` `.smd` `.bin` `.32x` `.cue` `.iso` `.chd` `.cso` `.m3u` `.68k` `.sgd` `.pco` `.zip` |
| `Sega Master System` | Master System | `.sms` `.gg` `.sg` `.sc` `.bin` `.zip` |
| `Game Gear` | Game Gear | `.gg` `.zip` |
| `PCE-TurboGrafx` | PC Engine | `.pce` `.sgx` `.cue` `.ccd` `.chd` `.toc` `.m3u` `.zip` |
| `Neo Geo Pocket` | Neo Geo Pocket | `.ngp` `.ngc` `.ngpc` `.npc` `.zip` |
| `WonderSwan` | WonderSwan | `.ws` `.wsc` `.pc2` `.zip` |
| `Atari lynx` | Atari Lynx | `.lnx` `.lyx` `.o` `.zip` |
| `Pokemon Mini` | Pokemon Mini | `.min` `.zip` |
| `PICO-8` | PICO-8 | `.p8` `.png` `.zip` |
| `PS1` | PlayStation | `.cue` `.bin` `.chd` `.pbp` `.m3u` `.iso` `.img` `.mdf` `.toc` `.cbn` |
| `MAME 2000` | Arcade (MAME) | `.zip` |
| `Final Burn Alpha 2012` | Arcade (FBA) | `.zip` |

**BIOS files** go in `FunKey/.picoarch/system/`. None is required:

| System | File | Without it |
|---|---|---|
| Game Boy Advance | `gba_bios.bin` | a built-in BIOS; a few games misbehave |
| PlayStation | `scph1001.bin` (or `scph5501.bin`, `scph7001.bin`) | a built-in BIOS; less compatible |
| PC Engine CD | `syscard3.pce` | CD games don't start (cards play) |
| Pokemon Mini | `bios.min` | a built-in BIOS |
| Atari Lynx | `lynxboot.img` | none needed - FunKey ships it and plorpOS puts it there |

Saves and PicoArch's settings are in `FunKey/.picoarch/` too - keep that
folder.

## Tidying your card

FunKey's image also fills the card with things only its own menus use.
These folders can be safely deleted if you don't ever want to revert back: `Applications`,
`Emulators`, `Native games`, `Settings` (FunKey's app launchers), and - if you
won't go back to FunKey's menu to play them - `DOOM`, `Quake`, `Quake II`,
`Wolfenstein 3D`, `Spear of Destiny` and `Libretro` (those games' engines).
Keep `FunKey`, `Music`, `plorpOS` and the game folders above (empty ones are
fine). The sample games in the game folders are free homebrew; delete any you
don't want.


## Using it

| Button | On the shelf | On Now Playing |
|---|---|---|
| D-pad | move; left/right a page | left/right seek 10 s |
| A | open, play | pause / play |
| B | back | back |
| X | Now Playing | the queue |
| Y | a game: Favorites on/off | play mode (in order, repeat all, repeat one, shuffle) |
| L / R | | previous / next track |
| START | pause / play, anywhere | pause / play |
| Power (tap) | dim to Now Playing | brightness back |

**Settings > Controls** shows the same, a page per place (Left/Right).

**FN is the SELECT button.** Volume and brightness are FunKey's keys
everywhere: **SELECT + A / Y** and **SELECT + X / B**; **SELECT + Up** saves a
screenshot to `FunKey/snapshots/`.

**In a game**, tap the **power** button: that is the Nano's MENU. The first
page is **MUSIC**: the track playing, A to pause or play, left/right for the
previous or next track. Up/Down change pages, B closes the menu. While music
plays the game is silent; pause the music and the game's sound comes back. The
rest of the menu is FunKey's: volume, brightness, save, load, aspect ratio,
exit.

**Favorites:** Y on a game adds it or takes it off; Favorites appears on the
shelf, A-Z, under Recently Played, once it has a game.

**Dim.** A power tap outside a game dims the screen to Now Playing (the
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

## Connecting

The install needs a shell on the Nano once. The simplest is **adb**, from
Google's [Android SDK Platform-Tools](https://developer.android.com/tools/releases/platform-tools)
(Windows, macOS, Linux; unzip it and run `adb` from that folder). On Linux it is
also a package: `android-tools` (Arch), `adb` (Debian, Ubuntu).

FunKey's USB modes are chosen by a file on the shared partition, read when the
Nano starts:

| File at the root of the drive | The Nano is |
|---|---|
| none | a USB drive (copying files) |
| `usbnet` | a network device: `ssh root@192.168.137.2`, password `funkey`, after giving the computer's side `192.168.137.1/24` |
| `adb` | an Android Debug Bridge device: `adb shell` |

Create the empty file, safely eject, restart the Nano - or choose it in
plorpOS's **Settings > USB at start** and restart.

## Trying it without installing

Over `adb shell` or ssh, with FunKey's menu still installed:

```
touch /mnt/disable_frontend; kill -9 $(pidof retrofe gmenu2x)
setsid sh /mnt/plorpOS/start.sh &
```

`rm /mnt/disable_frontend` and a restart bring FunKey's menu back.

## Notes

- **What it keeps where.** plorpOS: `/mnt/plorpOS/` (`recent.txt`,
  `favorites.txt`, `settings.txt`, and `queue.txt` - the queue and place, kept
  over a restart, stopped until you press play). Game saves and PicoArch settings: FunKey's own,
  `/mnt/FunKey/.picoarch/`. Logs: `/tmp/nanoshelf.log`, gone at restart.
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

If you ran `install-root.sh`:

```
sh /mnt/plorpOS/uninstall-root.sh
reboot
```

This puts back what `install-root.sh` changed or removed - FunKey's menu loop
and sound settings, RetroFE and GMenu2X - from `/mnt/plorpOS/backup/`, and
removes `/usr/local/plorpos`. Then delete the `plorpOS` folder.
