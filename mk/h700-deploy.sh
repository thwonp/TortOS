#!/bin/sh
# Put a PLATFORM=h700 build on an H700 running BaseOS, over adb (the USB-A to
# USB-C cable attached at power-on: BaseOS's port picks host role otherwise).
#
#   mk/h700-deploy.sh elf       the launcher; the launch loop restarts it
#   mk/h700-deploy.sh diatom    the resident emulator (../diatom, PORT=h700,
#                               or DIATOM_ELF); restarted with the launcher
#   mk/h700-deploy.sh muse      Muse and musectl, with the FFmpeg libraries
#   mk/h700-deploy.sh all       launcher, SDL2 and FFmpeg libraries, launch
#                               script, Splore's wget shim, Muse and diatom; the frontend session restarts to run it
#
# The first `all` on a card that ran plorpOS under ROCKNIX moves that build's
# tortos.elf, diatom, muse and musectl to TortOS/.rocknix/ - they cannot run on BaseOS, and
# moving rather than deleting keeps the way back.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-elf}
CARD=/mnt/SDCARD
ELF=$ROOT/build/h700/tortos.elf
LIB=$ROOT/sysroot-h700/usr/lib

DIATOM=${DIATOM_ELF:-$ROOT/../diatom/build/h700/diatom}

adb get-state > /dev/null 2>&1 || { echo "no device over adb" >&2; exit 1; }
[ -f "$ELF" ] || { echo "no $ELF; run: make PLATFORM=h700" >&2; exit 1; }
case $MODE in muse|all)
	[ -f "$ROOT/build/h700/muse" ] || { echo "no build/h700/muse; run: make PLATFORM=h700" >&2; exit 1; } ;;
esac
case $MODE in diatom|all)
	[ -f "$DIATOM" ] || { echo "no diatom at $DIATOM (set DIATOM_ELF)" >&2; exit 1; } ;;
esac

if [ "$MODE" = all ]; then
	adb shell "cd $CARD/TortOS && [ -d .rocknix ] || { mkdir .rocknix &&
		for f in tortos.elf diatom muse musectl; do [ -f \$f ] && mv \$f .rocknix/; done; true; }"
	adb shell "mkdir -p $CARD/TortOS/lib $CARD/System"
	for l in libSDL2-2.0.so.0 libSDL2_image-2.0.so.0 libSDL2_ttf-2.0.so.0; do
		adb push "$(readlink -f "$LIB/$l")" "$CARD/TortOS/lib/$l" > /dev/null
	done
	adb push "$ROOT/sd/h700/launch_frontend.sh" "$CARD/System/launch_frontend.sh" > /dev/null
	adb shell "mkdir -p $CARD/TortOS/pico8"
	adb push "$ROOT/sd/tortos/pico8/wget" "$CARD/TortOS/pico8/wget" > /dev/null
	adb shell "chmod +x $CARD/TortOS/pico8/wget"
fi
case $MODE in muse|all)
	adb shell "mkdir -p $CARD/TortOS/lib"
	for l in libavformat.so.60 libavcodec.so.60 libavfilter.so.9 libswresample.so.4 libavutil.so.58; do
		adb push "$(readlink -f "$LIB/$l")" "$CARD/TortOS/lib/$l" > /dev/null
	done
	adb push "$LIB/COPYING.LGPLv2.1" "$CARD/TortOS/lib/" > /dev/null
	for f in muse musectl; do
		adb push "$ROOT/build/h700/$f" "$CARD/TortOS/$f.new" > /dev/null
		adb shell "chmod +x $CARD/TortOS/$f.new && mv -f $CARD/TortOS/$f.new $CARD/TortOS/$f"
	done
	echo "deployed muse $(md5sum "$ROOT/build/h700/muse" | cut -c1-8)" ;;
esac
[ "$MODE" = muse ] && { adb shell 'killall muse 2>/dev/null; true'; echo "muse restarted on the launcher's next ask"; exit 0; }
case $MODE in diatom|all)
	adb push "$DIATOM" "$CARD/TortOS/diatom.new" > /dev/null
	adb shell "chmod +x $CARD/TortOS/diatom.new && mv -f $CARD/TortOS/diatom.new $CARD/TortOS/diatom"
	echo "deployed diatom $(md5sum "$DIATOM" | cut -c1-8)" ;;
esac
[ "$MODE" = diatom ] && { adb shell 'killall diatom tortos.elf 2>/dev/null; true'; echo "launcher and diatom restarted"; exit 0; }
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
