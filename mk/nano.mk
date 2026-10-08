# Runs inside the TortOS toolchain container, for the RG Nano (plorpos-ggv):
# FunKey's SDK at /sdk (mk/fetch-nano-sdk.sh), whose sysroot is the device's
# own SDL 1.2, and the FFmpeg that script builds. Nothing here shares cross.mk's
# SDL2/aarch64 setup, so it is its own file. The SDK's compiler wrapper already
# passes -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard.
BUILD := build/nano
CC := /sdk/bin/arm-funkey-linux-musleabihf-gcc
FF := sdk-nano/ffmpeg/usr
OPT := -O2 -mtune=cortex-a7
WARN := -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE

# Muse with MUSE_HANDOVER: no SysV IPC in this kernel, so no dmix, and Muse
# and the game take turns on the codec as on the H700. FFmpeg comes from
# plorpOS/lib beside bin/, found by the rpath.
MUSE_SRC := $(wildcard src/muse/*.c)
$(BUILD)/muse: $(MUSE_SRC) $(wildcard src/muse/*.h)
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -DMUSE_HANDOVER -I$(FF)/include -L$(FF)/lib \
	      -Wl,-rpath-link,$(FF)/lib -Wl,-rpath,'$$ORIGIN/../lib' -o $@ $(MUSE_SRC) \
	      -lavformat -lavcodec -lavfilter -lswresample -lavutil -lpthread -ldl

# The frontend: SDL 1.2 and SDL_ttf from the SDK's sysroot (the device's own),
# with the launcher's Muse client and library walk, which need no SDL.
SYSROOT := /sdk/arm-funkey-linux-musleabihf/sysroot
NANO_VERSION ?= 0.0
SHELF_SRC := src/nano/nanoshelf.c src/nano/plat_nano.c src/musec.c src/muselib.c src/musequeue.c
$(BUILD)/nanoshelf: $(SHELF_SRC) src/musec.h src/muselib.h src/musequeue.h src/library.h src/platproc.h
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -DTORTOS_VERSION='"$(NANO_VERSION)"' -I$(SYSROOT)/usr/include/SDL \
	      -o $@ $(SHELF_SRC) -lSDL_ttf -lSDL -lpthread

$(BUILD)/musectl: tools/musectl.c
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -o $@ $<

$(BUILD)/nanokey: tools/nanokey.c
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -o $@ $<
