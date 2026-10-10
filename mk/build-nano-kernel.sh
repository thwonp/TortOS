#!/bin/sh
# The RG Nano's kernel with plorpOS's boot logo (plorpos-ggv.41.8), into
# build/nano/zImage, for mk/nano-image.sh:
#
#   mk/build-nano-kernel.sh    (make nano-kernel)
#
# The boot logo is compiled into the kernel (CONFIG_LOGO,
# drivers/video/logo/logo_funkey_clut224.ppm). This builds exactly the kernel
# of DrUm78's fps-classics image - his linux tag v1.0-rg-nano with his
# FunKey-OS rg_nano linux.config, no patches (his Buildroot has none for
# linux), the FunKey SDK's GCC 10.2 as Buildroot used - with that one file
# replaced by res/nano/bootlogo.png: DrUm78's logo, "DrUm78's Custom OS"
# struck through and "plorpOS nano" above it. The release must stay
# 4.14.14-funkey, or the image's /lib/modules no longer load.
#
# Needs: docker, ImageMagick (magick), the Nano SDK (mk/fetch-nano-sdk.sh).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
K=$ROOT/build/nano/kernel
TAG=v1.0-rg-nano
TAR=$K/$TAG.tar.gz
TAR_SHA=d2f930b114da2e520f4b79f2c265df2d410c067b0f89aa5891e6fbc29e5de136
CFG_URL=https://raw.githubusercontent.com/DrUm78/FunKey-OS/384286a5e41ed160bc58bd3a3d5e22cfe1c5eb2e/FunKey/board/funkey/linux.config
CFG_SHA=7c1774626eb8dca22ea5277227ac516cf1f56566248694ce42187d6e9ce8b7a8
IMAGE=debian@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251   # bookworm-slim
RELEASE=4.14.14-funkey

command -v docker >/dev/null || { echo "need docker" >&2; exit 1; }
command -v magick >/dev/null || { echo "need ImageMagick (magick)" >&2; exit 1; }
[ -x "$ROOT/sdk-nano/sdk/bin/arm-funkey-linux-musleabihf-gcc" ] || { echo "no Nano SDK; run mk/fetch-nano-sdk.sh" >&2; exit 1; }
mkdir -p "$K"
[ -f "$TAR" ] || curl -sfL -o "$TAR" "https://github.com/DrUm78/linux/archive/refs/tags/$TAG.tar.gz"
echo "$TAR_SHA  $TAR" | sha256sum -c --quiet
[ -f "$K/linux.config" ] || curl -sfL -o "$K/linux.config" "$CFG_URL"
echo "$CFG_SHA  $K/linux.config" | sha256sum -c --quiet

rm -rf "$K/src"
mkdir -p "$K/src"
tar -xzf "$TAR" -C "$K/src" --strip-components=1
cp "$K/linux.config" "$K/src/.config"
# The kernel's logo format: plain PPM, at most 224 colours.
magick "$ROOT/res/nano/bootlogo.png" +dither -colors 224 -compress none \
	"$K/src/drivers/video/logo/logo_funkey_clut224.ppm"

docker run --rm -v "$K/src":/k -v "$ROOT/sdk-nano/sdk":/sdk:ro -w /k "$IMAGE" sh -c '
	set -e
	apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc make bc lzop perl libc6-dev >/dev/null
	M="make ARCH=arm CROSS_COMPILE=/sdk/bin/arm-funkey-linux-musleabihf- HOSTCFLAGS=-fcommon"
	$M olddefconfig >/dev/null
	[ "$($M -s kernelrelease)" = '"$RELEASE"' ] || { echo "kernel release $($M -s kernelrelease), not '"$RELEASE"'" >&2; exit 1; }
	$M -j$(nproc) zImage >/dev/null
'
cp "$K/src/arch/arm/boot/zImage" "$ROOT/build/nano/zImage"
echo "build/nano/zImage ($(du -k "$ROOT/build/nano/zImage" | cut -f1) kB, md5 $(md5sum "$ROOT/build/nano/zImage" | cut -c1-8)), $RELEASE"
