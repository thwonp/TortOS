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
# For radio.sh and bt-alsa.sh, here and in plat_sleep()'s shell, which sees
# only the launcher's environment. Bonds live where BaseOS's bluetoothd keeps
# them (/var/lib/bluetooth -> /data/bluetooth); bt-alsa.sh's guess would miss.
export TORTOS_DIR=$DIR USERDATA_PATH=$TORTOS_USERDATA LOGS_PATH=$LOGS
export TORTOS_BT_BONDS=/var/lib/bluetooth
# .asoundrc as a top-level config file, so a headset paired after Diatom or
# Muse opened ALSA is found at their next open (the Brick's launch.sh says
# how that was measured). alsa.conf still loads BaseOS's own asound.conf.
export ALSA_CONFIG_PATH=/usr/share/alsa/alsa.conf:$HOME/.asoundrc
# Into the log, without the bluealsa ALSA plugin's debug lines: BaseOS ships
# a debug build, ~20 `[pid] D: ...` lines on every open and close of a
# headset. awk with fflush so the rest still lands line by line.
logf() { awk '!/^\[[0-9]+\] D: /{ print; fflush() }' >> "$LOG"; }
mkdir -p "$LOGS" "$SD/Bios" "$SD/Saves" "$SD/Roms" "$SD/.userdata/shared"
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
: > "$LOG"
echo "plorpOS on $BASEOS_MODEL, BaseOS $BASEOS_VERSION, uptime $(cut -d' ' -f1 /proc/uptime)" >> "$LOG"

# Wi-Fi as the player left it (boot.env, which the launcher writes on every
# change): join the saved networks in the background, so the shelf never waits
# for a radio that can take until ~7 s uptime. The lease is asked for only once
# the link is up - asked earlier, udhcpc burns its tries on a dead link. The
# launcher does the same from then on (src/wifi.c).
WIFI=0
[ -f "$TORTOS_USERDATA/boot.env" ] && . "$TORTOS_USERDATA/boot.env"
WPA_CONF=$TORTOS_USERDATA/wpa_supplicant.conf
if [ "$WIFI" = 1 ] && [ -f "$WPA_CONF" ]; then
	(
		i=0
		while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 25 ]; do sleep 1; i=$((i + 1)); done
		wpa_supplicant -B -D nl80211,wext -i wlan0 -C /run/wpa_supplicant -c "$WPA_CONF" || exit
		i=0
		until wpa_cli -p /run/wpa_supplicant -i wlan0 status 2> /dev/null | grep -q '^wpa_state=COMPLETED'; do
			[ $i -ge 20 ] && exit
			sleep 1; i=$((i + 1))
		done
		udhcpc -i wlan0 -S -t 5 -T 7 -b -q
	) > /dev/null 2>&1 &
fi

# Bluetooth as the player left it, off unless asked for (battery), in the
# background: the attach and bluetoothd take seconds. radio.sh says the rest.
[ -f "$DIR/radio.sh" ] && . "$DIR/radio.sh"
[ "$BLUETOOTH" = 1 ] && bt_on > /dev/null 2>&1 &

export TORTOS_DIATOM_SOCKET=/tmp/diatom.sock
start_resident() {
	pidof diatom > /dev/null && return
	[ -x "$DIR/diatom" ] || return
	rm -f "$TORTOS_DIATOM_SOCKET"
	# Idle I/O class (the card is on CFQ): its premap of every core reads
	# ~1.8 s of fbneo alone off the card at boot, beside the launcher's own
	# start, and put the shelf up 2184 ms after the launcher began instead of
	# 1615 (plorpos-7ny.8). Idle only yields while someone else reads; in a
	# game the launcher reads nothing.
	ionice -c 3 "$DIR/diatom" --socket "$TORTOS_DIATOM_SOCKET" --cores "$DIR/cores" \
		--save "$SD/Saves" --system "$SD/Bios" 2>&1 | logf &
}

rm -f /tmp/tortos_poweroff
cd "$DIR" || { echo "no $DIR" >> "$LOG"; while :; do sleep 3600; done; }
FAILS=0
# One PCM per bonded headset, BEFORE the first start_resident: alsa-lib reads
# its config at the first open.
bt_write_asoundrc
while :; do
	start_resident
	START=$(cut -d. -f1 /proc/uptime)
	./tortos.elf 2>&1 | logf
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
