#!/bin/sh
# Push a build to the GKD 350H Ultra over ssh (key auth; ROCKNIX). The GKD's
# counterpart of mk/adb-deploy.sh.
#
# Everything lives on the SD card, mounted by ROCKNIX at /storage/games-external
# (and again at /storage/roms). Binaries go in under a new name and are renamed
# over the old, so a running launcher or diatom is not in the way; the new one
# runs from its next start. Nothing is restarted.
#
# boot installs the switch from ES to plorpOS: sd/gkd/launch.sh on the card and
# the systemd drop-ins under sd/gkd/system.d in /storage/.config/system.d. It
# takes effect at the next boot. Card out = stock ES; delete the drop-ins
# (the plorpos.conf files) to remove it for good.
#
# Usage: mk/gkd-deploy.sh [elf|res|vendor|diatom|boot]
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
$S "mkdir -p $P/cards $P/cores $P/res/web $P/shaders"

# scp, not tar: a handful of files, and the card is exFAT, which keeps no
# owners or modes for tar to restore (its fmask leaves every file executable).
put() { scp -q -r "$@"; }
# One binary, to $P/<name>, around whatever is running it.
swap() { put "$1" "$GKD:$P/$2.new" && $S "mv -f $P/$2.new $P/$2"; }

case $WHAT in elf)
	[ -f "$ROOT/build/gkd/tortos.elf" ] || { echo "run make PLATFORM=gkd first"; exit 1; }
	swap "$ROOT/build/gkd/tortos.elf" tortos.elf
	put "$ROOT/config/systems.cfg" "$GKD:$P/"
	echo "  + launcher"
	# Muse runs until it is told to quit or dies; the launcher starts the new
	# one the next time it finds no socket.
	swap "$ROOT/build/gkd/muse" muse
	swap "$ROOT/build/gkd/musectl" musectl
	echo "  + muse, musectl"
esac
case $WHAT in res)
	put "$ROOT/res/cards/"* "$GKD:$P/cards/"
	put "$ROOT/res/fonts/menu.ttf" "$ROOT/res/ssl/cacert.pem" "$GKD:$P/"
	put "$ROOT/res/web/"* "$GKD:$P/res/web/"
	put "$ROOT/res/fbneo-titles.tsv" "$GKD:$P/res/"
	put "$ROOT/res/fonts/menu.ttf" "$GKD:$P/res/web/menu.ttf"
	# diatom's GLSL passes and the in-game menu's list of them (plorpos-gkd.72)
	put "$ROOT/res/shaders/"* "$GKD:$P/shaders/"
	echo "  + assets"
esac
case $WHAT in diatom)
	D=${DIATOM_ELF:-$ROOT/../diatom/build/gkd/diatom}
	[ -f "$D" ] || { echo "no diatom at $D (set DIATOM_ELF)"; exit 1; }
	swap "$D" diatom
	echo "  + diatom"
esac
case $WHAT in boot)
	put "$ROOT/sd/gkd/launch.sh" "$GKD:$P/"
	$S "mkdir -p /storage/.config/system.d"
	put "$ROOT/sd/gkd/system.d/"* "$GKD:/storage/.config/system.d/"
	$S "systemctl daemon-reload"
	echo "  + boot (from the next boot)"
esac
case $WHAT in vendor)
	put "$ROOT/vendor/cores/"* "$GKD:$P/cores/"
	echo "  + cores"
esac
$S sync
echo "deployed '$WHAT' to $GKD"
