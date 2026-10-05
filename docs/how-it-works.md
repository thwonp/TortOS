# How TortOS works

The design metric is **speed**, and one decision carries most of it: **TortOS
never starts a process to run a game.**

A cold start costs about 1100 ms on this hardware and almost none of it is the
game - it is SDL, an EGL context, audio and settings. So the emulator comes up
once during the boot animation and stays up for the whole session, and a game
arrives as a single line on a socket.

Every number in this file was measured on the device rather than estimated,
but they were not all measured on the same day. The boot profile below was
re-taken 2026-09-13, and the 975 MB on 2026-09-07. The launch timings and the
core footprint date from when the resident emulator landed and have not been
re-checked since, so read them as the shape of the thing rather than today's
reading - and the method matters as much as the figure, which is why the boot
profile now says cold or warm, on which card, with how many games.

## Where the time goes

Starting a game on this hardware costs about **1100 ms**, and almost none of it
is the game:

| | |
|---|---|
| `GFX_init` - SDL video plus the EGL/GL context | ~620 ms |
| `dlopen` of the libretro core | ~170 ms |
| audio and settings | ~140 ms |
| **actually opening the ROM** | **~36 ms** |

Everything except the last line is the cost of *starting a process*. So TortOS
does not start one.

### The resident emulator

[Diatom](https://github.com/ericreinsmidt/diatom) - an MIT libretro frontend
built for this device - comes up on a Unix socket during the boot animation
and stays up for the life of the session. The launcher hands it a game as a
`RUN` line carrying the core, the ROM, and the paths where the resume state
and the card preview live, and reads back what actually happened: `RUNNING`,
`EXIT reason=`, or an `ERROR code=` it can show.

A warm launch - the process up, the core already mapped - is **26 to 44 ms**
from `RUN` to `RUNNING`, and about 70 ms from pressing A, measured on Contra
on the device. Every core is mapped at startup, during the boot animation, and
never unloaded, so no launch pays for opening one: six mapped plus one running
measured **15.0 MB** against the device's 975, which is what makes holding all
of them affordable rather than reckless. They are opened `RTLD_LOCAL`, so
libraries exporting the same twenty `retro_*` symbols cannot see each other.

Mapping them replaced reading them. The boot script used to pull the cores into
the page cache with `cat`, which cost 480 ms cold and bought only the I/O;
`dlopen` costs 382 ms, because it takes what it needs rather than every byte,
and it pays the dynamic linker as well. That is what a first launch used to pay
- and the `~170 ms` above is real, measured 179 ms for `genesis_plus_gx` cold,
the largest core. It is now paid once, in idle seconds, instead of by whoever
starts a Genesis game first. Measured 2026-09-08.

The in-game menu is the launcher's own: MENU makes diatom hand the display
over with a preview of the paused frame, and Continue, Save, Load, Reset and
Quit act through one protocol line each. **Display** cycles the running game's
mode as you press it, which is the one to use when you want to see the
difference rather than guess at it, and **Cheevos** shows how much of this
game's set you have earned, or reads `none` and stays unselectable when there
is no set. Volume and brightness set in a game
come back to the launcher's settings when the game ends, because the two
sides share one levels channel instead of overwriting each other.

Nothing depends on the resident emulator. If the socket is not there - in the
first second after boot, or if it has died - the launcher runs the same
`diatom` binary standalone, one process for that game, and starts a fresh
resident once the display is back.

### The boot animation runs *behind* startup

An animation that adds its own length to the boot is a delay with a picture on
it. TortOS plays its 2.4s animation in the background while the launcher does
its entire startup - the card scan, GL init, font and card decode - and while
the resident emulator builds its context and maps the cores it needs.

`ffmpeg` and the launcher both write to `/dev/fb0`, and it is last-writer-wins,
so they must never draw at the same time. A marker file is the handshake: the
launcher initializes freely, blocks on the marker, and presents its first frame
the moment the animation clears it. Bounded at 8 seconds, so a stuck decoder
cannot hang the boot.

The same idle seconds pull the emulator, the cores and their libraries into the
page cache. Cold reads of that set measure ~190 ms against ~30 ms warm.

Startup phases are logged rather than guessed at. Both columns below are from
one device boot on the main card - 1708 games across eleven systems - measured
2026-09-13. Cold is the first launcher of that boot; warm is the same launcher
restarted with the page cache already hot. A second boot reproduced all eight
figures within 5%. The figures are cumulative from process start, not per
phase:

```
                        cold      warm
boot: scan              249 ms    103 ms
boot: video+input       977 ms    524 ms
boot: font+settings    1040 ms    529 ms
boot: card assets      1181 ms    641 ms
```

**`card assets` is the phase that moved.** As a cost of its own it is 141 ms
cold, against 480 ms when this file last recorded it: the launcher no longer
decodes the opening shelf's covers before drawing, it queues them and a worker
picks them up while the shelf is already on screen. See below.

`scan` is the one that will keep moving: it was 64 ms when this file first
recorded it and the shelf has grown a long way since. The rest is fixed cost,
which is why warm is roughly half of cold across the board and none of it is
the library.

Being straight about the rest of the table: every phase except `card assets`
is slower than the 2026-09-07 figures it replaces, on the same 1708 games, and
nothing in this file explains why. The launcher has gained a good deal of code
since, which is the obvious suspect and is not the same as a measurement. It
is recorded here rather than quietly rounded away.

### Card art is decoded off the render thread

The shelf asks for every visible card every frame, and a card that is not
decoded yet used to be decoded right there, inside the frame. On this device
that is 14-60 ms against a 16.7 ms budget, so arriving somewhere the cache did
not reach meant one frame doing seven decodes and taking 100-400 ms. It did
not read as slowness, it read as the shelf stopping.

Decoding now happens on two worker threads. Only the last step, handing the
finished image to the GPU, needs the renderer and stays on the main thread at
1-2 ms. A card whose art has not arrived is simply not drawn, so a slot is
briefly empty instead of the shelf freezing - the art appears at the same
moment either way, and what changes is whether everything else kept moving
while it came.

A system's cards start decoding when the systems row lands on it, not when you
open it, so opening one shows them at once - measured 2026-09-14 on four
systems opened straight after a reboot.

Two workers rather than one, and that was measured rather than assumed: one
still drops the occasional card on the shelves with the largest covers, even
after those covers were resized. They sleep when there is nothing to decode.

### The launcher never goes away

With a resident emulator there is nothing to tear down, so the launcher keeps
its own GL context through the whole game. Coming back from a game is a frame,
not a second and a half of re-initializing a display.

## Saves

- **Autosave.** Every way out of a game - the Quit row, the power button, a
  stop from the launcher - writes the state and the preview at the paths the
  launch handed over. One funnel, so no exit can forget and none can save
  twice.
- **Auto-resume.** Every launch hands over the game's autosave,
  `<ROM file>.auto.state`. If one is there the game comes up exactly where it
  was left; if not it starts fresh.
- **Manual save and load**, six slots, from the in-game menu (`MENU`).
  Silent - the device shows no in-game chrome.

States are keyed on the system's **`Roms/` folder**, and battery `.srm` files
are named after the ROM, in `Saves/<Roms folder>/` on both devices, beside whatever the core keeps there itself (memory
cards, `fbneo/`), so the same title on two shelves has two saves. The **tag** in `systems.cfg` keys
something else: the per-system display mode, the sort order, and the favorites
list. This
paragraph claimed for a while that saves and states hung off the tag; they do
not, checked in the code on 2026-09-01.

Cartridge battery saves are separate from all of that: a game with a battery
gets a `.srm` beside the state. **Neo Geo Pocket Color is the exception** - its
core reports no save memory at all, measured as zero bytes on two carts that do
save, so nothing writes a `.srm` and no battery file exists to copy off the card.
Progress there lives entirely in the autosave state, which is written on every
exit like every other system, so in normal play nothing is lost. It only matters
if you load an older slot, which rewinds the cartridge's own save with it.

## Achievements

RetroAchievements, listed in the in-game menu, with what you have earned kept
across games and cards.

**The evaluation is diatom's, and that is not a delegation of convenience.**
Conditions compare against the *previous frame* - `0xH06f0<d0xH06f0` is "this
byte is lower than it was last frame" - and the launcher only sees the socket
every 100 ms against a core running at 60 Hz. That is six frames per poll, so
five of every six are invisible to it and unlocks would be missed silently.
The launcher declares which console the game is and hands over the set, and
diatom watches every frame
(its ADR-0025 and ADR-0026).

**The device does the normal thing.** Sign in once under `MENU` -> Wi-Fi Services -> Cheevos,
and the first time you launch a game TortOS hashes the ROM, asks
RetroAchievements which game it is, fetches the set and caches it at
`Roms/<System>/.cheevos/<name>.set`, beside the box art in `.media/`. After
that the launch is instant and works with Wi-Fi off.

That needs HTTPS, which the Brick turns out to have: `curl 7.54.1` against
`OpenSSL/1.1.0i`. What it does not have is anything to trust, so
`res/ssl/cacert.pem` ships with the launcher - see
[its README](../res/ssl/README.md) for the measurement. Only the password is
typed; RetroAchievements answers with a token, and that is what is stored.

`tools/ra-sets.py` does the same fetch from a host, for seeding a whole library
at once or working offline. It writes the identical file, and is a convenience
rather than the mechanism. Measured over the 180-ROM test library: **166 games
have a set, 8,297 achievements, and every condition in all of them parses.**

Three things exist twice, once for the device and once for the host tool: the
per-console hash rules, the JSON to set-file conversion, and the set format
itself. `make check-rahash` and `make check-raset` run both implementations
over the same real data and require them to agree, because the failure they
guard against is silent - a wrong hash looks exactly like a game
RetroAchievements does not know.

**Unlocks go back to your account.** They are sent once the game is over and
the launcher has the screen back, never from inside the frame loop, and what
will not send stays queued and is tried again next time - an achievement
earned on a plane is still earned. When a game starts, the account's record of
it is read back and merged as soon as it answers, so anything already held is
not offered again: the account wins on what exists, the local store wins on
what is still owed, and neither is thrown away.

RA still serves a `Warning: Unknown Emulator` entry with every set, because
this client is not registered with them. It is dropped rather than shown - it
is not an achievement, and recording it would put a row in the store that can
never be displayed and might later be submitted as a duplicate of something
that was never real.
