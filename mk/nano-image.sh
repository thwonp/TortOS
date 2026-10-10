#!/bin/sh
# The RG Nano's flashable card image (plorpos-ggv.41.4): DrUm78's FunKey-OS
# release image with plorpOS installed, into out/plorpOS-nano-vVERSION-sdcard.zip.
#
#   mk/nano-image.sh VERSION    (make nano-image, after make nano-zip)
#
# No root and no build of FunKey-OS: the released image is checked by its
# sha256, its system partition (the second, ext4) is copied out and changed
# with debugfs, and put back. The changes are install-root.sh's, made
# offline - /usr/local/plorpos from the payload, plorpOS's frontend and
# asound.conf, /root/.profile without `instant_play load`, RetroFE and
# GMenu2X gone (install-root.sh's MENUS) - and a clean card: FunKey's first
# boot, which makes the shared partition, unzips plorpos_files.zip there
# (plorpOS/, Music/, Bios/ and a folder per console in nanoshelf's SYS[])
# in place of FunKey's freeware games, menu themes and OPKs, all removed, and
# `share init` no longer makes FunKey's folder tree at every start.
# /boot/zImage is DrUm78's kernel rebuilt with plorpOS's boot logo
# (mk/build-nano-kernel.sh).
# Everything else is DrUm78's image, byte for byte.
#
# Needs: debugfs, e2fsck (e2fsprogs), sfdisk, zip, unzip, curl.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
V=$1
BASE_NAME=FunKey-sdcard-DrUm78_RG_Nano.img
BASE_URL=https://github.com/DrUm78/FunKey-OS/releases/download/fps-classics/$BASE_NAME
BASE_SHA=725fb73a176b35ea6101b38a0e9a0b70f20232ab608b83793bed05ca4a7ad065
BASE=$ROOT/build/nano/$BASE_NAME
P=$ROOT/build/nano/payload/plorpOS-nano-v$V/plorpOS
W=$ROOT/build/nano/image
FS=$W/root.ext4
C=$W/debugfs.cmds
IMG=$W/plorpOS-nano-v$V.img
OUT=$ROOT/out/plorpOS-nano-v$V-sdcard.zip

die() { echo "$*" >&2; exit 1; }
for t in debugfs e2fsck sfdisk zip unzip curl; do
	command -v $t >/dev/null || die "need $t"
done
[ "$(cat "$P/VERSION" 2>/dev/null)" = "$V" ] || die "no v$V payload in $P; run make nano-zip"
[ -f "$ROOT/build/nano/zImage" ] || die "no build/nano/zImage; run make nano-kernel"
[ -f "$BASE" ] || curl -fL -o "$BASE" "$BASE_URL"
echo "$BASE_SHA  $BASE" | sha256sum -c --quiet

# The system partition: the image's second.
set -- $(sfdisk -d "$BASE" | sed -n 's|^.*2 : start= *\([0-9]*\), size= *\([0-9]*\),.*|\1 \2|p')
START=$1 SIZE=$2
rm -rf "$W"
mkdir -p "$W" "$ROOT/out"
dd if="$BASE" of="$FS" bs=512 skip="$START" count="$SIZE" status=none

dbg() { debugfs -R "$1" "$FS" 2>/dev/null; }
cmd() { printf '%s\n' "$*" >> "$C"; }
# A file or folder from the host at an absolute path in the image, root's.
put() {   # put SRC DST [MODE]
	cmd "write \"$1\" \"$2\""
	cmd "sif \"$2\" uid 0"
	cmd "sif \"$2\" gid 0"
	[ -z "${3-}" ] || cmd "sif \"$2\" mode $3"
}
mkd() {
	cmd "mkdir \"$1\""
	cmd "sif \"$1\" uid 0"
	cmd "sif \"$1\" gid 0"
}
put_tree() {   # put_tree SRCDIR DST
	mkd "$2"
	( cd "$1" && find . -mindepth 1 -type d | sort ) | while read -r d; do mkd "$2/${d#./}"; done
	( cd "$1" && find . -type f | sort ) | while read -r f; do put "$1/${f#./}" "$2/${f#./}"; done
}
# Removed, folders and all, as the stock image has them.
rm_tree() {
	dbg "ls -p \"$1\"" | while IFS=/ read -r _ _ mode _ _ name _; do
		case $name in ''|.|..) continue ;; esac
		case $mode in
		04*) rm_tree "$1/$name" ;;
		*)   cmd "rm \"$1/$name\"" ;;
		esac
	done
	cmd "rmdir \"$1\""
}
gone() {
	case $(dbg "stat \"$1\"" | sed -n 's/.*Type: \([a-z]*\).*/\1/p') in
	directory) rm_tree "$1" ;;
	'')        ;;
	*)         cmd "rm \"$1\"" ;;
	esac
}
: > "$C"

# Removed first, to make the room: RetroFE and GMenu2X (install-root.sh's
# MENUS), FunKey's card files and OPKs.
eval "$(sed -n '/^MENUS="/,/"$/p' "$ROOT/src/nano/install-root.sh")"
for m in $MENUS; do gone "/$m"; done
for f in freeware_games.zip funkey_files.zip; do gone "/usr/local/share/$f"; done
gone /usr/local/share/OPKs

# The kernel with plorpOS's boot logo.
cmd "rm /boot/zImage"
put "$ROOT/build/nano/zImage" /boot/zImage 0100644

# install-root.sh, offline.
mkd /usr/local/plorpos
for d in bin lib res cores pico8rt; do put_tree "$P/$d" "/usr/local/plorpos/$d"; done
put "$P/VERSION" /usr/local/plorpos/VERSION 0100644
put "$P/start.sh" /usr/local/plorpos/start.sh 0100755
for f in "$P"/bin/*; do cmd "sif \"/usr/local/plorpos/bin/${f##*/}\" mode 0100755"; done
cmd "sif /usr/local/plorpos/pico8rt/ld-linux-armhf.so.3 mode 0100755"
cmd "rm /usr/local/sbin/frontend"
put "$P/root/frontend" /usr/local/sbin/frontend 0100755
cmd "rm /etc/asound.conf"
put "$P/root/asound.conf" /etc/asound.conf 0100644
dbg "cat /root/.profile" > "$W/profile"
sed -i 's|^instant_play load$|# instant_play load   (plorpOS: nanoshelf resumes the game)|' "$W/profile"
# (and no folders for the menus it no longer has, at every start)
sed -i -e '/^mkdir -p "${GMENU2X_HOME}"$/d' -e '/^mkdir -p "${RETROFE_HOME}"$/d' \
       -e '/^mkdir -p "${RETROFE_HOME}\/layouts"$/d' "$W/profile"
grep -q 'mkdir.*\(GMENU2X\|RETROFE\)' "$W/profile" && die "/root/.profile: menu folders still made"
grep -q '^# instant_play load' "$W/profile" || die "/root/.profile: no instant_play load line"
cmd "rm /root/.profile"
put "$W/profile" /root/.profile 0100755

# A clean card: first boot unzips plorpOS's folders only.
mkdir -p "$W/card/plorpOS" "$W/card/Music" "$W/card/Bios"
sed -n '/^static const sys_t SYS\[\] = {/,/^};/s/^\t{ "\([^"]*\)".*/\1/p' "$ROOT/src/nano/nanoshelf.c" |
	while read -r d; do mkdir -p "$W/card/$d"; done
[ "$(ls "$W/card" | wc -l)" -gt 10 ] || die "no consoles read from nanoshelf.c SYS[]"
( cd "$W/card" && zip -qrX ../plorpos_files.zip . )
put "$W/plorpos_files.zip" /usr/local/share/plorpos_files.zip 0100644
dbg "cat /usr/local/sbin/first_boot" > "$W/first_boot"
sed -i -e 's|^\(\s*unzip -q -o /usr/local/share/\)freeware_games.zip|\1plorpos_files.zip|' \
       -e "s|# Copy freeware games and other necessary mnt files|# plorpOS's folders (plorpOS nano image)|" \
       -e '/funkey_files.zip/d' -e '/# Copy OPKs/d' -e '/^\s\s*set [+-]f$/d' \
       -e '/OPKs\/\* \/mnt/d' "$W/first_boot"
grep -q 'plorpos_files.zip' "$W/first_boot" && ! grep -q 'OPKs\|freeware\|funkey_files' "$W/first_boot" ||
	die "first_boot: copy step not as expected"
grep -q '^set -f' "$W/first_boot" || die "first_boot: lost its top-level set -f"
cmd "rm /usr/local/sbin/first_boot"
put "$W/first_boot" /usr/local/sbin/first_boot 0100755
# ... and kept clean: FunKey's `share init`, at every start, makes its own
# folder tree (FunKey's apps, Libretro, RetroFE's collections) on the card.
dbg "cat /usr/local/sbin/share" > "$W/share"
sed -i -e '/^\s*(cd \/mnt; mkdir -p "PICO-8"/d' -e '/^\s*(mkdir -p "\/mnt\/FunKey\/.retrofe\/collections"/d' \
       -e 's|# Create the directory structure if required|# (plorpOS nano image: no FunKey folders made at every start)|' "$W/share"
grep -q '^\s*(cd /mnt; mkdir\|retrofe/collections' "$W/share" && die "share: folder making still there"
cmd "rm /usr/local/sbin/share"
put "$W/share" /usr/local/sbin/share 0100755

debugfs -w -f "$C" "$FS" > "$W/debugfs.log" 2>&1
grep -i -E 'error|not found|could not|no free|exists' "$W/debugfs.log" && die "debugfs: see $W/debugfs.log"
e2fsck -fn "$FS" > "$W/e2fsck.log" 2>&1 || die "e2fsck: see $W/e2fsck.log"

cp "$BASE" "$IMG"
dd if="$FS" of="$IMG" bs=512 seek="$START" conv=notrunc status=none
rm -f "$OUT"
( cd "$W" && zip -qX "$OUT" "${IMG##*/}" )
echo "free on the system partition: $(dumpe2fs -h "$FS" 2>/dev/null | sed -n 's/^Free blocks: *//p') kB"
echo "$OUT ($(du -k "$OUT" | cut -f1) kB, md5 $(md5sum "$OUT" | cut -c1-8)); image md5 $(md5sum "$IMG" | cut -c1-8)"
