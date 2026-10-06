#!/bin/sh
# plorpOS on an Anbernic H700 under BaseOS (plorpos-7ny). Copied to the card as
# System/launch_frontend.sh, which BaseOS's frontend-session runs through
# /bin/sh from /mnt/sdcard, with its output in /tmp/generic.log.
#
# THIS SCRIPT MUST NOT EXIT. busybox init respawns frontend-session when it
# does, and a launcher that cannot start would then loop forever. So a failing
# launcher ends in a sleep, with the reason in the log.
SD=/mnt/SDCARD
DIR=$SD/TortOS

# Which device: BaseOS writes it at build time. The userdata directory is per
# device, so one card can move between H700s without mixing their settings.
DEVICE=rgsp
[ -f /etc/baseos-release ] && . /etc/baseos-release && DEVICE=${BASEOS_DEVICE:-rgsp}
export TORTOS_USERDATA=$SD/.userdata/$DEVICE
export HOME=$TORTOS_USERDATA
# Our SDL2: BaseOS has no aarch64 one (mk/fetch-h700-sysroot.sh).
export LD_LIBRARY_PATH=$DIR/lib
LOGS=$TORTOS_USERDATA/logs
LOG=$LOGS/tortos.log
mkdir -p "$LOGS" "$SD/Bios" "$SD/Saves" "$SD/Roms" "$SD/.userdata/shared"
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
: > "$LOG"
echo "plorpOS on $BASEOS_MODEL, BaseOS $BASEOS_VERSION, uptime $(cut -d' ' -f1 /proc/uptime)" >> "$LOG"

# DEVELOPMENT ONLY, until the launcher joins Wi-Fi itself (plorpos-7ny.5): the
# spike's bring-up from System/wpa_supplicant.conf, in the background so the
# launcher does not wait for the radio, which can take until ~7 s uptime.
WPA_CONF=$SD/System/wpa_supplicant.conf
if [ -f "$WPA_CONF" ]; then
	(
		i=0
		while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 25 ]; do sleep 1; i=$((i + 1)); done
		ip link set wlan0 up
		wpa_supplicant -B -D nl80211,wext -i wlan0 -C /run/wpa_supplicant -c "$WPA_CONF" &&
			udhcpc -b -t 10 -T 2 -i wlan0 > /dev/null 2>&1
	) >> "$LOG" 2>&1 &
fi

export TORTOS_DIATOM_SOCKET=/tmp/diatom.sock
start_resident() {
	pidof diatom > /dev/null && return
	[ -x "$DIR/diatom" ] || return
	rm -f "$TORTOS_DIATOM_SOCKET"
	"$DIR/diatom" --socket "$TORTOS_DIATOM_SOCKET" --cores "$DIR/cores" \
		--save "$SD/Saves" --system "$SD/Bios" >> "$LOG" 2>&1 &
}

rm -f /tmp/tortos_poweroff
cd "$DIR" || { echo "no $DIR" >> "$LOG"; while :; do sleep 3600; done; }
FAILS=0
while :; do
	start_resident
	START=$(cut -d. -f1 /proc/uptime)
	./tortos.elf >> "$LOG" 2>&1
	if [ -f /tmp/tortos_poweroff ]; then
		sync
		exec poweroff        # BaseOS's: the PMIC's own power-off
	fi
	END=$(cut -d. -f1 /proc/uptime)
	if [ $((END - START)) -lt 5 ]; then
		FAILS=$((FAILS + 1))
		[ $FAILS -ge 5 ] && break
	else
		FAILS=0
	fi
	sleep 1
done
echo "launcher failed $FAILS times in a row; idling (see above)" >> "$LOG"
killall diatom 2> /dev/null
while :; do sleep 3600; done
