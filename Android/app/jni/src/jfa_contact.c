/*
 * jfa_contact.c - measured strike-contact windows (see jfa_contact.h).
 *
 * Data + pure queries only. The strike gate calls jfa_contact_frame_hits();
 * nothing here touches attack, health, input or rendering state, so this
 * module cannot change behaviour by itself. It depends only on the clip data
 * (jfa_loader.h) and the renderer's frame stepping (jfa_skin.h); no SDL,
 * OpenGL or asset access.
 */

#include "jfa_contact.h"
#include "jfa_skin.h"

/*
 * Transcribed from docs/phase0-contact-measurements.txt:
 *
 *   punch_left   53 frames @ 30 fps, ext peak f14 (1.4690), 90% span f12..f17
 *   punch_right  91 frames @ 30 fps, peaks f30 (0.8401) and f44 (0.8114),
 *                90% spans f29..f31 and f42..f46
 *   kick         70 frames @ 30 fps, left-foot peak f35 (0.9791),
 *                90% span f32..f36
 *
 * Order within a clip is increasing frame order. Kept in one table so a
 * typo is caught by tools/jfa_contact_test.c, which re-asserts these exact
 * values against the Phase 0 log.
 */
static const JFAContactWindow contact_windows[] = {
	/* clip,          first, last, frames, fps */
	{ JFA_PUNCH_LEFT,    12,   17,     53,  30 },
	{ JFA_PUNCH_RIGHT,   29,   31,     91,  30 },
	{ JFA_PUNCH_RIGHT,   42,   46,     91,  30 },
	{ JFA_KICK,          32,   36,     70,  30 },
};

#define CONTACT_WINDOW_COUNT \
	((int)(sizeof(contact_windows) / sizeof(contact_windows[0])))

int jfa_contact_count(int clip)
{
	int i, n = 0;

	if (clip < 0 || clip >= JFA_CLIP_COUNT)
		return 0;
	for (i = 0; i < CONTACT_WINDOW_COUNT; i++)
		if (contact_windows[i].clip == clip)
			n++;
	return n;
}

const JFAContactWindow *jfa_contact_window(int clip, int index)
{
	int i, n = 0;

	if (clip < 0 || clip >= JFA_CLIP_COUNT || index < 0)
		return NULL;
	for (i = 0; i < CONTACT_WINDOW_COUNT; i++) {
		if (contact_windows[i].clip != clip)
			continue;
		if (n == index)
			return &contact_windows[i];
		n++;
	}
	return NULL;
}

int jfa_contact_window_matches(const JFAContactWindow *w, const JFAClip *clip)
{
	if (!w || !clip)
		return 0;
	if (w->frames <= 0 || w->fps == 0)
		return 0;
	/* Clip data must be the data the window was measured on. */
	if (clip->frames != w->frames || clip->fps != w->fps)
		return 0;
	if (w->clip < 0 || w->clip >= JFA_CLIP_COUNT)
		return 0;
	if (w->firstFrame < 0 || w->lastFrame < w->firstFrame)
		return 0;
	if (w->lastFrame >= clip->frames)
		return 0;
	return 1;
}

int jfa_contact_span_seconds(const JFAContactWindow *w, const JFAClip *clip,
			     double *start, double *end)
{
	double s, e;

	if (!jfa_contact_window_matches(w, clip))
		return 0;
	s = (double)w->firstFrame / (double)clip->fps;
	e = (double)(w->lastFrame + 1) / (double)clip->fps;
	if (start)
		*start = s;
	if (end)
		*end = e;
	return 1;
}

int jfa_contact_frame_in_window(const JFAContactWindow *w, int frame)
{
	if (!w)
		return 0;
	return frame >= w->firstFrame && frame <= w->lastFrame;
}

int jfa_contact_frame_hits(int clipIndex, const JFAClip *clips, double elapsed)
{
	const JFAClip *clip;
	int frame, i, n;

	if (!clips || clipIndex < 0 || clipIndex >= JFA_CLIP_COUNT)
		return 0;
	clip = &clips[clipIndex];
	if (clip->frames <= 0 || clip->fps == 0)
		return 0;
	/* Reject NaN, backwards time and the end of the clip (the renderer
	 * clamps those to a frame, which would otherwise read as a hit). */
	if (!(elapsed >= 0.0))
		return 0;
	if (elapsed >= (double)clip->frames / (double)clip->fps)
		return 0;
	/* Same integer frame stepping the renderer uses. */
	frame = jfa_skin_frame_index(clip, elapsed);
	n = jfa_contact_count(clipIndex);
	for (i = 0; i < n; i++) {
		const JFAContactWindow *w = jfa_contact_window(clipIndex, i);

		if (jfa_contact_window_matches(w, clip) &&
		    jfa_contact_frame_in_window(w, frame))
			return 1;
	}
	return 0;
}
