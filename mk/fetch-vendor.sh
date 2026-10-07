#!/bin/sh
# Populate vendor/cores with the libretro cores TortOS redistributes.
#
# That is the whole of it. TortOS carries no third-party runtime libraries;
# measurement says nothing needs them:
#
#   the cores need  libc libm librt libstdc++ libgcc_s ld-linux
#   tortos.elf needs libSDL2 libSDL2_image libSDL2_ttf libm libdl libc
#
# Every one of those ships in the device's own firmware under /usr/trimui/lib
# or /usr/lib. Settings are TortOS's own code in src/platform.c, against the
# device's ALSA control and display-engine interfaces.
#
# Cores are libretro's own buildbot binaries, pinned by sha256 - the same hashes
# Diatom's CORES.md verifies, because they are the same binaries. They are
# fetched from a mirror, not the buildbot: the buildbot serves latest/ only and
# republishes on any upstream commit (weekly translation syncs included), so a
# pin against it breaks within days. By 2026-10-03 every core below had been
# republished, and snes9x2010's new build needs GLIBC_2.34, which the Brick
# lacks. The mirror is a release on our own repo holding the exact pinned
# bytes, unmodified; the hashes below are still what is trusted.
#
# Pinning a different core: verify it, attach it to a new release, point MIRROR
# there, and update the hash here and in Diatom's CORES.md together.
#
# Usage: mk/fetch-vendor.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VENDOR=$ROOT/vendor
DL=${1:-/tmp/tortos-vendor}
MIRROR=https://github.com/thwonp/TortOS/releases/download/cores-2026-08

command -v curl   > /dev/null || { echo "need curl" >&2; exit 1; }
command -v shasum > /dev/null || { echo "need shasum" >&2; exit 1; }

mkdir -p "$DL" "$VENDOR/cores"

fetch_core() { # name sha256
	if [ -f "$VENDOR/cores/${1}_libretro.so" ] &&
	   [ "$(shasum -a 256 "$VENDOR/cores/${1}_libretro.so" | cut -d' ' -f1)" = "$2" ]; then
		echo "  ok      $1"
		return
	fi
	# Verified in $DL and only then installed, so a bad download never
	# reaches vendor/cores.
	curl -sSfL -o "$DL/${1}_libretro.so" "$MIRROR/${1}_libretro.so"
	GOT=$(shasum -a 256 "$DL/${1}_libretro.so" | cut -d' ' -f1)
	if [ "$GOT" != "$2" ]; then
		echo "  MISMATCH $1" >&2
		echo "    want $2" >&2
		echo "    got  $GOT" >&2
		echo "  The mirror does not hold the pinned binary; vendor/cores untouched." >&2
		exit 1
	fi
	cp "$DL/${1}_libretro.so" "$VENDOR/cores/"
	echo "  fetched $1"
}

fetch_core fceumm            1b13b00d4680394dad8000d5175f97be727107e0945bc9b412da91d70c07b267
fetch_core snes9x2010        3933890f520abb9dbb0e5276460785b20ce54d25f552b369cafeca270b9dd44c
# mgba IS NOT FETCHED, and that is the one exception here. Run
# mk/build-mgba-bridge.sh instead; it builds this exact commit plus upstream's
# own fix for a crash the buildbot core still has.
#
#   fetch_core mgba            abde7a0764f08fa0cc2c7d3d9a29b9d1245a9f3b7df0e7a594b74df642ee53c6
#
# Every MBC2 Game Boy cartridge segfaults on the buildbot core - Kirby's Pinball
# Land, Wave Race, Golf, X, both Final Fantasy Legends. Regression in mgba
# a1b2b23, reported as mgba-emu/mgba#3859, fixed upstream 543a1975 the same day,
# and in no downloadable core because libretro builds from its own fork which
# has not synced since 2026-08-06.
#
# RESTORE THE LINE ABOVE AND DELETE THE BRIDGE the day that fork syncs. The
# official binary will contain exactly the change we are carrying; put it on a
# mirror release with the others and pin its new hash. The header of
# mk/build-mgba-bridge.sh says what to re-verify when swapping back.
# pcsx_rearmed IS NOT FETCHED either, and that one is permanent: the buildbot
# binary needs GLIBC_2.34 and the Brick has 2.33. Run mk/build-pcsx-rearmed.sh,
# which builds the same commit in the launcher's toolchain and pins its own sha.
# fbneo likewise (GLIBC_2.34 + GLIBCXX_3.4.29): run mk/build-fbneo.sh.
fetch_core picodrive         d0956ac7138ba4f8e5e849d4bd8c544f27108c17a03ea4944e56cc04a9c8028e
fetch_core mednafen_pce_fast aca90a14b18108c86398da2267ef40d5145eaddbc1c1b310614d745b258552b1
fetch_core mednafen_ngp      a2015668f9a9403b8bf6941b550fae2c618f37b79173e8ba27c95b95f96bdd99

echo "vendor/cores ready:"
ls "$VENDOR/cores"
