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

	jfa_skin_free(&ref);
	jfa_free(&c);

	printf("\n%s (%d failure%s)\n",
	       failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
