#!/bin/sh
# The RG Nano toolchain, for `make PLATFORM=nano` (plorpos-ggv): FunKey's own
# SDK, plus FFmpeg for Muse built with it.
#
# The Nano runs DrUm78's FunKey-OS 2.3.0 (rg_nano branch): armv7 Cortex-A7,
# musl, SDL 1.2. FunKey-sdk-2.3.0 is that release's Buildroot SDK - gcc 10.2,
# arm-funkey-linux-musleabihf - and its sysroot carries the same SDL 1.2,
# SDL_ttf, SDL_image, alsa-lib and libpng the device has, so what links here is
# what runs there. It is a self-contained x86_64 Buildroot SDK and runs inside
# the tortos-toolchain image unchanged, mounted at /sdk.
#
# FFmpeg: neither the SDK nor the device has any. Built here from the same 6.1
# tarball, hash and configure line as mk/fetch-h700-sysroot.sh - LGPL only,
# --disable-everything, then exactly what Muse plays - shared, to ship as
# plorpOS/lib/ with the LGPL's text beside it, as on the H700.
#
# Needs: docker with the tortos-toolchain image, curl, tar.
#
#   mk/fetch-nano-sdk.sh [dir]   default: sdk-nano
#
# Result: <dir>/sdk (the SDK, mounted at /sdk in the container) and
# <dir>/ffmpeg/usr/{include,lib} (FFmpeg, to compile and link Muse against).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/sdk-nano}
DL=/tmp/tortos-nano-sdk-dl

SDK_VER=2.3.0
SDK_SHA=f0b12a4a5300f674af8169ae03039fd0dd2fb94588ba96d17a353daba1ac3e35
FF_VER=6.1
FF_SHA=938dd778baa04d353163ca5cb06c909c918850055f549205b29b1224e45a5316

for t in docker curl tar; do
	command -v $t > /dev/null || { echo "need $t" >&2; exit 1; }
done
mkdir -p "$DL"

fetch() { # file sha url
	[ -f "$DL/$1" ] || curl -sSfL -o "$DL/$1" "$3"
	got=$(sha256sum "$DL/$1" | cut -d' ' -f1)
	[ "$got" = "$2" ] || { echo "$1: hash mismatch: $got" >&2; exit 1; }
}
fetch FunKey-sdk-$SDK_VER.tar.gz "$SDK_SHA" \
	"https://github.com/FunKey-Project/FunKey-OS/releases/download/FunKey-OS-$SDK_VER/FunKey-sdk-$SDK_VER.tar.gz"
fetch ffmpeg-$FF_VER.tar.gz "$FF_SHA" "https://ffmpeg.org/releases/ffmpeg-$FF_VER.tar.gz"

rm -rf "$OUT"
mkdir -p "$OUT/ffmpeg" "$DL/src"
tar -xzf "$DL/FunKey-sdk-$SDK_VER.tar.gz" -C "$OUT"
mv "$OUT/FunKey-sdk-$SDK_VER" "$OUT/sdk"
rm -rf "$DL/src/ffmpeg-$FF_VER"
tar -xzf "$DL/ffmpeg-$FF_VER.tar.gz" -C "$DL/src"

# Not relocated: relocate-sdk.sh only fixes the .la and pkg-config paths, which
# nothing here reads, and the compiler wrapper finds its sysroot relative to
# itself wherever the SDK is mounted.
docker run --rm -i -v "$OUT/sdk:/sdk" -v "$DL/src:/src" -v "$OUT/ffmpeg:/out" \
	tortos-toolchain sh -s << EOF
set -e
X=/sdk/bin/arm-funkey-linux-musleabihf-
cd /src/ffmpeg-$FF_VER
./configure --prefix=/usr --enable-cross-compile --arch=arm --cpu=cortex-a7 --target-os=linux \
	--cross-prefix=\$X --enable-shared --disable-static \
	--extra-cflags='-mfpu=neon-vfpv4 -mfloat-abi=hard' --enable-neon \
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
for l in libavformat.so.60 libavcodec.so.60 libavfilter.so.9 libswresample.so.4 libavutil.so.58; do
	\${X}strip --strip-unneeded "\$(readlink -f /out/usr/lib/\$l)"
done
echo "  built  ffmpeg $FF_VER (\$(grep ^License: /tmp/ffmpeg.log))"
EOF
cp "$DL/src/ffmpeg-$FF_VER/COPYING.LGPLv2.1" "$OUT/ffmpeg/usr/lib/"
echo "sdk ready: $OUT"
