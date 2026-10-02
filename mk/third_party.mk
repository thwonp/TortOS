# The vendored code under third_party/ (see third_party/README.md): libchdr
# and the disc half of rcheevos' rhash, which hash a CD game the way
# RetroAchievements does (plorpos-gkd.53). Included by mk/cross.mk for the
# devices and by the Makefile for the host checks; each sets TP_OUT (where the
# objects go) and TP_OPT (its optimisation and target flags) first.
#
# Compiled with their own flags and without -Wall: a warning in someone else's
# code is not one this repository can fix.
#
# TP_DEFS are not only theirs. rc_hash.h lays out rc_hash_iterator_t by them,
# so every file that includes it has to be compiled with the same ones - which
# is why consumers add TP_CFLAGS, not just the include paths. A mismatch is no
# compile error, just a struct of the wrong size.
TP_DEFS   := -DRC_HASH_NO_ROM -DRC_HASH_NO_ENCRYPTED -DRC_HASH_NO_ZIP
TP_CFLAGS := $(TP_DEFS) -Ithird_party/rcheevos/include -Ithird_party/libchdr/include

# libchdr's unity.c is the library in one file, all but the A/V Huffman codec,
# which chd.c's codec table still names.
TP_SRC := third_party/libchdr/unity.c \
          third_party/libchdr/src/libchdr_codec_avhuff.c \
          third_party/rcheevos/src/rc_compat.c \
          $(addprefix third_party/rcheevos/src/rhash/,hash.c hash_disc.c cdreader.c md5.c)
TP_OBJ := $(patsubst third_party/%.c,$(TP_OUT)/%.o,$(TP_SRC))

# RC_NO_THREADS: rc_compat's mutexes serve rc_client, not rhash, and would
# need -lpthread on the Brick, whose glibc keeps it apart from libc.
TP_INC := $(TP_CFLAGS) -DRC_NO_THREADS -Ithird_party/libchdr/deps/lzma-26.02/include \
          -Ithird_party/libchdr/deps/miniz-3.1.2 -Ithird_party/libchdr/deps/zstd-1.5.7

$(TP_OUT)/%.o: third_party/%.c mk/third_party.mk
	@mkdir -p $(@D)
	$(CC) $(TP_OPT) $(TP_INC) -c $< -o $@
