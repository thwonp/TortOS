#!/bin/sh
# Put a PLATFORM=h700 build on an H700 running BaseOS, over adb (the USB-A to
# USB-C cable attached at power-on: BaseOS's port picks host role otherwise).
#
#   mk/h700-deploy.sh elf       the launcher; the launch loop restarts it
#   mk/h700-deploy.sh all       launcher, SDL2 libraries and launch script;
#                               the frontend session restarts to run it
#
# The first `all` on a card that ran plorpOS under ROCKNIX moves that build's
# tortos.elf and diatom to TortOS/.rocknix/ - they cannot run on BaseOS, and
# moving rather than deleting keeps the way back.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-elf}
CARD=/mnt/SDCARD
ELF=$ROOT/build/h700/tortos.elf
LIB=$ROOT/sysroot-h700/usr/lib

adb get-state > /dev/null 2>&1 || { echo "no device over adb" >&2; exit 1; }
[ -f "$ELF" ] || { echo "no $ELF; run: make PLATFORM=h700" >&2; exit 1; }

if [ "$MODE" = all ]; then
	adb shell "cd $CARD/TortOS && [ -d .rocknix ] || { mkdir .rocknix &&
		for f in tortos.elf diatom; do [ -f \$f ] && mv \$f .rocknix/; done; true; }"
	adb shell "mkdir -p $CARD/TortOS/lib $CARD/System"
	for l in libSDL2-2.0.so.0 libSDL2_image-2.0.so.0 libSDL2_ttf-2.0.so.0; do
		adb push "$(readlink -f "$LIB/$l")" "$CARD/TortOS/lib/$l" > /dev/null
	done
	adb push "$ROOT/sd/h700/launch_frontend.sh" "$CARD/System/launch_frontend.sh" > /dev/null
fi
# Pushed aside and renamed, so a running launcher is never overwritten in place.
adb push "$ELF" "$CARD/TortOS/tortos.elf.new" > /dev/null
adb shell "chmod +x $CARD/TortOS/tortos.elf.new && mv -f $CARD/TortOS/tortos.elf.new $CARD/TortOS/tortos.elf && sync"
echo "deployed $(md5sum "$ELF" | cut -c1-8) ($MODE)"

if [ "$MODE" = all ]; then
	# A new launch script only runs in a new session: end this one (the
	# shell running System/launch_frontend.sh, found by its command line) and
	# init respawns frontend-session.
	adb shell 'for p in /proc/[0-9]*; do
		case "$(tr "\0" " " < $p/cmdline 2>/dev/null)" in
		"/bin/sh /mnt/sdcard/System/launch_frontend.sh"*) kill ${p#/proc/};;
		esac; done; killall tortos.elf 2>/dev/null; true'
	echo "frontend session restarted"
else
	adb shell 'killall tortos.elf 2>/dev/null; true'
	echo "launcher restarted"
fi
