#!/bin/sh
# plorpOS-Nano: start the shelf. FunKey's frontend loop runs this once
# install-root.sh has put plorpOS in it; by hand, over adb, stop the stock
# frontend first (docs/install-nano.md). Its log is a per-run summary in
# /tmp, not on the card.
cd /mnt/plorpOS || exit 1
# What /etc/profile gives a login shell, which the boot does not run: SDL with
# no mouse (or video fails "Unable to open mouse"), and FunKey's tools.
export SDL_NOMOUSE=1
export PATH=/usr/local/games:/usr/games:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
exec ./bin/nanoshelf 2>>/tmp/nanoshelf.log
