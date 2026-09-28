#!/bin/sh
# One named ALSA PCM per bonded headset: bt_pcm_name and bt_write_asoundrc.
#
# Sourced, never run. radio.sh sources it, so launch.sh has it at boot, to call
# bt_write_asoundrc before the emulator starts, and plat_sleep's radio.sh has
# bt_pcm_name after a wake; the launcher sources it into a shell of its own
# after a pair or a forget (bt_asoundrc in src/bt.c) and calls the same
# function.
#
# ONE COPY. This used to live in launch.sh with a second implementation in C,
# held together by a check that compared the names and nothing else - not the
# block around them, and not which bonds get one. launch.sh cannot call the
# launcher, which does not exist yet at boot, but the launcher can call this,
# and pairing is rare enough that starting a shell for it costs nobody a wait.
# What boot pays is one more small file: reading one cold off this card
# measured about 0.4ms, 2026-09-14.
#
# The file has to exist before the emulator's first PCM open. alsa-lib loads
# its config once and tracks the files it actually read: a .asoundrc created
# later is never noticed, so a config written when a headset CONNECTS arrives
# too late for a resident emulator that opened the speaker at boot. Measured
# 2026-09-05 - it failed in place and then worked immediately on restarting the
# emulator with the file already there.
#
# The bond is persistent (/etc/lib/bluetooth/<adapter>/<device>/), so every
# headset that could connect is known now, without waiting for one to. A PCM
# for a headset that is not connected simply fails to open, and the port falls
# back to the speaker and says so - which is ADR-0029's behavior anyway.
#
# One per device rather than one reused name, so switching headsets needs no
# restart. bt_pcm_name is the single place the naming is decided.

bt_pcm_name() {
	echo "bt_$(echo "$1" | tr ':' '_')"
}

bt_write_asoundrc() {
	rc=$USERDATA_PATH/.asoundrc
	: > "$rc.tmp"
	for d in /etc/lib/bluetooth/*/*:*; do
		[ -d "$d" ] || continue
		grep -q '^Trusted=true' "$d/info" 2> /dev/null || continue
		grep -q '^\[LinkKey\]' "$d/info" 2> /dev/null || continue   # see bt_reconnect
		mac=$(basename "$d")
		cat >> "$rc.tmp" <<-EOF
		pcm.$(bt_pcm_name "$mac") {
		    type bluealsa
		    device "$mac"
		    profile "a2dp"
		}
		EOF
	done
	# `default` is deliberately not redefined: ALSA reads this in addition to
	# /etc/asound.conf, and the speaker path must stay exactly as it was.
	mv "$rc.tmp" "$rc"
}
