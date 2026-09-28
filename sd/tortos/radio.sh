# Wi-Fi and Bluetooth control, shared by launch.sh (boot) and plat_sleep()
# (suspend/wake). Split out for the same reason as bt-alsa.sh: plat_sleep()
# needs these definitions without launch.sh's boot-only side effects (mkdir,
# GPIO exports, the boot animation) re-running on every sleep/wake cycle, not
# just once per boot.
#
# Sourced by launch.sh for its own boot-time use, and by plat_sleep() (see
# run_radio_fn() in src/platform.c) the same way src/bt.c's bt_asoundrc() runs
# bt_write_asoundrc: `/bin/sh -c '. "$0" && fn' radio.sh`, with the script path
# passed as $0, never pasted into the command, so it is never read as shell.

# bt_pcm_name, for bt_reconnect below - and bt_write_asoundrc for launch.sh,
# which gets it from here. Sourced here rather than in launch.sh because this
# file is the one that also runs without launch.sh around it (TortOS-1jx).
# Guarded, for the reason bt-alsa.sh's own header gives: `.` on a missing file
# ends a non-interactive shell.
[ -f "$TORTOS_DIR/bt-alsa.sh" ] && . "$TORTOS_DIR/bt-alsa.sh"

# ---- Wi-Fi ----

# Boot-time bring-down: retries stopping wpa_supplicant across a 20s window to
# win a race against /etc/rc.d/S96wpa_supplicant's own init-script retries.
# That race only exists while S96 itself is still starting up, seconds after
# boot - see the long comment at this function's call site in launch.sh. Kept
# here unchanged for that one caller; a live suspend cycle uses
# wifi_stop_once() below instead, since the race it guards against is gone by
# then and a 20s retry would just block every sleep for up to 20 seconds.
radio_off() {
	i=0
	while [ $i -lt 20 ]; do
		if pgrep -f '[w]pa_supplicant' > /dev/null; then
			/etc/init.d/wpa_supplicant stop > /dev/null 2>&1
			killall -q udhcpc 2> /dev/null
		fi
		ifconfig wlan0 down 2> /dev/null
		sleep 1
		i=$((i + 1))
	done
	pgrep -f '[w]pa_supplicant' > /dev/null || return 0
	echo "wifi: supplicant still up after ${i}s, giving up" >> "$LOGS_PATH/tortos.log"
}

# Bring the radio up and take a lease. Associating is not connecting: the
# supplicant joins a saved network on its own, but nothing on this device runs
# a DHCP client at boot, so without this the interface comes up with no
# address and every fetch fails in a way that looks like a dead network rather
# than a missing lease. Also used to bring Wi-Fi back up after a suspend cycle
# that had it running before sleep - backgrounded there (see plat_sleep()), so
# its own retry window never blocks wake.
wifi_on() {
	i=0
	/etc/init.d/wpa_supplicant start > /dev/null 2>&1
	while [ $i -lt 25 ]; do
		if wpa_cli -p /etc/wifi/sockets -i wlan0 status 2> /dev/null \
		   | grep -q '^wpa_state=COMPLETED'; then
			udhcpc -i wlan0 -S -t 5 -T 7 -b -q > /dev/null 2>&1
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	echo "wifi: no association after ${i}s" >> "$LOGS_PATH/tortos.log"
}

# Single-shot Wi-Fi stop for plat_sleep()'s pre-suspend step only. Mirrors
# NextUI's own before() exactly (one stop call, no retry): mid-session there is
# no S96 startup race to win, so radio_off()'s 20s retry window would only add
# up to 20 seconds of blocking delay to every sleep entry for nothing.
wifi_stop_once() {
	/etc/init.d/wpa_supplicant stop > /dev/null 2>&1
	killall -q udhcpc 2> /dev/null
	ifconfig wlan0 down 2> /dev/null
}

# ---- Bluetooth ----

# bt_on()'s reconnect loop's pid, so a later bt_off() can kill it. Needed
# because unlike boot (where bt_on() runs exactly once), plat_sleep() calls
# bt_on() again on every wake - without this, each cycle would leave the
# previous cycle's reconnect loop running forever, polling a radio bt_off()
# just tore down, stacking one more live loop per sleep/wake cycle.
BT_RECONNECT_PIDFILE=/tmp/tortos_bt_reconnect.pid

# Bluetooth, off unless asked for, because both radios are battery drain and
# boot time, and the state the player left it in wins over the shipped
# default.
bt_off() {
	if [ -f "$BT_RECONNECT_PIDFILE" ]; then
		kill "$(cat "$BT_RECONNECT_PIDFILE")" 2> /dev/null
		rm -f "$BT_RECONNECT_PIDFILE"
	fi
	# Before anything else: no radio means no sink, and a stale file would
	# leave the launcher routing sound at a device that is gone. The port
	# would fall back and say so (ADR-0029), but a launcher showing the wrong
	# answer for twenty seconds is a thing to avoid, not to recover from.
	rm -f /tmp/tortos_btsink
	# btplayer too: a bluetoothd started again later has forgotten its
	# registration, so bt_on starts a fresh one.
	killall -q btplayer bluealsa bluetoothd hciattach 2> /dev/null
	/etc/init.d/bluetooth stop 2> /dev/null
	rfkill block bluetooth 2> /dev/null
	echo 0 > /sys/class/rfkill/rfkill0/state 2> /dev/null
}

# Not start-stop-daemon, which would lose its log line: this is launch.sh and
# not an adb session, so `&` outlives nothing it should not.
bt_player() {
	pidof btplayer > /dev/null && return 0
	"$TORTOS_DIR/btplayer" >> "$LOGS_PATH/tortos.log" 2>&1 &
}

# Whether bluealsa has an A2DP stream for this address yet. The link comes up
# first and the stream a moment after, and a headset published in between
# is one that nothing can open: Muse got `No such device` and fell back to
# the speaker, and stayed there until something happened to re-send the
# route - seen 2026-09-26, switching to the OpenFit. -l lists each device's
# PCMs under its own line, so the A2DP line has to be under THIS one's.
bt_a2dp_ready() {
	bluealsa-aplay -l 2> /dev/null |
		awk -v m="$1" '/^hci/ { on = index($0, m) > 0 } on && /A2DP/ { f = 1 } END { exit !f }'
}

# The bring-up is the vendor's own, established on 2026-09-03. Three details in
# it are not guesses and should not be "simplified":
#
#   - The attach protocol is `xradio`. /etc/bluetooth/bt_init.sh on this rootfs
#     is the AIC variant and FAILS here ("bring up hci0 failed"; it also calls
#     hcidump_xr, which does not exist). /etc/init.d/hciattach has the right
#     invocation and is what this mirrors.
#   - rfkill needs a POWER CYCLE, not an unblock. After a failed attach the chip
#     wedges: hciattach runs happily and no hci0 ever appears. Every bt_init.sh
#     variant cycles the rail; /etc/init.d/hciattach does not, which is why the
#     boot service has never once succeeded - it is enabled as S80 and fails
#     against the radio we block four lines further down.
#   - hfp-ag is ASSUMED to matter, not measured. Another firmware on this
#     device reports that with a2dp-source alone a headset connects but behaves
#     oddly, and that the Hands-Free gateway role makes it connect the way it
#     would to a phone. That is plausible and costs nothing, so it is here - but
#     nobody has A/B'd it, and pairing was fixed on 2026-09-03 by an unrelated
#     change (the agent capability), so hfp-ag has never been shown to be doing
#     anything. To settle it: run bluealsa with -p a2dp-source alone, reconnect
#     the headset, and listen for whether it speaks or only beeps.
#
#     --a2dp-volume IS load-bearing and is not an assumption: it leaves volume
#     with the headset, so a BT sink stays outside both the speaker and jack
#     ladders in src/platform.c and must not be attenuated here.
#
# Called on every wake that had Bluetooth running before sleep too (see
# plat_sleep()), backgrounded there so its own retries never block wake -
# mirrors NextUI's own suspend script backgrounding this half with `after &`.
bt_on() {
	# Power cycle rather than unblock. See above.
	echo 0 > /sys/class/rfkill/rfkill0/state 2> /dev/null
	sleep 2
	echo 1 > /sys/class/rfkill/rfkill0/state 2> /dev/null
	sleep 2
	rfkill unblock bluetooth 2> /dev/null

	# Retried, not attempted once. Measured 2026-09-03: the identical command
	# takes ~4s and succeeds on a settled device, and FAILS outright when run
	# about two seconds after the unblock during boot - the chip is not ready
	# that early and the attach dies silently. A longer fixed sleep would be a
	# guess at how long boot happens to take on this card with this library;
	# a retry costs nothing when it works first time.
	#
	# The stock service is stopped first because /etc/rc.d/S80hciattach is
	# procd-managed and has already run and failed by the time we get here.
	# Leaving procd holding a failed instance means two things reaching for the
	# same UART.
	/etc/init.d/hciattach stop > /dev/null 2>&1
	killall -q hciattach 2> /dev/null

	try=0
	while [ $try -lt 3 ]; do
		start-stop-daemon -S -b -x /usr/bin/hciattach -- -n ttyS1 xradio > /dev/null 2>&1
		i=0
		while [ $i -lt 10 ]; do
			[ -d /sys/class/bluetooth/hci0 ] && break
			sleep 1
			i=$((i + 1))
		done
		[ -d /sys/class/bluetooth/hci0 ] && break
		try=$((try + 1))
		echo "bt: attach attempt $try produced no hci0" >> "$LOGS_PATH/tortos.log"
		# A dead attach leaves the chip wedged; only a rail cycle clears it.
		killall -q hciattach 2> /dev/null
		echo 0 > /sys/class/rfkill/rfkill0/state 2> /dev/null
		sleep 2
		echo 1 > /sys/class/rfkill/rfkill0/state 2> /dev/null
		sleep 2
	done
	if [ ! -d /sys/class/bluetooth/hci0 ]; then
		echo "bt: gave up after $try attach attempts" >> "$LOGS_PATH/tortos.log"
		return 1
	fi

	hciconfig hci0 up 2> /dev/null
	/etc/bluetooth/bluetoothd start > /dev/null 2>&1
	start-stop-daemon -S -b -x /usr/bin/bluealsa -- \
		-S -p a2dp-source -p hfp-ag --a2dp-volume > /dev/null 2>&1
	sleep 3          # let both profiles register with BlueZ before any connect

	# A media player registered with BlueZ, which does nothing else: without
	# one, BlueZ 5.54 drops the volume a headset reports and the transport
	# never gets a Volume, so no headset's volume can be set from here. See
	# tools/btplayer.c. Before bt_reconnect, so it is registered by the time a
	# headset connects: the headset only reports its volume at that moment.
	bt_player

	bt_reconnect &
	echo $! > "$BT_RECONNECT_PIDFILE"
}

# Keep a remembered headset connected, so powering it on reconnects it wherever
# you are rather than only at a screen that happens to be watching.
#
# Trusted devices with a key only: BlueZ writes those to
# /etc/lib/bluetooth/<adapter>/, NOT to /etc/bluetooth/keys/ - that directory
# is a decoy created by the bluetoothd init wrapper's `ln -snf ...
# /var/lib/bluetooth`, and bluetoothd never reads it because its storage path
# is compiled in with --localstatedir=/etc.
#
# Judge success by `info`, never by the return of `connect`: bluetoothctl reports
# Failed for a2dp even when the link came up.
bt_reconnect() {
	adapter=$(hciconfig hci0 2> /dev/null | sed -n 's/.*BD Address: \([0-9A-F:]*\).*/\1/p')
	[ -n "$adapter" ] || return 0
	latest=
	prev_links=
	soon=0
	while :; do
		# Back if it died. A restarted player is attached to the sessions that
		# exist, but a headset already connected reports its volume again only
		# when it reconnects.
		bt_player
		connected=
		bonds=
		for d in /etc/lib/bluetooth/"$adapter"/*:*; do
			[ -d "$d" ] || continue
			grep -q '^Trusted=true' "$d/info" 2> /dev/null || continue
			# And a key on the card, or it is not a bond: a memory-only pairing
			# leaves Trusted behind with no key after a restart, and dialing
			# that every pass connected it for four seconds at a time and
			# published a sink each time. See bond_name in src/bt.c.
			grep -q '^\[LinkKey\]' "$d/info" 2> /dev/null || continue
			bonds="$bonds $(basename "$d")"
		done
		# Whoever is already connected first, and nobody dialed while one is.
		# In one walk, a headset switched off but earlier in address order was
		# dialed before the connected one was even looked at: `connect` takes
		# about five seconds to give up, so publishing the OpenFit waited six
		# or seven on the OpenRun - measured 2026-09-25 - and the dead one was
		# paged every pass for as long as the live one was in use.
		#
		# And with two connected, the one connected LAST, as a phone does. Until
		# 2026-09-26 it was the first in address order, so with the OpenRun on
		# the OpenFit could be connected from the screen and the sound stayed on
		# the OpenRun. The order is kept here, pass to pass: a bond connected now
		# and not last pass, or on a different link handle - a reconnect - is
		# the latest. Not the handle's value itself, which is reused (the OpenFit
		# came back on 128 once the OpenRun's 128 was free). A connect from the
		# Bluetooth screen asks for a pass at once, so the headset chosen there
		# takes the sound within a second.
		cons=$(hcitool con 2> /dev/null)
		links=
		for mac in $bonds; do
			bluetoothctl info "$mac" 2> /dev/null | grep -q 'Connected: yes' || continue
			h=$(echo "$cons" | sed -n "s/.*ACL $mac handle \([0-9]*\).*/\1/p" | head -1)
			links="$links $mac/$h"
			case " $prev_links " in *" $mac/$h "*) ;; *) latest=$mac ;; esac
		done
		prev_links=$links
		# The latest while it stays connected; when it goes, one that has not.
		case "$links " in
		*" $latest/"*) connected=$latest ;;
		*) set -- $links; connected=${1%%/*} ;;
		esac
		# Nothing connected: say so NOW, before dialing. Dialing a headset that
		# is off takes about five seconds each, and one switched back on while
		# that went on was republished under a name that had never been taken
		# away - no change, so nothing was re-routed, and a song that had failed
		# over to the speaker stayed there. Seen 2026-09-25: the OpenFit off for
		# 26 seconds and the sink never withdrawn.
		[ -n "$connected" ] || rm -f /tmp/tortos_btsink
		for mac in $bonds; do
			[ -z "$connected" ] || break
			bluetoothctl connect "$mac" > /dev/null 2>&1
			# Judge by info, NEVER by the return. connect reports Failed for
			# a2dp even when the link came up - measured 2026-09-03, and
			# written in the backlog before this loop was, then used anyway.
			#
			# Trusting the return made this publish "no sink" while a headset
			# was connected, so the launcher fell back to the speaker, then
			# picked the sink up on the next pass and switched again. Every
			# flip reopens Diatom's audio device: 2026-09-05 that was nine
			# route changes and 407236 dropped audio frames in one game.
			sleep 1
			bluetoothctl info "$mac" 2> /dev/null | grep -q 'Connected: yes' &&
				connected=$mac
		done
		# Tell the launcher where the sound can go, if anywhere.
		#
		# A file rather than the launcher asking, because asking means forking
		# bluetoothctl out of a 119 MB process - the exact mistake menu_wifi
		# exists to prevent, measured at 12 fps with a menu open. This loop
		# already knows the answer, so it writes it and the launcher stats a
		# path. The ALSA device string is built here for the same reason: the
		# MAC is here and Diatom must never learn what kind of thing it names.
		if [ -n "$connected" ] && ! bt_a2dp_ready "$connected"; then
			# Connected, its stream not there yet: leave what is published -
			# during a switch that is the old headset, still playing - and
			# look again in a second rather than twenty. Fifteen times at
			# most, for a bond that never gets an audio stream at all.
			soon=$((soon + 1))
		elif [ -n "$connected" ]; then
			soon=0
			# Publish the PCM NAME, not a bluealsa device string.
			#
			# Measured 2026-09-05 with diatom/tools/btaudio.c - the same SDL,
			# the same writes, the same headset:
			#
			#   AUDIODEV=bluealsa:DEV=..,PROFILE=a2dp   queue climbs to 385 kB
			#                                           and never drains; the
			#                                           close never returns
			#   a named PCM of type bluealsa            drains to 0; close
			#                                           returns in ~105 ms
			#
			# Three runs each way. The device string goes through the plugin's
			# own argument parser and yields a PCM that SDL opens and then never
			# writes to; the config form goes through ALSA's normal path and
			# works. Wrapping the string form in `plug` does not help.
			#
			# The config itself is written at boot by bt_write_asoundrc, which
			# explains why it is not written here.
			#
			# And the link's ACL handle on a second line, which is new on every
			# connection even when the name is not. A headset that dropped and came
			# back between two looks here keeps its name, so without this the
			# launcher could not tell it had ever gone, and whoever had fallen back
			# to the speaker while it was away was never sent back to it.
			link=$(hcitool con 2> /dev/null |
				sed -n "s/.*ACL $connected handle \([0-9]*\).*/\1/p" | head -1)
			printf '%s\n%s\n' "$(bt_pcm_name "$connected")" "$link" \
				> /tmp/tortos_btsink.tmp && mv /tmp/tortos_btsink.tmp /tmp/tortos_btsink
		else
			soon=0
			rm -f /tmp/tortos_btsink
		fi
		# Twenty seconds, or less when the Bluetooth screen asks: it touches
		# /tmp/tortos_btpass after a connect, a disconnect or a forget, so the
		# sink moves with the screen rather than up to a pass later. Waiting a
		# whole pass sent anything pressed in between to the speaker - seen
		# 2026-09-25, a song resumed right after a pairing. A file and not a
		# signal, because the launcher has no pid for this loop and a signal
		# to the wrong shell would be launch.sh itself.
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
