#!/bin/sh
# The TrimUI Model S toolchain, for `make trimui` (plorpos-80b): the union
# trimui toolchain, plus FFmpeg for Muse built with it.
#
# The Model S runs TrimUI's stock Tina Linux 3.10: ARM926EJ-S (ARMv5TE, no FPU,
# no NEON), glibc 2.23, SDL 1.2. shauninman/union-trimui-toolchain v001 is a
# Buildroot 2016.05 SDK - gcc 6.1, arm926ej-s, soft-float, glibc 2.23 - whose
# sysroot carries the device's SDL 1.2, SDL_ttf and alsa-lib. Its host binaries
# were linked against libmpfr.so.4, which the tortos-toolchain image (bullseye)
# no longer has; the ABI of its libmpfr.so.6 is close enough for gcc, so
# sdk/hostlib holds a libmpfr.so.4 link to it and builds set LD_LIBRARY_PATH.
#
# FFmpeg: integer arithmetic only, since soft-float costs ~95% of the CPU for a
# float resampler here (measured). So: no `aac`, `mp3float`, `opus` or `vorbis`
# decoder - avcodec_find_decoder can then only return the fixed-point ones
# (`aac_fixed`, `mp3`, `flac`, `alac`, PCM) - no atempo, and Opus through
# libopus built fixed-point. LGPL only, shared, shipped as plorpOS/lib/ with
# the LGPL's text beside it, as on the Nano.
#
# Needs: docker with the tortos-toolchain image, curl, tar, xz.
#
#   mk/fetch-trimui-sdk.sh [dir]   default: sdk-trimui
#
# Result: <dir>/sdk (the toolchain, mounted at /sdk in the container) and
# <dir>/ffmpeg/usr/{include,lib} (FFmpeg, to compile and link Muse against).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/sdk-trimui}
DL=/tmp/tortos-trimui-sdk-dl

TC_SHA=e07cc5cf901afdd5e54c816430ba5895342fee2bb0e5b962ca0a3326a9d8e818
OPUS_VER=1.5.2
OPUS_SHA=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
FF_VER=6.1
FF_SHA=938dd778baa04d353163ca5cb06c909c918850055f549205b29b1224e45a5316

for t in docker curl tar xz; do
	command -v $t > /dev/null || { echo "need $t" >&2; exit 1; }
done
mkdir -p "$DL"

fetch() { # file sha url
	[ -f "$DL/$1" ] || curl -sSfL -o "$DL/$1" "$3"
	got=$(sha256sum "$DL/$1" | cut -d' ' -f1)
	[ "$got" = "$2" ] || { echo "$1: hash mismatch: $got" >&2; exit 1; }
}
fetch trimui-toolchain.tar.xz "$TC_SHA" \
	"https://github.com/shauninman/union-trimui-toolchain/releases/download/v001/trimui-toolchain.tar.xz"
fetch opus-$OPUS_VER.tar.gz "$OPUS_SHA" "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz"
fetch ffmpeg-$FF_VER.tar.gz "$FF_SHA" "https://ffmpeg.org/releases/ffmpeg-$FF_VER.tar.gz"

rm -rf "$OUT"
mkdir -p "$OUT/ffmpeg" "$DL/src"
tar -xJf "$DL/trimui-toolchain.tar.xz" -C "$OUT"
mv "$OUT/trimui-toolchain" "$OUT/sdk"
mkdir -p "$OUT/sdk/hostlib"
ln -s /usr/lib/x86_64-linux-gnu/libmpfr.so.6 "$OUT/sdk/hostlib/libmpfr.so.4"
rm -rf "$DL/src/opus-$OPUS_VER" "$DL/src/ffmpeg-$FF_VER" "$DL/src/opus"
tar -xzf "$DL/opus-$OPUS_VER.tar.gz" -C "$DL/src"
tar -xzf "$DL/ffmpeg-$FF_VER.tar.gz" -C "$DL/src"

# libopus goes into libavcodec statically (PIC), so the device needs no
# libopus.so. The image has no pkg-config, which FFmpeg's configure insists on
# for libopus; the shim answers for that one package.
docker run --rm -i -e LD_LIBRARY_PATH=/sdk/hostlib -v "$OUT/sdk:/sdk" -v "$DL/src:/src" \
	-v "$OUT/ffmpeg:/out" tortos-toolchain sh -s << EOF
set -e
X=/sdk/usr/bin/arm-buildroot-linux-gnueabi-
cd /src/opus-$OPUS_VER
./configure --host=arm-buildroot-linux-gnueabi CC=\${X}gcc AR=\${X}ar RANLIB=\${X}ranlib \
	CFLAGS='-O2 -mcpu=arm926ej-s' --prefix=/src/opus --enable-fixed-point --disable-shared \
	--enable-static --with-pic --disable-doc --disable-extra-programs > /tmp/opus.log 2>&1 &&
	make -j\$(nproc) install >> /tmp/opus.log 2>&1 ||
		{ tail -30 /tmp/opus.log; echo "!! opus failed" >&2; exit 1; }
cat > /src/pkg-config << 'PC'
#!/bin/sh
case "\$*" in
*--cflags*) echo "-I/src/opus/include/opus" ;;
*--libs*) echo "-L/src/opus/lib -lopus -lm" ;;
*--modversion*) echo "$OPUS_VER" ;;
esac
PC
chmod +x /src/pkg-config
cd /src/ffmpeg-$FF_VER
./configure --prefix=/usr --enable-cross-compile --arch=arm --cpu=arm926ej-s --target-os=linux \
	--cross-prefix=\$X --pkg-config=/src/pkg-config --enable-shared --disable-static \
	--disable-armv6 --disable-armv6t2 --disable-vfp --disable-neon --disable-vfpv3 \
	--disable-programs --disable-doc --disable-autodetect --disable-network \
	--disable-avdevice --disable-swscale --disable-postproc --disable-everything \
	--enable-swresample --enable-protocol=file --enable-libopus \
	--enable-demuxer=mp3,aac,mov,ogg,flac,wav \
	--enable-decoder=mp3,aac_fixed,alac,flac,libopus,pcm_s16le,pcm_s24le,pcm_s32le,pcm_u8 \
	--enable-parser=mpegaudio,aac,flac,opus \
	--enable-filter=abuffer,abuffersink,aresample,aformat,volume > /tmp/ffmpeg.log 2>&1 &&
	make -j\$(nproc) >> /tmp/ffmpeg.log 2>&1 &&
	make install DESTDIR=/out >> /tmp/ffmpeg.log 2>&1 ||
		{ tail -30 /tmp/ffmpeg.log; echo "!! ffmpeg failed" >&2; exit 1; }
grep -q "^License: LGPL" /tmp/ffmpeg.log || { echo "!! ffmpeg is not LGPL-only" >&2; exit 1; }
for l in libavformat.so.60 libavcodec.so.60 libavfilter.so.9 libswresample.so.4 libavutil.so.58; do
	\${X}strip --strip-unneeded "\$(readlink -f /out/usr/lib/\$l)"
done
echo "  built  ffmpeg $FF_VER + libopus $OPUS_VER fixed (\$(grep ^License: /tmp/ffmpeg.log))"
EOF
cp "$DL/src/ffmpeg-$FF_VER/COPYING.LGPLv2.1" "$OUT/ffmpeg/usr/lib/"
cp "$DL/src/opus-$OPUS_VER/COPYING" "$OUT/ffmpeg/usr/lib/COPYING.opus"
echo "sdk ready: $OUT"
