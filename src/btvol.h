/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_BTVOL_H
#define TORTOS_BTVOL_H
#include <stdbool.h>

/* Set a headset's own volume, 0..127 - AVRCP absolute volume, the headset
 * applying its own curve - through bluealsa's A2DP control for that one
 * device. `sink` is the PCM name launch.sh publishes, bt_AA_BB_CC_DD_EE_FF
 * (bt_pcm_name in bt-alsa.sh), from which the address is read back. False
 * when there is no such control: not connected, or not a headset name.
 *
 * Or a USB DAC's, when `sink` is the plughw:CARD=<id> string aout_usb_card
 * makes: 0..127 onto the range of the card's own volume control. True for a
 * DAC that has none, since there is nothing to retry.
 *
 * One mixer open per call, so it is for changes, not for every frame. */
bool btvol_set(const char *sink, int level);

#endif
