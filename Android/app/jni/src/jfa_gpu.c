/*
 * jfa_gpu.c - GPU skeletal skinning support for the JFA1 runtime
 * (M2 Stage C, Substage A). See jfa_gpu.h.
 */

#include "jfa_gpu.h"

#include <math.h>
#include <string.h>

int jfa_gpu_vertices(const JFACharacter *c, JFAGpuVertex *out,
		     size_t out_cap)
{
	if (!c->ready || !out)
		return 0;
	const int nb = c->nb;
	if (nb < 1 || nb > JFA_GPU_MAX_BONES)
		return 0;
	const size_t nv = (size_t)c->nv;
	if (out_cap < nv)
		return 0;

	for (size_t i = 0; i < nv; i++) {
		const JFAVertex *s = &c->v[i];
		JFAGpuVertex *d = &out[i];

		/* weights: finite, in [0,1], sum 1 (loader contract) */
		float sum = 0.0f;
		for (int k = 0; k < 4; k++) {
			float w = s->bw[k];
			if (!(w >= 0.0f && w <= 1.0f))
				return 0; /* NaN, negative or > 1 */
			sum += w;
		}
		if (fabsf(sum - 1.0f) > 1e-4f)
			return 0;

		/* indices: positive-weight slots must reference a real
		 * bone and fit uint8; zero-weight slots pin to bone 0 */
		for (int k = 0; k < 4; k++) {
			if (s->bw[k] > 0.0f) {
				if (s->bi[k] >= (uint32_t)nb)
					return 0;
				d->bone[k] = (uint8_t)s->bi[k];
			} else {
				d->bone[k] = 0;
			}
			d->weight[k] = s->bw[k]; /* exact copy */
		}

		memcpy(d->position, s->p, sizeof(d->position));
		/* V flipped for GLES, exactly like the CPU renderer's init
		 * prefill (jfaSkin.v[i].uv[1] = 1 - uv[1]); U copied as-is. */
		d->uv[0] = s->uv[0];
		d->uv[1] = 1.0f - s->uv[1];
		memcpy(d->normal, s->n, sizeof(d->normal));
	}
	return 1;
}

int jfa_gpu_budget_ok(int max_vuv, int nb)
{
	if (nb < 1 || nb > JFA_GPU_MAX_BONES)
		return 0;
	if (max_vuv < JFA_GPU_BASE_VECS + JFA_GPU_BONE_VECS)
		return 0;
	return 1;
}

void jfa_gpu_fold(const JFACharacter *c, int clip_index, int frame,
		  float scale, const float center[3], const float world[3],
		  float *out)
{
	const JFAClip *clip;
	const float *pose;
	const int nb = c->nb;
	const float cxm = center[0], cym = center[1], czm = center[2];
	const float wx = world[0], wy = world[1], wz = world[2];

	if (!c->ready || nb < 1 || clip_index < 0 ||
	    clip_index >= JFA_CLIP_COUNT)
		return;

	clip = &c->clips[clip_index];
	if (clip->frames <= 0)
		return;
	if (frame < 0 || frame >= clip->frames)
		frame = 0;

	/* All bones of one sampled frame, 12 floats (3x4, row-major). */
	pose = clip->mat + (size_t)frame * (size_t)nb * 12;

	for (int k = 0; k < nb; k++) {
		const float *m = pose + (size_t)k * 12;
		float *F = out + (size_t)k * 12;

		/* Same expressions, same order, as the fin[] folding in
		 * jfa_skin_frame (scale -> center -> world; yaw = 0). */
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
}

const char *jfa_gpu_vs =
"attribute vec3 aPosition; attribute vec2 aUV; attribute vec3 aNormal;"
"attribute vec4 aBoneIndex; attribute vec4 aBoneWeight;"
"uniform mat4 uMVP; uniform mat4 uModel;"
"uniform vec4 uBones[" /* JFA_GPU_MAX_BONES */ "33];"
"varying vec2 vUV; varying float vLight; varying vec3 vColor;"
"void main(){"
" vec3 sp=vec3(0.0); vec3 sn=vec3(0.0);"
" vec4 p4=vec4(aPosition,1.0);"
" for(int k=0;k<4;k++){"
"  float w=aBoneWeight[k];"
"  if(w>0.00001){"
"   int b=int(aBoneIndex[k]);"
"   vec4 r0=uBones[b*3+0];"
"   vec4 r1=uBones[b*3+1];"
"   vec4 r2=uBones[b*3+2];"
"   sp+=w*vec3(dot(r0,p4),dot(r1,p4),dot(r2,p4));"
"   sn+=w*(r0.xyz*aNormal.x+r1.xyz*aNormal.y+r2.xyz*aNormal.z);"
"  }"
" }"
" vec3 n=normalize(mat3(uModel)*normalize(sn));"
"vLight=0.35+0.65*max(dot(n,normalize(vec3(-0.4,0.8,0.6))),0.0);"
"vUV=aUV; vColor=vec3(1.0);"
" gl_Position=uMVP*vec4(sp,1.0); gl_PointSize=3.0; }";
