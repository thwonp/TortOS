#!/bin/sh
# The GKD 350H Ultra's sysroot, for `make PLATFORM=gkd`: the device's own SDL2,
# SDL2_image and SDL2_ttf (and whatever they pull in) from ROCKNIX's /usr/lib
# over ssh, plus the matching upstream headers, hash-pinned. The same idea as
# mk/fetch-sysroot.sh for the Brick; the pull and the dependency closure are
# diatom's tools/fetch-gkd-sysroot.sh, which has built its GKD port since
# gkd.2.
#
# glibc is not pulled: the toolchain's own (2.31) links, and the device's 2.40
# runs what it links. Everything else a library NEEDs is, so the linker can
# follow each one through -rpath-link rather than warn that it cannot.
#
# Usage: mk/fetch-gkd-sysroot.sh [out-dir]   (default sysroot-gkd/)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/sysroot-gkd}
GKD=${GKD:-root@192.168.0.55}
IMAGE=tortos-toolchain
DL=$OUT/.cache

# The versions ROCKNIX ships (libSDL2-2.0.so.0.3200.6, libSDL2_image-2.0.so.0.
# 800.2, libSDL2_ttf-2.0.so.0.2000.2). SDL2's hash is diatom's; SDL2_ttf's is
# the Brick's (same release); SDL2_image's was taken from the GitHub release
# on 2026-09-29.
SDL_VER=2.32.6
SDL_SHA=6a7a40d6c2e00016791815e1a9f4042809210bdf10cc78d2c75b45c4f52f93ad
IMG_VER=2.8.2
IMG_SHA=8f486bbfbcf8464dd58c9e5d93394ab0255ce68b51c5a966a918244820a76ddc
TTF_VER=2.20.2
TTF_SHA=9dc71ed93487521b107a2c4a9ca6bf43fb62f6bddd5c26b055e6b91418a22053

dev() { ssh -o ConnectTimeout=5 "$GKD" "$@"; }
dev true 2> /dev/null || { echo "no GKD over ssh at $GKD" >&2; exit 1; }
docker image inspect "$IMAGE" > /dev/null 2>&1 ||
	{ echo "toolchain image missing; run: make toolchain" >&2; exit 1; }
command -v curl > /dev/null || { echo "need curl" >&2; exit 1; }

LIB=$OUT/usr/lib
INC=$OUT/usr/include/SDL2
mkdir -p "$LIB" "$INC" "$DL"

pull() { scp -q "$GKD:/usr/lib/$1" "$LIB/" || { echo "could not pull /usr/lib/$1" >&2; exit 1; }; }

echo "pulling SDL from the GKD"
for base in SDL2-2.0 SDL2_image-2.0 SDL2_ttf-2.0; do
	pull "lib$base.so.0"
	ln -sf "lib$base.so.0" "$LIB/lib${base%-2.0}.so"   # what -lSDL2 etc. find
done

# Every NEEDED of every library here, until nothing is missing. glibc's own
# sonames are the toolchain's business, not the sysroot's.
GLIBC='libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libresolv.so.2
libutil.so.1 ld-linux-aarch64.so.1 libgcc_s.so.1'
needed() {
	docker run --rm -v "$LIB":/libs "$IMAGE" sh -c \
	  'for f in /libs/*.so*; do aarch64-linux-gnu-readelf -d "$f" 2>/dev/null; done' |
	  sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p' | sort -u
}
pass=0
while :; do
	pass=$((pass + 1))
	[ $pass -le 10 ] || { echo "dependency closure did not converge" >&2; exit 1; }
	missing=
	for so in $(needed); do
		[ -e "$LIB/$so" ] && continue
		case " $(echo $GLIBC) " in *" $so "*) continue ;; esac
		missing="$missing $so"
	done
	[ -n "$missing" ] || break
	for so in $missing; do echo "  + $so"; pull "$so"; done
done

fetch_headers() { # name version sha url subdir
	f="$DL/$1-$2.tar.gz"
	[ -f "$f" ] || curl -sSfL -o "$f" "$4"
	got=$(sha256sum "$f" | cut -d' ' -f1)
	[ "$got" = "$3" ] || { echo "$1: hash mismatch: $got" >&2; rm -f "$f"; exit 1; }
	rm -rf "${DL:?}/$5"
	tar -xzf "$f" -C "$DL"
	cp "$DL/$5"/include/*.h "$INC/" 2> /dev/null || true
	cp "$DL/$5"/SDL_*.h     "$INC/" 2> /dev/null || true
}
echo "fetching headers"
fetch_headers SDL2 "$SDL_VER" "$SDL_SHA" \
	"https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" \
	"SDL2-$SDL_VER"
fetch_headers SDL2_image "$IMG_VER" "$IMG_SHA" \
	"https://github.com/libsdl-org/SDL_image/releases/download/release-$IMG_VER/SDL2_image-$IMG_VER.tar.gz" \
	"SDL2_image-$IMG_VER"
fetch_headers SDL2_ttf "$TTF_VER" "$TTF_SHA" \
	"https://github.com/libsdl-org/SDL_ttf/releases/download/release-$TTF_VER/SDL2_ttf-$TTF_VER.tar.gz" \
	"SDL2_ttf-$TTF_VER"
for h in SDL.h SDL_image.h SDL_ttf.h; do
	[ -f "$INC/$h" ] || { echo "no $h extracted" >&2; exit 1; }
done

{
	echo "generated: $(date +%Y-%m-%d)"
	echo "device:    $GKD ($(dev hostname))"
	echo "kernel:    $(dev uname -r)"
	echo "glibc:     $(dev /usr/lib/libc.so.6 | head -1)"
	echo "headers:   SDL2 $SDL_VER, SDL2_image $IMG_VER, SDL2_ttf $TTF_VER (upstream, sha256-pinned)"
} > "$OUT/PROVENANCE"
echo "done:"
ls "$LIB"
cat "$OUT/PROVENANCE"
