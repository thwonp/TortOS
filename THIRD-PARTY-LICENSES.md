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
| `fbneo_libretro.so` | Arcade, Neo Geo | **Non-commercial**, FBNeo's own license plus MAME's; full text in `LICENSE-FBNeo.txt` (built from libretro/FBNeo source with one patch, `mk/build-fbneo.sh`) |

**snes9x2010, genesis_plus_gx and fbneo carry a non-commercial restriction.** They are not open source
under either the OSI or FSF definition and they restrict commercial
redistribution outright, which constrains what a card carrying them may be
sold as - hobby redistribution is what every firmware shipping them relies
on. The reasoning is worked through in diatom's ADR-0023. FBNeo's license also
requires its full text on the card, verbatim (`LICENSE-FBNeo.txt`), and its
source changes published (`mk/patches/fbneo-rotate.patch`). A card built
without SNES, the Sega systems, Arcade and Neo Geo carries no such restriction.

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

## Disc hashing, linked into the launcher (`third_party/`)

The launcher identifies a CD game for RetroAchievements with RA's own code and
reads CHD images with libchdr; both are compiled into `tortos.elf`. Sources and
pinned versions: `third_party/README.md`.

- **rcheevos** (rhash only) - Copyright (c) 2018 RetroAchievements.org -
  **MIT**. Full text below.
- **libchdr** - Copyright Romain Tisserand - **BSD-3-Clause**. Full text below.
- **zstd** (decoder, bundled with libchdr) - Copyright (c) Meta Platforms, Inc.
  and affiliates - **BSD**. Full text below.
- **LZMA SDK** (decoder, bundled with libchdr) - Igor Pavlov - **public domain**.
- **miniz** (inflate, bundled with libchdr) - Rich Geldreich and contributors -
  **Unlicense** (public domain).

## Fonts

- `res/fonts/menu.ttf` - **Josefin Sans**, SIL Open Font License 1.1. Full text
  in `res/fonts/OFL.txt`. It is TortOS's UI face, the in-game menu's face, and
  the face the boot animation and the system cards are lettered in.

---

## Full license texts for `third_party/`

### rcheevos (MIT)

```
MIT License

Copyright (c) 2018 RetroAchievements.org

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### libchdr (BSD-3-Clause)

```
Copyright Romain Tisserand
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### zstd (BSD)

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
