/*
 * jfa_skin_test.c - desktop unit tests for the JFA CPU skinning module.
 *
 * Links the REAL Android modules (jfa_loader.c, jfa_skin.c) and checks:
 *
 *  1. Synthetic 2-bone character with hand-computed expected output
 *     (scale folding, center offset, world translation, weighted blend).
 *  2. Independent raw-file cross-check: re-parse model0_idle.jfa with
 *     its own stdio parser (16-float matrices) and compare posed
 *     vertices against jfa_skin output for several frames - this pins
 *     down the 12-vs-16 float matrix layout and frame-major indexing.
 *  3. Invariants over a full idle cycle: finite positions, plausible
 *     bounds, unit-length normals, animated (frame-to-frame) motion.
 *  4. UV fields untouched by skinning; frame index clamping.
 *
 * Exit code 0 = all tests passed.
 */

#include "../Android/app/jni/src/jfa_skin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures = 0;

static void check(int cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
	if (!cond)
		failures++;
}

static int check_alloc(int ok)
{
	if (!ok) {
		printf("  [FAIL] alloc\n");
		failures++;
	}
	return ok;
}

/* ------------------------------------------------------------------ */
/* 1. Synthetic character                                              */
/* ------------------------------------------------------------------ */

static void synthetic_tests(void)
{
	/*
	 * 2 bones: b0 root, b1 child of b0.
	 * Vertex A: weight fully on b0. Vertex B: 0.75 b0 + 0.25 b1.
	 * Clip frame 0: b0 = translate(10,0,0), b1 = translate(0,5,0).
	 * Character: target height 2.0 over bbox extent 4.0 -> scale 0.5;
	 * bind bbox: x[-2..2] y[-1..1] z[0..0] -> center = (0, -1, 0).
	 * world = (100, 0, 0).
	 *
	 * Expected A = 0.5 * (10,0,0) - 0.5*(0,-1,0)... per-axis:
	 *   F0 = 0.5*10 = 5 ; F[3] = 0.5*(10-0)+100 = 105
	 *   F[7] = 0.5*(0-(-1)) + 0 = 0.5
	 *   A = (105, 0.5, 0)
	 * B = 0.75*A + 0.25*(b1 result): b1: F[3]=0.5*(0-0)+100=100,
	 *   F[7]=0.5*(5+1)=3 -> (100,3,0)
	 *   B = (0.75*105+0.25*100, 0.75*0.5+0.25*3, 0) = (103.75, 1.125, 0)
	 */
	JFACharacter c;
	JFASkinned s;
	float scale, center[3], world[3] = { 100, 0, 0 };

	memset(&c, 0, sizeof(c));
	c.ready = 1;
	c.nb = 2;
	c.nv = 2;
	c.parent[0] = -1;
	c.parent[1] = 0;
	strcpy(c.bone_name[0], "root");
	strcpy(c.bone_name[1], "child");
	c.minv[0] = -2; c.maxv[0] = 2;
	c.minv[1] = -1; c.maxv[1] = 1;
	c.minv[2] = 0;  c.maxv[2] = 0;
	/* NOTE: c.v is malloc'd here and c.clips[0].mat points at static
	 * storage - do NOT jfa_free(&c); free by hand below instead. */
	c.v = calloc(2, sizeof(JFAVertex));
	if (!check_alloc(c.v != NULL))
		return;

	/* vertex A: full weight b0; vertex B: 0.75/0.25 */
	for (int i = 0; i < 2; i++) {
		c.v[i].bi[0] = i == 0 ? 0 : 0;
		c.v[i].bi[1] = i == 0 ? 0 : 1;
		if (i == 0) {
			c.v[i].bw[0] = 1.0f;
		} else {
			c.v[i].bw[0] = 0.75f;
			c.v[i].bw[1] = 0.25f;
		}
	}

	static float mats[2 * 2 * 12]; /* 2 frames, 2 bones */
	/* frame 0 and frame 1 identical */
	for (int f = 0; f < 2; f++) {
		float *b0 = mats + ((size_t)f * 2 + 0) * 12;
		float *b1 = mats + ((size_t)f * 2 + 1) * 12;
		memset(b0, 0, 12 * sizeof(float));
		memset(b1, 0, 12 * sizeof(float));
		b0[0] = b0[5] = b0[10] = 1.0f; b0[3] = 10.0f;
		b1[0] = b1[5] = b1[10] = 1.0f; b1[7] = 5.0f;
	}
	c.clips[0].frames = 2;
	c.clips[0].fps = 2;
	c.clips[0].mat = mats;

	printf("== synthetic: fin folding, weights, world offset ==\n");
	jfa_skin_setup(&c, 2.0f, &scale, center);
	check(fabsf(scale - 0.5f) < 1e-6f, "scale = 0.5");
	check(center[0] == 0.0f && center[1] == -1.0f && center[2] == 0.0f,
	      "center = (0, minY, 0)");

	if (!check_alloc(jfa_skin_alloc(&s, 2)))
		return;

	jfa_skin_frame(&c, 0, 0, scale, center, world, &s);
	check(fabsf(s.v[0].position[0] - 105.0f) < 1e-5f &&
	      fabsf(s.v[0].position[1] - 0.5f) < 1e-5f &&
	      fabsf(s.v[0].position[2] - 0.0f) < 1e-5f,
	      "vertex A = (105, 0.5, 0)");
	check(fabsf(s.v[1].position[0] - 103.75f) < 1e-4f &&
	      fabsf(s.v[1].position[1] - 1.125f) < 1e-4f,
	      "vertex B = weighted blend (103.75, 1.125, 0)");

	jfa_skin_free(&s);
	free(c.v); /* manual: mats is static, jfa_free must not be used here */
}

/* ------------------------------------------------------------------ */
/* 2+3+4. Real-data tests                                              */
/* ------------------------------------------------------------------ */

#define ASSETS "Android/app/src/main/assets/animations"

/* Independent mini-parser: reads the RAW idle file (16-float matrices). */
typedef struct {
	uint32_t nb, nv, ni, nf, fps;
	unsigned char *d;
	size_t size;
} RawJFA;

static int raw_open(RawJFA *r, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	r->d = malloc((size_t)sz);
	r->size = (size_t)sz;
	if (!r->d || fread(r->d, 1, r->size, f) != r->size) {
		fclose(f);
		free(r->d);
		return 0;
	}
	fclose(f);
	if (memcmp(r->d, "JFA1", 4) != 0)
		return 0;
	r->nb = r->d[8] | (r->d[9] << 8) | ((uint32_t)r->d[10] << 16) |
		((uint32_t)r->d[11] << 24);
	r->nv = r->d[12] | (r->d[13] << 8) | ((uint32_t)r->d[14] << 16) |
		((uint32_t)r->d[15] << 24);
	r->ni = r->d[16] | (r->d[17] << 8) | ((uint32_t)r->d[18] << 16) |
		((uint32_t)r->d[19] << 24);
	r->nf = r->d[20] | (r->d[21] << 8) | ((uint32_t)r->d[22] << 16) |
		((uint32_t)r->d[23] << 24);
	return 1;
}

static void raw_pose_vertex(const RawJFA *r, int frame, uint32_t vi,
			    float out[3])
{
	size_t vertoff = 32 + 927; /* bone table 927 bytes for the idle file */
	size_t matoff = vertoff + (size_t)r->nv * 64 + (size_t)r->ni * 4;
	const unsigned char *v = r->d + vertoff + (size_t)vi * 64;
	float p[3], bw[4];
	uint32_t bi[4];

	memcpy(p, v, 12);
	memcpy(bi, v + 32, 16);
	memcpy(bw, v + 48, 16);

	out[0] = out[1] = out[2] = 0;
	for (int k = 0; k < 4; k++) {
		if (bw[k] <= 1e-9f)
			continue;
		const float *m = (const float *)(r->d + matoff +
		    ((size_t)frame * r->nb + bi[k]) * 64);
		out[0] += bw[k] * (m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]);
		out[1] += bw[k] * (m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7]);
		out[2] += bw[k] * (m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]);
	}
}

static void real_tests(void)
{
	JFACharacter c;
	JFASkinned s;
	RawJFA raw;
	char err[512];
	float scale, center[3];
	float target = 2.2f;
	const float world[3] = { 0, 0, 0 };

	printf("== real data: idle skinning vs independent raw-file reference ==\n");
	if (!jfa_load_all(&c, ASSETS, err, sizeof(err))) {
		printf("  [FAIL] loader: %s\n", err);
		failures++;
		return;
	}
	if (!raw_open(&raw, ASSETS "/model0_idle.jfa")) {
		printf("  [FAIL] raw parse\n");
		failures++;
		return;
	}
	jfa_skin_setup(&c, target, &scale, center);
	if (!check_alloc(jfa_skin_alloc(&s, (size_t)c.nv))) {
		jfa_free(&c);
		return;
	}

	/* UVs untouched by skinning (sentinel check) */
	for (size_t i = 0; i < s.nv; i++) {
		s.v[i].uv[0] = 42.0f;
		s.v[i].uv[1] = -42.0f;
	}

	/* cross-check frames 0, 75, 150 on a spread of vertices */
	int cross_ok = 1;
	const int frames[] = { 0, 75, 150 };
	const size_t verts[] = { 0, 1, 4476, 8951 };
	for (int fi = 0; fi < 3 && cross_ok; fi++) {
		jfa_skin_frame(&c, JFA_IDLE, frames[fi], scale, center, world,
			       &s);
		for (size_t g = 0; g < sizeof(verts) / sizeof(verts[0]); g++) {
			float ref[3];
			raw_pose_vertex(&raw, frames[fi], verts[g], ref);
			/* loader path: scale*(m.t - center) + world */
			for (int k = 0; k < 3; k++) {
				float expect = scale *
				    (ref[k] - center[k]) + world[k];
				float got = s.v[verts[g]].position[k];
				if (fabsf(got - expect) > 1e-4f) {
					printf("  cross-check mismatch: frame %d vert %zu axis %d: %g vs %g\n",
					       frames[fi], verts[g], k,
					       got, expect);
					cross_ok = 0;
					break;
				}
			}
			if (!cross_ok)
				break;
		}
	}
	check(cross_ok, "posed positions match independent raw-file math");

	/* invariants across the full idle cycle */
	int finite = 1, unit = 1;
	float ymax = -1e30f, ymin = 1e30f;
	for (int f = 0; f < c.clips[JFA_IDLE].frames && finite; f++) {
		jfa_skin_frame(&c, JFA_IDLE, f, scale, center, world, &s);
		for (size_t i = 0; i < s.nv; i++) {
			const JFASkinVertex *v = &s.v[i];
			for (int k = 0; k < 3; k++) {
				float x = v->position[k];
				if (!(x == x) || fabsf(x) > 100.0f)
					finite = 0;
			}
			float nl = sqrtf(v->normal[0] * v->normal[0] +
					 v->normal[1] * v->normal[1] +
					 v->normal[2] * v->normal[2]);
			if (fabsf(nl - 1.0f) > 1e-3f)
				unit = 0;
			if (v->position[1] > ymax)
				ymax = v->position[1];
			if (v->position[1] < ymin)
				ymin = v->position[1];
		}
	}
	check(finite, "all skinned positions finite and bounded");
	check(unit, "all skinned normals unit-length");
	check(ymin > -0.5f && ymax < target + 0.5f,
	      "skinned Y stays within plausible character bounds");

	/* UV sentinels survived the skinning */
	check(s.v[0].uv[0] == 42.0f && s.v[0].uv[1] == -42.0f,
	      "skinning leaves UV fields untouched");

	/* frame index clamping */
	{
		const JFAClip *cl = &c.clips[JFA_IDLE];
		check(jfa_skin_frame_index(cl, -1.0) == 0, "negative t clamps to frame 0");
		check(jfa_skin_frame_index(cl, 0.0) == 0, "t=0 -> frame 0");
		check(jfa_skin_frame_index(cl, 0.5) == 15, "t=0.5 @30fps -> frame 15");
		check(jfa_skin_frame_index(cl, 100.0) == cl->frames - 1,
		      "past-end clamps to last frame");
	}

	/* two different frames produce different poses */
	jfa_skin_frame(&c, JFA_IDLE, 0, scale, center, world, &s);
	float p0x = s.v[5000].position[0];
	jfa_skin_frame(&c, JFA_IDLE, 75, scale, center, world, &s);
	check(fabsf(s.v[5000].position[0] - p0x) > 1e-4f,
	      "idle frame 0 vs 75 move vertices (animation present)");

	jfa_skin_free(&s);
	free(raw.d);
	jfa_free(&c);
}

int main(void)
{
	synthetic_tests();
	real_tests();

	printf("\n%s (%d failure%s)\n",
	       failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
