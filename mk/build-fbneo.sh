#!/bin/sh
# Build the arcade core, FBNeo, from libretro's own tree. One core serves two
# shelves, Arcade and Neo Geo, the way mednafen_ngp serves both Pockets.
#
# Built from source for the reason build-pcsx-rearmed.sh gives: the buildbot's
# fbneo demands GLIBC_2.34 and GLIBCXX_3.4.29 (measured 2026-10-01), and the
# Brick has glibc 2.33 and libstdc++ 6.0.28. The bullseye toolchain's g++-10
# tops out at GLIBCXX_3.4.28, so libstdc++ links dynamically, like every other
# C++ core on the card.
#
# Two patches. mk/patches/fbneo-rotate.patch, taken from NextUI's tg5050 build:
# vertical games (1943, Galaga) are rotated upright inside the core, and the
# core never asks the frontend to rotate. diatom declines SET_ROTATION, and
# without this a vertical game plays on its side.
#
# And mk/patches/fbneo-unloaded-port.patch: after a game, a romset that fails
# to load (missing files) leaves no driver but a stale input count, and the
# frontend's set_controller_port_device then walked a freed input array -
# SIGSEGV in the resident diatom (plorpos-gkd.83.11). Refresh only with a
# driver loaded.
#
# Long: thousands of drivers, about 20 minutes on four cores.
#
# Needs: docker, and the tortos-toolchain image (mk/toolchain.Dockerfile).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-/tmp/tortos-fbneo}
PIN=a4012b161e48b33b94940f07987323574c237448   # libretro/FBNeo master, 2026-09-28
SHA=082059ba970ff2c2d0bc263ae807d1539d06fed25ff7e4e0b6efba5c3eb4f4c9

command -v docker >/dev/null || { echo "need docker" >&2; exit 1; }

if [ ! -d "$SRC/.git" ]; then
	git clone -q https://github.com/libretro/FBNeo.git "$SRC"
fi
cd "$SRC"
git fetch -q origin "$PIN" 2>/dev/null || git fetch -q origin
git checkout -q -f "$PIN"
git apply "$ROOT/mk/patches/fbneo-rotate.patch"
git apply "$ROOT/mk/patches/fbneo-unloaded-port.patch"
EPOCH=$(git log -1 --format=%ct)

# Flags through the environment: the Makefile appends to CFLAGS and CXXFLAGS.
# The toolchain image has no git, so the Makefile's GIT_VERSION stamp falls
# back to nothing - which is what keeps the sha below reproducible.
H="-D_FORTIFY_SOURCE=2 -fstack-protector-strong"
docker run --rm -v "$SRC:/src" -w /src/src/burner/libretro \
	-e SOURCE_DATE_EPOCH="$EPOCH" -e CFLAGS="$H" -e CXXFLAGS="$H" \
	tortos-toolchain bash -c '
set -e
make clean >/dev/null 2>&1 || true
make platform=unix CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
  AR=aarch64-linux-gnu-ar -j$(nproc) >/dev/null
so=fbneo_libretro.so
aarch64-linux-gnu-strip --strip-unneeded $so
# Never ship one that cannot load. A C++ core leans on libstdc++ and libgcc_s
# (GLIBCXX_, CXXABI_, GCC_ versions), which NEEDED covers; what cannot resolve
# is an UNVERSIONED undefined symbol, the shape dir_list_new had in pcsx_rearmed.
[ "$(aarch64-linux-gnu-nm -D -u $so | grep -v " w " | grep -vc "@")" = 0 ] \
  || { echo "!! undefined symbols - the core will not dlopen" >&2; exit 1; }
[ "$(aarch64-linux-gnu-objdump -T $so | grep -cE "__stack_chk|_chk$")" -ge 13 ] \
  || { echo "!! not hardened - the official build has 13 such symbols" >&2; exit 1; }
'
SO=src/burner/libretro/fbneo_libretro.so
# The Brick refuses anything newer, see above.
TOP=$(objdump -T "$SO" | grep -oE 'GLIBC(XX)?_[0-9.]+' | sort -uV)
if echo "$TOP" | grep -qE '^GLIBC_2\.(3[4-9])|^GLIBCXX_3\.4\.(29|3[0-9])'; then
	echo "!! fbneo needs a newer glibc/libstdc++ than the Brick has:" >&2
	echo "$TOP" | tail -3 >&2
	exit 1
fi
GOT=$(shasum -a 256 "$SO" | cut -d' ' -f1)
git checkout -q -- .   # leave the clone clean for the next run
if [ "$GOT" != "$SHA" ]; then
	echo "!! fbneo sha256 $GOT, expected $SHA" >&2
	echo "  The toolchain image or the source changed. Verify the core, then" >&2
	echo "  update SHA here." >&2
	exit 1
fi
mkdir -p "$ROOT/vendor/cores"
cp "$SO" "$ROOT/vendor/cores/"
echo "  built  fbneo ($PIN)"
echo "  sha256 $GOT"
