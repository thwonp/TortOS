# third_party

Other people's code, carried here because the launcher links it. Not under
`vendor/`, which is gitignored and holds the cores the build fetches.

Both are here for one job: hashing a CD game the way RetroAchievements does
(plorpos-gkd.53), so a disc on the PC Engine or PlayStation shelf finds its
achievement set. A disc is identified by what boots it, not by its bytes, and
RA's own code is the only reliable statement of those rules - the same reason
diatom vendors rcheevos' evaluator rather than rewriting it.

Built by `mk/third_party.mk`, which lists every compiled file by name: a file
appearing here should be a decision, not something a glob picks up. Carried
unmodified; to update, replace the files from the pinned upstream and rebuild.

| | upstream | pinned | license |
|---|---|---|---|
| `rcheevos/` | https://github.com/RetroAchievements/rcheevos | `f1417fb`, the commit diatom's evaluator is pinned to | MIT |
| `libchdr/` | https://github.com/rtissera/libchdr | `607694c` | BSD-3-Clause |
| `libchdr/deps/lzma-26.02/` | LZMA SDK, via libchdr | | public domain |
| `libchdr/deps/miniz-3.1.2/` | miniz, via libchdr | | Unlicense |
| `libchdr/deps/zstd-1.5.7/` | zstd's single-file decoder, via libchdr | | BSD (chosen of its BSD/GPLv2 dual license); `LICENSE` from zstd v1.5.7 |

## rcheevos: what is here

`src/rhash/` `hash.c`, `hash_disc.c`, `cdreader.c`, `md5.c` and their headers,
`src/rc_compat.c`, and the three public headers they include. Compiled with
`RC_HASH_NO_ROM`, `RC_HASH_NO_ENCRYPTED` and `RC_HASH_NO_ZIP`: cartridges are
still hashed by `src/rahash.c`, and no shelf has encrypted or zipped discs.
`RC_NO_THREADS` drops `rc_compat`'s mutexes, which only `rc_client` uses.

rcheevos has no CHD reader of its own - each frontend brings one. Ours is
`src/chdread.c`, over libchdr.

## libchdr: what is here

`include/`, `src/` (C only - the micro-flac C++ backend is not carried; FLAC
goes through `dr_flac`), `unity.c`, and the three bundled decompressors. The
build compiles `unity.c` plus `src/libchdr_codec_avhuff.c`, which `unity.c`
leaves out and the codec table still names.
