#!/bin/sh
# Assemble the installable SD payload under out/sd/.
#
# Copy the CONTENTS of out/sd/ to the root of a FAT32 SD card, put it in a
# stock Brick and power on: the first boot installs the runtrimui.sh hook and
# every boot after that comes straight up in TortOS.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out/sd
P=$OUT/TortOS

[ -f "$ROOT/build/tortos.elf" ] || { echo "run make first"; exit 1; }
# A release ships with the ScreenScraper developer pair built in, or box art
# quietly falls back to libretro for everyone who installs it. A build without
# one is fine for development; it is not fine to package. The pair comes from
# .screenscraper.env (see the Makefile). ALLOW_NO_SS=1 packages one anyway.
if grep -q '^#define SS_DEVID *""' "$ROOT/build/ss_creds.h" 2>/dev/null ||
   [ ! -f "$ROOT/build/ss_creds.h" ]; then
	if [ "${ALLOW_NO_SS:-0}" != 1 ]; then
		echo "payload: build/tortos.elf has no ScreenScraper developer pair." >&2
		echo "payload: add .screenscraper.env, rebuild, or set ALLOW_NO_SS=1" >&2
		exit 1
	fi
	echo "payload: WARNING: packaging without the ScreenScraper developer pair" >&2
fi
DIATOM_ELF=${DIATOM_ELF:-$ROOT/../diatom/build/brick/diatom}
[ -f "$DIATOM_ELF" ] || { echo "no diatom at $DIATOM_ELF (set DIATOM_ELF)"; exit 1; }
[ -f "$ROOT/vendor/cores/fceumm_libretro.so" ] || { echo "run mk/fetch-vendor.sh first"; exit 1; }

rm -rf "$OUT"
mkdir -p "$P/cards" "$P/cores" "$P/res/web" \
         "$OUT/.tmp_update" "$OUT/trimui/app" \
         "$OUT/Roms" "$OUT/Bios" "$OUT/Saves"

cp "$ROOT/build/tortos.elf" "$P/"
cp "$ROOT/build/setbright" "$P/"          # brightness before the boot animation
cp "$ROOT/build/btplayer" "$P/"           # lets a headset's volume through BlueZ
cp "$ROOT/build/muse" "$P/"               # the audio player's engine; Muse cannot play without it
cp "$ROOT/sd/tortos/launch.sh" "$P/"
cp "$ROOT/sd/tortos/bt-alsa.sh" "$P/"     # sourced by launch.sh, run by the launcher
cp "$ROOT/sd/tortos/radio.sh" "$P/"       # sourced by launch.sh AND by plat_sleep()
# Only systems.cfg is shipped now. tortos.cfg, turbo.cfg and coreopts.cfg are
# compiled into the launcher and seed the settings database on first run, so
# there is no file to ship and none to drift from the code that reads it.
#
# systems.cfg stays a file because it is BUILD input, not a setting: the check
# below reads it to refuse a card whose cores are missing, and the loop further
# down reads it to create the ROM folders. Both run on the host, before any
# database exists.
cp "$ROOT/config/systems.cfg" "$P/"
cp -R "$ROOT/res/cards/." "$P/cards/"     # the classic/ and fancy/ sets, as adb-deploy.sh pushes them
cp "$ROOT/res/fonts/menu.ttf" "$P/"       # the UI face, and the in-game menu's
# Over The Hare's page. The launcher serves these off the card at P_WEB, so a
# payload without them is a card whose transfer screen starts a server and then
# answers its own page with a 404. adb-deploy.sh has always pushed them and
# this did not, which is a difference that only shows on a built card - the
# reason to build one before calling it a release. The font goes in twice
# rather than being kept in the repo twice: the launcher reads $P/menu.ttf and
# a browser asks for /web/menu.ttf.
cp "$ROOT/res/web/"* "$P/res/web/"
cp "$ROOT/res/fonts/menu.ttf" "$P/res/web/menu.ttf"
cp "$ROOT/res/boot/tortos-boot.mp4" "$P/"
cp "$ROOT/res/boot/bootlogo.bmp" "$P/"    # u-boot splash, applied on first boot
cp "$ROOT/res/boot/splash.png" "$P/"      # the pic2fb loading splash, likewise
# The device has curl and OpenSSL but nothing to trust - see res/ssl/README.md.
# Without this, every HTTPS request fails verification and achievements never
# arrive, with an error that reads like the network being down.
cp "$ROOT/res/ssl/cacert.pem" "$P/"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$P/"  # notices for the redistributed software
cp "$DIATOM_ELF" "$P/diatom"
cp "$ROOT/vendor/cores/"*.so "$P/cores/"

# Every system on the shelf must have its core on the card.
#
# The copy above is a glob: it ships whatever vendor/cores happens to hold and
# says nothing about what is missing. That is how a card was nearly built with
# four of nine systems dead - systems.cfg gained SNES and the Sega machines,
# vendor/ still held the original three, and nothing failed. A shelf whose
# cards open onto nothing is a worse failure than a build that refuses.
missing=
for core in $(awk -F'|' '$1=="sys"{gsub(/^[ \t]+|[ \t]+$/,"",$4); print $4}' \
              "$ROOT/config/systems.cfg" | sort -u); do
	[ -f "$P/cores/${core}_libretro.so" ] || missing="$missing $core"
done
if [ -n "$missing" ]; then
	echo "payload: systems.cfg needs cores that vendor/ does not have:$missing" >&2
	echo "payload: run mk/fetch-vendor.sh" >&2
	exit 1
fi

cp "$ROOT/sd/.tmp_update/updater" "$ROOT/sd/.tmp_update/tg3040.sh" "$OUT/.tmp_update/"
cp "$ROOT/sd/trimui/app/MainUI" "$ROOT/sd/trimui/app/runtrimui.sh" "$OUT/trimui/app/"

chmod +x "$OUT/.tmp_update/updater" "$OUT/.tmp_update/tg3040.sh" \
         "$OUT/trimui/app/MainUI" "$OUT/trimui/app/runtrimui.sh" \
         "$P/launch.sh" "$P/tortos.elf" "$P/diatom" "$P/setbright" "$P/btplayer" "$P/muse"

# One ROM folder per system, with the .media folder box art goes in. Read from
# systems.cfg (awk, not sed: folder names contain spaces).
awk -F'|' '$1=="sys"{gsub(/^[ \t]+|[ \t]+$/,"",$3); print $3}' "$ROOT/config/systems.cfg" |
while IFS= read -r folder; do
	mkdir -p "$OUT/Roms/$folder/.media"
done
# Bios/ is created above and stays FLAT. It is handed to the core as its system
# directory and a core asks for a filename inside it - mgba wants gba_bios.bin,
# mednafen wants syscard3.pce - so a folder per system is a place a BIOS goes to
# be ignored. This line used to read `mkdir -p "$OUT/Bios/GBA"`, directly under a
# comment saying not to litter the card with empty Bios folders, and it shipped
# in v1.0: an empty GBA/ that contradicted the README two directories away.
# Nothing errors when a BIOS lands in it. mgba falls back to its built-in one and
# the only symptom is the boot animation you were trying to enable not appearing.

du -sh "$OUT"
echo "payload ready: $OUT"

VERSION=${VERSION:-1.0}
ZIP="$ROOT/out/TortOS-v$VERSION.zip"
rm -f "$ZIP"
( cd "$OUT" && zip -qr "$ZIP" . -x '.DS_Store' '._*' )
echo "release zip:  $ZIP  ($(du -h "$ZIP" | cut -f1))"
