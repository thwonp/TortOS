#!/bin/sh
# Build the PICO-8 core, fake08, from jtothebell's tree.
#
# Built from source for the glibc reason build-pcsx-rearmed.sh gives (ROCKNIX's
# fake08 demands GLIBC_2.38, the Brick has 2.33) and for two patches, without
# which save states, rewind and resume do not work:
#
#   fake08-load.patch - upstream's retro_load_game only queues the cart
#   and loads it on the first retro_run, but diatom restores the autosave
#   before that first frame. A reused resident core crashed on the restore, a
#   fresh one had the late cart load wipe it. The patch loads and starts the
#   cart inside retro_load_game; a cart that will not load refuses the game.
#   It also keeps a copy of the cart for retro_reset, which reloaded by file
#   name - a cart from memory has none, so Reset fell into the fake08 shell.
#
#   fake08-states.patch - a 2024 debugging commit (47c48ad) disabled eris's
#   init_persist_all, so every state silently saved no Lua at all: a load put
#   back RAM and the game drew itself over it. Re-enabled, the restore then
#   corrupted the heap: eris hooks half-built protos into objects the
#   incremental GC has already marked, so a GC step mid-restore frees them
#   (found with ASan). The patch holds the GC for the restore, then runs a
#   full collection: restarting the GC zeroes its debt, so without one every
#   restored heap was garbage nothing paid for - rewind (a restore a frame)
#   grew Celeste by ~170 MB a second until the GKD swapped and OOM-killed
#   diatom (plorpos-gkd.50.9). It also
#   points the registry's __PICO8_SANDBOX (z8lua's nil-global fallback) at
#   the restored sandbox: the cart coroutine sets it on its first frame, which
#   a restore skips, so a resumed cart in a reused core ran the previous
#   cart's _update60 and ignored input.
#
#   fake08-bound-lua.patch - the Lua state was copied into a fixed 1 MB buffer
#   with no bound, and fake08 does not enforce PICO-8's 2 MB Lua limit (Ma
#   Puzzle's state reached 2.5 MB). The budget is now 2 MB; a state that does
#   not fit makes the save fail before anything is written, and a load checks
#   every size the state claims before restoring any of it.
#
# The pin is the commit ROCKNIX ships (fake08-lr package.mk), which is also
# upstream master as of 2026-10-01. z8lua is a submodule.
#
# Needs: docker, and the tortos-toolchain image (mk/toolchain.Dockerfile).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-/tmp/tortos-fake08}
PIN=814991a2571ad3970e386cef48f3b148aa1c27b9   # jtothebell/fake-08 master, 2026-06-13
SHA=0d3f707786716e79b000e1275b695b50d0e62f67292bd78ba18bf242bf6ebef6

command -v docker >/dev/null || { echo "need docker" >&2; exit 1; }

if [ ! -d "$SRC/.git" ]; then
	git clone -q https://github.com/jtothebell/fake-08.git "$SRC"
fi
cd "$SRC"
git fetch -q origin "$PIN" 2>/dev/null || git fetch -q origin
git checkout -q -f "$PIN"
git submodule update -q --init --recursive
git apply "$ROOT/mk/patches/fake08-load.patch"
git apply "$ROOT/mk/patches/fake08-states.patch"
git apply "$ROOT/mk/patches/fake08-bound-lua.patch"
EPOCH=$(git log -1 --format=%ct)

# Flags through the environment: the Makefile appends to CFLAGS and CXXFLAGS.
H="-D_FORTIFY_SOURCE=2 -fstack-protector-strong"
docker run --rm -v "$SRC:/src" -w /src/platform/libretro \
	-e SOURCE_DATE_EPOCH="$EPOCH" -e CFLAGS="$H" -e CXXFLAGS="$H" \
	tortos-toolchain bash -c '
set -e
make clean >/dev/null 2>&1 || true
make platform=unix CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
  AR=aarch64-linux-gnu-ar -j$(nproc) >/dev/null
so=fake08_libretro.so
aarch64-linux-gnu-strip --strip-unneeded $so
# Never ship one that cannot load: an unversioned undefined symbol cannot
# resolve (see build-fbneo.sh).
[ "$(aarch64-linux-gnu-nm -D -u $so | grep -v " w " | grep -vc "@")" = 0 ] \
  || { echo "!! undefined symbols - the core will not dlopen" >&2; exit 1; }
[ "$(aarch64-linux-gnu-objdump -T $so | grep -cE "__stack_chk|_chk$")" -ge 9 ] \
  || { echo "!! not hardened - the first build had 9 such symbols" >&2; exit 1; }
'
SO=platform/libretro/fake08_libretro.so
# The Brick refuses anything newer, see above.
TOP=$(objdump -T "$SO" | grep -oE 'GLIBC(XX)?_[0-9.]+' | sort -uV)
if echo "$TOP" | grep -qE '^GLIBC_2\.(3[4-9])|^GLIBCXX_3\.4\.(29|3[0-9])'; then
	echo "!! fake08 needs a newer glibc/libstdc++ than the Brick has:" >&2
	echo "$TOP" | tail -3 >&2
	exit 1
fi
GOT=$(shasum -a 256 "$SO" | cut -d' ' -f1)
git checkout -q -- .   # leave the clone clean for the next run
if [ "$GOT" != "$SHA" ]; then
	echo "!! fake08 sha256 $GOT, expected $SHA" >&2
	echo "  The toolchain image or the source changed. Verify the core, then" >&2
	echo "  update SHA here." >&2
	exit 1
fi
mkdir -p "$ROOT/vendor/cores"
cp "$SO" "$ROOT/vendor/cores/"
echo "  built  fake08 ($PIN)"
echo "  sha256 $GOT"
