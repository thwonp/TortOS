/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_BTVOL_H
#define TORTOS_BTVOL_H
#include <stdbool.h>

/* Set a headset's own volume, 0..127 - AVRCP absolute volume, the headset
 * applying its own curve - through bluealsa's A2DP control for that one
 * device. `sink` is the PCM name launch.sh publishes, bt_AA_BB_CC_DD_EE_FF
 * (bt_pcm_name in bt-alsa.sh), from which the address is read back. False
 * when there is no such control: not connected, or not a headset name.
 *
 * One mixer open per call, so it is for changes, not for every frame. */
bool btvol_set(const char *sink, int level);

#endif
