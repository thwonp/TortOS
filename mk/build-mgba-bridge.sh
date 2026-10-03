#!/bin/sh
# Build the mGBA core TortOS ships UNTIL LIBRETRO SYNCS ITS FORK.
#
# THIS SCRIPT IS MEANT TO BE DELETED. Every core TortOS ships is an official
# libretro buildbot binary, fetched and hash-pinned by mk/fetch-vendor.sh. This
# is the one exception, and it exists for a reason with an expiry date:
#
#   mGBA's buildbot core segfaults on the first read of cartridge RAM for every
#   MBC2 Game Boy cartridge - Kirby's Pinball Land, Wave Race, Golf, X, both
#   Final Fantasy Legends. A regression in mgba a1b2b23 (2026-07-30), reported
#   as mgba-emu/mgba#3859 and fixed upstream the SAME DAY in 543a1975.
#
#   The fix is in no core that can be downloaded. libretro builds from its own
#   fork, github.com/libretro/mgba, unsynced since 2026-08-06.
#
# So this builds libretro's own tree at the exact commit the pin is built from,
# applies only upstream's own one-hunk fix, and hardens it to match the official
# build. No patch of ours is in it.
#
# WHEN LIBRETRO SYNCS: restore the `fetch_core mgba` line in fetch-vendor.sh,
# delete this script, and verify a save state made on this core still loads on
# the official one, and that an MBC2 cartridge boots and saves.
#
# Needs: docker, and the tortos-toolchain image (mk/toolchain.Dockerfile).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-/tmp/tortos-mgba-bridge}
PIN=e31759b24e7a4e3899285ff720d7b573ac328ae7   # libretro/mgba, what CORES.md pins
FIX=543a197582c30364584d773a974d7f991892fa43   # mgba-emu/mgba, the upstream fix
# Reproducible (SOURCE_DATE_EPOCH, fixed toolchain snapshot). The first bridge
# core shipped, 1c180827, came from an image that no longer exists; this one
# differs from it only in the 20-byte .note.gnu.build-id, every other byte equal.
SHA=35ad00f2f8ccd2c6a2d1ec9fa4b910aff4dadf9af6163449ef6c84ad65469c72

command -v docker >/dev/null || { echo "need docker" >&2; exit 1; }

if [ ! -d "$SRC/.git" ]; then
	git init -q "$SRC" && git -C "$SRC" remote add origin https://github.com/libretro/mgba.git
fi
cd "$SRC"
# Both commits fetched by full sha, not as "within N of master": master moves,
# and a depth that once reached a commit stops reaching it. Depth 2 for FIX so
# `git show` has its parent to diff against.
git fetch -q --depth 1 origin "$PIN"
git checkout -q "$PIN"
git remote get-url up >/dev/null 2>&1 || git remote add up https://github.com/mgba-emu/mgba.git
git fetch -q --depth 2 up "$FIX"
git checkout -q -- src/gb/mbc.c
git show "$FIX" -- src/gb/mbc.c | git apply
# project(mGBA) declares C and CXX; the libretro core is pure C, so it is
# configured as C only, which is also how the shipped bytes were built. perl rather than sed -i,
# which takes a backup suffix on BSD and not on GNU - written with sed first,
# where it silently did nothing on macOS and CMake then asked for a C++
# compiler. No `|| true` either: if this edit fails the build must stop.
perl -pi -e 's/^project\(mGBA\)$/project(mGBA C)/' CMakeLists.txt
grep -q '^project(mGBA C)$' CMakeLists.txt || { echo "!! CMakeLists edit failed" >&2; exit 1; }

# -DZLIB_LIBRARIES is not a typo for the singular. mGBA's CMake has a gap: the
# libretro target appends $ZLIB_LIBRARIES to OS_LIB, but the bundled-zlib path
# sets ZLIB_LIBRARY, so zlib is compiled and never linked. The core then builds,
# loads, and dies at dlopen with "undefined symbol: crc32".
#
# gcc-ar/gcc-ranlib for the same symptom from another cause: mGBA builds with
# -flto, so libz.a holds LTO-only objects, and plain ar indexes none of their
# symbols unless binutils can find the LTO plugin, which a cross-only image
# cannot. The archive is then searched, crc32 is not found, and the link
# succeeds with crc32 undefined.
#
# The clock pinned to the commit's own time, as in build-pcsx-rearmed.sh, so
# anything stamped from __DATE__ cannot move the sha below.
EPOCH=$(git log -1 --format=%ct)
docker run --rm -v "$SRC:/src" -e SOURCE_DATE_EPOCH="$EPOCH" tortos-toolchain bash -lc '
set -e
rm -rf /src/bl
HARD="-D_FORTIFY_SOURCE=2 -fstack-protector-strong -O2"
cmake -S /src -B /src/bl -DBUILD_LIBRETRO=ON -DSKIP_LIBRARY=ON \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_AR=/usr/bin/aarch64-linux-gnu-gcc-ar-10 \
  -DCMAKE_RANLIB=/usr/bin/aarch64-linux-gnu-gcc-ranlib-10 \
  -DCMAKE_C_FLAGS="$HARD" \
  -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON -DZLIB_LIBRARIES=zlibstatic \
  -DUSE_FFMPEG=OFF -DUSE_PNG=OFF -DUSE_LIBZIP=OFF -DUSE_SQLITE3=OFF \
  -DUSE_ELF=OFF -DUSE_EPOXY=OFF -DUSE_DISCORD_RPC=OFF -DUSE_LZMA=OFF \
  -DBUILD_QT=OFF -DBUILD_SDL=OFF >/dev/null
cmake --build /src/bl -j$(nproc) >/dev/null
aarch64-linux-gnu-strip --strip-unneeded /src/bl/mgba_libretro.so
# Never ship one that cannot load. Two cores were deployed and called "tested"
# before anyone noticed they had never linked at all.
[ "$(aarch64-linux-gnu-nm -D -u /src/bl/mgba_libretro.so | grep -c crc32)" = 0 ] \
  || { echo "!! undefined crc32 - zlib did not link" >&2; exit 1; }
[ "$(aarch64-linux-gnu-nm -D -u /src/bl/mgba_libretro.so | grep -cE "__stack_chk|_chk@")" -ge 9 ] \
  || { echo "!! not hardened - the official build has 9 such symbols" >&2; exit 1; }
'
GOT=$(shasum -a 256 "$SRC/bl/mgba_libretro.so" | cut -d' ' -f1)
if [ "$GOT" != "$SHA" ]; then
	echo "!! mgba sha256 $GOT, expected $SHA - vendor/cores untouched" >&2
	exit 1
fi
mkdir -p "$ROOT/vendor/cores"
cp "$SRC/bl/mgba_libretro.so" "$ROOT/vendor/cores/mgba_libretro.so"
echo "  built  mgba (bridge, $PIN + $FIX)"
echo "  sha256 $GOT"
