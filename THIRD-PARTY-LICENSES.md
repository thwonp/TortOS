# Third-party notices

plorpOS (working name) itself - everything under `src/`, `tools/`, `mk/`, and
the configs, scripts and generated art written for this project - is licensed
**MIT** (`LICENSE`), except the NextUI-derived parts listed in `NOTICE`, which
keep NextUI's **PolyForm Noncommercial 1.0.0** (`LICENSES/`). It is a fork of
Eric Reinsmidt's TortOS, also MIT.

A built TortOS card (`out/sd/`) also redistributes third-party software that
keeps its own license. This file lists those components and their terms, and is
copied onto the card as `TortOS/THIRD-PARTY-LICENSES.md` by `mk/payload.sh`.

---

## diatom (the in-game libretro host)

- **Origin:** an independent frontend, built in its own repository and shipped
  as `TortOS/diatom`. This fork's copy: https://github.com/thwonp/diatom
  (upstream: https://github.com/ericreinsmidt/diatom)
- **License:** **MIT**, like plorpOS, except its NextUI-derived parts
  (fast-forward, rewind and the hotkeys), which are PolyForm Noncommercial
  1.0.0 - its `NOTICE` lists them. Its vendored `libretro.h` and `rcheevos`
  stay MIT, `lz4` BSD-2-Clause and `stb_image_write` (the screenshot PNG
  encoder) public domain or MIT, under their own notices.
- TortOS's launcher runs it as a resident process and talks to it over a Unix
  socket; it does not link against it. Diatom ships no cores of its own.

## No third-party firmware components

TortOS ships no launcher, emulator or settings library from another project.
Volume and brightness are TortOS's own code against the device's ALSA control
and display-engine interfaces, and the toolchain is a stock Debian
cross-compiler pinned by digest (`mk/toolchain.Dockerfile`).

On the TrimUI devices the SDL2 libraries TortOS links against are the device's
own, in `/usr/trimui/lib`, and are not redistributed on the card. The H700 card
carries its own; see Runtime libraries.

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
| `fake08_libretro.so` | PICO-8 | MIT, with components under their own terms (Lua MIT, Zepto 8 WTFPL 2, LodePNG custom, an oval routine CC BY-SA 3.0, others); full text in `LICENSE-fake08.md` (built from jtothebell/fake-08 source with three patches of ours, `mk/build-fake08.sh`; the patches are MIT) |
| `fbneo_libretro.so` | Arcade, Neo Geo | **Non-commercial**, FBNeo's own license plus MAME's; full text in `LICENSE-FBNeo.txt` (built from libretro/FBNeo source with one patch, `mk/build-fbneo.sh`) |

The GKD card ships all nine. The Brick card ships the first six only - no
pcsx_rearmed, fake08 or fbneo, and so no `LICENSE-fake08.md` or `LICENSE-FBNeo.txt`.

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

### Anbernic H700 on BaseOS (`TortOS/lib/` on the H700 card only)

BaseOS has no aarch64 SDL2 and no FFmpeg, so the H700 card carries both,
built from upstream source by `mk/fetch-h700-sysroot.sh` (tarball URLs and
SHA-256 pins are in the script, as is every configure option):

| Library | Version | License |
|---|---|---|
| SDL2 (`libSDL2-2.0.so.0`) | 2.30.8 + `mk/patches/sdl2-h700.patch` | zlib (the patch too) |
| SDL2_image (`libSDL2_image-2.0.so.0`) | 2.6.3, stb_image backend | zlib; stb_image public domain / MIT |
| SDL2_ttf (`libSDL2_ttf-2.0.so.0`) | 2.20.2, vendored FreeType | zlib; FreeType under the FreeType License (FTL) |
| FFmpeg (`libavformat.so.60`, `libavcodec.so.60`, `libavfilter.so.9`, `libswresample.so.4`, `libavutil.so.58`) | 6.1, unmodified | LGPL 2.1 or later |

FFmpeg is built LGPL-only - no `--enable-gpl`, no `--enable-nonfree`, and the
build fails unless configure reports "License: LGPL version 2.1 or later" -
with only the demuxers, decoders, parsers and filters Muse uses. It is linked
dynamically, so any of the five libraries can be replaced with another build
of the same sonames. The corresponding source is the unmodified release
tarball, https://ffmpeg.org/releases/ffmpeg-6.1.tar.gz (SHA-256
`938dd778baa04d353163ca5cb06c909c918850055f549205b29b1224e45a5316`), and the
script above rebuilds the shipped libraries from it. The LGPL's text ships on
the card beside them as `TortOS/lib/COPYING.LGPLv2.1`.

---

## Launcher code derived from NextUI (sleep and suspend, CHD reading)

- **Origin:** NextUI, an independent handheld-launcher fork.
  https://github.com/LoveRetro/NextUI
- **License:** **PolyForm Noncommercial 1.0.0** (source-available, not OSI
  open source). Full text in `LICENSES/PolyForm-Noncommercial-1.0.0.txt`.
- Which code: `src/chdread.{c,h}` whole, and the fenced sleep and suspend
  regions of `src/main.c`, `src/platform.c`, `src/platform.h`,
  `src/platform_brick.c` and `sd/tortos/radio.sh` - `NOTICE` has the list.
  The launcher's fast-forward, rewind and hotkey bindings are this fork's own
  (MIT); the ones ported from NextUI live in diatom and are marked there.
- **Noncommercial only.** A card carrying this code may not be sold or
  otherwise put to commercial use - the same restriction the noncommercial
  cores above already put on it. Everything else in plorpOS is MIT.

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

## Shaders (`res/shaders/`, shipped as `TortOS/shaders/`)

Copied unmodified from NextUI (`skeleton/BASE/Shaders/glsl`), which collected
them from libretro's shader repositories; NextUI's license does not cover them.
Each file's terms are its own, traced to the libretro original where the copy
states none:

| File | Origin | License |
|------|--------|---------|
| barrel-distortion.glsl | davej, 2015 | GPL-2.0-or-later |
| fast-sharpen.glsl | guest(r), 2005-2019 | GPL-2.0-or-later |
| lcd1x.glsl | as stated in the file | GPL-2.0-or-later |
| retro-v2.glsl | Hyllian, 2013 (libretro common-shaders `handheld/shaders/retro-v2.cg`) | GPL-2.0-or-later |
| pixellate.glsl | Fes, 2011-2012 | ISC-style notice, in the file |
| lcd3x.glsl | Gigaherz (common-shaders `handheld/shaders/lcd3x.cg`) | public domain |
| scanline.glsl | Themaister (common-shaders `misc/scanline.cg`) | public domain |
| res-independent-scanlines.glsl | RiskyJumps | public domain |
| sharp-shimmerless.glsl, sharp-shimmerless-grid.glsl | zadpos | public domain |
| stock.glsl | libretro's pass-through (common-shaders `stock.cg`) | none stated; a pass-through with no expression in it |
| PT_SkyWalker541.glsl | SkyWalker541, v1.8.0 (github.com/SkyWalker541/PT-SkyWalker541; not from NextUI) | MIT, full text below |
| edge1pixel.glsl | decavoid (glsl-shaders `pixel-art-scaling/shaders/edge1pixel.glsl`) | **No license could be found** (see below) |

**edge1pixel.glsl: no license could be found.** Neither the file nor libretro's
glsl-shaders or slang-shaders repositories, where it is published, state terms
for it (checked 2026-10-03). It is shipped as published there, unmodified and
credited to its author, decavoid, as libretro distributes it. It is not covered
by this project's MIT license. If the author objects, it will be removed.

GPL-2.0-or-later shaders are source files, shipped as source. `shaders.cfg`,
the list itself, is this project's (MIT).

## Certificates

- `res/ssl/cacert.pem` - Mozilla's root CA list as published by the curl
  project; **MPL-2.0**. Source and checksum in `res/ssl/README.md`.

## Fonts

- `res/fonts/menu.ttf` - **Josefin Sans**, SIL Open Font License 1.1. Full text
  in `res/fonts/OFL.txt`. It is TortOS's UI face, the in-game menu's face, and
  the face the boot animation and the system cards are lettered in.
- `res/fonts/wordmark.ttf` - **Josefin Sans** too, the same OFL 1.1
  (`res/fonts/OFL.txt`), for the plorpOS wordmark.

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

### PT_SkyWalker541.glsl (MIT)

```
MIT License

Copyright (c) 2026 SkyWalker541

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
