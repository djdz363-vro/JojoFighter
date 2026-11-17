/*
 * jfa_gpu_test.c - desktop unit tests for M2 Stage C Substage A.
 *
 * Validates the GPU matrix staging fold (jfa_gpu_fold) against the
 * proven CPU skinning reference (jfa_skin_frame):
 *
 *  1. Budget checks (uniform vectors, bone count).
 *  2. Layout/algorithm parity: the staging rows equal an independent
 *     inline replication of the fin[] folding (exact float compare).
 *  3. End-to-end: skinning sample vertices with the staged rows
 *     (same weighted-sum math the vertex shader performs) reproduces
 *     jfa_skin_frame's positions AND normals across several clips
 *     (30 fps and 60 fps), frames and vertices.
 *  4. Clamp semantics: out-of-range frame and degenerate characters.
 *
 * Links the REAL Android modules; runs from the repo root. Exit 0 =
 * all tests passed.
 */

#include "../Android/app/jni/src/jfa_gpu.h"
#include "../Android/app/jni/src/jfa_skin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

static int failures = 0;

static void check(int cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
	if (!cond)
		failures++;
}

#define ASSETS "Android/app/src/main/assets/animations"

/* Independent inline replication of the fin[] folding (jfa_skin.c):
 * same expressions, written out longhand so the comparison is
 * meaningful. exp[b*12 + e]. */
static void fold_reference(const JFACharacter *c, const float *pose,
			   float scale, const float center[3],
			   const float world[3], float *exp)
{
	for (int b = 0; b < c->nb; b++) {
		const float *m = pose + (size_t)b * 12;
		float *F = exp + (size_t)b * 12;

		F[0] = scale * m[0];
		F[1] = scale * m[1];
		F[2] = scale * m[2];
		F[3] = scale * (m[3] - center[0]) + world[0];

		F[4] = scale * m[4];
		F[5] = scale * m[5];
		F[6] = scale * m[6];
		F[7] = scale * (m[7] - center[1]) + world[1];

		F[8] = scale * m[8];
		F[9] = scale * m[9];
		F[10] = scale * m[10];
		F[11] = scale * (m[11] - center[2]) + world[2];
	}
}

int main(void)
{
	JFACharacter c;
	char err[512];
	float scale, center[3];
	const float world[3] = { 0, 0, 0 };
	static float staged[JFA_GPU_FLOATS];
	static float expect[JFA_MAX_BONES * 12];

	printf("== 1. budget checks ==\n");
	check(jfa_gpu_budget_ok(128, 33), "128 vectors, 33 bones: OK");
	check(jfa_gpu_budget_ok(107, 33), "107 vectors, 33 bones: exactly OK");
	check(!jfa_gpu_budget_ok(106, 33), "106 vectors: rejected");
	check(!jfa_gpu_budget_ok(128, 34), "34 bones: rejected");
	check(!jfa_gpu_budget_ok(128, 0), "0 bones: rejected");

	printf("== 2+3. fold vs CPU reference (real assets) ==\n");
	if (!jfa_load_all(&c, ASSETS, err, sizeof(err))) {
		printf("  [FAIL] loader: %s\n", err);
		return 1;
	}
	jfa_skin_setup(&c, 2.2f, &scale, center);
	check(c.nb == 33, "character has the 33-bone skeleton");

	JFASkinned ref;
	if (!jfa_skin_alloc(&ref, (size_t)c.nv)) {
		printf("  [FAIL] alloc\n");
		jfa_free(&c);
		return 1;
	}

	/* Spread of clips: 30 fps and 60 fps sources. */
	const int clips[] = { JFA_IDLE, JFA_WALK, JFA_GUARD, JFA_PUNCH_LEFT,
			      JFA_KNOCKDOWN };

	int parity_exact = 1, pos_ok = 1, nrm_ok = 1;
	double max_delta = 0.0;

	for (size_t ci = 0; ci < sizeof(clips) / sizeof(clips[0]); ci++) {
		const JFAClip *cl = &c.clips[clips[ci]];
		const int frames[] = { 0, cl->frames / 2, cl->frames - 1 };

		for (size_t fi = 0; fi < 3; fi++) {
			jfa_gpu_fold(&c, clips[ci], frames[fi], scale,
				     center, world, staged);

			/* (2) exact parity with the inline fin[] math */
			const float *pose = cl->mat +
			    (size_t)frames[fi] * (size_t)c.nb * 12;
			fold_reference(&c, pose, scale, center, world,
				       expect);
			if (memcmp(staged, expect,
				   (size_t)c.nb * 12 * sizeof(float)) != 0)
				parity_exact = 0;

			/* reference CPU skin */
			jfa_skin_frame(&c, clips[ci], frames[fi], scale,
				       center, world, &ref);

			/* (3) weighted-sum with the staged rows -- exactly
			 * the math the vertex shader performs -- compared
			 * against the CPU reference output. */
			for (size_t i = 0; i < ref.nv; i += 7) {
				const JFAVertex *sv = &c.v[i];
				float ax = 0, ay = 0, az = 0;
				float an0 = 0, an1 = 0, an2 = 0;
				for (int k = 0; k < 4; k++) {
					float w = sv->bw[k];
					const float *F =
					    staged + (size_t)sv->bi[k] * 12;
					if (w <= 0.00001f)
						continue;
					ax += w * (F[0] * sv->p[0] +
						   F[1] * sv->p[1] +
						   F[2] * sv->p[2] + F[3]);
					ay += w * (F[4] * sv->p[0] +
						   F[5] * sv->p[1] +
						   F[6] * sv->p[2] + F[7]);
					az += w * (F[8] * sv->p[0] +
						   F[9] * sv->p[1] +
						   F[10] * sv->p[2] + F[11]);
					an0 += w * (F[0] * sv->n[0] +
						    F[1] * sv->n[1] +
						    F[2] * sv->n[2]);
					an1 += w * (F[4] * sv->n[0] +
						    F[5] * sv->n[1] +
						    F[6] * sv->n[2]);
					an2 += w * (F[8] * sv->n[0] +
						    F[9] * sv->n[1] +
						    F[10] * sv->n[2]);
				}
				float nl = sqrtf(an0 * an0 + an1 * an1 +
						 an2 * an2);
				if (nl > 1e-6f) {
					an0 /= nl; an1 /= nl; an2 /= nl;
				}

				double dpos =
				    fabs(ax - ref.v[i].position[0]) +
				    fabs(ay - ref.v[i].position[1]) +
				    fabs(az - ref.v[i].position[2]);
				double dnrm =
				    fabs(an0 - ref.v[i].normal[0]) +
				    fabs(an1 - ref.v[i].normal[1]) +
				    fabs(an2 - ref.v[i].normal[2]);
				if (dpos > max_delta)
					max_delta = dpos;
				if (dpos > 1e-4)
					pos_ok = 0;
				if (dnrm > 1e-4)
					nrm_ok = 0;
			}
		}
	}
	check(parity_exact, "staging rows bit-identical to inline fin[] math");
	check(pos_ok, "GPU-layout skinning matches CPU positions (sum|d| <= 1e-4)");
	check(nrm_ok, "GPU-layout normal math matches CPU normals (sum|d| <= 1e-4)");
	printf("  max position delta across all samples: %.3g\n", max_delta);

	/* 4. clamp semantics */
	printf("== 4. clamp semantics ==\n");
	jfa_gpu_fold(&c, JFA_IDLE, -5, scale, center, world, staged);
	jfa_gpu_fold(&c, JFA_IDLE, 100000, scale, center, world, staged);
	static float s2[JFA_GPU_FLOATS];
	jfa_gpu_fold(&c, JFA_IDLE, 0, scale, center, world, s2);
	check(memcmp(staged, s2, (size_t)c.nb * 12 * sizeof(float)) == 0,
	      "out-of-range frames clamp to frame 0 (bit-identical)");
	jfa_gpu_fold(&c, -1, 0, scale, center, world, staged); /* must not crash */
	jfa_gpu_fold(&c, JFA_CLIP_COUNT, 0, scale, center, world, staged);
	check(1, "degenerate clip indices do not crash");

	/* 5. GPU vertex preparation (Substage B): layout, conversion,
	 *    weight preservation, zero-weight pinning, malformed input. */
	printf("== 5. GPU vertex preparation (jfa_gpu_vertices) ==\n");
	check(sizeof(JFAGpuVertex) == 52, "sizeof(JFAGpuVertex) == 52 bytes");
	check(offsetof(JFAGpuVertex, position) == 0, "aPosition offset 0");
	check(offsetof(JFAGpuVertex, uv) == 12, "aUV offset 12");
	check(offsetof(JFAGpuVertex, normal) == 20, "aNormal offset 20");
	check(offsetof(JFAGpuVertex, bone) == 32, "aBoneIndex offset 32");
	check(offsetof(JFAGpuVertex, weight) == 36, "aBoneWeight offset 36");

	JFAGpuVertex *gv = malloc((size_t)c.nv * sizeof(JFAGpuVertex));
	if (!gv) {
		printf("  [FAIL] GPU vertex alloc\n");
		jfa_skin_free(&ref);
		jfa_free(&c);
		return 1;
	}
	check(jfa_gpu_vertices(&c, gv, (size_t)c.nv) == 1,
	      "converts all real vertices");
	check(jfa_gpu_vertices(&c, gv, (size_t)c.nv - 1) == 0,
	      "out_cap < nv rejected");
	check(jfa_gpu_vertices(&c, gv, 0) == 0, "out_cap 0 rejected");
	check(jfa_gpu_vertices(&c, NULL, (size_t)c.nv) == 0,
	      "NULL output rejected");

	/* Full-coverage comparison of the converted mesh against the
	 * shared bind-pose source. */
	size_t zero_slots = 0;
	int pos_exact = 1, uv_ok = 1, w_exact = 1, bone_ok = 1, pin_ok = 1;
	for (size_t i = 0; i < (size_t)c.nv; i++) {
		const JFAVertex *s = &c.v[i];
		const JFAGpuVertex *d = &gv[i];

		if (memcmp(d->position, s->p, sizeof(d->position)) != 0 ||
		    memcmp(d->normal, s->n, sizeof(d->normal)) != 0)
			pos_exact = 0;
		if (d->uv[0] != s->uv[0] ||
		    d->uv[1] != 1.0f - s->uv[1])
			uv_ok = 0;
		if (memcmp(d->weight, s->bw, sizeof(d->weight)) != 0)
			w_exact = 0;
		for (int k = 0; k < 4; k++) {
			if (s->bw[k] > 0.0f) {
				if (d->bone[k] != (uint8_t)s->bi[k])
					bone_ok = 0;
			} else {
				zero_slots++;
				if (d->bone[k] != 0)
					pin_ok = 0;
			}
		}
	}
	check(pos_exact, "positions and normals bit-exact for all vertices");
	check(uv_ok, "U as-is, V flipped (1 - v) for all vertices");
	check(w_exact, "all four weights bit-exact for all vertices");
	check(bone_ok, "positive-weight bones: u8 index == u32 index");
	check(pin_ok, "zero-weight slots pinned to bone 0");
	printf("  zero-weight slots pinned: %zu\n", zero_slots);

	/* Malformed input: corrupt vertex 0 (and restore) -- the
	 * converter must reject without producing output, and the
	 * restored data must convert again to the identical bytes. */
	JFAGpuVertex *gv2 = malloc((size_t)c.nv * sizeof(JFAGpuVertex));
	if (!gv2) {
		printf("  [FAIL] GPU vertex alloc 2\n");
		free(gv);
		jfa_skin_free(&ref);
		jfa_free(&c);
		return 1;
	}
	check(jfa_gpu_vertices(&c, gv2, (size_t)c.nv) == 1, "reference copy converts");
	check(memcmp(gv, gv2, (size_t)c.nv * sizeof(JFAGpuVertex)) == 0,
	      "conversion is deterministic");

	const JFAVertex v0save = c.v[0];
	int ik = -1, zk = -1;
	for (int k = 0; k < 4; k++) {
		if (ik < 0 && v0save.bw[k] > 0.0f)
			ik = k;
		if (zk < 0 && v0save.bw[k] == 0.0f)
			zk = k;
	}

	if (ik >= 0) {
		c.v[0].bi[ik] = 9999u;
		check(jfa_gpu_vertices(&c, gv2, (size_t)c.nv) == 0,
		      "nonzero-weight bone index >= nb rejected");
		c.v[0] = v0save;

		c.v[0].bw[ik] += 0.5f;
		check(jfa_gpu_vertices(&c, gv2, (size_t)c.nv) == 0,
		      "weight sum != 1 rejected");
		c.v[0] = v0save;

		c.v[0].bw[ik] = NAN;
		check(jfa_gpu_vertices(&c, gv2, (size_t)c.nv) == 0,
		      "NaN weight rejected");
		c.v[0] = v0save;

		c.v[0].bw[ik] = -0.25f;
		check(jfa_gpu_vertices(&c, gv2, (size_t)c.nv) == 0,
		      "negative weight rejected");
		c.v[0] = v0save;
	} else {
		printf("  [SKIP] vertex 0 has no positive-weight slot\n");
	}
	if (zk >= 0) {
		/* A zero-weight slot with a garbage index must still be
		 * accepted (pinned to bone 0, weight stays exactly 0) --
		 * invalid NONZERO-weight indices are the ones that
		 * reject; this one is not hidden, just neutralized. */
		c.v[0].bi[zk] = 9999u;
		const int ok = jfa_gpu_vertices(&c, gv2, (size_t)c.nv);
		check(ok == 1 && gv2[0].bone[zk] == 0 &&
			      gv2[0].weight[zk] == 0.0f,
		      "zero-weight garbage index pinned, not hidden");
		c.v[0] = v0save;
	} else {
		printf("  [SKIP] vertex 0 has no zero-weight slot\n");
	}
	check(jfa_gpu_vertices(&c, gv2, (size_t)c.nv) == 1 &&
		      memcmp(gv, gv2, (size_t)c.nv * sizeof(JFAGpuVertex)) == 0,
	      "restored data converts bit-identically again");

	/* Degenerate character states. */
	{
		JFACharacter bad;
		memset(&bad, 0, sizeof(bad));
		check(jfa_gpu_vertices(&bad, gv2, 1) == 0,
		      "unready character rejected");

		bad = c; /* shallow copy: read-only use, never freed */
		bad.ready = 1;
		bad.nb = 0;
		check(jfa_gpu_vertices(&bad, gv2, (size_t)bad.nv) == 0,
		      "0 bones rejected");
		bad.nb = JFA_GPU_MAX_BONES + 1;
		check(jfa_gpu_vertices(&bad, gv2, (size_t)bad.nv) == 0,
		      "34 bones rejected");
	}

	free(gv2);
	free(gv);

	jfa_skin_free(&ref);
	jfa_free(&c);

	printf("\n%s (%d failure%s)\n",
	       failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
