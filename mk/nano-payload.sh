#!/bin/sh
# The RG Nano's install zip (plorpos-ggv.9): one folder, plorpOS/, for the
# root of the card's shared partition (/mnt). Everything runs from there - the
# partition is vfat but mounted exec, and the libraries are copied under their
# soname file names, so it needs no symlinks.
#
#   mk/nano-payload.sh VERSION    (make PLATFORM=nano zip)
#
# PicoArch is the fork thwonp/picoarch, checked out and built at $PICOARCH by
# `make nano-picoarch`. It is GPL-2.0-or-later: the zip names the fork's
# commit and carries the changes against DrUm78/picoarch as a patch.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
V=$1
PICOARCH=${PICOARCH:-$ROOT/build/nano/picoarch}
STAGE=$ROOT/build/nano/payload
P=$STAGE/plorpOS
ZIP=$ROOT/out/plorpOS-nano-v$V.zip

for f in nanoshelf muse musectl; do
	[ -x "$ROOT/build/nano/$f" ] || { echo "no build/nano/$f; run make PLATFORM=nano" >&2; exit 1; }
done
[ -x "$PICOARCH/picoarch" ] || { echo "no $PICOARCH/picoarch; run make nano-picoarch" >&2; exit 1; }
if [ -n "$(git -C "$PICOARCH" status --porcelain --untracked-files=no -- . ':!libpicofe')" ]; then
	echo "$PICOARCH has uncommitted changes; the patch would not match the binary" >&2
	exit 1
fi

rm -rf "$STAGE"
mkdir -p "$P/bin" "$P/lib" "$P/res" "$P/src" "$ROOT/out"
cp "$ROOT/build/nano/nanoshelf" "$ROOT/build/nano/muse" "$ROOT/build/nano/musectl" \
   "$PICOARCH/picoarch" "$P/bin/"
for l in libavformat.so.60 libavcodec.so.60 libavfilter.so.9 libswresample.so.4 libavutil.so.58; do
	cp -L "$ROOT/sdk-nano/ffmpeg/usr/lib/$l" "$P/lib/"
done
cp "$ROOT/sdk-nano/ffmpeg/usr/lib/COPYING.LGPLv2.1" "$P/lib/"
cp "$ROOT/res/fonts/menu.ttf" "$ROOT/res/fonts/OFL.txt" "$P/res/"
for f in start.sh install-root.sh uninstall-root.sh; do
	[ -f "$ROOT/src/nano/$f" ] && cp "$ROOT/src/nano/$f" "$P/"
done
[ -d "$ROOT/src/nano/root" ] && cp -r "$ROOT/src/nano/root" "$P/"
BASE=$(git -C "$PICOARCH" merge-base HEAD origin/main)
git -C "$PICOARCH" diff "$BASE" HEAD -- . ':!libpicofe' \
	> "$P/src/picoarch-plorpos.patch"
cat > "$P/src/SOURCE.txt" << T
plorpOS nano v$V

nanoshelf, muse, musectl: https://github.com/thwonp/TortOS (MIT; src/nano/,
  src/muse/), built with FunKey-sdk-2.3.0.
picoarch: GPL-2.0-or-later. Source: https://github.com/thwonp/picoarch at
  $(git -C "$PICOARCH" rev-parse HEAD | cut -c1-12) (branch plorpos-nano), a fork of
  https://github.com/DrUm78/picoarch at $(echo "$BASE" | cut -c1-12); the changes are also
  picoarch-plorpos.patch here.
FFmpeg 6.1 (lib/): LGPL-2.1-or-later, https://ffmpeg.org/releases/ffmpeg-6.1.tar.gz,
  configured as mk/fetch-nano-sdk.sh shows; licence in lib/COPYING.LGPLv2.1.
Font (res/menu.ttf): SIL Open Font License, res/OFL.txt.
T
rm -f "$ZIP"
( cd "$STAGE" && zip -qrX "$ZIP" plorpOS )
echo "$ZIP ($(du -k "$ZIP" | cut -f1) kB, md5 $(md5sum "$ZIP" | cut -c1-8))"
