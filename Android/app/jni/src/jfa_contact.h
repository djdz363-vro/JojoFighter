/*
 * jfa_contact.h - measured strike-contact windows for the JFA attack clips.
 *
 * Phase B: the Android strike gate (jfa_attack_in_strike_window in main.c)
 * calls jfa_contact_frame_hits(), so an attack can strike only on the frames
 * its striking limb is actually extended. The old fraction-based window
 * JFA_HIT_WINDOW_START..JFA_HIT_WINDOW_END is kept there purely as the
 * fallback for clip data this module cannot describe (unmeasured or re-baked
 * assets).
 *
 * The windows are transcribed from docs/phase0-contact-measurements.txt
 * (produced by tools/jfa_contact_analysis.c, which skins every frame with the
 * real jfa_skin.c code path). A window is the contiguous span of frames whose
 * forward extension stays within 90% of the measured peak: the "limb is out"
 * interval, which is a superset of the peak frame itself.
 *
 * Every window remembers the clip data it was measured on (frame count and
 * fps). JFA1 assets carry their own frame count and rate, so if a clip is ever
 * re-baked with a different length or sampling rate the stored frame numbers
 * no longer describe it: every query here refuses that window instead of
 * applying stale timings to different animation data.
 *
 * Time convention: a frame index f is on screen during [f/fps, (f+1)/fps), the
 * same integer stepping jfa_skin_frame_index() uses for rendering. A window's
 * time span is therefore [firstFrame/fps, (lastFrame+1)/fps) - inclusive of
 * the whole last frame. (The Phase 0 log prints frame *start* times for both
 * ends, e.g. kick "1.067..1.200 s"; the same window covers up to 1.233 s of
 * displayed time.)
 */

#ifndef JFA_CONTACT_H
#define JFA_CONTACT_H

#include <stdint.h>

#include "jfa_loader.h"

/* One measured contact window of one clip. */
typedef struct {
	int clip;        /* JFA_* clip index the window belongs to */
	int firstFrame;  /* first contact frame, inclusive */
	int lastFrame;   /* last contact frame, inclusive */
	int frames;      /* clip frame count the window was measured on */
	uint32_t fps;    /* clip fps the window was measured on */
} JFAContactWindow;

/*
 * Number of measured contact windows for `clip`, 0 when the clip has none
 * (idle, locomotion, hit, knockdown, getup, ...) or the index is invalid.
 */
int jfa_contact_count(int clip);

/*
 * The `index`-th measured window for `clip`, in increasing frame order, or
 * NULL when `clip` has no measured data or `index` is out of range.
 * punch_right has two (a right-hand strike, then a second extension).
 */
const JFAContactWindow *jfa_contact_window(int clip, int index);

/*
 * 1 when `w` still describes `clip`'s data: same frame count, same fps, frame
 * numbers ordered and inside the clip. 0 for NULL pointers, invalid data, or a
 * clip that was re-baked with a different length/rate.
 */
int jfa_contact_window_matches(const JFAContactWindow *w, const JFAClip *clip);

/*
 * Convert a window to displayed seconds. Writes [firstFrame/fps,
 * (lastFrame+1)/fps) through start/end (either may be NULL) and returns 1, or
 * returns 0 and writes nothing when the window does not match `clip`.
 */
int jfa_contact_span_seconds(const JFAContactWindow *w, const JFAClip *clip,
			     double *start, double *end);

/* 1 when `frame` is inside the window's frame range. NULL window -> 0. */
int jfa_contact_frame_in_window(const JFAContactWindow *w, int frame);

/*
 * The query a later phase will use in place of the fraction-based gate: 1 when
 * the frame of `clips[clipIndex]` displayed at `elapsed` seconds into the clip
 * falls inside one of that clip's measured windows.
 *
 * Returns 0 for an invalid clip index, NULL clip array, unusable clip data,
 * a negative or past-the-end elapsed time, a clip with no measured windows,
 * and a clip whose frame count/fps no longer match the measurement (so the
 * caller can fall back to the existing window instead of trusting it).
 *
 * This does not read, modify or depend on any attack state; it is a pure
 * function of the clip data and an elapsed time.
 */
int jfa_contact_frame_hits(int clipIndex, const JFAClip *clips, double elapsed);

#endif /* JFA_CONTACT_H */
