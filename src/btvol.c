/* SPDX-License-Identifier: MIT */
/* See btvol.h. alsa-lib's simple mixer against bluealsa's control plugin (or a
 * USB DAC's own card),
 * reached with dlopen like Muse reaches ALSA: the launcher links no libasound,
 * and forking amixer out of this process is what menu_wifi exists to avoid. */
#include "btvol.h"

/* bluealsa on the TrimUI and the H700. The GKD takes the host stubs until
 * gkd.9. */
#if defined(__linux__) && !defined(PLATFORM_GKD)
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int  (*m_open)(void **, int);
static int  (*m_attach)(void *, const char *);
static int  (*m_register)(void *, void *, void *);
static int  (*m_load)(void *);
static int  (*m_close)(void *);
static void *(*m_first)(void *);
static void *(*m_next)(void *);
static const char *(*m_name)(void *);
static int  (*m_has_volume)(void *);
static int  (*m_set_all)(void *, long);
static int  (*m_range)(void *, long *, long *);
static int  (*m_has_switch)(void *);
static int  (*m_switch_all)(void *, int);

static bool bind(void)
{
	static int state;          /* 0 not tried, 1 bound, -1 failed */
	void *lib;

	if (state) return state > 0;
	state = -1;
	if (!(lib = dlopen("libasound.so.2", RTLD_NOW))) return false;
#define SYM(v, n) if (!(*(void **)&v = dlsym(lib, n))) return false
	SYM(m_open,       "snd_mixer_open");
	SYM(m_attach,     "snd_mixer_attach");
	SYM(m_register,   "snd_mixer_selem_register");
	SYM(m_load,       "snd_mixer_load");
	SYM(m_close,      "snd_mixer_close");
	SYM(m_first,      "snd_mixer_first_elem");
	SYM(m_next,       "snd_mixer_elem_next");
	SYM(m_name,       "snd_mixer_selem_get_name");
	SYM(m_has_volume, "snd_mixer_selem_has_playback_volume");
	SYM(m_set_all,    "snd_mixer_selem_set_playback_volume_all");
	SYM(m_range,      "snd_mixer_selem_get_playback_volume_range");
	SYM(m_has_switch, "snd_mixer_selem_has_playback_switch");
	SYM(m_switch_all, "snd_mixer_selem_set_playback_switch_all");
#undef SYM
	state = 1;
	return true;
}

/* A USB DAC's own volume: the first control on its card that has one, by
 * whatever name the DAC gives it (the KA13's is PCM; others say Master, or
 * Speaker, or the product name), mapped straight onto that control's range.
 * Raw steps are usually decibels on a DAC, so straight is already a curve.
 *
 * A DAC with no volume control at all has nothing to set: true, logged once,
 * so the caller does not retry a mixer open every two seconds forever. It
 * plays at full scale - the one case that needs a software gain, if one turns
 * up. Level 0 also turns the switch off where there is one, because the
 * bottom of a range is often only very quiet. */
static bool usbvol_set(const char *sink, int level)
{
	static char warned[48];
	char card[48];
	const char *id = sink + 12, *end = strchr(id, ',');
	void *mixer = NULL, *e;
	bool done = false, found = false;

	snprintf(card, sizeof card, "hw:CARD=%.*s", end ? (int)(end - id) : (int)strlen(id), id);
	if (!bind() || m_open(&mixer, 0) < 0) return false;
	if (m_attach(mixer, card) < 0 || m_register(mixer, NULL, NULL) < 0 ||
	    m_load(mixer) < 0) {
		m_close(mixer);
		return false;
	}
	for (e = m_first(mixer); e; e = m_next(e)) {
		long lo = 0, hi = 0;

		if (!m_has_volume(e) || m_range(e, &lo, &hi) < 0 || hi <= lo) continue;
		found = true;
		done = m_set_all(e, lo + ((hi - lo) * level + 63) / 127) >= 0;
		if (m_has_switch(e)) m_switch_all(e, level > 0);
		break;
	}
	m_close(mixer);
	if (!found) {
		if (strcmp(warned, card)) {
			fprintf(stderr, "audio: %s has no volume control; it plays at full scale\n", card);
			snprintf(warned, sizeof warned, "%s", card);
		}
		return true;
	}
	return done;
}

bool btvol_set(const char *sink, int level)
{
	char card[48];
	void *mixer = NULL, *e;
	bool done = false;
	int i;

	if (level < 0) level = 0;
	if (level > 127) level = 127;
	if (sink && !strncmp(sink, "plughw:CARD=", 12)) return usbvol_set(sink, level);
	/* bt_C0_86_B3_A7_48_7F back to C0:86:B3:A7:48:7F. */
	if (!sink || strncmp(sink, "bt_", 3) || strlen(sink) != 3 + 17) return false;
	snprintf(card, sizeof card, "bluealsa:DEV=%s", sink + 3);
	for (i = 13 + 2; card[i]; i += 3) card[i] = ':';
	if (!bind() || m_open(&mixer, 0) < 0) return false;
	/* One device's controls, by its address. The whole `bluealsa` card lists
	 * every connected headset's, and with two connected the first one's was
	 * set - not necessarily the one being heard. Scoped, the control is named
	 * plainly `A2DP`, and a device that is not connected is refused. */
	if (m_attach(mixer, card) < 0 || m_register(mixer, NULL, NULL) < 0 ||
	    m_load(mixer) < 0) {
		m_close(mixer);
		return false;
	}
	for (e = m_first(mixer); e; e = m_next(e)) {
		const char *name = m_name(e);

		if (!name || strcmp(name, "A2DP") || !m_has_volume(e)) continue;
		done = m_set_all(e, level) >= 0;
		break;
	}
	m_close(mixer);
	return done;
}

#else   /* not __linux__ */

bool btvol_set(const char *sink, int level) { (void)sink; (void)level; return false; }

#endif
