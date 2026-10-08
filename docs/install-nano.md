# Installing on an Anbernic RG Nano

plorpOS-Nano is a small plorpOS for the **RG Nano**, where music comes first:
the shelf plays your albums, and they keep playing while you play a game. It is
not the full plorpOS - the Nano has 64 MB of memory and one slow core - but
three things in one folder:

- **nanoshelf**, the menu: your consoles, their games, Recently Played, and Music
  (Now Playing, artists > albums > tracks, the queue);
- **Muse**, plorpOS's music player, in the background;
- **PicoArch** for the games (a fork of [DrUm78's](https://github.com/DrUm78/picoarch)
  with a MUSIC page in its menu), using the cores already on the Nano.

It runs on **FunKey-OS 2.3.0 for the RG Nano** (DrUm78's build, the system
the Nano ships with). It uses that system's own games folders, cores, saves,
volume and brightness keys; it does not replace them.

## 1. Copy plorpOS to the card

Download **`plorpOS-nano-v<version>.zip`** and unzip it. Copy the `plorpOS`
folder to the root of the Nano's shared partition - the drive that appears
when the Nano is connected to a computer by USB with nothing else set up, next
to `Game Boy/`, `Music/` and the rest.

Put music in `Music/`, one folder per artist and one per album inside it
(`Music/<artist>/<album>/<tracks>`). MP3, FLAC, AAC/M4A, Ogg and Opus play.

## 2. Make plorpOS the Nano's menu

This makes plorpOS the menu the Nano starts into, in place of FunKey's
RetroFE and GMenu2X, which it removes. Everything it changes or removes in the
Nano's system is kept first in `plorpOS/backup/`, so it can be undone (see
Removing).

Connect a shell to the Nano (see [Connecting](#connecting)), then:

```
sh /mnt/plorpOS/install-root.sh
reboot
```

It:

- copies plorpOS's programs to `/usr/local/plorpos/` and runs them from there,
  so the card can be lent to a computer while plorpOS is running;
- changes `/usr/local/sbin/frontend`, FunKey's menu loop, to start plorpOS (and
  start it again if it ever stops);
- changes `/etc/asound.conf`, the speaker mix, to half level per channel so
  music is not clipped on the mono speaker;
- removes RetroFE and GMenu2X. FunKey's cores, PicoArch's menu, the apps and
  your saves stay.

FunKey's own USB-audio handling stays: it sets a DAC's level when it is
plugged in and points the volume keys at it.

**Updating:** copy the new `plorpOS` folder over the old one, run
`install-root.sh` again and restart.

## Using it

| Button | On the shelf | On Now Playing |
|---|---|---|
| D-pad | move; left/right a page | left/right seek 10 s |
| A | open, play | pause / play |
| B | back | back |
| X | Now Playing | the queue |
| Y | | play mode (in order, repeat all, repeat one, shuffle) |
| L / R | | previous / next track |
| START | pause / play, anywhere | pause / play |

**In a game**, press the **MENU** button. The first page is **MUSIC**: the
track playing, A to pause or play, left/right for the previous or next track.
While music plays the game is silent; pause the music and the game's sound
comes back. The rest of the menu is FunKey's: volume, brightness, save, load,
aspect ratio, exit.

Volume and brightness are FunKey's keys everywhere: **FN + A / Y** and
**FN + X / B**.

**Settings** (last on the shelf):

- **Share the card with a computer** - the Nano's card appears on the computer
  as a USB drive; the music stops. Eject it on the computer, then press B. This
  needs **USB at start: USB drive** (the Nano's usual mode).
- **USB at start** - USB drive, Network (ssh) or adb, from the next start (see
  Connecting).
- **Restart**, **Power off** - press A twice.

## A USB DAC

Plug it into the Nano's USB-C port. The music moves to it within two seconds,
and a game started while it is plugged in plays through it too. Pull it out and
the music stops, paused, on the speaker.

The Nano has one USB port, so a DAC and a computer cannot be connected at the
same time.

## Connecting

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

- **What it keeps where.** plorpOS: `/mnt/plorpOS/` (Recently Played in
  `recent.txt`). Game saves and PicoArch settings: FunKey's own,
  `/mnt/FunKey/.picoarch/`. Logs: `/tmp/nanoshelf.log`, gone at restart.
- **Consoles shown** are those with a game in their FunKey folder: Game Boy,
  Game Boy Color, Game Boy Advance, NES, Super NES, Genesis, Master System,
  Game Gear, PC Engine, Neo Geo Pocket, WonderSwan, Atari Lynx, Pokemon Mini,
  PICO-8, PlayStation, Arcade (MAME 2000, FBA 2012).
- **Full speed with music playing** was measured for Game Boy, Game Boy Advance
  and Genesis. Heavier systems may slow down while music decodes.
- **Resume.** When a game exits, FunKey saves its place, and the next start of
  that game asks whether to resume.
- **Power.** The power key in a game saves and turns off, as before; the next
  start goes straight back into that game, and plorpOS (with the music) comes
  up when you leave it.

## Removing plorpOS

If you ran `install-root.sh`:

```
sh /mnt/plorpOS/uninstall-root.sh
reboot
```

This puts back what `install-root.sh` changed or removed - FunKey's menu loop
and sound settings, RetroFE and GMenu2X - from `/mnt/plorpOS/backup/`, and
removes `/usr/local/plorpos`. Then delete the `plorpOS` folder.
