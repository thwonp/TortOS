#!/bin/sh
# Every --shot the launcher can draw, rendered headless into one directory.
#
# The point is comparison: run it before a change and after, then
#   diff -rq <before> <after>
# A layout change that must not move the Brick (gkd.11) is checked by this
# coming back empty. The shots are deterministic - same source, same card,
# same image, same bytes - which is what mk/shots.Dockerfile is for.
#
# The card is a copy of a real one (TortOS/, .userdata/, Saves/, the Roms
# tree's .media art, the ROMs themselves as empty files). It is mounted as an
# overlay, so whatever a shot writes - the library db, a log - is thrown away
# and every run starts from the same card.
#
# Usage: TORTOS_SHOT_CARD=/path/to/card mk/shots.sh <outdir>
#
# SHOT_WINDOW=1600x1440 renders as the GKD instead of the Brick (the host
# build's TORTOS_WINDOW); unset, the shots are the Brick's. SHOT_BATT is the
# battery they show (TORTOS_FAKE_BATT: 80 unset, "80c" charging).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?usage: TORTOS_SHOT_CARD=/path/to/card mk/shots.sh <outdir>}
CARD=${TORTOS_SHOT_CARD:?set TORTOS_SHOT_CARD to a copy of a card}
IMAGE=${SHOTS_IMAGE:-tortos-shots}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
docker image inspect "$IMAGE" > /dev/null 2>&1 ||
	docker build -f "$ROOT/mk/shots.Dockerfile" -t "$IMAGE" "$ROOT/mk"

# The list. One line per shot: a name, then the launcher's arguments. --phase 0
# on all of them freezes whatever pulses or scrolls, so a still is the same
# still every time.
cat > "$OUT/.list" << 'EOF'
systems
systems-move       --sysmove 0.5
games              --screen games
games-move         --screen games --gamemove 0.5
games-jump         --screen games --jump 3
menu-top           --menu 0
menu-low           --screen games --menu 9
keyboard-0         --keyboard 0
keyboard-1         --keyboard 1
keyboard-2         --keyboard 2
cheevos            --screen games --cheevos-screen 0
cheevos-sel        --screen games --cheevos-screen 5
synopsis           --screen games --synopsis
info               --screen games --info
controls-0         --controls 0
controls-1         --controls 1
controls-2         --controls 2
controls-3         --controls 3
art                --art
hare               --hare
wait               --wait Wi-Fi
notice             --screen games --notice Violated_Heavens
slots-0            --screen games --slots 0
slots-wide         --screen games --slots 2 1.78
nowplaying         --nowplaying 0 1 30
nowplaying-paused  --nowplaying 0 1 30 --paused
EOF

# The vendored objects get their own directory: the host checks build the same
# ones into build-native/tp with the host libc, which this image cannot link.
docker run --rm -e TORTOS_WINDOW="${SHOT_WINDOW:-}" -e SHOT_BATT="${SHOT_BATT:-80}" -v "$ROOT:/work" -v "$CARD:/card:O" -v "$OUT:/out" -w /work \
	"$IMAGE" sh -c '
set -e
make -f mk/native.mk VERSION=shots TP_OUT=build-native/tp-shots > /out/.build.log 2>&1 ||
	{ tail -20 /out/.build.log; exit 1; }
export TORTOS_ROOT=/card/TortOS TORTOS_CARD=/card TORTOS_ROMS=/card/Roms \
       TORTOS_USERDATA=/card/.userdata/tg3040 \
       TORTOS_SHARED=/card/.userdata/shared \
       TORTOS_FONT=/card/TortOS/menu.ttf TORTOS_FAKE_BATT=$SHOT_BATT
# No font is not an error to the launcher - it draws the shelf without text -
# and every panel would be a collapsed line in every shot.
[ -s "$TORTOS_FONT" ] || { echo "no font at $TORTOS_FONT"; exit 1; }
xvfb-run -a -s "-screen 0 1600x1440x24" sh -c "
	while read -r name args; do
		build-native/tortos --shot /out/\$name.png --phase 0 \$args \
			> /out/.\$name.log 2>&1 || echo \"FAILED \$name\"
		[ -s /out/\$name.png ] || echo \"NO SHOT \$name\"
	done < /out/.list
"'
ls "$OUT"/*.png | wc -l | sed 's/$/ shots/'
