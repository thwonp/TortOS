#!/bin/sh
# The TrimUI Model S's update zip (plorpos-80b.4): plorpOS-trimui-vVERSION/
# holding .tmp_update/updater and plorpOS/, both for the root of the card.
# Copied there, the stock boot loop runs the hook at the next start and
# plorpOS comes up instead of the stock menu; nothing is installed on the
# device, nothing is written to its NAND, and deleting plorpOS/ gives the
# stock menu back. Everything runs from the card (vfat, mounted exec).
#
#   mk/trimui-payload.sh VERSION    (make trimui-zip)
#
# PicoArch and the cores come from PICOARCH, a checkout of the fork
# thwonp/picoarch built there by its build-trimui.sh and
# build-trimui-cores.sh. PicoArch is GPL-2.0-or-later: the zip names the
# fork's commit and carries its changes against DrUm78/picoarch as a patch.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
V=$1
PICOARCH=${PICOARCH:?PICOARCH=<picoarch checkout built for trimui>}
STAGE=$ROOT/build/trimui/payload
TOP=plorpOS-trimui-v$V
P=$STAGE/$TOP/plorpOS
ZIP=$ROOT/out/plorpOS-trimui-v$V-UPDATE.zip
CORES="gambatte gpsp picodrive quicknes snes9x2002 pcsx_rearmed"

for f in shelf muse musectl trimuimon; do
	[ -x "$ROOT/build/trimui/$f" ] || { echo "no build/trimui/$f; run make trimui" >&2; exit 1; }
done
[ -x "$PICOARCH/picoarch" ] || { echo "no $PICOARCH/picoarch; run its build-trimui.sh" >&2; exit 1; }
for c in $CORES; do
	[ -f "$PICOARCH/${c}_libretro.so" ] || { echo "no $PICOARCH/${c}_libretro.so; run its build-trimui-cores.sh" >&2; exit 1; }
done
if [ -n "$(git -C "$PICOARCH" status --porcelain --untracked-files=no -- . ':!libpicofe')" ]; then
	echo "$PICOARCH has uncommitted changes; the patch would not match the binary" >&2
	exit 1
fi

rm -rf "$STAGE"
mkdir -p "$P/bin" "$P/lib" "$P/res" "$P/cores" "$P/src" "$STAGE/$TOP/.tmp_update" "$ROOT/out"
cp "$ROOT/src/trimui/updater" "$STAGE/$TOP/.tmp_update/"
cp "$ROOT/build/trimui/shelf" "$ROOT/build/trimui/muse" "$ROOT/build/trimui/musectl" \
   "$ROOT/build/trimui/trimuimon" "$PICOARCH/picoarch" "$P/bin/"
for l in libavformat.so.60 libavcodec.so.60 libavfilter.so.9 libswresample.so.4 libavutil.so.58; do
	cp -L "$ROOT/sdk-trimui/ffmpeg/usr/lib/$l" "$P/lib/"
done
cp "$ROOT/sdk-trimui/ffmpeg/usr/lib/COPYING.LGPLv2.1" "$ROOT/sdk-trimui/ffmpeg/usr/lib/COPYING.opus" "$P/lib/"
cp "$ROOT/res/fonts/menu.ttf" "$ROOT/res/fonts/OFL.txt" "$P/res/"
cp -r "$ROOT/res/nano/maps" "$P/res/"
: > "$P/cores/SOURCES.txt"
for c in $CORES; do
	cp "$PICOARCH/${c}_libretro.so" "$P/cores/"
	echo "$c $(git -C "$PICOARCH/$c" remote get-url origin) $(git -C "$PICOARCH/$c" rev-parse HEAD)" \
		>> "$P/cores/SOURCES.txt"
done
cp "$ROOT/src/trimui/start.sh" "$P/"
echo "$V" > "$P/VERSION"
BASE=$(git -C "$PICOARCH" merge-base HEAD origin/main)
git -C "$PICOARCH" diff "$BASE" HEAD -- . ':!libpicofe' > "$P/src/picoarch-plorpos.patch"
cat > "$P/src/SOURCE.txt" << T
plorpOS trimui v$V

shelf, muse, musectl, trimuimon: https://github.com/thwonp/plorpOS (MIT;
  src/nano/, src/muse/, tools/), built with shauninman/union-trimui-toolchain v001.
picoarch: GPL-2.0-or-later. Source: https://github.com/thwonp/picoarch at
  $(git -C "$PICOARCH" rev-parse HEAD | cut -c1-12), a fork of https://github.com/DrUm78/picoarch
  at $(echo "$BASE" | cut -c1-12); the changes are also picoarch-plorpos.patch here.
cores/: libretro cores, each under its own licence, built from the
  repositories and commits in cores/SOURCES.txt with the fork's patches/.
FFmpeg 6.1 (lib/): LGPL-2.1-or-later, https://ffmpeg.org/releases/ffmpeg-6.1.tar.gz,
  configured as mk/fetch-trimui-sdk.sh shows; licence in lib/COPYING.LGPLv2.1.
  libopus 1.5.2 (fixed point) is linked into libavcodec: BSD, lib/COPYING.opus.
Font (res/menu.ttf): SIL Open Font License, res/OFL.txt.
T
rm -f "$ZIP"
( cd "$STAGE" && zip -qrX "$ZIP" "$TOP" )
echo "$ZIP ($(du -k "$ZIP" | cut -f1) kB, md5 $(md5sum "$ZIP" | cut -c1-8))"
