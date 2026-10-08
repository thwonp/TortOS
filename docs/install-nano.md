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

This replaces FunKey's menu (RetroFE or GMenu2X) with plorpOS at every start.
It changes four files in the Nano's system and keeps copies of them first
(`plorpOS/backup/`), so it can be undone (see Removing).

Connect a shell to the Nano (see [Connecting](#connecting)), then:

```
sh /mnt/plorpOS/install-root.sh
reboot
```

It changes:

- `/usr/local/sbin/frontend`, FunKey's menu loop, to start plorpOS;
- FunKey's USB-audio watcher (`/etc/init.d/S49audio`, `S52audioinit`), turned
  off: plorpOS moves the music to a DAC by itself;
- `/etc/asound.conf`, the speaker mix, at half level per channel so music is
  not clipped on the mono speaker.

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

Create the empty file, safely eject, restart the Nano.

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
  start goes straight back into that game.

## Removing plorpOS

If you ran `install-root.sh`:

```
sh /mnt/plorpOS/uninstall-root.sh
reboot
```

This puts back the files `install-root.sh` changed, from
`/mnt/plorpOS/backup/`. Then delete the `plorpOS` folder.
