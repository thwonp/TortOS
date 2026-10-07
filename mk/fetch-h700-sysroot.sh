#!/bin/sh
# The H700 sysroot, for `make PLATFORM=h700`: SDL2, SDL2_image, SDL2_ttf and
# FFmpeg built from source, because BaseOS (the H700 base, TF1) has no aarch64 SDL2
# at all - only the stock firmware's armhf one in /usr/lib32. What links here
# is also what ships on the card (TF2), so build and device agree by
# construction rather than by pulling.
#
# Versions are the Brick's (mk/fetch-sysroot.sh), so both platforms compile
# against the same SDL API, and the tarball hashes are the same pins.
#
# One patch, mk/patches/sdl2-h700.patch, zlib like SDL itself:
#   - a "mali" video driver: one fullscreen EGL window on /dev/fb0 through
#     the Mali blob's fbdev winsys. Upstream SDL2 has no such driver, the H700
#     stock kernel has no DRM for kmsdrm, and the Brick's TrimUI SDL carries
#     its own. Measured on the RG SP (plorpos-7ny.2): opengles2 renderer,
#     vsync granted, 59.6 fps with 24 scaled alpha quads per frame.
#   - the built-in gamepad ("ANBERNIC-keys") reports its d-pad as a hat and
#     has no ABS_X/ABS_Y, so SDL's evdev classifier never called it a
#     joystick. BTN_A together with BTN_START now does.
#
# Built lean: video mali (EGL/GLES2, dlopened from the device's Mali blob),
# audio ALSA (dlopened), Linux joystick + evdev input; no X11, Wayland, DRM,
# udev, D-Bus or PulseAudio, none of which BaseOS has. SDL2_image decodes PNG
# and JPEG with its built-in stb_image, SDL2_ttf carries its own FreeType, so
# the three libraries need nothing from the device but glibc.
#
# FFmpeg for Muse (plorpos-7ny.10): BaseOS has none, and the Brick links its
# firmware's 6.1. Built here from the same 6.1 tarball and hash as
# mk/fetch-sysroot.sh, LGPL only (no --enable-gpl, no nonfree), with
# --disable-everything and then exactly what Muse plays: the formats
# src/muselib.c lists (mp3, m4a/m4b, aac, flac, ogg/oga, opus, wav), the file
# protocol, and src/muse/dec.c's filter chain (atempo for SPEED, volume for
# its 2 dB headroom - without it every track failed, plorpos-xpt.5). Cover art is
# the attached picture's bytes, so no image decoder. Shared, to ship as
# TortOS/lib/ beside SDL with the LGPL's text (usr/lib/COPYING.LGPLv2.1),
# which keeps its relinking terms simple; see THIRD-PARTY-LICENSES.md.
#
# ALSA's headers and import library come from Debian bullseye - the snapshot
# mk/toolchain.Dockerfile pins - not from the device: SDL only needs them to
# compile, and loads the device's libasound.so.2 by soname at run time. So no
# device is needed to build this.
#
# Needs: docker with the tortos-toolchain image, curl, git, ar, tar.
#
#   mk/fetch-h700-sysroot.sh [sysroot-dir]   default: sysroot-h700
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/sysroot-h700}
DL=/tmp/tortos-h700-sysroot-dl

SDL_VER=2.30.8
SDL_SHA=380c295ea76b9bd72d90075793971c8bcb232ba0a69a9b14da4ae8f603350058
IMG_VER=2.6.3
IMG_SHA=931c9be5bf1d7c8fae9b7dc157828b7eee874e23c7f24b44ba7eff6b4836312c
TTF_VER=2.20.2
TTF_SHA=9dc71ed93487521b107a2c4a9ca6bf43fb62f6bddd5c26b055e6b91418a22053
SNAP=http://snapshot.debian.org/archive/debian/20260901T000000Z/pool/main/a/alsa-lib
ALSA_VER=1.2.4-1.1
ALSA_SHA=8fabd77256593c42e2ad1578f938b56d0a5ed19aa61c6c72c890fd761fd129a8
ALSA_DEV_SHA=5acd1f3cd1aa11aeeb9dcd2467e8142ced0bdf112b813d134e63baebf342ee29
FF_VER=6.1
FF_SHA=938dd778baa04d353163ca5cb06c909c918850055f549205b29b1224e45a5316

for t in docker curl git ar tar; do
	command -v $t > /dev/null || { echo "need $t" >&2; exit 1; }
done
mkdir -p "$DL"

fetch() { # file sha url
	[ -f "$DL/$1" ] || curl -sSfL -o "$DL/$1" "$3"
	got=$(shasum -a 256 "$DL/$1" | cut -d' ' -f1)
	[ "$got" = "$2" ] || { echo "$1: hash mismatch: $got" >&2; exit 1; }
}
fetch SDL2-$SDL_VER.tar.gz "$SDL_SHA" \
	"https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
fetch SDL2_image-$IMG_VER.tar.gz "$IMG_SHA" \
	"https://github.com/libsdl-org/SDL_image/releases/download/release-$IMG_VER/SDL2_image-$IMG_VER.tar.gz"
fetch SDL2_ttf-$TTF_VER.tar.gz "$TTF_SHA" \
	"https://github.com/libsdl-org/SDL_ttf/releases/download/release-$TTF_VER/SDL2_ttf-$TTF_VER.tar.gz"
fetch ffmpeg-$FF_VER.tar.gz "$FF_SHA" "https://ffmpeg.org/releases/ffmpeg-$FF_VER.tar.gz"
fetch libasound2.deb "$ALSA_SHA" "$SNAP/libasound2_${ALSA_VER}_arm64.deb"
fetch libasound2-dev.deb "$ALSA_DEV_SHA" "$SNAP/libasound2-dev_${ALSA_VER}_arm64.deb"

# Fresh trees every run, so the patch always applies to pristine source.
SRC=$DL/src
rm -rf "$SRC" "$OUT"
mkdir -p "$SRC" "$OUT/usr/lib" "$OUT/usr/include"
for t in SDL2-$SDL_VER SDL2_image-$IMG_VER SDL2_ttf-$TTF_VER; do
	tar -xzf "$DL/$t.tar.gz" -C "$SRC"
done
tar -xzf "$DL/ffmpeg-$FF_VER.tar.gz" -C "$SRC"
git -C "$SRC/SDL2-$SDL_VER" apply "$ROOT/mk/patches/sdl2-h700.patch"

# ALSA: headers and the import library, out of the two .debs.
for d in libasound2 libasound2-dev; do
	mkdir -p "$SRC/$d"
	(cd "$SRC/$d" && ar x "$DL/$d.deb" data.tar.xz && tar -xJf data.tar.xz)
done
cp -r "$SRC/libasound2-dev/usr/include/alsa" "$OUT/usr/include/"
cp "$SRC/libasound2/usr/lib/aarch64-linux-gnu/libasound.so.2.0.0" "$OUT/usr/lib/libasound.so.2"
ln -sf libasound.so.2 "$OUT/usr/lib/libasound.so"

docker run --rm -i -v "$SRC:/src" -v "$OUT:/out" tortos-toolchain sh -s << EOF
set -e
cat > /tmp/tc.cmake << T
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /out)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
T
C="-DCMAKE_TOOLCHAIN_FILE=/tmp/tc.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH=/out/usr"
build() { # name srcdir options...
	n=\$1; s=\$2; shift 2
	cmake -S "\$s" -B /tmp/b-\$n \$C "\$@" > /tmp/\$n.log 2>&1 &&
	cmake --build /tmp/b-\$n -j\$(nproc) >> /tmp/\$n.log 2>&1 &&
	DESTDIR=/out cmake --install /tmp/b-\$n >> /tmp/\$n.log 2>&1 ||
		{ tail -30 /tmp/\$n.log; echo "!! \$n failed" >&2; exit 1; }
	echo "  built  \$n"
}
# SDL's own Khronos headers stand in for the EGL/GLES ones the image lacks.
build SDL2 /src/SDL2-$SDL_VER -DCMAKE_C_FLAGS=-I/src/SDL2-$SDL_VER/src/video/khronos \
	-DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_RPATH=OFF \
	-DSDL_MALI=ON -DSDL_OPENGLES=ON -DSDL_OPENGL=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF \
	-DSDL_KMSDRM=OFF -DSDL_VIVANTE=OFF -DSDL_VULKAN=OFF -DSDL_DIRECTFB=OFF -DSDL_RPI=OFF \
	-DSDL_OFFSCREEN=OFF -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_PULSEAUDIO=OFF \
	-DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=OFF -DSDL_LIBSAMPLERATE=OFF \
	-DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF -DSDL_HIDAPI=OFF
grep -q "define SDL_VIDEO_DRIVER_MALI 1" /out/usr/include/SDL2/SDL_config.h ||
	{ echo "!! SDL2 built without the mali driver" >&2; exit 1; }
build SDL2_image /src/SDL2_image-$IMG_VER -DSDL2IMAGE_BACKEND_STB=ON -DSDL2IMAGE_VENDORED=OFF \
	-DSDL2IMAGE_DEPS_SHARED=OFF -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF \
	-DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF -DSDL2IMAGE_JPG=ON -DSDL2IMAGE_PNG=ON
build SDL2_ttf /src/SDL2_ttf-$TTF_VER -DSDL2TTF_VENDORED=ON -DSDL2TTF_HARFBUZZ=OFF \
	-DSDL2TTF_SAMPLES=OFF -DBUILD_SHARED_LIBS=ON
cd /src/ffmpeg-$FF_VER
./configure --prefix=/usr --enable-cross-compile --arch=aarch64 --target-os=linux \
	--cross-prefix=aarch64-linux-gnu- --enable-shared --disable-static \
	--disable-programs --disable-doc --disable-autodetect --disable-network \
	--disable-avdevice --disable-swscale --disable-postproc --disable-everything \
	--enable-swresample --enable-protocol=file \
	--enable-demuxer=mp3,aac,mov,ogg,flac,wav \
	--enable-decoder=mp3,mp3float,aac,alac,flac,vorbis,opus,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8 \
	--enable-parser=mpegaudio,aac,flac,opus,vorbis \
	--enable-filter=abuffer,abuffersink,aresample,aformat,atempo,volume > /tmp/ffmpeg.log 2>&1 &&
	make -j\$(nproc) >> /tmp/ffmpeg.log 2>&1 &&
	make install DESTDIR=/out >> /tmp/ffmpeg.log 2>&1 ||
		{ tail -30 /tmp/ffmpeg.log; echo "!! ffmpeg failed" >&2; exit 1; }
grep -q "^License: LGPL" /tmp/ffmpeg.log || { echo "!! ffmpeg is not LGPL-only" >&2; exit 1; }
echo "  built  ffmpeg $FF_VER (\$(grep ^License: /tmp/ffmpeg.log))"
for l in libSDL2-2.0.so.0 libSDL2_image-2.0.so.0 libSDL2_ttf-2.0.so.0 \
         libavformat.so.60 libavcodec.so.60 libavfilter.so.9 libswresample.so.4 libavutil.so.58; do
	aarch64-linux-gnu-strip --strip-unneeded "\$(readlink -f /out/usr/lib/\$l)"
done
EOF

# BaseOS is glibc 2.35 and the bullseye image cannot emit newer than 2.31;
# check anyway, as fbneo's script does, so a toolchain change cannot slip by.
for l in libSDL2-2.0.so.0 libSDL2_image-2.0.so.0 libSDL2_ttf-2.0.so.0 \
         libavformat.so.60 libavcodec.so.60 libavfilter.so.9 libswresample.so.4 libavutil.so.58; do
	[ -e "$OUT/usr/lib/$l" ] || { echo "!! $l was not built" >&2; exit 1; }
	if objdump -T "$OUT/usr/lib/$l" | grep -qE 'GLIBC_2\.(3[6-9]|[4-9][0-9])'; then
		echo "!! $l needs a newer glibc than BaseOS's 2.35" >&2
		exit 1
	fi
done
cp "$SRC/ffmpeg-$FF_VER/COPYING.LGPLv2.1" "$OUT/usr/lib/"
echo "sysroot ready: $OUT"
