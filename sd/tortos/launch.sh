#!/bin/sh
# TortOS boot entry. Called from .tmp_update/tg3040.sh; never returns.
#
# The order of things in here is the boot time. The animation runs in the
# background and everything else -- the launcher's whole startup and the
# resident emulator's -- happens behind it, so the animation costs its length
# and nothing else.

# Exported for radio.sh: plat_sleep() runs it in a shell of its own, which
# sees only the launcher's environment (TortOS-1jx).
export TORTOS_DIR=/mnt/SDCARD/TortOS
SDCARD=/mnt/SDCARD

export PLATFORM=tg3040
export DEVICE=brick
export SDCARD_PATH=$SDCARD
export BIOS_PATH=$SDCARD/Bios
export ROMS_PATH=$SDCARD/Roms
export SAVES_PATH=$SDCARD/Saves
export CHEATS_PATH=$SDCARD/Cheats
export SYSTEM_PATH=$TORTOS_DIR
export CORES_PATH=$TORTOS_DIR/cores
export USERDATA_PATH=$SDCARD/.userdata/tg3040
export SHARED_USERDATA_PATH=$SDCARD/.userdata/shared
export LOGS_PATH=$USERDATA_PATH/logs
export HOME=$USERDATA_PATH
# .asoundrc named as a top-level ALSA config file, and not only reached through
# alsa.conf's @hooks load. alsa-lib 1.1.8 re-reads its config at the next open
# when a file in this list changes, and it never checks files a hook loaded -
# so a headset paired after Diatom or Muse started, whose PCM bt_write_asoundrc
# has just added, was `Unknown PCM` to both until they restarted. Measured
# 2026-09-25 with a probe that opens once, waits, and opens a PCM added in
# between: not found without this, found with it, and a missing or empty
# .asoundrc at start is fine either way.
export ALSA_CONFIG_PATH=/usr/share/alsa/alsa.conf:$HOME/.asoundrc
export LD_LIBRARY_PATH=/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=/usr/trimui/bin:$PATH

# Cleared by the boot animation when it finishes. The launcher does all of its
# startup while the animation plays, then blocks on this and presents its first
# frame the moment the animation clears it.
TORTOS_ANIM_FLAG=/tmp/tortos_bootanim
export TORTOS_ANIM_FLAG

mkdir -p "$BIOS_PATH" "$ROMS_PATH" "$SAVES_PATH" "$USERDATA_PATH" "$LOGS_PATH" \
         "$SHARED_USERDATA_PATH"

# Settings live in a database now, and this script cannot read one: there is no
# sqlite3 binary on the device. So the launcher exports the five values needed
# before it exists, and this sources them. One read, no forks - it replaces six
# seds across four files, so the boot path got shorter rather than longer.
#
# The defaults below are for the very first boot of a fresh card, before the
# launcher has ever run. After that boot.env is rewritten on every start and
# whenever one of these changes.
#
# Sourced rather than parsed because parsing costs a fork per value. That trusts
# the file, which is the same trust this script already places in itself: both
# are on the card, and anyone who can edit one can edit the other.
BRIGHTNESS=7
WIFI=0
BLUETOOTH=0
TIMEZONE=America/New_York
BOOT_ENV=$USERDATA_PATH/boot.env
[ -f "$BOOT_ENV" ] && . "$BOOT_ENV"

# radio_off/wifi_on/bt_off/bt_on and their helpers: in a file of its own, same
# reason as bt-alsa.sh below, so plat_sleep() can re-run them on every
# sleep/wake cycle without launch.sh's boot-only side effects. Tested first for
# the same reason as bt-alsa.sh's own guard: `.` on a missing file ends a
# non-interactive shell, which powers the device off (see .tmp_update/updater).
[ -f "$TORTOS_DIR/radio.sh" ] && . "$TORTOS_DIR/radio.sh"

# All LEDs off: TortOS shows no chrome, and the lights are pure battery drain.
# A function, because trimui_inputd re-lights them when it starts.
leds_off() {
	echo 0 > /sys/class/led_anim/effect_enable 2> /dev/null
	for g in l r lr m f1 f2; do echo "000000 " > /sys/class/led_anim/effect_rgb_hex_$g 2> /dev/null; done
	echo 0 > /sys/class/led_anim/max_scale 2> /dev/null
	echo 0 > /sys/class/led_anim/max_scale_lr 2> /dev/null
	echo 0 > /sys/class/led_anim/max_scale_f1f2 2> /dev/null
	# Only lit channels: a burst of raw writes overflows the LED controller, and
	# on the Brick Pro that starves the stick chip's i2c bus until the kernel
	# panics (TortOS-pky.9).
	for f in /sys/class/leds/sunxi_led*/brightness; do [ "$(cat "$f" 2> /dev/null)" = 0 ] || echo 0 > "$f" 2> /dev/null; done
}

# Apply the configured brightness now, so the boot animation is not dimmer than
# everything after it. tortos.elf has not started yet and cannot do it.
#
# This ladder is TortOS's own and is shared verbatim with platform.c and with
# the emulator: twelve geometric rungs, the first being the panel's measured
# floor (0 and 1 are black on this display). A saved level in
# The level the player last chose, which the launcher already resolved against
# the shipped default before exporting it - so there is one value here now
# rather than a saved level and a fallback.
brightness_raw() {
	B=$BRIGHTNESS
	case "$B" in
		0) echo 2;;  1) echo 4;;   2) echo 8;;   3) echo 16;;
		4) echo 32;; 5) echo 48;;  6) echo 72;;  7) echo 96;;
		8) echo 128;; 9) echo 160;; 10) echo 192;; 11) echo 255;;
		*) echo 96;;
	esac
}
[ -x "$TORTOS_DIR/setbright" ] && "$TORTOS_DIR/setbright" "$(brightness_raw)"
leds_off

# One-time: replace the stock u-boot splash with a black frame, so the handoff
# into the boot animation is seamless rather than a TrimUI logo followed by a
# cut. Guarded by a marker; the stock logo is backed up first.
if [ -f "$TORTOS_DIR/bootlogo.bmp" ] && [ ! -f "$TORTOS_DIR/.bootlogo_applied" ]; then
	mkdir -p /mnt/boot
	if mount -t vfat /dev/mmcblk0p1 /mnt/boot 2> /dev/null; then
		# Ask the image on the boot partition what it is, rather than asking a
		# marker on the card.
		#
		# The marker is the card's, the image is the device's, and a new card
		# means a missing marker over an already-applied image. Reinstalling on
		# a freshly formatted card then "backed up the stock logo" - which by
		# then was OUR logo - straight over the real one, and the genuine stock
		# image was gone for good on a device that had been reinstalled once.
		# Found by doing exactly that, 2026-08-30.
		if cmp -s /mnt/boot/bootlogo.bmp "$TORTOS_DIR/bootlogo.bmp"; then
			touch "$TORTOS_DIR/.bootlogo_applied"      # already ours
		else
			if [ -f /mnt/boot/bootlogo.bmp ] && [ ! -f "$TORTOS_DIR/bootlogo.stock.bmp" ]; then
				cp /mnt/boot/bootlogo.bmp "$TORTOS_DIR/bootlogo.stock.bmp"
			fi
			cp "$TORTOS_DIR/bootlogo.bmp" /mnt/boot/bootlogo.bmp && sync
			touch "$TORTOS_DIR/.bootlogo_applied"
		fi
		umount /mnt/boot
	fi
fi

# One-time: the stock "loading" splash that pic2fb blits from /etc/splash.png.
# Same black frame, same reason.
if [ -f "$TORTOS_DIR/splash.png" ] && [ ! -f "$TORTOS_DIR/.splash_applied" ]; then
	# Same trap as the bootlogo above, same answer: compare, do not assume.
	if cmp -s /etc/splash.png "$TORTOS_DIR/splash.png"; then
		touch "$TORTOS_DIR/.splash_applied"           # already ours
	else
		if [ -f /etc/splash.png ] && [ ! -f "$TORTOS_DIR/splash.stock.png" ]; then
			cp /etc/splash.png "$TORTOS_DIR/splash.stock.png"
		fi
		cp "$TORTOS_DIR/splash.png" /etc/splash.png && sync
		touch "$TORTOS_DIR/.splash_applied"
	fi
fi

# One-time: pic2fb blits that splash at the hardware default brightness, which
# is not ours, so the screen visibly steps partway through the boot. Call the
# brightness helper before it. Reversible: the stock init is backed up.
if [ -x "$TORTOS_DIR/setbright" ] && [ ! -f "$TORTOS_DIR/.brightboot_applied" ]; then
	cp "$TORTOS_DIR/setbright" /usr/trimui/bin/setbright 2> /dev/null && chmod +x /usr/trimui/bin/setbright
	cat > /usr/trimui/bin/tortos-bootbright.sh <<'BB'
#!/bin/sh
BR=$(sed -n "s/^BRIGHTNESS='\\(.*\\)'$/\\1/p" /mnt/SDCARD/.userdata/tg3040/boot.env 2> /dev/null | tail -1)
case "$BR" in
	0) R=1;; 1) R=8;; 2) R=16;; 3) R=32;; 4) R=48;; 5) R=72;;
	6) R=96;; 7) R=128;; 8) R=160;; 9) R=192;; 10) R=255;; *) R=160;;
esac
[ -x /usr/trimui/bin/setbright ] && /usr/trimui/bin/setbright "$R"
BB
	chmod +x /usr/trimui/bin/tortos-bootbright.sh
	if [ -f /etc/init.d/runtrimui ] && ! grep -q tortos-bootbright /etc/init.d/runtrimui; then
		cp /etc/init.d/runtrimui /etc/init.d/runtrimui.tortos-bak
		awk '/pic2fb/ && !d {print "/usr/trimui/bin/tortos-bootbright.sh"; d=1} {print}' \
			/etc/init.d/runtrimui.tortos-bak > /etc/init.d/runtrimui
		sh -n /etc/init.d/runtrimui 2> /dev/null || cp /etc/init.d/runtrimui.tortos-bak /etc/init.d/runtrimui
		chmod +x /etc/init.d/runtrimui
	fi
	sync
	touch "$TORTOS_DIR/.brightboot_applied"
fi

# The boot animation, played in the BACKGROUND.
#
# An animation that adds its own length to the boot is just a delay with a
# picture on it. This one plays while the launcher does its entire startup --
# card scan, GL init, font and card decode -- and while the resident emulator
# builds its GL context and maps every core on the card.
#
# ffmpeg and the launcher both write to /dev/fb0, so they must never draw at
# the same time: last writer wins, and they would fight at 30fps against 60.
# The marker file is the handshake -- the launcher initializes freely, blocks
# on the marker, and presents the moment the animation clears it. Removed in
# the same subshell so it goes even if the decoder dies.
if [ -f "$TORTOS_DIR/tortos-boot.mp4" ]; then
	: > "$TORTOS_ANIM_FLAG"
	(
		ffmpeg -hide_banner -loglevel quiet -re -i "$TORTOS_DIR/tortos-boot.mp4" \
		       -pix_fmt bgra -f fbdev /dev/fb0 2> /dev/null
		rm -f "$TORTOS_ANIM_FLAG"
	) &

	# Free seconds: pull what the first launch needs off the card and into the
	# page cache while nothing else is using the disk.
	#
	# The CORES are no longer read here. Diatom maps them itself at startup
	# with --cores, which is the same work for less: measured 2026-09-08,
	# reading all six through cost 480ms cold where mapping them costs 382ms,
	# because dlopen takes what it needs rather than every byte - and mapping
	# also pays the dynamic linker, which reading never did. A first launch
	# used to pay that: 179ms of cold dlopen for genesis_plus_gx alone.
	#
	# The emulator binary is still read here. Diatom cannot warm the file it
	# is about to be executed from.
	(
		cat "$TORTOS_DIR/diatom" > /dev/null 2>&1
	) &
fi

# Rumble off, mute-switch gpio readable
echo 227 > /sys/class/gpio/export 2> /dev/null
echo -n out > /sys/class/gpio/gpio227/direction 2> /dev/null
echo -n 0 > /sys/class/gpio/gpio227/value 2> /dev/null

# Timezone, before anything that writes a timestamp.
#
# The stock firmware points /tmp/localtime at Asia/Shanghai, which is eight
# hours out for anyone who did not buy the device there, and every date the
# launcher shows - the one under a save slot - is local time. The zoneinfo
# database is already on the device; this only chooses from it.
#
# The clock itself is not set here and does not need to be: /etc/rc.d/S98sysntpd
# runs ntpd, which corrects the time within a minute of Wi-Fi connecting. That
# is also why the device sat in 1970 until Wi-Fi worked - ntpd was running the
# whole time with nothing to reach.
TZNAME=$TIMEZONE
if [ -n "$TZNAME" ] && [ -f "/usr/share/zoneinfo/$TZNAME" ]; then
	ln -sf "/usr/share/zoneinfo/$TZNAME" /tmp/localtime
	rm -f /tmp/TZ
elif [ -n "$TZNAME" ]; then
	echo "tz: no zoneinfo for '$TZNAME', keeping the firmware default" \
	     >> "$LOGS_PATH/tortos.log"
fi

# Radio silence. TortOS has nothing to talk to yet: no downloads, no pairing,
# no achievements. Both radios are battery drain and boot time.
#
# Wi-Fi on in the settings -- or a .devwifi marker -- keeps WiFi up so a
# development unit stays reachable over ssh. Without it there is no way to
# diagnose a problem on hardware except by pulling the card, which is a bad
# place to be when something does not come up.
#
# Stop the supplicant and drop the interface rather than rfkill-blocking, so
# the radio is left in a state the firmware understands.
#
# Both halves of this were dead until 2026-08-29, and measurably so: the
# supplicant was running on a device whose own log said radio silence.
#
# The `on` half called /etc/wifi/wifi_init.sh, which does not exist anywhere
# on the device and never has, so it failed silently and the flag did nothing.
# The stock service is procd-managed, so starting it is what the init script
# is for.
#
# The `off` half lost a race. /etc/rc.d/S96wpa_supplicant is USE_PROCD=1 and
# procd is pid 1, so procd owns the process and respawns a bare kill. The
# init script's `stop` handles that correctly -- but its `start_service`
# retries `ifconfig wlan0 up` five times with usleep 500000 between, so S96
# is still inside that loop when S99 runs us, and it finishes and starts the
# supplicant after we asked for it to be stopped. PIDs told the story:
# launch.sh 1839, tortos.elf 2156, wpa_supplicant 2286.
#
# So the stop is repeated across a window rather than once, and backgrounded,
# because waiting out someone else's usleeps is not worth the boot time.
#
# The window, and not just a retry-until-gone loop, because stopping it once
# is not the same as it staying stopped. Measured on 2026-08-29: a loop that
# exited on the first clear reading left the supplicant gone at 20s and back
# at 37s, restarted after tortos.elf was already up. It then exited on its
# own around 90s, but only because wlan0 was down underneath it -- which is
# luck, not a mechanism. Keep stopping it for the whole window instead.
#
# radio_off() and wifi_on() (the bring-up counterpart) now live in radio.sh,
# sourced above - see that file's header for why.

# What THIS device was doing when it was last shut down, which is what someone
# who turned Wi-Fi on expects to find. The launcher resolved that against the
# shipped default before exporting, so $WIFI is already the answer.
if [ "$WIFI" = "1" ] || [ -f "$TORTOS_DIR/.devwifi" ]; then
	wifi_on &
else
	radio_off &
fi

# Bluetooth, off unless asked for, because both radios are battery drain and
# boot time, and the state the player left it in wins over the shipped
# default. bt_off()/bt_on() and their helpers now live in radio.sh, sourced
# above - see that file's header for why, and its comment on bt_on() for the
# bring-up rationale (rfkill power-cycle, xradio attach, hfp-ag) unchanged from
# here.

if [ "$BLUETOOTH" = "1" ]; then
	bt_on &
else
	bt_off &
fi

# CPU: interactive scaling
echo interactive > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null

# EVERY button and the d-pad arrive through the stock GPIO input daemon's
# virtual joystick at /dev/input/event3. Without this daemon there is no d-pad
# and no face buttons at all -- only volume and power, which come from real
# kernel devices at event0/event1. It is the single line the whole control
# scheme depends on.
pgrep trimui_inputd > /dev/null || trimui_inputd &

# LEDs off again: trimui_inputd re-enables them when it starts.
sleep 1
leds_off

# One log per boot, carrying the launcher AND everything it starts. Without
# this a failure inside a game goes to a console nobody reads.
#
# Ten boots kept: this one and tortos.log.1 to .9, newest first. One was not
# enough - on the first fresh-card test the boots that mattered were gone
# before anyone read the log, and a player's report can come several reboots
# after the fault. Measured on the device 2026-09-29, a boot's log runs 3 to
# 30 KB, so ten is 300 KB at most.
LOG=$LOGS_PATH/tortos.log
i=9
while [ $i -gt 1 ]; do
	[ -f "$LOG.$((i - 1))" ] && mv -f "$LOG.$((i - 1))" "$LOG.$i"
	i=$((i - 1))
done
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
: > "$LOG"

# The resident emulator. It holds the GL context and every core between
# games, which takes a launch from ~1100ms to ~200ms. Started here so its ~1s
# of setup happens during the boot animation alongside the launcher's own --
# and so the clearing it does while creating its context is hidden under the
# animation rather than flashing over the launcher.
#
# Nothing depends on it: the launcher checks for its fifos and runs a game the
# old way, one process per game, when they are not there. That is what happens
# for a launch in the first second after boot, and if this ever dies.
# The resident emulator is Diatom. It preloads nothing: a core is mapped the
# first time a game needs it and kept for the life of the process, so there
# is no core list to hand over and nothing here changes when a system is
# added. The fallback for a resident that dies mid-session is the same
# binary run standalone by the launcher - one emulator, held two ways.
export TORTOS_DIATOM_SOCKET=/tmp/diatom.sock
# Lost once, in 1258dd7's move of the radio functions to radio.sh, with both
# calls left behind: sh fails a missing function quietly, so every boot ran its
# first game the old way, where a POWER tap powers off (TortOS-5bu).
start_resident() {
	pgrep -f "TortOS/diatom --socket" > /dev/null && return
	rm -f "$TORTOS_DIATOM_SOCKET"
	LD_LIBRARY_PATH=/usr/trimui/lib \
		"$TORTOS_DIR/diatom" --socket "$TORTOS_DIATOM_SOCKET" \
		--cores "$CORES_PATH" \
		--save "$SDCARD/Saves" --system "$SDCARD/Bios" >> "$LOG" 2>&1 &
	echo $! > /tmp/diatom.pid
}
# BEFORE start_resident, and that order is the whole point: alsa-lib caches its
# config at the first PCM open, so a definition written afterwards is invisible
# to this process for as long as it lives.
bt_write_asoundrc
start_resident

rm -f /tmp/tortos_poweroff
# Nothing is CONNECTED yet; bt_reconnect writes this when something is. The
# .asoundrc is not removed with it - it describes bonds, which outlive any
# connection, and removing it here would undo the line above.

# AppleDouble litter, swept in the background.
#
# Copying to the card from a Mac leaves ._name beside every file, and the
# project's own install does the same: 76 of them from a 43-file payload on a
# freshly formatted card, measured 2026-09-02. Harmless - lib_scan skips every
# dot-prefixed name in all three of its readdir loops, so they never become
# phantom games - but they double the directory entries on a FAT card and
# "._Contra (USA).zip" sitting beside the real one is confusing to read.
#
# Backgrounded behind a sleep because it is housekeeping and not a
# precondition: the scan already ignores them, so nothing waits on this. A
# card-wide sweep measured 190ms against a boot of about 900, which is too
# much to spend on the critical path and nothing at all once it is off it. The
# sleep also keeps it clear of the library scan's own I/O.
#
# ONLY the ._ prefix. .media, .cheevos and .tortos are ours and the whole
# library hangs off them. busybox find has no -delete, and -exec is used
# rather than xargs because these names contain spaces.
( sleep 8
  find /mnt/SDCARD -name '._*' -exec rm -f {} \; ) >/dev/null 2>&1 &

# Restart loop: only ever exits for a power-off.
cd "$TORTOS_DIR"
FAILS=0
while : ; do
	leds_off
	start_resident          # bring it back if it died
	START=$(cut -d. -f1 /proc/uptime)
	./tortos.elf >> "$LOG" 2>&1
	[ -f /tmp/tortos_poweroff ] && break
	END=$(cut -d. -f1 /proc/uptime)
	# A launcher that dies immediately, five times running, is not going to
	# start on the sixth. Stop rather than strobe.
	if [ $((END - START)) -lt 5 ]; then
		FAILS=$((FAILS + 1))
		[ $FAILS -ge 5 ] && break
	else
		FAILS=0
	fi
	sleep 1
done

sync
poweroff
sleep 10
