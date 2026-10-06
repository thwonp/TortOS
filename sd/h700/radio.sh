# SPDX-License-Identifier: MIT
# Bluetooth on an H700 under BaseOS (plorpos-7ny.26): the Brick's radio.sh
# shape - bt_on, bt_off, bt_reconnect, the same files to the launcher - on
# BaseOS's tools. Wi-Fi is not here: the launcher runs it (src/wifi.c).
#
# Sourced by launch_frontend.sh at boot and by plat_sleep() around a suspend
# (src/platform_h700.c), as `/bin/sh -c '. "$0" && fn' radio.sh`. Shipped as
# TortOS/radio.sh, the name the launcher calls on every platform.
#
# What differs from the Brick, and why:
#   - The bring-up is BaseOS's own: /mnt/vendor/ctrl/setBluetooth.sh attaches
#     the RTL8821CS over ttyS1 and raises hci0, BaseOS's systemctl shim starts
#     dbus and bluetoothd. Neither unblocks rfkill, so that is done here.
#   - Everything long-lived under setsid: started from a shell that later
#     ends, bluetoothd and bluealsa died with it (SIGHUP, 2026-10-06).
#   - No hcitool on BaseOS. Who is connected comes from bluetoothctl; a link
#     is told apart by its A2DP transport's path, whose fdN BlueZ numbers
#     afresh for every stream (fd0, then fd1 after a reconnect, 2026-10-06).
#   - a2dp-source only, no hfp-ag: audio out is the whole job, and a headset
#     played with a2dp-source alone.

# bt_pcm_name and bt_write_asoundrc. Guarded: `.` on a missing file ends a
# non-interactive shell.
[ -f "$TORTOS_DIR/bt-alsa.sh" ] && . "$TORTOS_DIR/bt-alsa.sh"

BT_RECONNECT_PIDFILE=/tmp/tortos_bt_reconnect.pid
# The headset last published, dialed first: with two in reach, a wake or a
# boot brings back the one in use rather than the first in address order
# (the Brick's way; the user's choice, 2026-10-06). On the card, so a reboot
# keeps it; written only when it changes.
BT_LAST=$USERDATA_PATH/bt_last

# Stop one process by name and wait up to 2 s for it to be gone.
bt_stop_proc() {
	killall -q "$1" 2> /dev/null
	n=0
	while pidof "$1" > /dev/null && [ $n -lt 20 ]; do
		sleep 0.1; n=$((n + 1))
	done
}

# The kernel's own list of links: one hci0:<handle> per ACL connection.
bt_any_link() {
	ls -d /sys/class/bluetooth/hci0:* > /dev/null 2>&1
}

bt_off() {
	if [ -f "$BT_RECONNECT_PIDFILE" ]; then
		kill "$(cat "$BT_RECONNECT_PIDFILE")" 2> /dev/null
		rm -f "$BT_RECONNECT_PIDFILE"
	fi
	# No radio, no sink: the launcher must not route at a headset that is gone.
	rm -f /tmp/tortos_btsink
	killall -q btplayer 2> /dev/null
	# The Brick's order (radio.sh, plorpos-pky.11): close the links while
	# bluetoothd still owns them, then the daemons, then the adapter, then
	# the UART and the rail. Pulling them all at once, with a headset
	# streaming, corrupted the 4.9 kernel's L2CAP state there. bluetoothctl
	# only with a bluetoothd to talk to: without one it waits forever.
	if pidof bluetoothd > /dev/null; then
		for mac in $(timeout 5 bluetoothctl devices Connected 2> /dev/null | awk '/^Device/{print $2}'); do
			timeout 5 bluetoothctl disconnect "$mac" > /dev/null 2>&1
		done
		i=0
		while bt_any_link && [ $i -lt 30 ]; do
			sleep 0.1; i=$((i + 1))
		done
	fi
	bt_stop_proc bluealsa
	bt_stop_proc bluetoothd
	hciconfig hci0 down 2> /dev/null
	bt_stop_proc rtk_hciattach
	rfkill block bluetooth 2> /dev/null
	# setBluetooth.sh's lock: a stale one makes the next init skip the attach.
	rm -f /tmp/.init_bt
}

bt_player() {
	[ -x "$TORTOS_DIR/btplayer" ] || return 0
	pidof btplayer > /dev/null && return 0
	setsid "$TORTOS_DIR/btplayer" >> "$LOGS_PATH/tortos.log" 2>&1 < /dev/null &
}

# Whether bluealsa has an A2DP stream for this address yet; see the Brick's
# radio.sh for why publishing before it is a headset nothing can open.
bt_a2dp_ready() {
	bluealsa-aplay -l 2> /dev/null |
		awk -v m="$1" '/^hci/ { on = index($0, m) > 0 } on && /A2DP/ { f = 1 } END { exit !f }'
}

# Every A2DP transport BlueZ has: dev_<mac>/sepN/fdM, one per line.
bt_streams() {
	dbus-send --system --print-reply --dest=org.bluez / \
		org.freedesktop.DBus.ObjectManager.GetManagedObjects 2> /dev/null |
		grep -oE 'dev_[0-9A-F_]+/sep[0-9]+/fd[0-9]+' | sort -u
}

# This address's stream id out of bt_streams' list, or nothing: sepN/fdM
# after bluetoothd's pid, because a bluetoothd started again (bt_off, then
# bt_on at a wake) counts from fd0 again, and the same id twice is a
# reconnect the launcher never sees (2026-10-06).
bt_stream_of() {
	s=$(echo "$2" | sed -n "s|^dev_$(echo "$1" | tr ':' '_')/||p" | head -1)
	[ -n "$s" ] && echo "$(pidof bluetoothd)/$s"
}

bt_on() {
	rfkill unblock bluetooth 2> /dev/null
	# Twice at most. The attach logged an H5 sync timeout once and then came
	# up; one that does not gets a clean second go.
	try=0
	while [ $try -lt 2 ]; do
		setsid /mnt/vendor/ctrl/setBluetooth.sh all > /dev/null 2>&1 < /dev/null
		i=0
		while [ ! -d /sys/class/bluetooth/hci0 ] && [ $i -lt 10 ]; do
			sleep 1; i=$((i + 1))
		done
		[ -d /sys/class/bluetooth/hci0 ] && break
		try=$((try + 1))
		echo "bt: attach attempt $try produced no hci0" >> "$LOGS_PATH/tortos.log"
		bt_stop_proc rtk_hciattach
		rm -f /tmp/.init_bt
	done
	if [ ! -d /sys/class/bluetooth/hci0 ]; then
		echo "bt: gave up after $try attach attempts" >> "$LOGS_PATH/tortos.log"
		return 1
	fi
	# Started already by an earlier enable, setBluetooth.sh's hciconfig may
	# have run before hci0 existed.
	hciconfig hci0 up 2> /dev/null
	setsid systemctl start bluetooth > /dev/null 2>&1 < /dev/null
	# --a2dp-volume leaves volume with the headset, as on the Brick. Its
	# debug build logs every step, so not to the card.
	pidof bluealsa > /dev/null ||
		setsid bluealsa -p a2dp-source --a2dp-volume > /dev/null 2>&1 < /dev/null &
	sleep 2          # registered with BlueZ before any connect
	bt_player
	# Its own session too, so it outlives the shell that started it.
	setsid /bin/sh -c '. "$0" && bt_reconnect' "$TORTOS_DIR/radio.sh" \
		> /dev/null 2>&1 < /dev/null &
	echo $! > "$BT_RECONNECT_PIDFILE"
}

# The Brick's bt_reconnect (radio.sh has the reasoning for every rule here);
# only how a link is seen differs, see the header.
bt_reconnect() {
	adapter=$(hciconfig hci0 2> /dev/null | sed -n 's/.*BD Address: \([0-9A-F:]*\).*/\1/p')
	[ -n "$adapter" ] || return 0
	latest=
	prev_links=
	soon=0
	last=$(cat "$BT_LAST" 2> /dev/null)
	while :; do
		bt_player
		connected=
		bonds=
		for d in "$TORTOS_BT_BONDS/$adapter"/*:*; do
			[ -d "$d" ] || continue
			grep -q '^Trusted=true' "$d/info" 2> /dev/null || continue
			grep -q '^\[LinkKey\]' "$d/info" 2> /dev/null || continue
			mac=$(basename "$d")
			if [ "$mac" = "$last" ]; then bonds="$mac $bonds"; else bonds="$bonds $mac"; fi
		done
		streams=$(bt_streams)
		links=
		for mac in $bonds; do
			timeout 5 bluetoothctl info "$mac" 2> /dev/null | grep -q 'Connected: yes' || continue
			h=$(bt_stream_of "$mac" "$streams")
			links="$links $mac/$h"
			case " $prev_links " in *" $mac/$h "*) ;; *) latest=$mac ;; esac
		done
		prev_links=$links
		case "$links " in
		*" $latest/"*) connected=$latest ;;
		*) set -- $links; connected=${1%%/*} ;;
		esac
		[ -n "$connected" ] || rm -f /tmp/tortos_btsink
		for mac in $bonds; do
			[ -z "$connected" ] || break
			timeout 10 bluetoothctl connect "$mac" > /dev/null 2>&1
			# Judge by info, never by connect's return (see the Brick's).
			sleep 1
			timeout 5 bluetoothctl info "$mac" 2> /dev/null | grep -q 'Connected: yes' &&
				connected=$mac
		done
		if [ -n "$connected" ] && ! bt_a2dp_ready "$connected"; then
			soon=$((soon + 1))
		elif [ -n "$connected" ]; then
			soon=0
			# PCM name, then this stream's id: new on every connection. Read
			# again, not the pass's: the stream may have come up since.
			link=$(bt_stream_of "$connected" "$(bt_streams)")
			printf '%s\n%s\n' "$(bt_pcm_name "$connected")" "$link" \
				> /tmp/tortos_btsink.tmp && mv /tmp/tortos_btsink.tmp /tmp/tortos_btsink
			if [ "$connected" != "$last" ]; then
				last=$connected
				echo "$last" > "$BT_LAST"
			fi
		else
			soon=0
			rm -f /tmp/tortos_btsink
		fi
		n=0
		wait=20
		[ $soon -gt 0 ] && [ $soon -le 15 ] && wait=1
		while [ $n -lt $wait ] && [ ! -e /tmp/tortos_btpass ]; do
			sleep 1
			n=$((n + 1))
		done
		rm -f /tmp/tortos_btpass
	done
}
