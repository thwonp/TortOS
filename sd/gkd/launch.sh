#!/bin/sh
# plorpOS boot entry on the GKD 350H Ultra: essway.service runs this in place
# of EmulationStation (sd/gkd/system.d/essway.service.d, installed by
# mk/gkd-deploy.sh boot). The GKD's counterpart of sd/tortos/launch.sh.
#
# Card out, or the launcher failing to start five times running: stock ES.

FALLBACK=/run/plorpos-fallback
# Set by an earlier run of this script that gave up. /run is tmpfs, so the
# next boot tries plorpOS again.
[ -e $FALLBACK ] && exec /usr/bin/start_es.sh

# essway's EnvironmentFile only parses /etc/profile; the Wayland variables
# are in the profile.d files it sources.
. /etc/profile

# plorpOS decides what the power and sleep keys do (gkd.7), so logind must
# not suspend under it. Held for the whole script, restarts included, and
# released on the way to ES.
if [ -z "$PLORPOS_INHIBITED" ]; then
	export PLORPOS_INHIBITED=1
	exec systemd-inhibit --what=handle-power-key:handle-suspend-key:handle-hibernate-key \
		--mode=block --who=plorpOS --why="plorpOS handles the power key" \
		/bin/sh "$0"
fi

CARD=/storage/games-external
DIR=$CARD/TortOS
LOGS=$CARD/.userdata/gkd/logs
mkdir -p "$LOGS" "$CARD/Saves" "$CARD/Bios"
# Ten boots kept, newest first, as on the Brick (sd/tortos/launch.sh), so
# Over The Hare's Download logs has the boots a report is about.
LOG=$LOGS/tortos.log
i=9
while [ $i -gt 1 ]; do
	[ -f "$LOG.$((i - 1))" ] && mv -f "$LOG.$((i - 1))" "$LOG.$i"
	i=$((i - 1))
done
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
: > "$LOG"

# The resident emulator, as on the Brick. A child of this script, so it lives
# through launcher restarts; respawn_resident starts it again if it dies.
export TORTOS_DIATOM_SOCKET=/tmp/diatom.sock
start_resident() {
	pgrep -f "TortOS/diatom --socket" > /dev/null && return
	rm -f "$TORTOS_DIATOM_SOCKET"
	"$DIR/diatom" --socket "$TORTOS_DIATOM_SOCKET" --cores "$DIR/cores" \
		--save "$CARD/Saves" --system "$CARD/Bios" >> "$LOG" 2>&1 &
}

rm -f /tmp/tortos_poweroff
cd "$DIR"
FAILS=0
while : ; do
	start_resident
	START=$(cut -d. -f1 /proc/uptime)
	./tortos.elf >> "$LOG" 2>&1
	if [ -f /tmp/tortos_poweroff ]; then
		sync
		exec systemctl poweroff
	fi
	END=$(cut -d. -f1 /proc/uptime)
	# A launcher that dies immediately, five times running, is not going to
	# start on the sixth: hand the device to ES until the next boot.
	if [ $((END - START)) -lt 5 ]; then
		FAILS=$((FAILS + 1))
		[ $FAILS -ge 5 ] && break
	else
		FAILS=0
	fi
	sleep 1
done

echo "launcher failed $FAILS times; falling back to ES" >> "$LOG"
touch $FALLBACK
pkill -f "TortOS/diatom --socket"
systemctl start input.service
# essway restarts this script (Restart=always), which sees the flag and runs
# ES outside the inhibitor.
exit 0
