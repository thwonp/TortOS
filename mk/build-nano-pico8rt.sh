#!/bin/sh
# The runtime the owner's native PICO-8 needs on the RG Nano (plorpos-ggv.42),
# into build/nano/pico8rt:
#
#   mk/build-nano-pico8rt.sh    (make nano-pico8rt)
#
# PICO-8's Raspberry Pi build, pico8_dyn, is armhf glibc (GLIBC_2.17 at most)
# linked to SDL2. The Nano's FunKey-OS is musl with SDL 1.2 only, so both come
# alongside: Debian bookworm's armhf glibc (2.36) loader and libraries, run as
# `ld-linux-armhf.so.3 --library-path DIR pico8_dyn`, and an SDL2 with nothing
# but the dummy video driver and OSS audio. The Nano has no DRM and SDL2 has no
# framebuffer driver; the screen, the buttons and Muse are the job of the
# preload built here too, tools/nano-pico8.c (ggv.42.2). Measured in spike
# ggv.40: 2.8 MB, 30 fps carts at full speed.
#
# Needs: docker. Downloads SDL2's release tarball once into build/nano.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/nano/pico8rt
SDL=SDL2-2.32.10
SDL_SHA=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
IMAGE=debian@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251   # bookworm-slim

command -v docker >/dev/null || { echo "need docker" >&2; exit 1; }
TAR=$ROOT/build/nano/$SDL.tar.gz
mkdir -p "$ROOT/build/nano"
[ -f "$TAR" ] || curl -sfL -o "$TAR" \
	"https://github.com/libsdl-org/SDL/releases/download/release-${SDL#SDL2-}/$SDL.tar.gz"
echo "$SDL_SHA  $TAR" | sha256sum -c --quiet

rm -rf "$OUT"
mkdir -p "$OUT"
docker run --rm -v "$TAR:/sdl.tar.gz:ro" -v "$ROOT/tools/nano-pico8.c:/nano-pico8.c:ro" -v "$OUT:/out" -e SDL="$SDL" "$IMAGE" sh -c '
set -eu
dpkg --add-architecture armhf
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
  gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf cmake make libc6:armhf >/dev/null
cd /tmp && tar xzf /sdl.tar.gz && mkdir b && cd b
cmake ../$SDL -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-mcpu=cortex-a7 -mfpu=neon-vfpv4" \
  -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
  -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF \
  -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=ON -DSDL_DUMMYVIDEO=ON \
  -DSDL_OFFSCREEN=OFF -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_HIDAPI=OFF >/tmp/cmake.log
grep -q "SDL_OSS .*: ON" /tmp/cmake.log || { echo "!! SDL2 without OSS" >&2; exit 1; }
make -j"$(nproc)" >/dev/null
cmake --install . --prefix /tmp/sdl >/dev/null
arm-linux-gnueabihf-gcc -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -Wall -Wextra -shared -fPIC \
  -I/tmp/sdl/include/SDL2 -o /out/nano-pico8.so /nano-pico8.c -L. -lSDL2-2.0 -ldl
arm-linux-gnueabihf-strip --strip-unneeded /out/nano-pico8.so
cp -L libSDL2-2.0.so.0 /out/
arm-linux-gnueabihf-strip --strip-unneeded /out/libSDL2-2.0.so.0
cp ../$SDL/LICENSE.txt /out/LICENSE.SDL2.txt
L=/usr/lib/arm-linux-gnueabihf
for l in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1; do
	cp -L $L/$l /out/
done
cp /usr/share/doc/libc6/copyright /out/LICENSE.glibc.txt
{ echo "$SDL (zlib), built with OSS audio and dummy video only"
  echo "glibc: Debian libc6:armhf $(dpkg-query -W -f "\${Version}" libc6:armhf) (LGPL-2.1+)"; } >/out/VERSIONS.txt
'
ls -l "$OUT"
du -sh "$OUT"
