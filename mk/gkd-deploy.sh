#!/bin/sh
# Push a build to the GKD 350H Ultra over ssh (key auth; ROCKNIX). The GKD's
# counterpart of mk/adb-deploy.sh.
#
# Everything lives on the SD card, mounted by ROCKNIX at /storage/games-external
# (and again at /storage/roms). Nothing supervises the launcher there yet
# (gkd.4), so there is no restart: run it by hand.
#
# Usage: mk/gkd-deploy.sh [elf|res|vendor|diatom]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WHAT=${1:-elf}
GKD=${GKD:-root@192.168.0.55}
CARD=/storage/games-external
P=$CARD/TortOS

S="ssh -o ConnectTimeout=5 $GKD"
$S true 2> /dev/null || { echo "no GKD over ssh at $GKD"; exit 1; }

# The card, not the directory under it: with the card out, /storage/games-
# external is an empty directory on internal storage and a push "succeeds".
$S "grep -q ' $CARD ' /proc/mounts" || {
	echo "!! $CARD is not mounted on the GKD -- nothing was deployed."
	exit 1
}
$S "mkdir -p $P/cards $P/cores $P/res/web"

# scp, not tar: a handful of files, and the card is exFAT, which keeps no
# owners or modes for tar to restore (its fmask leaves every file executable).
put() { scp -q -r "$@"; }

case $WHAT in elf)
	[ -f "$ROOT/build/gkd/tortos.elf" ] || { echo "run make PLATFORM=gkd first"; exit 1; }
	put "$ROOT/build/gkd/tortos.elf" "$ROOT/config/systems.cfg" "$GKD:$P/"
	echo "  + launcher"
esac
case $WHAT in res)
	put "$ROOT/res/cards/"* "$GKD:$P/cards/"
	put "$ROOT/res/fonts/menu.ttf" "$ROOT/res/ssl/cacert.pem" "$GKD:$P/"
	put "$ROOT/res/web/"* "$GKD:$P/res/web/"
	put "$ROOT/res/fonts/menu.ttf" "$GKD:$P/res/web/menu.ttf"
	echo "  + assets"
esac
case $WHAT in diatom)
	D=${DIATOM_ELF:-$ROOT/../diatom/build/gkd/diatom}
	[ -f "$D" ] || { echo "no diatom at $D (set DIATOM_ELF)"; exit 1; }
	put "$D" "$GKD:$P/diatom"
	echo "  + diatom"
esac
case $WHAT in vendor)
	put "$ROOT/vendor/cores/"* "$GKD:$P/cores/"
	echo "  + cores"
esac
$S sync
echo "deployed '$WHAT' to $GKD"
