#!/bin/sh
# Build the PlayStation core, pcsx_rearmed, from libretro's own tree.
#
# Every other core TortOS ships is an official buildbot binary (fetch-vendor.sh)
# or, for mgba, a bridge meant to be deleted. This one is built for good, and
# the reason is glibc, not a bug:
#
#   glibc 2.34 folded libpthread and libdl into libc, so anything linked
#   against 2.34 or newer asks for pthread_create@GLIBC_2.34, dlopen@GLIBC_2.34
#   and so on. The buildbot moved to such a host; its pcsx_rearmed (r26-112,
#   ff81ed1) demands GLIBC_2.34, measured 2026-10-01. The Brick's firmware has
#   2.33, and dlopen refuses a library over one missing version tag. The older
#   cores in fetch-vendor.sh predate the move and stop at 2.29.
#
# There is no older buildbot binary to pin: it serves `latest` only. So this
# builds the exact commit that binary came from in the same bullseye toolchain
# the launcher uses (glibc 2.31), unpatched, hardened to match the official
# build. One binary for the Brick and the GKD, so states move between them.
#
# HAVE_PHYSICAL_CDROM=0 is the one departure. Upstream's Makefile compiles
# libretro-cdrom.c but never links dir_list.o, which it calls; the buildbot's
# toolchain papers over that and bullseye's does not, so the core built,
# stripped, and died at dlopen on "undefined symbol: dir_list_new". A handheld
# has no CD drive to read.
#
# Needs: docker, and the tortos-toolchain image (mk/toolchain.Dockerfile).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-/tmp/tortos-pcsx-rearmed}
PIN=ff81ed17a15241d2f3730cdd7585b38e172532ca   # libretro/pcsx_rearmed, = buildbot r26-112
SHA=ab7b64874432fc771d7a6c61b5a8e29e22814f05e85b1854c8d30ec2f6aa6671

command -v docker >/dev/null || { echo "need docker" >&2; exit 1; }

if [ ! -d "$SRC/.git" ]; then
	git clone -q https://github.com/libretro/pcsx_rearmed.git "$SRC"
fi
cd "$SRC"
git fetch -q origin
git checkout -q "$PIN"
# The core stamps "build time: __DATE__ __TIME__" into itself; pinning the clock
# to the commit's own time is what makes the sha below reproducible.
EPOCH=$(git log -1 --format=%ct)

# CFLAGS through the environment, not the command line: the Makefile appends to
# CFLAGS, and a command-line value would silently replace everything it adds.
docker run --rm -v "$SRC:/src" -w /src -e SOURCE_DATE_EPOCH="$EPOCH" \
	-e CFLAGS="-D_FORTIFY_SOURCE=2 -fstack-protector-strong" tortos-toolchain bash -c '
set -e
make -f Makefile.libretro clean >/dev/null 2>&1 || true
make -f Makefile.libretro platform=unix HAVE_PHYSICAL_CDROM=0 \
  CC=aarch64-linux-gnu-gcc CXX=false AR=aarch64-linux-gnu-ar -j$(nproc) >/dev/null
aarch64-linux-gnu-strip --strip-unneeded pcsx_rearmed_libretro.so
so=pcsx_rearmed_libretro.so
# Never ship one that cannot load - see dir_list_new above.
[ "$(aarch64-linux-gnu-nm -D -u $so | grep -v GLIBC | grep -vc " w ")" = 0 ] \
  || { echo "!! undefined symbols - the core will not dlopen" >&2; exit 1; }
[ "$(aarch64-linux-gnu-objdump -T $so | grep -cE "__stack_chk|_chk$")" -ge 12 ] \
  || { echo "!! not hardened - the official build has 12 such symbols" >&2; exit 1; }
'
GOT=$(shasum -a 256 pcsx_rearmed_libretro.so | cut -d' ' -f1)
if [ "$GOT" != "$SHA" ]; then
	echo "!! pcsx_rearmed sha256 $GOT, expected $SHA" >&2
	echo "  The toolchain image or the source changed. Verify the core, then" >&2
	echo "  update SHA here." >&2
	exit 1
fi
mkdir -p "$ROOT/vendor/cores"
cp pcsx_rearmed_libretro.so "$ROOT/vendor/cores/"
echo "  built  pcsx_rearmed ($PIN)"
echo "  sha256 $GOT"
