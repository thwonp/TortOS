# Third-party notices

plorpOS (working name) itself - everything under `src/`, `tools/`, `mk/`, and
the configs, scripts and generated art written for this project - is licensed
**PolyForm Noncommercial 1.0.0**; see `LICENSE`. It is a fork of Eric
Reinsmidt's TortOS, whose MIT notice is kept in `NOTICE`.

A built TortOS card (`out/sd/`) also redistributes third-party software that
keeps its own license. This file lists those components and their terms, and is
copied onto the card as `TortOS/THIRD-PARTY-LICENSES.md` by `mk/payload.sh`.

---

## diatom (the in-game libretro host)

- **Origin:** an independent frontend, built in its own repository and shipped
  as `TortOS/diatom`. This fork's copy: https://github.com/thwonp/diatom
  (upstream: https://github.com/ericreinsmidt/diatom)
- **License:** **PolyForm Noncommercial 1.0.0**, like plorpOS; Eric
  Reinsmidt's original is MIT, and his notice is in `NOTICE`. Its vendored
  `libretro.h` and `rcheevos` stay MIT under their own notices.
- TortOS's launcher runs it as a resident process and talks to it over a Unix
  socket; it does not link against it. Diatom ships no cores of its own.

## No third-party firmware components

TortOS ships no launcher, emulator or settings library from another project.
Volume and brightness are TortOS's own code against the device's ALSA control
and display-engine interfaces, and the toolchain is a stock Debian
cross-compiler pinned by digest (`mk/toolchain.Dockerfile`).

The SDL2 libraries TortOS links against are the device's own, in
`/usr/trimui/lib`, and are not redistributed on the card.

---

## libretro cores (`vendor/cores/`, shipped as `TortOS/cores/`)

| Core | System | License |
|------|--------|---------|
| `fceumm_libretro.so` | NES | GPL-2.0-or-later |
| `mednafen_pce_fast_libretro.so` | TurboGrafx-16 / PC Engine | GPL-2.0-or-later (Mednafen-derived) |
| `mgba_libretro.so` | Game Boy, Game Boy Color, Game Boy Advance | MPL-2.0 |
| `snes9x2010_libretro.so` | SNES | **Non-commercial** |
| `genesis_plus_gx_libretro.so` | Genesis, Master System, Game Gear | **Non-commercial** |
| `mednafen_ngp_libretro.so` | Neo Geo Pocket, Neo Geo Pocket Color | GPL-2.0 (Beetle NeoPop, Mednafen-derived) |
| `pcsx_rearmed_libretro.so` | PlayStation | GPL-2.0 (built from libretro/pcsx_rearmed source, `mk/build-pcsx-rearmed.sh`) |

**The last two carry a non-commercial restriction.** They are not open source
under either the OSI or FSF definition and they restrict commercial
redistribution outright, which constrains what a card carrying them may be
sold as - hobby redistribution is what every firmware shipping them relies
on. The reasoning is worked through in diatom's ADR-0023. A card built
without SNES and the Sega systems carries no such restriction.

Core source: the libretro organization and each core's upstream repository
(https://github.com/libretro).

---

## Runtime libraries

None are redistributed. Measured 2026-08-26, the cores need only
`libc`, `libm`, `librt`, `libstdc++`, `libgcc_s` and `ld-linux`, and the
launcher needs those plus `libSDL2`, `libSDL2_image` and `libSDL2_ttf` - every
one of which ships in the device's own firmware. The eight compression and
codec libraries TortOS once carried were a previous emulator's dependencies and
left with it.

---

## Launcher features derived from NextUI (sleep mode, hotkey shortcuts, fast-forward/rewind)

- **Origin:** NextUI, an independent handheld-launcher fork.
  https://github.com/LoveRetro/NextUI
- **License:** **PolyForm Noncommercial 1.0.0** (source-available, not OSI
  open source). Full terms: https://polyformproject.org/licenses/noncommercial/1.0.0
- This is a personal fork of TortOS (github.com/thwonp/TortOS) that will not
  be upstreamed to github.com/ericreinsmidt/TortOS. Given that, the features
  tracked as bd issues TortOS-1v7.1 (sleep mode), TortOS-1v7.2 (hotkey
  submenu) and TortOS-1v7.3 (fast-forward/rewind) are built by porting or
  adapting NextUI's source directly, rather than as clean-room
  reimplementations.
- **Same restriction this file already carries for `snes9x2010_libretro.so`
  and `genesis_plus_gx_libretro.so` above: noncommercial only.** A build of
  this fork that includes any of these three features may not be sold or
  otherwise put to commercial use; hobby redistribution (a free public git
  repo, sharing a card image with other hobbyists) is what PolyForm
  Noncommercial's license is for and is what this fork relies on, the same
  way the two non-commercial cores already do.
- Since 2026-09-28 the whole fork is PolyForm Noncommercial 1.0.0 (root
  `LICENSE`), so this carve-out no longer marks the only noncommercial code;
  it is kept as the record of which features came from NextUI.

## Fonts

- `res/fonts/menu.ttf` - **Josefin Sans**, SIL Open Font License 1.1. Full text
  in `res/fonts/OFL.txt`. It is TortOS's UI face, the in-game menu's face, and
  the face the boot animation and the system cards are lettered in.
