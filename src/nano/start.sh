#!/bin/sh
# plorpOS-Nano: start the shelf, from wherever this script is - installed,
# /usr/local/plorpos (FunKey's frontend loop runs it, install-root.sh); tried
# out, the card's /mnt/plorpOS (by hand over adb, with the stock frontend
# stopped first: docs/install-nano.md). Its log is a per-run summary in /tmp,
# not on the card.
cd "$(dirname "$0")" || exit 1
# What /etc/profile gives a login shell, which the boot does not run: SDL with
# no mouse (or video fails "Unable to open mouse"), and FunKey's tools.
export SDL_NOMOUSE=1
export PATH=/usr/local/games:/usr/games:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
exec ./bin/nanoshelf 2>>/tmp/nanoshelf.log
