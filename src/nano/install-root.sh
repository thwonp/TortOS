#!/bin/sh
# plorpOS nano: make plorpOS the RG Nano's menu (docs/install-nano.md).
# Run as root on the Nano, then reboot. Undone by uninstall-root.sh.
#
# Every change to the read-only system partition, each kept first in
# backup/ (the first install's copies are never replaced, so installing again
# cannot overwrite them with plorpOS's):
#   /usr/local/plorpos/        bin/ lib/ res/ start.sh, copied from here: run
#                              from the system partition, plorpOS can lend the
#                              card to a computer (share), as RetroFE could
#   /usr/local/sbin/frontend   FunKey's menu loop -> plorpOS's (root/frontend)
#   /etc/asound.conf           speaker mix at half per channel (root/asound.conf)
#   /root/.profile             its `instant_play load` off (backup/root-profile.tar):
#                              nanoshelf resumes a game the power key saved, so
#                              the music is there during it
#   /mnt/adb, /mnt/usbnet      removed on every run: the Nano starts as a USB
#                              drive (Settings > USB at start changes it)
#   RetroFE and GMenu2X        removed (backup/root-menus.tar); FunKey's cores,
#                              PicoArch's menu files and its apps stay
# Run it again after copying a new plorpOS folder to the card: it updates
# /usr/local/plorpos.
set -e
HERE=/mnt/plorpOS
B=$HERE/backup
APP=/usr/local/plorpos
FILES="usr/local/sbin/frontend etc/asound.conf"
MENUS="usr/games/retrofe usr/games/RetroFE.ico usr/games/RetroFE.png usr/games/README.txt
       usr/games/collections usr/games/controls.conf usr/games/launchers usr/games/layout.conf
       usr/games/layouts usr/games/log.txt usr/games/meta.db usr/games/settings.conf
       usr/bin/gmenu2x usr/share/gmenu2x"

for f in bin/nanoshelf bin/muse bin/picoarch start.sh root/frontend root/asound.conf; do
	[ -f "$HERE/$f" ] || { echo "missing $HERE/$f" >&2; exit 1; }
done
mkdir -p "$B"
if [ ! -f "$B/root-stock.tar" ]; then
	(cd / && tar -cf "$B/root-stock.tar" $FILES)
	echo "kept FunKey's files in $B/root-stock.tar"
fi
# The zip's adb file was only for running this: start the Nano as a USB drive
# again (no FunKey USB flag file). Settings > USB at start changes it.
rm -f /mnt/adb /mnt/usbnet
echo "USB at start: USB drive (from the next start)"
if [ ! -f "$B/root-menus.tar" ]; then
	present=""
	for m in $MENUS; do [ -e "/$m" ] && present="$present $m"; done
	(cd / && tar -cf "$B/root-menus.tar" $present)
	echo "kept RetroFE and GMenu2X in $B/root-menus.tar"
fi
if [ ! -f "$B/root-profile.tar" ]; then
	(cd / && tar -cf "$B/root-profile.tar" root/.profile)
	echo "kept FunKey's /root/.profile in $B/root-profile.tar"
fi

/usr/local/sbin/rw
# Read-only again after; while the old menu loop still has its script open
# that fails, and the root stays writable until the restart, which mounts it
# read-only.
trap 'sync; /usr/local/sbin/ro 2>/dev/null || echo "(system partition read-only again after the restart)"' EXIT
rm -rf "$APP.new"
mkdir -p "$APP.new"
cp -r "$HERE/bin" "$HERE/lib" "$HERE/res" "$HERE/start.sh" "$APP.new/"
chmod 755 "$APP.new/bin/"* "$APP.new/start.sh"
rm -rf "$APP"
mv "$APP.new" "$APP"
cp "$HERE/root/frontend" /usr/local/sbin/frontend.new
chmod 755 /usr/local/sbin/frontend.new
mv -f /usr/local/sbin/frontend.new /usr/local/sbin/frontend
cp "$HERE/root/asound.conf" /etc/asound.conf.new
mv -f /etc/asound.conf.new /etc/asound.conf
cp -p /root/.profile /root/.profile.new
sed 's|^instant_play load$|# instant_play load   (plorpOS: nanoshelf resumes the game)|' /root/.profile > /root/.profile.new
mv -f /root/.profile.new /root/.profile
for m in $MENUS; do rm -rf "/$m"; done
rm -f /mnt/disable_frontend
sync
echo "plorpOS nano installed; restart the Nano to start it"
