#!/bin/sh
# plorpOS-Nano: put back what install-root.sh changed, from the copies it kept
# in /mnt/plorpOS/backup/. Run as root on the Nano, then reboot.
set -e
B=/mnt/plorpOS/backup
[ -f "$B/root-stock.tar" ] || { echo "no $B/root-stock.tar: nothing to put back" >&2; exit 1; }
/usr/local/sbin/rw
# Read-only again after; while the old menu loop still has its script open
# that fails, and the root stays writable until the restart, which mounts it
# read-only.
trap 'sync; /usr/local/sbin/ro 2>/dev/null || echo "(system partition read-only again after the restart)"' EXIT
for t in "$B"/root-*.tar; do
	tar -xf "$t" -C /
	echo "restored $(basename "$t")"
done
sync
echo "FunKey's menu restored; restart the Nano"
