/*
 * jfa_skin.c - CPU skeletal skinning for the JFA1 runtime (Android).
 *
 * See jfa_skin.h. The per-bone matrix folding mirrors the PSP
 * "FAST SKINNING" block in the PSP main.c exactly (same order of
 * operations): scale, bind-pose centering, yaw rotation about Y, and
 * world translation, folded into nb+1 3x4 matrices ("fin[]"). The
 * extra last entry is the identity fallback used for vertices with no
 * bone influence.
 */

#include "jfa_skin.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void jfa_skin_setup(const JFACharacter *c, float target_height,
		    float *out_scale, float out_center[3])
{
	float extent = c->maxv[0] - c->minv[0];
	float ey = c->maxv[1] - c->minv[1];
	float ez = c->maxv[2] - c->minv[2];

	if (ey > extent)
		extent = ey;
	if (ez > extent)
		extent = ez;

	*out_scale = extent > 1e-4f ? target_height / extent : 1.0f;
	out_center[0] = (c->minv[0] + c->maxv[0]) * 0.5f;
	out_center[1] = c->minv[1];
	out_center[2] = (c->minv[2] + c->maxv[2]) * 0.5f;
}

int jfa_skin_alloc(JFASkinned *s, size_t nv)
{
	memset(s, 0, sizeof(*s));
	if (nv < 1)
		return 0;
	s->v = calloc(nv, sizeof(JFASkinVertex));
	if (!s->v)
		return 0;
	s->nv = nv;
	return 1;
}

void jfa_skin_free(JFASkinned *s)
{
	if (!s)
		return;
	free(s->v);
	memset(s, 0, sizeof(*s));
}

int jfa_skin_frame_index(const JFAClip *clip, double t)
{
	int af;

	if (clip->frames <= 0)
		return 0;
	af = (int)(t * (double)clip->fps);
	if (af < 0)
		af = 0;
	if (af >= clip->frames)
		af = clip->frames - 1;
	return af;
}

void jfa_skin_frame(const JFACharacter *c, int clip_index, int frame,
		    float scale, const float center[3], const float world[3],
		    JFASkinned *out)
{
	/* fin[b] = final 3x4 matrix for bone b; fin[nb] = identity fallback
	 * (static: sized once, reused every frame, no per-frame allocation). */
	static float fin[JFA_MAX_BONES + 1][12];
	static const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };

	const JFAClip *clip = &c->clips[clip_index];
	const float *pose;
	const int nb = c->nb;
	const float cxm = center[0], cym = center[1], czm = center[2];
	const float wx = world[0], wy = world[1], wz = world[2];

	if (!c->ready || !out->v || nb < 1 || clip->frames <= 0)
		return;
	if (frame < 0 || frame >= clip->frames)
		frame = 0;

	/* All bones of one sampled frame, each 12 floats (3x4, row-major). */
	pose = clip->mat + (size_t)frame * (size_t)nb * 12;

	for (int k = 0; k < nb; k++) {
		const float *m = pose + (size_t)k * 12;
		float *F = fin[k];
		/* Fold scale + center + world translation (yaw = 0: the
		 * renderer rotates via its model matrix). Same math as the
		 * PSP folding with C = 1, S = 0. */
		F[0] = scale * m[0];
		F[1] = scale * m[1];
		F[2] = scale * m[2];
		F[3] = scale * (m[3] - cxm) + wx;
		F[4] = scale * m[4];
		F[5] = scale * m[5];
		F[6] = scale * m[6];
		F[7] = scale * (m[7] - cym) + wy;
		F[8] = scale * m[8];
		F[9] = scale * m[9];
		F[10] = scale * m[10];
		F[11] = scale * (m[11] - czm) + wz;
	}
	memcpy(fin[nb], ident, sizeof(ident));

	for (size_t i = 0; i < out->nv; i++) {
		const JFAVertex *sv = &c->v[i];
		JFASkinVertex *o = &out->v[i];
		const float p0 = sv->p[0], p1 = sv->p[1], p2 = sv->p[2];
		const float n0 = sv->n[0], n1 = sv->n[1], n2 = sv->n[2];
		float ax = 0.0f, ay = 0.0f, az = 0.0f;
		float an0 = 0.0f, an1 = 0.0f, an2 = 0.0f;
		float sum = 0.0f;

		for (int k = 0; k < 4; k++) {
			float w = sv->bw[k];
			const float *F;
			if (w <= 0.00001f)
				continue;
			F = fin[sv->bi[k]];
			ax += w * (F[0] * p0 + F[1] * p1 + F[2] * p2 + F[3]);
			ay += w * (F[4] * p0 + F[5] * p1 + F[6] * p2 + F[7]);
			az += w * (F[8] * p0 + F[9] * p1 + F[10] * p2 + F[11]);
			/* rotate the bind-pose normal by the same bone
			 * (rotation part only, no translation) */
			an0 += w * (F[0] * n0 + F[1] * n1 + F[2] * n2);
			an1 += w * (F[4] * n0 + F[5] * n1 + F[6] * n2);
			an2 += w * (F[8] * n0 + F[9] * n1 + F[10] * n2);
			sum += w;
		}
		if (sum < 0.0001f) {
			/* no bone influence: bind pose (PSP behavior) */
			const float *F = fin[nb];
			ax = F[0] * p0 + F[1] * p1 + F[2] * p2 + F[3];
			ay = F[4] * p0 + F[5] * p1 + F[6] * p2 + F[7];
			az = F[8] * p0 + F[9] * p1 + F[10] * p2 + F[11];
			an0 = F[0] * n0 + F[1] * n1 + F[2] * n2;
			an1 = F[4] * n0 + F[5] * n1 + F[6] * n2;
			an2 = F[8] * n0 + F[9] * n1 + F[10] * n2;
		}

		{
			float nl = sqrtf(an0 * an0 + an1 * an1 + an2 * an2);
			if (nl > 1e-6f) {
				an0 /= nl;
				an1 /= nl;
				an2 /= nl;
			} else {
				an0 = 0.0f;
				an1 = 1.0f;
				an2 = 0.0f;
			}
		}

		o->position[0] = ax;
		o->position[1] = ay;
		o->position[2] = az;
		o->normal[0] = an0;
		o->normal[1] = an1;
		o->normal[2] = an2;
	}
}
