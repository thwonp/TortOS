#!/bin/sh
# plorpOS on the TrimUI Model S (plorpos-80b): run by .tmp_update/updater at
# boot, in place of the stock menu, and never returns while the shelf works.
# Everything it says goes to /tmp: stock pipes the hook into a log on the card.
exec > /tmp/plorpos-start.log 2>&1
cd "$(dirname "$0")" || exit 1
P=$(pwd)
# The stock updater's progress screen, started with the hook.
killall -s KILL updateui 2>/dev/null
export SDL_NOMOUSE=1
export HOME=$P
export PATH=$P/bin:/usr/trimui/bin:/usr/bin:/usr/sbin:/bin:/sbin
# SDL 1.2 and SDL_ttf are the system's own, in /usr/trimui/lib.
export LD_LIBRARY_PATH=/usr/trimui/lib:/usr/lib:/lib
# No swap: the alpha's games fit in RAM (PlayStation peaked at 21 MB), and
# swap on the card puts every process at the mercy of its slowest read.
./bin/trimuimon "$P/volume.txt" &
# The shelf, again whenever it quits; five quick failures in a row and this
# gives up, so the stock menu comes back rather than a black screen.
fails=0
while [ $fails -lt 5 ]; do
	# A shelf that died during a game left the game running: a new one on
	# top of it would start a second game (and two do not fit in 58 MB).
	while pidof picoarch > /dev/null; do sleep 1; done
	t0=$(cut -d. -f1 /proc/uptime)
	./bin/shelf 2>> /tmp/shelf.log
	if [ $(( $(cut -d. -f1 /proc/uptime) - t0 )) -lt 10 ]; then fails=$((fails + 1)); else fails=0; fi
	sleep 1
done
