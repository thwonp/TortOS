# Installing on the TrimUI Brick

plorpOS runs from the microSD card. The first boot puts a small hook on the
Brick that looks for the card; take the card out and the Brick starts its stock
system as before.

It runs on the **TrimUI Brick** and the **Brick Hammer** (the same board). The
Brick Pro is not supported in this release.

You need the Brick, a microSD card and, just this once, a computer.

Download **`plorpOS-brick-v1.1.zip`** from
[Releases](https://github.com/thwonp/TortOS/releases/latest) and unzip it.
It goes on the card as it is, this guide (`INSTALL.md`) included:

```
TortOS/      tortos.elf  diatom  muse  launch.sh  systems.cfg
             cards/  cores/  res/  LICENSES/  ...
.tmp_update/ and trimui/   hand the Brick's boot to plorpOS
Roms/        NES/  SNES/  Game Boy/ ...   one per console
Music/
Audiobooks/
Bios/
Saves/       made per console on first play
```

## 1. Prepare the card

1. **Format it as exFAT,** with a Master Boot Record partition scheme. On a
   Mac, open Disk Utility, choose *View > Show All Devices*, and erase the card
   itself rather than its volume.
2. **Copy everything inside the zip to the root of the card.**
3. **Add games** to their console's folder in `Roms/`, albums to `Music/` and
   books to `Audiobooks/`, now or over Wi-Fi later. BIOS files go loose in
   `Bios/`; which ones each console needs is in
   [Supported systems](https://github.com/thwonp/TortOS/blob/main/README.md#supported-systems).

> [!IMPORTANT]
> `.tmp_update` starts with a dot, so most computers hide it and it gets left
> behind. Without it the first boot powers off and the Brick keeps starting its
> stock system. On a Mac, press <kbd>Cmd</kbd>+<kbd>Shift</kbd>+<kbd>.</kbd> in
> Finder to show hidden files before you copy.

## 2. Put the card in and turn the Brick on

The first boot installs the hook, and every boot after that comes up in
plorpOS.

## Upgrading a TortOS card

Copy everything inside the zip over the card, replacing what's there. Your
settings, saves, favorites and play time are kept.

## Adding games over Wi-Fi

1. Join a network under **MENU > Wi-Fi Services > Wi-Fi**.
2. Open **MENU > Wi-Fi Services > Over The Hare**. It shows an address and a
   PIN.
3. Open that address in a browser on your phone or computer, and type the PIN.
4. Drag games into their console's folder, albums into `Music` or books into
   `Audiobooks`. They're on the shelf when you leave the screen.

The PIN is new every time you open Over The Hare.

## Removing plorpOS

Take the card out. The Brick boots its own system again.

Before you reformat the card or give the Brick away:

- **Copy `TortOS/bootlogo.stock.bmp` and `TortOS/splash.stock.png` somewhere
  safe.** They're the Brick's original boot pictures, and the card holds the
  only copy.
- **Forget your Wi-Fi networks** with X on the Wi-Fi screen. Their passwords
  are stored on the Brick, not on the card.

The files the hook leaves on the Brick itself are listed in the README, under
*What TortOS leaves on the Brick itself*.
