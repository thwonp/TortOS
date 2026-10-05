#!/bin/sh
# Assemble the installable payload and its release zip.
#
# Brick (the default): out/sd/. Copy the CONTENTS of out/sd/ to the root of a
# FAT32 SD card, put it in a stock Brick or Brick Hammer and power on: the first
# boot installs the runtrimui.sh hook and every boot after that comes straight
# up in TortOS.
#
# GKD (PLATFORM=gkd): out/gkd/, the two folders docs/install-gkd.md installs -
# copy_to_sd/ (TortOS/ and the card's empty Roms/, Music/, Audiobooks/, Bios/,
# Saves/) for the card and system.d/ for the GKD's storage - and that guide as
# INSTALL.md. No boot hook: ROCKNIX owns the GKD's boot.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PLATFORM=${PLATFORM:-brick}
VERSION=${VERSION:-1.01}
if [ "$PLATFORM" = gkd ]; then
	B=$ROOT/build/gkd
	OUT=$ROOT/out/gkd
	CARD=$OUT/copy_to_sd
	ZIP=$ROOT/out/plorpOS-gkd-v$VERSION.zip
else
	B=$ROOT/build
	OUT=$ROOT/out/sd
	CARD=$OUT
	ZIP=$ROOT/out/plorpOS-brick-v$VERSION.zip
fi
P=$CARD/TortOS

[ -f "$B/tortos.elf" ] || { echo "run make PLATFORM=$PLATFORM first"; exit 1; }
# A release ships with the ScreenScraper developer pair built in, or box art
# quietly falls back to libretro for everyone who installs it. A build without
# one is fine for development; it is not fine to package. The pair comes from
# .screenscraper.env (see the Makefile). ALLOW_NO_SS=1 packages one anyway.
if grep -q '^#define SS_DEVID *""' "$B/ss_creds.h" 2>/dev/null ||
   [ ! -f "$B/ss_creds.h" ]; then
	if [ "${ALLOW_NO_SS:-0}" != 1 ]; then
		echo "payload: $B/tortos.elf has no ScreenScraper developer pair." >&2
		echo "payload: add .screenscraper.env, rebuild, or set ALLOW_NO_SS=1" >&2
		exit 1
	fi
	echo "payload: WARNING: packaging without the ScreenScraper developer pair" >&2
fi
DIATOM_ELF=${DIATOM_ELF:-$ROOT/../diatom/build/$PLATFORM/diatom}
[ -f "$DIATOM_ELF" ] || { echo "no diatom at $DIATOM_ELF (set DIATOM_ELF)"; exit 1; }
[ -f "$ROOT/vendor/cores/fceumm_libretro.so" ] || { echo "run mk/fetch-vendor.sh first"; exit 1; }

rm -rf "$OUT"
mkdir -p "$P/cards" "$P/cores" "$P/res/web" \
         "$CARD/Roms" "$CARD/Music" "$CARD/Audiobooks" "$CARD/Bios" "$CARD/Saves"

cp "$B/tortos.elf" "$P/"
cp "$B/muse" "$P/"                        # the audio player's engine; Muse cannot play without it
if [ "$PLATFORM" = gkd ]; then
	cp "$B/musectl" "$P/"
	cp "$ROOT/sd/gkd/launch.sh" "$P/"
	# diatom's GLSL passes and the in-game menu's list of them (plorpos-gkd.72)
	mkdir -p "$P/shaders"
	cp "$ROOT/res/shaders/"* "$P/shaders/"
	# The three systemd drop-ins, folders and all: each .conf only works
	# inside its <service>.service.d/.
	cp -R "$ROOT/sd/gkd/system.d" "$OUT/"
	# ROCKNIX's boot touches an empty roms/pico-8/Splore.png (for its own
	# EmulationStation) unless this file is there, and the shelf would list
	# it as a cart. It does nothing to plorpOS's Splore (plorpos-gkd.32.8).
	mkdir -p "$CARD/Roms/Pico-8"
	touch "$CARD/Roms/Pico-8/.disable_splore"
	cp "$ROOT/docs/install-gkd.md" "$OUT/INSTALL.md"
else
	mkdir -p "$OUT/.tmp_update" "$OUT/trimui/app"
	cp "$B/setbright" "$P/"               # brightness before the boot animation
	cp "$B/btplayer" "$P/"                # lets a headset's volume through BlueZ
	cp "$ROOT/sd/tortos/launch.sh" "$P/"
	cp "$ROOT/sd/tortos/bt-alsa.sh" "$P/" # sourced by launch.sh, run by the launcher
	cp "$ROOT/sd/tortos/radio.sh" "$P/"   # sourced by launch.sh AND by plat_sleep()
	cp "$ROOT/res/boot/tortos-boot.mp4" "$P/"
	cp "$ROOT/res/boot/bootlogo.bmp" "$P/"    # u-boot splash, applied on first boot
	cp "$ROOT/res/boot/splash.png" "$P/"      # the pic2fb loading splash, likewise
	cp "$ROOT/docs/install-brick.md" "$OUT/INSTALL.md"  # lands on the card root; harmless
fi
# Only systems.cfg is shipped now. tortos.cfg, turbo.cfg and coreopts.cfg are
# compiled into the launcher and seed the settings database on first run, so
# there is no file to ship and none to drift from the code that reads it.
#
# systems.cfg stays a file because it is BUILD input, not a setting: the check
# below reads it to refuse a card whose cores are missing, and the loop further
# down reads it to create the ROM folders. Both run on the host, before any
# database exists.
#
# The Brick ships without the cores it has not been released on yet
# (plorpos-gkd.84): pcsx_rearmed (PlayStation) and fake08 (PICO-8) each come
# back under their own bead; fbneo (Arcade, Neo Geo) came back in
# plorpos-reo.1. Their shelves go with them, so the card's systems.cfg - not
# config/ - is what everything below reads: the cores copied, the check, the
# ROM folders.
if [ "$PLATFORM" = gkd ]; then
	cp "$ROOT/config/systems.cfg" "$P/"
else
	awk -F'|' 'BEGIN { drop["pcsx_rearmed"]; drop["fake08"] }
	           { c = $4; gsub(/^[ \t]+|[ \t]+$/, "", c) }
	           !($1 == "sys" && c in drop)' \
	    "$ROOT/config/systems.cfg" > "$P/systems.cfg"
fi
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
# The device has curl and OpenSSL but nothing to trust - see res/ssl/README.md.
# Without this, every HTTPS request fails verification and achievements never
# arrive, with an error that reads like the network being down.
cp "$ROOT/res/ssl/cacert.pem" "$P/"
cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$P/"  # MIT, and which files are not (NOTICE)
cp -r "$ROOT/LICENSES" "$P/"            # PolyForm Noncommercial, for the NextUI-derived parts
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$P/"  # notices for the redistributed software
# What a core needs on the card besides itself, only when it ships.
if grep -q '^sys|[^|]*|[^|]*|fbneo|' "$P/systems.cfg"; then
	cp "$ROOT/res/fbneo-titles.tsv" "$P/res/"   # Arcade and Neo Geo titles (src/titles.c)
	cp "$ROOT/LICENSE-FBNeo.txt" "$P/"     # FBNeo requires its full text, verbatim
fi
if grep -q '^sys|[^|]*|[^|]*|fake08|' "$P/systems.cfg"; then
	cp "$ROOT/LICENSE-fake08.md" "$P/"     # fake08's MIT notice and its components' terms
fi
cp "$DIATOM_ELF" "$P/diatom"

# The cores are the ones the card's systems.cfg names - no more, no fewer.
#
# This used to be a glob over vendor/cores, which ships whatever that folder
# happens to hold and says nothing about what is missing. That is how a card was
# nearly built with four of nine systems dead - systems.cfg gained SNES and the
# Sega machines, vendor/ still held the original three, and nothing failed. A
# shelf whose cards open onto nothing is a worse failure than a build that
# refuses.
missing=
for core in $(awk -F'|' '$1=="sys"{gsub(/^[ \t]+|[ \t]+$/,"",$4); print $4}' \
              "$P/systems.cfg" | sort -u); do
	cp "$ROOT/vendor/cores/${core}_libretro.so" "$P/cores/" 2>/dev/null ||
		missing="$missing $core"
done
if [ -n "$missing" ]; then
	echo "payload: systems.cfg needs cores that vendor/ does not have:$missing" >&2
	echo "payload: run mk/fetch-vendor.sh" >&2
	exit 1
fi

# One ROM folder per system, with the .media folder box art goes in. Read from
# systems.cfg (awk, not sed: folder names contain spaces).
awk -F'|' '$1=="sys"{gsub(/^[ \t]+|[ \t]+$/,"",$3); print $3}' "$P/systems.cfg" |
while IFS= read -r folder; do
	mkdir -p "$CARD/Roms/$folder/.media"
done
# Bios/ is created above and stays FLAT. It is handed to the core as its system
# directory and a core asks for a filename inside it - mgba wants gba_bios.bin,
# mednafen wants syscard3.pce - so a folder per system is a place a BIOS goes to
# be ignored. This line used to read `mkdir -p "$OUT/Bios/GBA"`, directly under a
# comment saying not to litter the card with empty Bios folders, and it shipped
# in v1.0: an empty GBA/ that contradicted the README two directories away.
# Nothing errors when a BIOS lands in it. mgba falls back to its built-in one and
# the only symptom is the boot animation you were trying to enable not appearing.

chmod +x "$P/launch.sh" "$P/tortos.elf" "$P/diatom" "$P/muse"
if [ "$PLATFORM" = gkd ]; then
	chmod +x "$P/musectl"
	du -sh "$OUT"
	echo "payload ready: $OUT"
	rm -f "$ZIP"
	( cd "$OUT" && zip -qr "$ZIP" . -x '.DS_Store' '._*' )
	echo "release zip:  $ZIP  ($(du -h "$ZIP" | cut -f1))"
	exit 0
fi

cp "$ROOT/sd/.tmp_update/updater" "$ROOT/sd/.tmp_update/tg3040.sh" "$OUT/.tmp_update/"
cp "$ROOT/sd/trimui/app/MainUI" "$ROOT/sd/trimui/app/runtrimui.sh" "$OUT/trimui/app/"

chmod +x "$OUT/.tmp_update/updater" "$OUT/.tmp_update/tg3040.sh" \
         "$OUT/trimui/app/MainUI" "$OUT/trimui/app/runtrimui.sh" \
         "$P/setbright" "$P/btplayer"

du -sh "$OUT"
echo "payload ready: $OUT"

rm -f "$ZIP"
( cd "$OUT" && zip -qr "$ZIP" . -x '.DS_Store' '._*' )
echo "release zip:  $ZIP  ($(du -h "$ZIP" | cut -f1))"
