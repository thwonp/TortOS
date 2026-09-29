/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef TORTOS_ARTSHRINK_H
#define TORTOS_ARTSHRINK_H

/* Shrink a fetched cover in place to the largest size it is ever drawn at.
 *
 * Queues it and returns at once; a thread of its own does the work. Silent and
 * best-effort: anything that fails leaves the file exactly as it arrived,
 * because art that is too large still works and a half-written PNG does not.
 * Never upscales. */
void art_shrink(const char *path);

/* Finish the cover in hand, drop any still queued, and stop the thread.
 *
 * Before IMG_Quit, because the thread is in IMG_Load and IMG_SavePNG. A no-op if
 * no cover was ever fetched this session, since the thread starts on the first. */
void art_shrink_stop(void);

#endif
