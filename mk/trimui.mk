# Runs inside the TortOS toolchain container, for the TrimUI Model S
# (plorpos-80b): the union trimui toolchain at /sdk (mk/fetch-trimui-sdk.sh),
# whose sysroot is the device's own SDL 1.2 and glibc 2.23, and the integer
# FFmpeg that script builds. The same sources as mk/nano.mk, with -DTRIMUI.
# The toolchain's host binaries want libmpfr.so.4: /sdk/hostlib has it.
export LD_LIBRARY_PATH := /sdk/hostlib
BUILD := build/trimui
CC := /sdk/usr/bin/arm-buildroot-linux-gnueabi-gcc
FF := sdk-trimui/ffmpeg/usr
OPT := -O2 -mcpu=arm926ej-s
WARN := -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE

# Muse with MUSE_HANDOVER (the game's SDL plays through OSS on the same codec,
# so they take turns, as on the Nano) and MUSE_FIXED: no FPU, so an
# integer-only chain (src/muse/dec.c).
MUSE_SRC := $(wildcard src/muse/*.c)
$(BUILD)/muse: $(MUSE_SRC) $(wildcard src/muse/*.h)
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -DMUSE_HANDOVER -DMUSE_FIXED -I$(FF)/include -L$(FF)/lib \
	      -Wl,-rpath-link,$(FF)/lib -Wl,-rpath,'$$ORIGIN/../lib' -o $@ $(MUSE_SRC) \
	      -lavformat -lavcodec -lavfilter -lswresample -lavutil -lpthread -ldl -lm

SYSROOT := /sdk/usr/arm-buildroot-linux-gnueabi/sysroot
TRIMUI_VERSION ?= 0.0
SHELF_SRC := src/nano/nanoshelf.c src/nano/plat_nano.c src/musec.c src/muselib.c src/musequeue.c src/audioout.c
$(BUILD)/shelf: $(SHELF_SRC) src/musec.h src/muselib.h src/musequeue.h src/library.h src/platproc.h src/audioout.h
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -DTRIMUI -DTORTOS_VERSION='"$(TRIMUI_VERSION)"' \
	      -I$(SYSROOT)/usr/include/SDL -o $@ $(SHELF_SRC) -lSDL_ttf -lSDL -lpthread -lm

$(BUILD)/musectl: tools/musectl.c
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -o $@ $<

$(BUILD)/trimuikey: tools/nanokey.c
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -DTRIMUI -o $@ $<
