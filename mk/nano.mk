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

$(BUILD)/musectl: tools/musectl.c
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -o $@ $<

$(BUILD)/nanokey: tools/nanokey.c
	mkdir -p $(BUILD)
	$(CC) $(OPT) $(WARN) -o $@ $<
