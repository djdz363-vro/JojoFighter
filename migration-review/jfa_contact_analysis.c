/*
 * jfa_contact_analysis.c - host-side measurement of striking-limb
 * extension in the JFA1 attack clips (Phase 0, read-only).
 *
 * Purpose
 *   Gameplay derives each strike window from a fixed fraction of the clip
 *   (30%..70% of clip seconds). Before any retiming, this tool measures
 *   where the striking hand/foot actually reaches forward, frame by frame,
 *   from the real JFA matrices and the real CPU skinning code
 *   (Android/app/jni/src/jfa_skin.c).
 *
 * How it measures
 *   - Parses the JFA1 files directly (no SDL, no Android) into the same
 *     JFACharacter structure the runtime uses.
 *   - Groups mesh vertices by the bone that dominates their weights, and
 *     takes each limb as the whole subtree under a root bone (a fist
 *     includes the index chain; a kicking surface includes the toes).
 *   - Skins every sampled frame with jfa_skin_frame() and records the
 *     forward-most position of each limb group.
 *   - Forward is derived from the bind pose (hips centroid -> toe
 *     centroids), so neither the axis order nor the facing direction is
 *     assumed.
 *   - For every clip all four limbs are measured; the striking limb is
 *     chosen by measurement (largest extension range), not by name.
 *
 * Contact frames
 *   A contact frame is a local maximum of forward extension with enough
 *   prominence to be a real strike rather than idle sway. The tool also
 *   reports the contiguous span where extension stays above 90% of each
 *   peak (the plausible "limb is out" interval), the peak approach speed
 *   and the exact peak-to-peak spacing.
 *
 * Read-only: the tool only reads the .jfa files and prints results.
 *
 * Build (from the repo root):
 *   cc -std=gnu99 -O2 -I Android/app/jni/src \
 *      tools/jfa_contact_analysis.c Android/app/jni/src/jfa_skin.c \
 *      -lm -o build/jfa_contact_analysis
 *
 * Usage:
 *   build/jfa_contact_analysis [--bones] [--all] [clip ...]
 *     --bones  also print the bone table
 *     --all    print the full per-frame table for every limb
 *   with no clip arguments: punch_left, punch_right, kick.
 */

#include "../Android/app/jni/src/jfa_loader.h"
#include "../Android/app/jni/src/jfa_skin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSETS "Android/app/src/main/assets/animations"

/* Model-space forward unit vector, derived from the bind pose in main(). */
static float g_fwd[3];

/* ------------------------------------------------------------------ */
/* Minimal JFA1 reader (same layout the loader parses; no SDL)         */
/* ------------------------------------------------------------------ */

static unsigned char *slurp(const char *path, size_t *outlen)
{
	FILE *f = fopen(path, "rb");
	unsigned char *d;
	long sz;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	d = malloc((size_t)sz);
	if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f);
		free(d);
		return NULL;
	}
	fclose(f);
	*outlen = (size_t)sz;
	return d;
}

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float lef(const unsigned char *p)
{
	uint32_t v = le32(p);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

typedef struct {
	uint32_t nb, nv, ni, nf, fps;
	int32_t parent[JFA_MAX_BONES];
	char name[JFA_MAX_BONES][JFA_MAX_NAME];
	JFAVertex *v;
	uint32_t *indices;
	float *mat; /* nf * nb * 12 */
} Raw;

static void raw_free(Raw *r)
{
	free(r->v);
	free(r->indices);
	free(r->mat);
	memset(r, 0, sizeof(*r));
}

static int raw_load(const char *path, Raw *r)
{
	unsigned char *d;
	size_t size, pos, expect;
	uint32_t version;

	memset(r, 0, sizeof(*r));
	d = slurp(path, &size);
	if (!d) {
		fprintf(stderr, "cannot read %s\n", path);
		return 0;
	}
	if (size < 32 || memcmp(d, "JFA1", 4) != 0) {
		fprintf(stderr, "%s: not a JFA1 file\n", path);
		free(d);
		return 0;
	}
	version = le32(d + 4);
	if (version != 1) {
		fprintf(stderr, "%s: unsupported version %u\n", path,
			(unsigned)version);
		free(d);
		return 0;
	}
	r->nb = le32(d + 8);
	r->nv = le32(d + 12);
	r->ni = le32(d + 16);
	r->nf = le32(d + 20);
	r->fps = le32(d + 24);
	if (r->nb < 1 || r->nb > JFA_MAX_BONES || r->nv < 1 ||
	    r->nv > JFA_MAX_VERTS || r->ni < 1 || r->ni > JFA_MAX_INDICES ||
	    r->nf < 1 || r->nf > JFA_MAX_FRAMES || r->fps < 1 ||
	    r->fps > 1000) {
		fprintf(stderr, "%s: counts out of range\n", path);
		free(d);
		return 0;
	}

	pos = 32;
	for (uint32_t b = 0; b < r->nb; b++) {
		uint32_t len;
		if (pos + 8 > size)
			goto trunc;
		r->parent[b] = (int32_t)le32(d + pos);
		len = le32(d + pos + 4);
		pos += 8;
		if (len < 1 || len >= JFA_MAX_NAME || pos + len > size)
			goto trunc;
		memcpy(r->name[b], d + pos, len);
		r->name[b][len] = 0;
		pos += len;
	}

	expect = (size_t)r->nv * 64;
	if (pos + expect > size)
		goto trunc;
	r->v = malloc(expect);
	memcpy(r->v, d + pos, expect);
	pos += expect;

	expect = (size_t)r->ni * 4;
	if (pos + expect > size)
		goto trunc;
	r->indices = malloc(expect);
	memcpy(r->indices, d + pos, expect);
	pos += expect;

	expect = (size_t)r->nf * r->nb * 12 * 4;
	if (pos + expect > size)
		goto trunc;
	r->mat = malloc((size_t)r->nf * r->nb * 12 * sizeof(float));
	{
		const unsigned char *p = d + pos;
		size_t n = (size_t)r->nf * r->nb;
		for (size_t m = 0; m < n; m++)
			for (int k = 0; k < 12; k++)
				r->mat[m * 12 + k] =
					lef(p + (m * 16 + k) * 4);
	}

	free(d);
	return 1;
trunc:
	fprintf(stderr, "%s: truncated\n", path);
	raw_free(r);
	free(d);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Fill a JFACharacter from one raw clip (shared skeleton/mesh)        */
/* ------------------------------------------------------------------ */

/* Local stand-in for jfa_free (the real one lives in jfa_loader.c, which
 * pulls in SDL; this tool only owns the structures it allocated). */
static void character_free(JFACharacter *c)
{
	for (int i = 0; i < JFA_CLIP_COUNT; i++)
		free(c->clips[i].mat);
	free(c->v);
	free(c->indices);
	memset(c, 0, sizeof(*c));
}

static int character_init(JFACharacter *c, const Raw *idle)
{
	memset(c, 0, sizeof(*c));
	c->nb = (int)idle->nb;
	c->nv = (int)idle->nv;
	c->ni = (int)idle->ni;
	for (uint32_t b = 0; b < idle->nb; b++) {
		c->parent[b] = idle->parent[b];
		memcpy(c->bone_name[b], idle->name[b], JFA_MAX_NAME);
	}
	c->v = malloc((size_t)idle->nv * sizeof(JFAVertex));
	c->indices = malloc((size_t)idle->ni * sizeof(uint32_t));
	if (!c->v || !c->indices) {
		character_free(c);
		return 0;
	}
	memcpy(c->v, idle->v, (size_t)idle->nv * sizeof(JFAVertex));
	memcpy(c->indices, idle->indices, (size_t)idle->ni * sizeof(uint32_t));
	for (int k = 0; k < 3; k++) {
		c->minv[k] = 1e30f;
		c->maxv[k] = -1e30f;
	}
	for (int i = 0; i < c->nv; i++)
		for (int k = 0; k < 3; k++) {
			float x = c->v[i].p[k];
			if (x < c->minv[k])
				c->minv[k] = x;
			if (x > c->maxv[k])
				c->maxv[k] = x;
		}
	c->ready = 1;
	return 1;
}

static int character_set_clip(JFACharacter *c, int slot, const Raw *r)
{
	JFAClip *cl = &c->clips[slot];
	size_t n = (size_t)r->nf * r->nb * 12;

	cl->frames = (int)r->nf;
	cl->fps = r->fps;
	cl->mat = malloc(n * sizeof(float));
	if (!cl->mat)
		return 0;
	memcpy(cl->mat, r->mat, n * sizeof(float));
	return 1;
}

/* ------------------------------------------------------------------ */
/* Limb classification                                                 */
/* ------------------------------------------------------------------ */

/* Dominant bone (largest weight) of a vertex. */
static int dominant_bone(const JFAVertex *v)
{
	int best = 0;
	float bw = -1.0f;
	for (int k = 0; k < 4; k++) {
		if (v->bw[k] > bw) {
			bw = v->bw[k];
			best = (int)v->bi[k];
		}
	}
	return best;
}

static int bone_by_name(const JFACharacter *c, const char *want)
{
	for (int b = 0; b < c->nb; b++)
		if (strcmp(c->bone_name[b], want) == 0)
			return b;
	return -1;
}

/* Mark every bone in the subtree rooted at `root` (parents precede
 * children in the JFA bone table, so one forward pass is enough). */
static void subtree_mark(const JFACharacter *c, int root, char *mark)
{
	for (int b = 0; b < c->nb; b++)
		mark[b] = (b == root);
	for (int b = root + 1; b < c->nb; b++)
		mark[b] = mark[c->parent[b]];
}

/* Bind-pose centroid (used for the forward-axis derivation). */
static int group_centroid(const JFACharacter *c, int bone, float out[3],
			  int *count)
{
	int n = 0;
	double sx = 0, sy = 0, sz = 0;

	for (int i = 0; i < c->nv; i++) {
		if (dominant_bone(&c->v[i]) != bone)
			continue;
		sx += c->v[i].p[0];
		sy += c->v[i].p[1];
		sz += c->v[i].p[2];
		n++;
	}
	*count = n;
	if (!n)
		return 0;
	out[0] = (float)(sx / n);
	out[1] = (float)(sy / n);
	out[2] = (float)(sz / n);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Per-limb measurement                                                */
/* ------------------------------------------------------------------ */

typedef struct {
	int root;         /* root bone of the group */
	const char *name; /* display label */
	int nv;           /* vertices in the group */
	int *idx;         /* vertex indices */
	float ext[JFA_MAX_FRAMES];  /* forward-most position per frame */
	float top[JFA_MAX_FRAMES];  /* highest position per frame */
	float cx[JFA_MAX_FRAMES];   /* centroid X per frame */
	float cz[JFA_MAX_FRAMES];   /* centroid Z per frame */
} Limb;

/*
 * Build a limb group. subtree = 1 includes every bone below the root
 * (a fist owns the index chain, a foot owns the toes); subtree = 0
 * uses only the root bone's own vertices (the pelvis).
 */
static int limb_init(const JFACharacter *c, Limb *l, const char *root_name,
		     const char *label, int subtree)
{
	char mark[JFA_MAX_BONES];
	int root = bone_by_name(c, root_name);
	int n = 0;

	memset(l, 0, sizeof(*l));
	if (root < 0)
		return 0;
	if (subtree) {
		subtree_mark(c, root, mark);
	} else {
		for (int b = 0; b < c->nb; b++)
			mark[b] = (b == root);
	}
	for (int i = 0; i < c->nv; i++)
		if (mark[dominant_bone(&c->v[i])])
			n++;
	if (!n)
		return 0;
	l->root = root;
	l->name = label;
	l->nv = n;
	l->idx = malloc((size_t)n * sizeof(int));
	if (!l->idx)
		return 0;
	n = 0;
	for (int i = 0; i < c->nv; i++)
		if (mark[dominant_bone(&c->v[i])])
			l->idx[n++] = i;
	return 1;
}

static void limb_free(Limb *l)
{
	free(l->idx);
	memset(l, 0, sizeof(*l));
}

/* Skin every frame of `slot` once and fill l->ext. */
static void limb_measure(const JFACharacter *c, int slot, Limb *l,
			 JFASkinned *sk)
{
	const JFAClip *cl = &c->clips[slot];
	const float zero[3] = { 0, 0, 0 };

	for (int f = 0; f < cl->frames; f++) {
		float best = -1e30f, high = -1e30f;
		double sx = 0, sz = 0;
		jfa_skin_frame(c, slot, f, 1.0f, zero, zero, sk);
		for (int k = 0; k < l->nv; k++) {
			const float *p = sk->v[l->idx[k]].position;
			sx += p[0];
			sz += p[2];
			float e = p[0] * g_fwd[0] + p[1] * g_fwd[1] +
				  p[2] * g_fwd[2];
			if (e > best)
				best = e;
			if (p[1] > high)
				high = p[1];
		}
		l->ext[f] = best;
		l->top[f] = high;
		l->cx[f] = (float)(sx / l->nv);
		l->cz[f] = (float)(sz / l->nv);
	}
}

typedef struct {
	int frame;
	float ext;
	float prom;        /* prominence over the surrounding valleys */
	int span_a, span_b; /* frames >= 90% of the peak */
} Peak;

static int cmp_peak(const void *a, const void *b)
{
	const Peak *x = a, *y = b;

	if (x->ext < y->ext)
		return 1;
	if (x->ext > y->ext)
		return -1;
	return x->frame - y->frame;
}

/*
 * Local maxima with a prominence above `min_prom`, then the contiguous
 * 90%-of-peak span around each one. Frame f is displayed during
 * [f/fps, (f+1)/fps); times printed are frame-start times.
 */
static int find_peaks(const float *ext, int frames, float min_prom,
		      Peak *out, int maxout)
{
	int n = 0;

	for (int f = 1; f < frames - 1; f++) {
		float lo_l = ext[f], lo_r = ext[f];
		float prom;
		int i;

		if (!(ext[f] >= ext[f - 1] && ext[f] > ext[f + 1]))
			continue;
		for (i = f - 1; i >= 0 && ext[i] < ext[f]; i--)
			if (ext[i] < lo_l)
				lo_l = ext[i];
		for (i = f + 1; i < frames && ext[i] < ext[f]; i++)
			if (ext[i] < lo_r)
				lo_r = ext[i];
		prom = ext[f] - (lo_l > lo_r ? lo_l : lo_r);
		if (prom < min_prom || n >= maxout)
			continue;
		out[n].frame = f;
		out[n].ext = ext[f];
		out[n].prom = prom;
		out[n].span_a = f;
		out[n].span_b = f;
		{
			float thr = 0.90f * ext[f];
			int a = f, b = f;
			while (a > 0 && ext[a - 1] >= thr)
				a--;
			while (b < frames - 1 && ext[b + 1] >= thr)
				b++;
			out[n].span_a = a;
			out[n].span_b = b;
		}
		n++;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	static const char *default_clips[] = { "punch_left", "punch_right",
					       "kick" };
	const char *clips[16];
	int nclips = 0;
	int show_bones = 0, show_all = 0;
	JFACharacter c;
	Raw idle;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bones") == 0)
			show_bones = 1;
		else if (strcmp(argv[i], "--all") == 0)
			show_all = 1;
		else if (nclips < 16)
			clips[nclips++] = argv[i];
		else {
			fprintf(stderr, "too many clips\n");
			return 2;
		}
	}
	if (!nclips)
		for (size_t i = 0; i < sizeof(default_clips) /
					   sizeof(default_clips[0]); i++)
			clips[nclips++] = default_clips[i];

	{
		char path[512];
		snprintf(path, sizeof(path), "%s/model0_idle.jfa", ASSETS);
		if (!raw_load(path, &idle))
			return 1;
	}
	if (!character_init(&c, &idle)) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	printf("== rig ==\n");
	printf("bones: %d   mesh vertices: %d   indices: %d\n", c.nb, c.nv,
	       c.ni);
	printf("bind-pose bbox: x[%.3f %.3f] y[%.3f %.3f] z[%.3f %.3f]\n",
	       c.minv[0], c.maxv[0], c.minv[1], c.maxv[1], c.minv[2],
	       c.maxv[2]);
	if (show_bones) {
		printf("\n-- bone table --\n");
		for (int b = 0; b < c.nb; b++)
			printf("  %2d parent=%2d %s\n", b, c.parent[b],
			       c.bone_name[b]);
	}

	/*
	 * Forward axis: in the bind pose the toes are in front of the hips.
	 * Use the dominant horizontal component of hips->toe and its sign,
	 * so neither the axis order nor the facing direction is assumed.
	 */
	{
		float hips[3], toeavg[3] = { 0, 0, 0 };
		int nh = 0, nt = 0, ntoe = 0;
		const char *toe_bones[] = { "mixamorig:LeftToe_End",
					    "mixamorig:RightToe_End",
					    "mixamorig:LeftToeBase",
					    "mixamorig:RightToeBase" };
		float d[3];
		int axis;

		if (!group_centroid(&c, bone_by_name(&c, "mixamorig:Hips"),
				    hips, &nh)) {
			fprintf(stderr, "no hips vertices\n");
			return 1;
		}
		for (size_t i = 0; i < sizeof(toe_bones) /
					   sizeof(toe_bones[0]); i++) {
			float p[3];
			int n = 0;
			int b = bone_by_name(&c, toe_bones[i]);
			if (b >= 0 && group_centroid(&c, b, p, &n) && n) {
				toeavg[0] += p[0] * n;
				toeavg[1] += p[1] * n;
				toeavg[2] += p[2] * n;
				nt += n;
				ntoe++;
			}
		}
		if (!ntoe) {
			fprintf(stderr, "no toe vertices\n");
			return 1;
		}
		toeavg[0] /= nt;
		toeavg[1] /= nt;
		toeavg[2] /= nt;
		d[0] = toeavg[0] - hips[0];
		d[1] = toeavg[1] - hips[1];
		d[2] = toeavg[2] - hips[2];
		axis = (fabsf(d[0]) > fabsf(d[2])) ? 0 : 2;
		printf("hips centroid  (bind): %.3f %.3f %.3f\n", hips[0],
		       hips[1], hips[2]);
		printf("toe  centroid  (bind): %.3f %.3f %.3f\n", toeavg[0],
		       toeavg[1], toeavg[2]);
		printf("forward axis: %c%c (hips->toe horizontal %.3f %.3f)\n",
		       d[axis] < 0 ? '-' : '+', axis == 0 ? 'X' : 'Z', d[0],
		       d[2]);
		g_fwd[0] = g_fwd[1] = g_fwd[2] = 0.0f;
		g_fwd[axis] = d[axis] < 0 ? -1.0f : 1.0f;
	}

	{
		static const struct {
			const char *clip;
			int slot;
		} map[] = { { "punch_left", JFA_PUNCH_LEFT },
			    { "punch_right", JFA_PUNCH_RIGHT },
			    { "kick", JFA_KICK },
			    { "idle", JFA_IDLE },
			    { "hit", JFA_HIT },
			    { "guard", JFA_GUARD },
			    { "walk", JFA_WALK },
			    { "backwalk", JFA_BACKWALK },
			    { "knockdown", JFA_KNOCKDOWN },
			    { "getup", JFA_GETUP } };
		static const char *limb_roots[] = { "mixamorig:LeftHand",
						    "mixamorig:RightHand",
						    "mixamorig:LeftFoot",
						    "mixamorig:RightFoot",
						    "mixamorig:Hips" };
		static const char *limb_names[] = { "left hand",
						    "right hand",
						    "left foot",
						    "right foot",
						    "pelvis" };

		for (int ci = 0; ci < nclips; ci++) {
			Raw r;
			JFACharacter one;
			JFASkinned sk;
			char path[512];
			int slot = -1;
			Limb limbs[5];
			int have[5] = { 0, 0, 0, 0, 0 };
			float range[5] = { 0, 0, 0, 0, 0 };
			int best = -1;

			/* Which limb type can be the striking limb: a punch
			 * strikes with a fist, a kick with a foot. Choosing
			 * among the two limbs of that type (by measured
			 * range) avoids mistaking the counter-swinging arms
			 * for the strike. */
			int type_lo = 0, type_hi = 2; /* hands by default */

			for (size_t k = 0; k < sizeof(map) / sizeof(map[0]);
			     k++)
				if (strcmp(map[k].clip, clips[ci]) == 0)
					slot = map[k].slot;
			if (strcmp(clips[ci], "kick") == 0) {
				type_lo = 2;
				type_hi = 4;
			}
			if (slot < 0) {
				fprintf(stderr, "unknown clip '%s'\n",
					clips[ci]);
				continue;
			}

			snprintf(path, sizeof(path), "%s/model0_%s.jfa",
				 ASSETS, clips[ci]);
			if (!raw_load(path, &r))
				continue;
			one = c;
			one.clips[slot].frames = 0;
			one.clips[slot].mat = NULL;
			if (!character_set_clip(&one, slot, &r)) {
				fprintf(stderr, "out of memory\n");
				return 1;
			}
			if (!jfa_skin_alloc(&sk, (size_t)one.nv)) {
				fprintf(stderr, "out of memory\n");
				return 1;
			}

			{
				int nf = one.clips[slot].frames;
				int fps = (int)one.clips[slot].fps;

				printf("\n== %s: %d frames @ %d fps = %.3f s ==\n",
				       clips[ci], nf, fps, (double)nf / fps);

				for (int k = 0; k < 5; k++) {
					float lo = 1e30f, hi = -1e30f;
					int fmax = 0;

					if (!limb_init(&one, &limbs[k],
						       limb_roots[k],
						       limb_names[k],
						       k < 4))
						continue;
					have[k] = 1;
					limb_measure(&one, slot, &limbs[k],
						     &sk);
					for (int f = 0; f < nf; f++) {
						float e = limbs[k].ext[f];
						if (e < lo)
							lo = e;
						if (e > hi) {
							hi = e;
							fmax = f;
						}
					}
					range[k] = hi - lo;
					printf("  %-10s %4d verts  range %.4f  (%.4f..%.4f)  max at f%-3d t=%.3f s\n",
					       limb_names[k], limbs[k].nv,
					       range[k], lo, hi, fmax,
					       (double)fmax / fps);
					if (k >= type_lo && k < type_hi &&
					    (best < 0 || range[k] > range[best]))
						best = k;
				}

				if (best < 0 || range[best] < 1e-4f) {
					printf("  NOTE: no limb moves; this clip has no measurable strike\n");
				} else {
					Limb *l = &limbs[best];
					float lo = 1e30f, hi = -1e30f;
					float maxstep = 0;
					int fstep = 0, fmax = 0;
					Peak peaks[8];
					int np;

					for (int f = 0; f < nf; f++) {
						float e = l->ext[f];
						if (e < lo)
							lo = e;
						if (e > hi) {
							hi = e;
							fmax = f;
						}
					}
					for (int f = 1; f < nf; f++) {
						float d =
							l->ext[f] -
							l->ext[f - 1];
						if (fabsf(d) > maxstep) {
							maxstep = fabsf(d);
							fstep = f;
						}
					}
					printf("\n  striking limb (largest range): %s\n",
					       l->name);
					printf("  global max at frame %d (t=%.3f s), extension %.4f\n",
					       fmax, (double)fmax / fps, hi);
					printf("  fastest approach: %.4f model units/frame at f%d (t=%.3f s)\n",
					       maxstep, fstep,
					       (double)fstep / fps);

					np = find_peaks(l->ext, nf,
							0.15f * (hi - lo), peaks,
							8);
					qsort(peaks, (size_t)np, sizeof(peaks[0]),
					      cmp_peak);
					printf("\nframe  t(s)    fwd-ext   delta    height   cent-x   cent-z\n");
					for (int f = 0; f < nf; f++) {
						float d = f == 0
								  ? 0.0f
								  : l->ext[f] -
									    l->ext[f - 1];
						printf("%5d  %5.3f  %8.4f  %+7.4f  %7.4f  %+7.4f  %+7.4f\n",
						       f, (double)f / fps,
						       l->ext[f], d, l->top[f],
						       l->cx[f], l->cz[f]);
					}
					printf("\ncandidate contact frames (local maxima, prominence >= 15%% of range %.4f):\n",
					       hi - lo);
					if (!np)
						printf("  none\n");
					for (int i = 0; i < np; i++)
						printf("  frame %3d t=%.3f s  ext=%.4f  prom=%.4f  >=90%% span f%d..f%d (%.3f..%.3f s, %.3f s)\n",
						       peaks[i].frame,
						       (double)peaks[i].frame /
							       fps,
						       peaks[i].ext,
						       peaks[i].prom,
						       peaks[i].span_a,
						       peaks[i].span_b,
						       (double)peaks[i].span_a /
							       fps,
						       (double)peaks[i].span_b /
							       fps,
						       (double)(peaks[i].span_b -
								peaks[i].span_a) /
							       fps);
					for (int i = 1; i < np; i++)
						printf("  spacing %d -> %d: %d frames = %.3f s\n",
						       peaks[i - 1].frame,
						       peaks[i].frame,
						       peaks[i].frame -
							       peaks[i - 1].frame,
						       (double)(peaks[i].frame -
								peaks[i - 1].frame) /
							       fps);

					if (show_all)
						for (int k = 0; k < 5; k++) {
							if (!have[k] || k == best)
								continue;
							printf("\n  -- %s --\n",
							       limbs[k].name);
							for (int f = 0; f < nf; f++)
								printf("%5d  %5.3f  %8.4f\n",
								       f,
								       (double)f / fps,
								       limbs[k].ext[f]);
						}
				}
			}

			for (int k = 0; k < 5; k++)
				if (have[k])
					limb_free(&limbs[k]);
			jfa_skin_free(&sk);
			free(one.clips[slot].mat);
			raw_free(&r);
		}
	}

	raw_free(&idle);
	character_free(&c);
	return 0;
}
