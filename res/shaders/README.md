# Shaders

GLSL shaders for diatom's GLES path on the GKD 350H Ultra (plorpos-gkd.72),
listed for the in-game menu by `shaders.cfg`. They are third-party files,
copied unmodified - headers, notices and all - from NextUI
(`skeleton/BASE/Shaders/glsl`, LoveRetro/NextUI at 73522546), which collected
them from the libretro shader repositories. NextUI's own license does not
cover them; each file's terms are its own.

| File | Author / origin (as stated in the file) | License (as stated in the file) |
|---|---|---|
| barrel-distortion.glsl | davej, 2015 | GPL v2 or later |
| edge1pixel.glsl | decavoid | none stated, here or upstream (glsl-shaders, slang-shaders) |
| fast-sharpen.glsl | guest(r), 2005-2019 | GPL v2 or later |
| lcd1x.glsl | (based on public-domain work) | GPL v2 or later |
| lcd3x.glsl | cg2glsl conversion of Gigaherz's handheld/shaders/lcd3x.cg | public domain (stated in the .cg) |
| pixellate.glsl | Fes, 2011-2012 | ISC-style permission notice |
| res-independent-scanlines.glsl | RiskyJumps | public domain |
| retro-v2.glsl | cg2glsl conversion of Hyllian's handheld/shaders/retro-v2.cg, 2013 | GPL v2 or later (stated in the .cg) |
| scanline.glsl | cg2glsl conversion of Themaister's misc/scanline.cg (same 0.05/0.15 constants) | public domain (stated in the .cg) |
| sharp-shimmerless.glsl | zadpos | public domain |
| sharp-shimmerless-grid.glsl | zadpos | public domain |
| PT_SkyWalker541.glsl | SkyWalker541, v1.8.0 (github.com/SkyWalker541/PT-SkyWalker541 at f201f21, `Standard RetroArch/shaders_glsl/handheld/shaders/`) - not from NextUI | MIT (the repository's LICENSE; full text in THIRD-PARTY-LICENSES.md) |
| stock.glsl | libretro's stock.cg pass-through | none stated; nothing in it to license |

The three conversions were traced to their libretro common-shaders originals
in plorpos-4g4 (2026-10-03). edge1pixel.glsl is the one with no terms found
anywhere: no license could be found, it is kept as published, credited to
decavoid, and THIRD-PARTY-LICENSES.md says so (plorpos-gkd.81). a71b077 brought 16 files; 54cd96b removed four (scale3x,
sharp-bilinear, sharp-shimmerless-subpixel-vrgb, waterpaint), leaving these 12; plorpos-gkd.86.1 added PT_SkyWalker541.glsl, unmodified, from its own repository.
