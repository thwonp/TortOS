# TortOS's own toolchain: a stock Debian cross-compiler pinned by digest, with
# the device's SDL2 in sysroot/. Every dependency is TortOS's own or the
# TrimUI's SDK. Build it once with `make toolchain`, and the sysroot once with
# `mk/fetch-sysroot.sh` (needs the device on adb).
IMAGE := tortos-toolchain
# The Brick is .100. The .101 this used to say is the address in the global
# notes, and it is wrong - every SSH target needed BRICK= on the command line
# to work at all.
BRICK ?= 192.168.1.100
# UserKnownHostsFile=/dev/null is not laziness: when the recorded host key does
# not match, ssh DISABLES password authentication to protect the password, and
# every SSH target here fails with "Permission denied (publickey,password)"
# while the credential is perfectly correct. The device is on the LAN and gets
# reflashed, so its key changes; there is nothing here worth pinning it for.
SSH := sshpass -p 'tina' ssh -o StrictHostKeyChecking=no \
       -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$(BRICK)

.PHONY: all clean native toolchain vendor boot checkmark payload release install-card \
        adb adb-elf adb-res adb-vendor adb-restart adb-run adb-log \
        check check-cheevos check-hare check-httpd check-idle check-rahash \
        check-raset check-xfer check-menus check-artscrape check-artrun check-audioout \
        check-db check-stats check-sort check-bt check-backlog check-ss check-hkbind \
        hooks storeprobe deploy restart logs

all: build/tortos.elf

# Everything offline, in one command. There was no umbrella target: every check
# had to be remembered by name, which is a suite in the same sense that a list
# of good intentions is a plan. A check nobody runs is a check that does not
# exist, and check-menus was about to join eight others in that state.
CHECKS = check-cheevos check-hare check-httpd check-idle check-rahash \
         check-raset check-xfer check-menus check-artscrape check-artrun check-audioout \
         check-db check-stats check-sort check-bt check-backlog check-ss \
         check-muselib check-musequeue check-museart check-controls check-hkbind

check:
	@fail=0; for c in $(CHECKS); do \
		printf '\n=== %s ===\n' "$$c"; \
		$(MAKE) --no-print-directory "$$c" || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then printf '\nsome checks FAILED\n' >&2; exit 1; fi; \
	printf '\nok: every check passed\n'

# One version number: the zip name and the About page both read it from here.
VERSION ?= 1.0

# The ScreenScraper developer pair, from .screenscraper.env in this directory
# (gitignored, never committed). A value already in the environment wins, and
# no file is still a supported build: the defines come out empty and art falls
# back to libretro. payload.sh is what refuses to ship a build like that.
SS_ENV := .screenscraper.env
ss_value = $(shell [ -f $(SS_ENV) ] && sed -n 's/^$(1)=//p' $(SS_ENV) | head -1)
SS_DEVID ?= $(call ss_value,SS_DEVID)
SS_DEVPASS ?= $(call ss_value,SS_DEVPASS)
export SS_DEVID SS_DEVPASS

# The header the pair is compiled from, regenerated on the host every run.
# mk/cross.mk's creds target only replaces it when the contents change, so the
# elf relinks exactly when the pair does - not when the file's date says so.
build/ss_creds.h: FORCE
	@$(MAKE) --no-print-directory -f mk/cross.mk creds

build/tortos.elf: $(wildcard src/*.c) $(wildcard src/*.h) tools/setbright.c mk/cross.mk \
                  $(wildcard src/muse/*.c) $(wildcard src/muse/*.h) tools/musectl.c \
                  tools/btplayer.c \
                  build/ss_creds.h
	@docker image inspect $(IMAGE) > /dev/null 2>&1 || { \
		echo "toolchain image missing; run: make toolchain" >&2; exit 1; }
	@[ -d sysroot/usr/include/SDL2 ] || { \
		echo "no sysroot; run: mk/fetch-sysroot.sh (needs the device)" >&2; exit 1; }
	@# -e VAR with no value passes the HOST's value through, so the pair
	@# reaches the container without appearing in this command line.
	docker run --rm -e SS_DEVID -e SS_DEVPASS -v $(CURDIR):/work -w /work $(IMAGE) \
		make -f mk/cross.mk SYSROOT=/work/sysroot VERSION=$(VERSION) creds build/tortos.elf build/setbright build/muse build/musectl build/btplayer
	@# Refuse to be quiet about an output older than its own source.
	@#
	@# Docker on macOS can show the container a stale mtime for a file the host
	@# just wrote, so make inside the container decides a target is current when
	@# it is not. Nothing fails: the build reports success, the push reports
	@# success, and the device runs the PREVIOUS binary. It happened on
	@# 2026-09-04 - a string change to src/main.c produced a byte-identical elf
	@# and was deployed and "verified" before anyone noticed the elf was older
	@# than the file it was built from.
	@#
	@# Diatom's tools/brick-make.sh has carried this check since 2026-08-25,
	@# where the same thing cost an hour twice. A comment here used to claim
	@# mk/cross.mk carried one too. It did not.
	@for src in $(wildcard src/*.c) $(wildcard src/*.h) mk/cross.mk build/ss_creds.h; do \
		if [ "$$src" -nt build/tortos.elf ]; then \
			echo "STALE: build/tortos.elf is older than $$src" >&2; \
			echo "  the container did not rebuild. rm build/tortos.elf and try again." >&2; \
			exit 1; \
		fi; \
	done
	@[ tools/setbright.c -nt build/setbright ] && { \
		echo "STALE: build/setbright is older than tools/setbright.c" >&2; exit 1; } || true
	@# And the other binaries this builds, which went unchecked until
	@# 2026-09-26: build/btplayer came out 26 minutes older than its source,
	@# silently, and a deploy's hash check could not see it - it compares the
	@# stale build with the copy made from it. Caught only by grepping the
	@# binary for a string from the change.
	@for src in $(wildcard src/muse/*.c) $(wildcard src/muse/*.h); do \
		if [ "$$src" -nt build/muse ]; then \
			echo "STALE: build/muse is older than $$src" >&2; \
			echo "  the container did not rebuild. rm build/muse and try again." >&2; \
			exit 1; \
		fi; \
	done
	@for pair in musectl:tools/musectl.c btplayer:tools/btplayer.c; do \
		bin=build/$${pair%%:*}; src=$${pair#*:}; \
		if [ "$$src" -nt "$$bin" ]; then \
			echo "STALE: $$bin is older than $$src" >&2; \
			echo "  the container did not rebuild. rm $$bin and try again." >&2; \
			exit 1; \
		fi; \
	done

# The check binaries are rebuilt every time, deliberately.
#
# They take about a second each, and make's mtime comparison is second-granular
# - so editing a source and re-running a check inside the same second silently
# tests the PREVIOUS binary. That is not theoretical: it happened three times on
# 2026-08-30 while checking whether a check actually catches the bug it claims
# to, and each time a clean source read as a failing one. The device build above
# now carries a staleness check for the same reason, where a rebuild is
# too slow to just repeat.
#
# A check that quietly tests the wrong binary is worse than no check.
FORCE:

# The achievement half, checked on the host. It parses a file and filters a
# list - no SDL, no device, no emulator - and every one of its claims fails
# invisibly on hardware, which is the argument for checking it here.
check-cheevos: build-native/cheevos-check
	@./build-native/cheevos-check

build-native/cheevos-check: tools/cheevos-check.c src/cheevos.c src/cheevos.h \
                            src/atomic.c src/atomic.h src/db.c src/db.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/cheevos-check.c src/cheevos.c src/atomic.c src/db.c

# Hare's routes: is anything reachable without the PIN? check-xfer proves a
# path cannot climb out of a root; this proves the routes actually ask it, and
# that they are behind the lock - separate claims, and the ones that regress
# when an endpoint is added.
check-hare: build-native/hare-check
	@./build-native/hare-check

build-native/hare-check: tools/hare-check.c src/hare.c src/httpd.c src/xfer.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/hare-check.c src/hare.c src/httpd.c src/xfer.c

# Hare's transport, driven by a real client over a real socket. An HTTP parser
# is where "looks right" and "is right" part company: every browser sends the
# well-formed case, and only the interesting failures send anything else.
check-httpd: build-native/httpd-check
	@./build-native/httpd-check

build-native/httpd-check: tools/httpd-check.c src/httpd.c src/httpd.h src/xfer.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/httpd-check.c src/httpd.c

# Hare's path safety: can a browser on the LAN climb out of the three roots?
# Every other check in here protects a feature; this one protects the device.
check-xfer: build-native/xfer-check
	@./build-native/xfer-check

# What a menu contains, for a given state. The point of ADR-0001: a screen's
# build function takes state and produces rows with no renderer and no device,
# so this is answerable here instead of by looking at a handheld. Every menu
# defect this screen has had was some form of "nobody noticed the list was
# wrong", and every one of them needed a device to see.
#
# It links the pure halves of the screens and NOT SDL. If it ever needs SDL to
# link, ADR-0001 has failed - reopen it rather than adding the flag.
check-menus: build-native/menu-check
	@./build-native/menu-check

# Where sound goes, given a cable, a headset and a setting. Eight combinations
# and a five-line rule, which is exactly the kind of thing that is obviously
# right until a headset connects mid-game and it is not.
check-audioout: build-native/audioout-check
	@./build-native/audioout-check

build-native/audioout-check: tools/audioout-check.c src/audioout.c \
                            src/audioout.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/audioout-check.c src/audioout.c

# What MENU > Controls says: every button on exactly one page, and rows short
# enough to sit still. The page is the device's own copy of the controls list,
# so what it omits is invisible until somebody goes looking for a button.
check-controls: build-native/controls-check
	@./build-native/controls-check

build-native/controls-check: tools/controls-check.c src/controls.c \
                            src/controls.h src/menu.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/controls-check.c src/controls.c

build-native/menu-check: tools/menu-check.c src/wifi_menu.c src/wifi.c \
                        src/sys_menu.c src/menu.c src/game_menu.c \
                        src/audioout.c src/wifi_menu.h src/sys_menu.h \
                        src/game_menu.h src/menu.h src/audioout.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -o $@ \
	      tools/menu-check.c src/wifi_menu.c src/wifi.c src/sys_menu.c \
	      src/menu.c src/game_menu.c src/audioout.c

build-native/xfer-check: tools/xfer-check.c src/xfer.c src/xfer.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/xfer-check.c src/xfer.c

# Auto Off's clock. It has broken five times, once in this arithmetic and four
# times in wiring; this covers the arithmetic, and `grep -n idle_due src/main.c`
# covers the wiring by listing every screen that honors it.
check-idle: build-native/idle-check
	@./build-native/idle-check

build-native/idle-check: tools/idle-check.c src/idle.c src/idle.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/idle-check.c src/idle.c

# The device's title matcher against the host tool's. A drift here does not
# fail, it just finds fewer games - see tools/artscrape-check.c.
check-artscrape: build-native/artscrape-check
	@ART_SCORING=1 ./build-native/artscrape-check
	@python3 tools/artscrape-check.py

build-native/artscrape-check: tools/artscrape-check.c src/artscrape.c src/artscrape.h src/urlenc.c
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -Isrc \
	      -DTORTOS_VERSION='"check"' \
	      -o $@ tools/artscrape-check.c src/artscrape.c src/net.c src/urlenc.c

# Replace fetches over a cover rather than deleting it first. The real scraper
# against a stubbed network, so a hit and a miss are both pinned with no device
# and no download - see tools/artrun-check.c.
check-artrun: build-native/artrun-check
	@./build-native/artrun-check

build-native/artrun-check: tools/artrun-check.c src/artscrape.c src/artscrape.h src/urlenc.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -Isrc -DSS_BUSY_WAIT=0 -DART_QUIET \
	      -o $@ tools/artrun-check.c src/artscrape.c src/urlenc.c

# The two hashers - one C for the device, one Python for the host tools - over
# every ROM in the library. A wrong rule does not crash; it produces a hash RA
# has never seen, which is indistinguishable from a game RA does not know.
check-rahash: build-native/rahash-check
	@python3 tools/rahash-check.py

build-native/rahash-check: tools/rahash-check.c src/rahash.c src/rahash.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/rahash-check.c src/rahash.c

# The two set converters - one C on the device, one Python in the bulk tool -
# over real RetroAchievements responses. Needs credentials and a network;
# skips cleanly without them.
check-raset: build-native/raset-check
	@python3 tools/raset-check.py

build-native/raset-check: tools/raset-check.c src/rafetch.c src/rajson.c src/rahash.c src/net.c src/atomic.c src/db.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -DTORTOS_VERSION='"check"' \
	      -o $@ tools/raset-check.c src/rafetch.c src/rajson.c src/rahash.c src/net.c src/atomic.c src/db.c

# Whether this build can reach ScreenScraper, and whether an account survives
# a restart. The live half needs a network and .screenscraper.env, and skips
# cleanly without them - the same bargain check-raset makes.
check-ss: build-native/ss-check
	@./build-native/ss-check

build-native/ss-check: tools/ss-check.c src/ss.c src/ss.h src/ssfetch.c src/ssfetch.h src/db.c src/net.c src/urlenc.c src/rajson.c FORCE
	@mkdir -p build-native
	@$(MAKE) -f mk/native.mk creds
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g -Ibuild-native \
	      -DTORTOS_VERSION='"check"' \
	      -o $@ tools/ss-check.c src/ss.c src/ssfetch.c src/db.c src/net.c src/urlenc.c src/rajson.c

# One-time: the cross-compiler image. Pinned by digest, so it does not drift.
toolchain:
	docker build -f mk/toolchain.Dockerfile -t $(IMAGE) mk

# Host build of the launcher, for working on how the shelf looks.
native:
	$(MAKE) -f mk/native.mk VERSION=$(VERSION)

# The libretro cores TortOS redistributes, hash-pinned from libretro's own
# buildbot. Needed once, before the first payload. No runtime libraries: the
# cores need only what the device's firmware already has.
vendor:
	./mk/fetch-vendor.sh

# Assets in res/ are committed ready to ship; nothing needs generating to build.
#
# The boot animation still has a generator. The system cards no longer do: they
# are drawn by hand, gencards.py could not reproduce any of the nine, and its
# only remaining effect would have been to overwrite three of them. Their accent
# rule is kept in step with config/systems.cfg by tools/recolor-cards.py.
boot:
	python3 tools/genboot.py

# What a session record costs to commit, measured on the filesystem you point
# it at. Built here rather than described in a commit message, because the
# numbers behind the game-stats storage decision will be re-run: the SD card
# and /mnt/UDISK answer differently, and so did this Mac, which is the reason
# the probe exists rather than an argument about fsync counts.
#
#   make storeprobe && adb push build/storeprobe /tmp/storeprobe
#   adb shell '/tmp/storeprobe /mnt/SDCARD/.userdata/tg3040 60'
#
# It writes only inside <dir>/storeprobe.tmp and removes it on the way out.
storeprobe:
	@docker image inspect $(IMAGE) > /dev/null 2>&1 || { \
		echo "toolchain image missing; run: make toolchain" >&2; exit 1; }
	docker run --rm -v $(CURDIR):/work -w /work $(IMAGE) \
		make -f mk/cross.mk build/storeprobe
	@# The same guard build/tortos.elf carries, and it earned its place on the
	@# first run of this target: after `rm -f build/storeprobe` the container
	@# still reported "up to date" and produced nothing, because Docker on macOS
	@# showed it a stale view of a file the host had just deleted.
	@[ -f build/storeprobe ] || { \
		echo "STALE: the container reported success and produced nothing." >&2; \
		echo "  run it again." >&2; exit 1; }
	@[ tools/storeprobe.c -nt build/storeprobe ] && { \
		echo "STALE: build/storeprobe is older than tools/storeprobe.c" >&2; \
		exit 1; } || true

# A worktree is a checkout of TRACKED files, so the four gitignored working
# documents stay behind in the main tree and a worktree session starts without
# the device rules. git runs post-checkout after `git worktree add`, and hooks
# come from the shared common dir, so this is a one-time install per clone.
hooks:
	git config core.hooksPath mk/hooks
	@echo "hooks: post-checkout will copy the working documents into new worktrees"

# BACKLOG.md is the other open-item store, and until 2026-09-05 it was the only
# one with nothing checking it. It drifted exactly that far: a heading still
# reading DECIDED, NOT BUILT while that work was being finished, and four
# Diatom-owned sections sitting where check-register could not see them.
#
# The file is gitignored, so this skips cleanly when it is absent rather than
# failing a worktree over a file a worktree cannot have.
# Fourteen config files became two databases, so properties that used to be
# obvious from looking at a text file now need asserting - chiefly that a
# shipped default never overwrites a choice, which is a bug this project
# already shipped once.
#
# Exit 77 means no libsqlite3 on this machine. That fails rather than skips:
# the launcher needs it too, so a machine without it cannot run TortOS at all.
check-db: build-native/db-check
	@./build-native/db-check; s=$$?; \
	if [ $$s -eq 77 ]; then \
		echo "  install sqlite3 - the launcher needs it, not just this check" >&2; \
	fi; exit $$s

build-native/db-check: tools/db-check.c src/db.c src/db.h src/atomic.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/db-check.c src/db.c src/atomic.c

# The in-game Hotkeys screen's binding parser (sibling Diatom feature,
# ADR-0035 there). Links src/hkbind.c and NOT SDL, same split as db-check.
check-hkbind: build-native/hkbind-check
	@./build-native/hkbind-check

build-native/hkbind-check: tools/hkbind-check.c src/hkbind.c src/hkbind.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/hkbind-check.c src/hkbind.c

# Play time. The assertion that earns this its place is that a LAUNCH writes
# nothing: that is a claim about a path nobody watches, and it stops being true
# one reasonable-looking write at a time.
check-stats: build-native/stats-check
	@./build-native/stats-check; s=$$?; \
	if [ $$s -eq 77 ]; then \
		echo "  install sqlite3 - the launcher needs it, not just this check" >&2; \
	fi; exit $$s

build-native/stats-check: tools/stats-check.c src/stats.c src/stats.h src/db.c src/atomic.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/stats-check.c src/stats.c src/db.c src/atomic.c

# What order a shelf ends up in. Two of the four orders read the session rows,
# so this links stats and the database - but not library.c: game_entry is a
# struct in the header and nothing here scans a directory.
check-sort: build-native/sort-check
	@./build-native/sort-check; s=$$?; \
	if [ $$s -eq 77 ]; then \
		echo "  install sqlite3 - the launcher needs it, not just this check" >&2; \
	fi; exit $$s

build-native/sort-check: tools/sort-check.c src/sort.h src/library.h src/stats.c \
                         src/stats.h src/db.c src/atomic.c FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/sort-check.c src/stats.c src/db.c src/atomic.c

# Bluetooth. A device NAME is arbitrary bytes chosen by whoever owns the
# headset, arriving over the air into a process running as root; an ADDRESS is
# the one value handed back to bluetoothctl. This checks the boundary between
# them.
check-bt: build-native/bt-check
	@./build-native/bt-check

build-native/bt-check: tools/bt-check.c src/bt.c src/bt.h src/bt_menu.c src/bt_menu.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/bt-check.c src/bt.c src/bt_menu.c

# Muse's view of the Music folder: the two shapes it reads, the litter a Mac
# leaves beside every track, and where an album's cover is kept.
check-muselib: build-native/muselib-check
	@./build-native/muselib-check

build-native/muselib-check: tools/muselib-check.c src/muselib.c src/muselib.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/muselib-check.c src/muselib.c

# The play modes: the last track, the first, a shuffle running out, a track
# that will not open, and a mode changed under a playing track.
check-musequeue: build-native/musequeue-check
	@./build-native/musequeue-check

build-native/musequeue-check: tools/musequeue-check.c src/musequeue.c src/musequeue.h FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/musequeue-check.c src/musequeue.c

# Album covers from MusicBrainz: the rule that picks the record, against what
# MusicBrainz said about the first card's albums, and a run on stubbed network.
check-museart: build-native/museart-check
	@./build-native/museart-check

build-native/museart-check: tools/museart-check.c src/museart.c src/museart.h \
                            src/urlenc.c src/rajson.c tools/fixtures/musicbrainz/*.json FORCE
	@mkdir -p build-native
	$(CC) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O1 -g \
	      -o $@ tools/museart-check.c src/museart.c src/urlenc.c src/rajson.c

check-backlog:
	@python3 tools/backlog-check.py

# src/main.c hand-keeps a copy of the mark that C cannot import from
# tools/markdef.py. payload depends on this for the same reason payload.sh
# refuses a card whose systems.cfg names cores vendor/ does not have: the
# failure is silent otherwise, and it ships.
checkmark:
	python3 tools/checkmark.py

payload: all checkmark
	./mk/payload.sh

release: payload
	@echo "out/TortOS-v$(VERSION).zip"

# Copy the assembled payload onto a mounted FAT32 card. CARD must be set.
install-card: payload
	@[ -n "$(CARD)" ] || { echo "usage: make install-card CARD=/Volumes/YOURCARD"; exit 1; }
	./mk/install-card.sh "$(CARD)"

# --- ADB over USB: works with no WiFi and no SSH, whenever the device is on.
adb: all
	./mk/adb-deploy.sh all
adb-elf: all
	./mk/adb-deploy.sh elf
adb-res:
	./mk/adb-deploy.sh res
adb-vendor:
	./mk/adb-deploy.sh vendor

# Every target below picks the TrimUI out of whatever else is plugged in, the
# same way mk/adb-deploy.sh does. A bare `adb shell` fails with "more than one
# device" the moment anything else is attached - and adb re-connects remembered
# network devices BY ITSELF, so this breaks without anyone plugging anything in.
# It failed exactly that way on 2026-09-04 with a stale 192.168.1.247:5555 entry
# reappearing between one command and the next, and the failure looks like the
# restart simply not happening.
ADBSEL = adb -s $$(adb devices | awk '/\tdevice$$/{print $$1}' | while read s; do \
	if adb -s "$$s" shell 'grep -qi TG.040 /proc/cpuinfo && echo yes' 2>/dev/null \
	   | grep -q yes; then echo "$$s"; break; fi; done)

# Kill the launcher so launch.sh's restart loop picks up a freshly pushed
# build. NEVER kill launch.sh itself: the boot hook's failsafe powers the
# device off when the launch loop exits.
adb-restart:
	$(ADBSEL) shell 'killall -q tortos.elf; exit 0'

# Restart the resident emulator too -- needed after pushing a new diatom.
adb-restart-all:
	$(ADBSEL) shell 'killall -q diatom; killall -q tortos.elf; exit 0'

# Run the launcher by hand with its output on your terminal: the fastest way
# to tell "the scan is wrong" from "the display is wrong".
adb-run:
	$(ADBSEL) shell 'killall -q tortos.elf; cd /mnt/SDCARD/TortOS && \
	  ROMS_PATH=/mnt/SDCARD/Roms \
	  LD_LIBRARY_PATH=/mnt/SDCARD/TortOS/lib:/usr/trimui/lib \
	  ./tortos.elf 2>&1' | head -60

adb-log:
	$(ADBSEL) shell 'tail -60 /mnt/SDCARD/.userdata/tg3040/logs/tortos.log 2>/dev/null'

# --- Push to a running device over SSH. BRICK=<ip> to override.
#
# The card art rides along, because a set of system images that does not reach
# the device is a change that silently does nothing - and the only sign is the
# shelf looking exactly as it did before.
#
# THREE FAULTS LIVED IN HERE, all of them invisible because BRICK defaulted to
# an address the device is not at, so this target was never once run:
#
#   - tortos.cfg was in the file list. It was deleted in 89ca6e5 when settings
#     moved into the database.
#   - the -C chain is RELATIVE AND CUMULATIVE. After `-C ../sd/tortos` the
#     working directory is sd/tortos, so `-C ../res/fonts` resolved to
#     sd/res/fonts and the font was never sent. It needs ../../ from there.
#   - the card art was never included at all.
#
# The first two both surface at the far end as "tar: short read", which names
# neither the file nor even which side failed. Run the sending tar on its own
# if this ever breaks again; it says exactly what it could not find.
#
# NOT a match for `make adb all`, which also sends Over The Hare's web assets,
# the boot video, the bootlogo and splash, the CA bundle and Diatom itself.
# Those change rarely and two of them are applied once and guarded by markers;
# this target is for the loop you are actually in, which is the launcher and
# what it draws.
deploy: all
	@# Silent, every line that carries $(SSH): it expands to the password, and
	@# make echoes a recipe before running it - into a terminal, a CI log, or a
	@# transcript. The same reason mk/cross.mk writes the ScreenScraper key
	@# into a header rather than onto a compile line.
	@tar -cf - -C build tortos.elf setbright muse musectl btplayer -C ../config systems.cfg \
	    -C ../sd/tortos launch.sh bt-alsa.sh radio.sh -C ../../res/fonts menu.ttf | \
	    $(SSH) 'tar -xf - -C /mnt/SDCARD/TortOS'
	@tar -cf - -C res cards | $(SSH) 'tar -xf - -C /mnt/SDCARD/TortOS'

restart:
	@$(SSH) 'killall -q tortos.elf; exit 0'

logs:
	@$(SSH) 'tail -60 /mnt/SDCARD/.userdata/tg3040/logs/tortos.log 2>/dev/null'

clean:
	rm -rf build build-native out
