# TortOS guide

The [README](../README.md) keeps each topic short. This is the long version:
every control, menu row and setting, and the reasons behind them.

## Installing

**Format the card exFAT**, with a Master Boot Record partition scheme.

| | |
|---|---|
| **macOS** | Disk Utility, `View -> Show All Devices` so you get the whole card rather than just its volume, then `Erase`: format **ExFAT**, scheme **Master Boot Record**. |
| **Linux** | `mkfs.exfat` on an MBR-partitioned card. |
| **Windows** | Right-click the card, `Format`, **exFAT**. |

FAT32 works too if that is what the card already is - the kernel has both - but
exFAT is what every formatter offers at the size of card anyone actually uses,
so it is the one worth naming.

Copying from macOS leaves `._name` metadata files beside everything. They are
harmless - the launcher skips every name beginning with a dot, so they never
show up as games - and `dot_clean /Volumes/YOURCARD` removes them if they
bother you.

The first boot is the one that installs: the stock firmware runs the card's
installer, which sets `/usr/trimui/bin/runtrimui.sh` aside as
`runtrimui-original.sh`, puts its own there, replaces the two splash images
and then comes up in TortOS like every boot after it.

That hook is what starts TortOS, and all it does is look for the card - no
card, or a card without TortOS on it, and it hands straight back to the
original. **Taking the card out is enough to get the stock system back**;
[Undoing it](#undoing-it) has the rest.

> **`.tmp_update` starts with a dot**, so Finder and most file managers hide
> it and a drag of "everything" leaves it behind. A card missing it gets
> through the installer and then powers straight off looking for the half that
> is not there, and boots stock from then on. In Finder, `Cmd-Shift-.` shows
> hidden files; from a terminal, `cp -R` the unzipped contents and it comes
> along on its own.

Building it yourself instead is in [building.md](building.md).

BIOS files go **loose in `Bios/`**, not in a folder per system - that directory
is handed to the core as its system directory, and a core asks for a filename
inside it:

```
Bios/syscard3.pce             TurboGrafx-16 CD games (.chd, .cue, .m3u ...)
```

That one is required: a PC Engine CD will not start without it, and TortOS says
so by name rather than letting the core refuse the disc. HuCards on the same
shelf need nothing and are unaffected.

Nothing else on the shelf needs a BIOS. mGBA has a built-in one, so Game Boy
Advance runs without the real thing; if you want the original it is
`Bios/gba_bios.bin`, and the difference is the boot animation.

The consoles on the shelf come from `systems.cfg`, so the list in the README is
the shipped one rather than a fixed one: a line removed from the config takes
its shelf with it, and a system whose folder is empty is hidden until there is
something in it.

### Undoing it

TortOS replaces the boot splash and the loading splash on first boot, and backs
both up beside itself (`bootlogo.stock.bmp`, `splash.stock.png`) along with the
stock init script (`/etc/init.d/runtrimui.tortos-bak`). Removing the card is
enough to boot stock again; `/usr/trimui/bin/runtrimui-original.sh` is the
original hook.

What removing the card does **not** undo is anything written to the device
itself. The stock root filesystem is read-only and everything writable is an
overlay on the internal eMMC, so what TortOS puts there stays there:

```
/usr/trimui/bin/runtrimui.sh            the hook
/usr/trimui/bin/runtrimui-original.sh   the stock hook, moved aside
/usr/trimui/bin/setbright               brightness before the animation,
/usr/trimui/bin/tortos-bootbright.sh    re-copied by launch.sh every boot
/etc/init.d/runtrimui                   patched to call the line above
/etc/init.d/runtrimui.tortos-bak        the version before that patch
/etc/splash.png                         the pic2fb loading splash
/mnt/boot/bootlogo.bmp                  the u-boot splash, on mmcblk0p1
```

Eight files, none of them a driver and none of them replacing anything the
system needs to run. TortOS boots on top of the stock system rather than
replacing it - which is also why **the Wi-Fi networks and their passwords
survive** a reformatted card. They live in `/etc/wifi/wpa_supplicant.conf`,
written by the stock `wpa_supplicant` that TortOS drives rather than replaces,
on the eMMC and never on the card. Forget them from the Wi-Fi screen before the
device goes to anyone else.

The two splashes are the awkward pair: they are replaced on the device, and the
originals are saved **to the card** as `bootlogo.stock.bmp` and
`splash.stock.png`. Keep a copy of those somewhere else if the stock boot logo
matters to you - a card is the one part of this that gets reformatted.

## Controls and menus

| | |
|---|---|
| **Left/Right** | move along the row, when the shelf runs horizontally. In `Cubic`, always the games |
| **Up/Down** | move along the shelf when it runs vertically - **up advances**. In `Cubic`, always the systems |
| **the other axis** | jump to the previous / next initial (games). Not in `Cubic`, where both axes are taken |
| **L1/R1** | jump a screenful (games) |
| **A** | open a system, or start a game. In `Cubic`, start it |
| **B** | back to the systems row. In `Cubic`, this system's menu |
| **X** | game info for the card under the cursor |
| **Y** | favorite it - Favorites is a shelf of its own. Nothing on Muse's shelf |
| **Volume rocker** | volume, everywhere, including in game |
| **F1/F2** | brightness, everywhere, including in game |
| **MENU** (on the systems row) | the TortOS menu - settings that are about the firmware |
| **MENU** (inside a system) | that system's menu, below |
| **MENU** (in `Cubic`) | always the TortOS menu; B is the system's |
| **MENU** (in game) | the in-game menu: Continue, Save, Load, Display, Cheevos, Sleep, Reset, Quit |
| **MENU** (inside any menu) | closes the whole menu, however deep in it you are. B goes back one screen |
| **SELECT** | Muse, the music player, from anywhere but a running game - the in-game menu included. Again to close it |
| **POWER** | a tap sleeps: the screen goes dark at once, and the device suspends after the **Suspend Timeout**; a tap wakes it. A hold powers off. In a game, the game is saved first either way |

MENU means three different menus depending on where you are, and each one is
about the thing you are looking at: the firmware on the systems row, one
console inside it, the running game in a game. Inside any of them MENU closes
the whole menu at once - from Play Time or Wi-Fi straight back to where you
were, or back into the game - and B goes back one screen at a time.

**The Brick carries this table itself**, under **MENU > Controls**: five pages
that left and right step through - moving, the shelf, a game, Muse, and the
buttons that work anywhere. It is the copy that is always to hand, and the one
that follows your UI Direction, so the page shows the axes as they are on your
device rather than all three at once. This guide is the long version of the
same thing.

The TortOS menu opens from the systems row and is about the device rather
than any one console:

| | |
|---|---|
| **Play Time** | per game or per system, by day, week, month, year or all time |
| **Wi-Fi** | the network's name when connected, or why it is not |
| **Bluetooth** | the connected headset, or `not connected` - pair, connect and forget |
| **Audio Output** | `Auto` or `Speaker`, and where Auto landed - see **Audio** below |
| **Over The Hare** | the file server; needs Wi-Fi and says so when there is none |
| **Auto Off** | how long without a button before the device powers itself down instead of sleeping. Setting it turns Auto Sleep to `never`, and the other way round |
| **Auto Sleep** | how long without a button before the device sleeps, the same as a tap of POWER. Not during play - only on the shelf and in the menus, the in-game menu included |
| **Suspend Timeout** | how long a sleeping device waits for POWER before it suspends |
| **UI Theme** | `Plain Jane` or `Fancy Pants` - which art the shelves wear |
| **UI Direction** | `Horizontal`, `Vertical` or `Cubic` - see below |
| **Box Art** | fetch what the whole library is missing |
| **Cheevos** | the RetroAchievements account, or `sign in` |
| **ScreenScraper** | the account that brings covers with each game's year, genre and synopsis, `sign in`, or `not in this build` |
| **Controls** | every button and what it does, a page per place |
| **About TortOS** | version, address, battery, uptime |

Charging, or plugged into a computer, the device never sleeps, suspends or
turns itself off: Auto Off, Auto Sleep and Suspend Timeout all wait until it is
unplugged. As in NextUI - suspending on external power hangs the Brick.

Over The Hare and Box Art need a network, and go quiet without one rather than
disappearing - a row that vanishes teaches nobody why. Cheevos stays reachable
either way, because signing in is the thing you go there to do, and Bluetooth
and Play Time need no network at all.

On the Play Time screen, **left and right** change the window - all time, this
year, this month, this week, today - and **Y** switches between one row per
game and one per system. The row under the cursor gets its own line at the
bottom: how many times it has been launched, its longest single session, when
it was last played, and how many of those sessions ended in a flat battery or
a crash rather than a quit. That last number only appears when it is not zero.

On the Wi-Fi screen, **Y** rescans and **X** forgets the network under the
cursor, behind a confirm. Forgetting the one you are connected through is
allowed - refusing would leave a row that is visibly saved and visibly
un-forgettable, which is worse to explain than the consequence.

The system menu opens on a shelf of games and applies to that system alone:

| | |
|---|---|
| **Games** | how many the shelf found |
| **Core** | which libretro core runs them |
| **Sort By** | left/right; `Name`, `Play Time`, `Last Played` or `Recently Added` |
| **Display Mode** | left/right cycles it; saved the moment it changes |
| **Box Art** | fetch what this system is missing, and nothing else |
| **Rescan Folder** | read the card again, for ROMs that arrived since boot |

On Favorites the menu is only Games and Sort By. The rest is about one
console, and a game there takes its console's Display Mode wherever it was
started from.

Display Mode is per-system, because a Game Boy and a Genesis do not want the
same answer. Set here it applies from the next launch; the same row in the
in-game menu changes the running game as you press it, which is the one to use
when you want to see the difference rather than guess at it. Either way it is
written the moment it changes - there is no confirm step to hang the save off.

Sort By is per-system for the same reason. Alphabetical is the only sane way
to find a title you can name, and no help at all when you are coming back to
the two or three you are actually playing, which on a large shelf are
scattered through the alphabet. Play Time and Last Played read the same
session rows the Play Time screen does; Recently Added is the ROM's timestamp
on the card. Whichever order you pick, games it knows nothing about sort to
the bottom rather than into the middle, and two games it cannot tell apart
fall back to name - a shelf that reshuffles itself when nothing has changed
reads as broken even when the top of it is right.

Rescan Folder is what makes a ROM that arrived after boot appear without a
restart. Over The Hare already does it for you on the way out of the transfer
screen; this is the same thing by hand, for a card written some other way.

**In a game, X and Y are turbo A and turbo B** - hold one down and it presses
the button repeatedly for you instead of you mashing it. It applies to a whole
system rather than to one game.

Nine of the eleven have it: **NES, Master System, TurboGrafx-16, Game Boy, Game
Boy Color, Game Boy Advance, Game Gear, Neo Geo Pocket and Neo Geo Pocket
Color**. Those consoles had two face buttons,
so X and Y are spare and turbo can have them. Genesis and SNES are left out
because their pads use X and Y for real buttons. Which systems get it, and how
fast, is in [turbo.md](turbo.md).

Diatom does the pulsing, not the emulator core, which is why it works the same
on all nine rather than only on the one core that happens to implement turbo.

Volume and brightness draw the same thin line across the top of the screen in
the launcher, in a game, and in the in-game menu. One firmware, one piece of
feedback - tinted by which of the two it is, warm for brightness and cyan for
volume, so the line says what it is without a glyph or a number on it.

## Audio

Sound can come out of three places, and TortOS picks in a fixed order:

**wired headphones, then Bluetooth, then the speaker.**

A cable wins outright, in every setting. Someone who physically plugged
something in has said what they want more plainly than any menu can, and a
headset that merely happens to be connected has not said anything at all.

The **Audio Output** row has two positions rather than three. `Auto` follows
the rule above; `Speaker` refuses Bluetooth and nothing else - a cable still
works through it. There is no third "Headset" position because it would do
nothing `Auto` does not already do: `Auto` takes a headset whenever one is
connected, and neither setting can route to one that is not there. The row
shows where the sound actually went - `auto (wired)` - because `Auto` on its
own names a rule, not a place you can hear.

The wired jack has its own volume range, not the speaker's. The two are about
9 dB apart, and the level is re-mapped the moment a cable goes in or out, so
plugging in mid-game does not arrive at nine decibels louder than you left it.

### Bluetooth

A paired headset reconnects by itself at boot and mid-session, and game audio
follows it without relaunching anything. The bond survives a reboot.

**Pairing is on the Bluetooth screen.** Put the headset in pairing mode, press
**Y** to search, **A** to pair and connect, **X** to forget. The toggle at the
top turns the radio on and off. Connecting a headset there disconnects any
other, because choosing one is the point; a headset that reconnects by itself
never pushes another off.

Only devices that advertise a name are listed. A scan in an ordinary room finds
a dozen BLE beacons and somebody's television, and BlueZ names everything it
cannot identify after its own address - so the list would otherwise be
unusable. A device you have already paired is always shown, named or not.

**Forget means forget.** The bond, the ALSA PCM and BlueZ's cached copy of the
device all go. Scanning leaves a cache entry for everything in range, so
leaving the screen sweeps the ones you never paired with.

The one thing that matters underneath is the agent: headsets pair "Just Works"
and need `NoInputNoOutput`. With the default agent every attempt fails with an
authentication error that looks like a broken key, a broken chip, or broken
headphones, and is none of them.

**A headset paired now carries sound straight away**, a running game's
included.

**The volume keys set the headset's own volume**, through its absolute volume
control, so there is still one volume and not two in series. The headset's
volume buttons move the Brick's level the other way. On a few headsets, the
Brick's buttons set only the starting volume; after that, change it on the
headset itself. In a game, a press on the headset changes the level without
showing the bar.

**The headset's play, pause and skip buttons control Muse**, wherever you are,
a game included.

One thing behaves differently on a Bluetooth sink and is not a bug:

- **there is roughly 100-150 ms of latency**, from SBC, the radio and the
  headset's own buffer. That is what Bluetooth audio costs on any device and
  nothing here can tune it away.

If a headset is switched off or walks out of range mid-game, sound falls back
to the speaker within a second or two and the game keeps running. It never ends
a game to report an audio problem. That fallback is the emulator's own - it
does not wait for the launcher to notice the headset is gone.

## Muse

Muse plays the music on the card. Its card at the end of the shelf opens Muse,
the same as SELECT does: Now Playing when something is playing or paused, and
otherwise a shelf of album covers, the way a console's opens onto its games -
in all three layouts, with the other axis jumping from one initial to the
next. A on an
album opens its tracks. MENU, anywhere in Muse, has the album count, Sort By,
Album Art (below), and Rescan Folder for music copied on while the Brick was
running.

**Sort By** puts the shelf in order by artist, the way the folders are, or by
album title, two of the same title going in their artists' order. The jump
goes by the initial of whichever it is: an artist's, or an album's.

**SELECT** opens Muse from anywhere that is not a running game: the
shelf, every menu, and the in-game menu, where the game waits paused
underneath. SELECT again closes it from any screen in Muse, and the music keeps
playing. B goes back one screen: Now Playing to the album's tracks, the tracks
to the shelf, and the shelf out of Muse. The exceptions are Over The Hare and a
Box Art fetch, which only keep working while they are on screen.

Albums go in `Music/`, a folder per artist and one inside it per album:
`Music/Radiohead/The Bends/01 Planet Telex.mp3`. A folder of tracks straight
under `Music/` - a podcast, a mix - is an album of its own. Over The Hare
reaches `Music/` too, and albums sent that way are on the shelf when you leave
its screen. MP3, M4A, M4B, AAC, FLAC, Ogg, Opus and WAV play. Tracks play in
file-name order, and the number at the front of a file name is left off the
name shown.

With something playing or paused, SELECT goes straight to **Now Playing**: the
cover, the track, where in it you are, and what comes next.

| | |
|---|---|
| **A** | pause, or play |
| **L1/R1** | previous / next track. More than three seconds in, L1 starts the track over |
| **Left/Right** | back or ahead ten seconds |
| **Y** | the play mode |
| **B** | the album's tracks |
| **MENU** | Muse's menu |
| **SELECT** | close Muse |

**Y changes the play mode on Now Playing**, and only there: in order, then
repeat all, then repeat one, then shuffle, and round again. Its mark sits
beside the track count on that screen, which is the one place a mode is shown -
so the mode is set where it can be read. Shuffle starts from the track you
chose and plays the rest in a random order, and when
it runs out it shuffles again and carries on - never starting the new round
with the song that ended the last. The mode is kept across restarts.

**While music plays, a running game is silent.** Only the game's own sound:
the music, the volume keys and the mute switch all carry on as before. Pause
the music, or let the album end, and the game is heard again within a tenth of
a second. SELECT in the in-game menu is how to switch between them mid-game.

An album's tracks are a list over its shelf, with what is playing under a rule
at the foot. A on a track plays the album from there and opens Now Playing; on
the track already playing it just opens Now Playing. The shoulders and the
d-pad's sides do what they do on Now Playing. Auto Sleep and Auto Off wait while
music plays, the way they do on the charger.

Covers come from the music. The first time an album is shown, Muse takes the
picture its files carry and keeps it beside the album, in
`Music/<artist>/.media/`, the way box art sits in `Roms/<system>/.media`. An
album whose files carry none gets a generated card.

**Album Art**, in MENU on Muse's shelf, fetches covers over Wi-Fi for every
album with none, or with one smaller than the shelf draws it - 461px. It asks
MusicBrainz which record the album is, from the artist and album folder names
and the number of tracks - the track count is what tells a same-named single,
or another band, from the album - and takes that record's front cover from
the Cover Art Archive at 500px. An album it has supplied is remembered and not
asked about again; one it could not find is asked about on every run, since a
cover may be added later. MusicBrainz takes a request a second, so a large
library takes a while: seven albums took a minute on the Brick.

## The shelf

A single row of cards in perspective, with reflections - Cover Flow, carried
over from an earlier project by the same author and retuned. The focused card
sits in a soft glow tinted with its system's color, and the whole background
carries a wash of that color that eases as you move between systems.

Two things about that are yours to choose, and they are independent.

**UI Theme** picks the art. `Plain Jane` is a drawn card per system, with the
name on it. `Fancy Pants` is a photograph of the console itself, background
removed, and because a photograph does not name itself the shelf writes the
name underneath. A theme is a directory under `res/cards/`, so adding one is
dropping in a folder - no code and no configuration.

**UI Direction** picks how you move. `Horizontal` is the row above.

`Vertical` is that row stood on its end. The cards slide up and down instead
of left and right, flat and unrotated and all one size, with the one under the
cursor filling most of the screen and its neighbors pushed off the top and
bottom edges. It is laid out the way `Horizontal` is and moves at the row's
pace; only `Cubic` turns a solid.

`Cubic` turns that cube both ways and merges the two shelves into one surface.
**Up and down change system, left and right move through that system's
games.** There is no entering and no going back, because what you are looking
at is already the thing you can act on - so B, which has nothing to return to,
opens the system's menu instead, and MENU is always the firmware's.

Every face is a game. Turning to another system shows the game you were last
on in it: each system keeps its own place, so glancing at one costs you
nothing. Two rails, because there are two positions to be in - systems down
the left, games along the bottom - with `n / total` and the system's name on
the bottom line.

Vertically the shelf runs **A at the bottom to Z at the top**, and up
advances. The origin is the bottom left and the index grows with x and with
y, which is the same rule the horizontal row has always followed; a list that
numbers downward is the screen's convention, not this one's. The position
rail moves to the left edge and runs bottom-up with it, and on the games
shelf the `n / total` sits in the bottom left beside it, stationary, rather
than turning away with the face.

Card art comes from, in order:

1. box art in `Roms/<system>/.media/<name>.png`, whether you put it there or
   the Box Art row fetched it. With a ScreenScraper account the fetch asks
   ScreenScraper first, and brings the game's year, genre and synopsis with the
   cover. Without one, or for a game ScreenScraper does not have, it asks
   libretro: by file name, and where no name matches, by checksum: a zip
   records its ROM's CRC, which names the game in No-Intro's list - the catalog
   libretro files its covers under - so a file still carrying an older name
   finds its box anyway. On the main card that found 29 more, every one the
   right game. Fetched art is shrunk to the size it is drawn at - libretro
   ships covers at its own resolution, which on some systems is two to three
   times the pixels this screen can show, and every one of those pixels is
   decode time on every scroll past. Art you put there yourself is never
   touched, whatever size it is - only what the fetch brings in;
2. **the autosave preview** - the frame you were looking at when you stopped,
   which for a game in progress is a better card than any box;
3. a generated slab: the system's color, the title, and the title's first
   letter enormous and barely there behind it.

**Adding a cover by hand.** Some games have no cover anywhere the fetch looks -
fan translations, homebrew, a dump no catalog lists. Put one in yourself over
Over The Hare: open `Roms/<system>/.media/` in its file browser and upload a
PNG named exactly like the ROM file, with `.png` in place of its extension -
`Black Castle.gb` wants `Black Castle.png`. Box Art never overwrites a cover
that is already there, and Replace only swaps yours out when ScreenScraper or
libretro has a cover to put in its place. Keep it near 512 pixels on the long
side: a cover you add is drawn as it is and never resized, so a 2000-pixel scan
is decoded at full size whenever its card comes into view.

Pressing A on a game with an autosave does not start it, it continues it.
