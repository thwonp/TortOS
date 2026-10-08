#!/bin/sh
# plorpOS-Nano: make plorpOS the RG Nano's menu (docs/install-nano.md).
# Run as root on the Nano, then reboot. Undone by uninstall-root.sh.
#
# Changes two files in the read-only system partition, after keeping FunKey's
# own in backup/root-stock.tar (the first install's copy is never replaced, so
# installing again cannot overwrite it with plorpOS's):
#   /usr/local/sbin/frontend   FunKey's menu loop -> plorpOS's (root/frontend)
#   /etc/asound.conf           speaker mix at half per channel (root/asound.conf)
set -e
HERE=/mnt/plorpOS
B=$HERE/backup
FILES="usr/local/sbin/frontend etc/asound.conf"

for f in bin/nanoshelf start.sh root/frontend root/asound.conf; do
	[ -f "$HERE/$f" ] || { echo "missing $HERE/$f" >&2; exit 1; }
done
mkdir -p "$B"
if [ ! -f "$B/root-stock.tar" ]; then
	(cd / && tar -cf "$B/root-stock.tar" $FILES)
	echo "kept FunKey's files in $B/root-stock.tar"
fi

/usr/local/sbin/rw
# Read-only again after; while the old menu loop still has its script open
# that fails, and the root stays writable until the restart, which mounts it
# read-only.
trap 'sync; /usr/local/sbin/ro 2>/dev/null || echo "(system partition read-only again after the restart)"' EXIT
cp "$HERE/root/frontend" /usr/local/sbin/frontend.new
chmod 755 /usr/local/sbin/frontend.new
mv -f /usr/local/sbin/frontend.new /usr/local/sbin/frontend
cp "$HERE/root/asound.conf" /etc/asound.conf.new
mv -f /etc/asound.conf.new /etc/asound.conf
rm -f /mnt/disable_frontend
sync
echo "plorpOS installed; restart the Nano to start it"
