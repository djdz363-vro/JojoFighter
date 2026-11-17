/*
 * jfa_skin.h - CPU skeletal skinning for the JFA1 runtime (Android).
 *
 * Stage B: transforms the static bind-pose mesh with the per-frame bone
 * matrices from jfa_loader, reproducing the PSP renderer's semantics
 * (see PSP main.c "FAST SKINNING"): per-bone final matrices fold the
 * character scale, bind-pose center offset, character yaw and world
 * translation ONCE PER BONE, so each vertex is just a weighted sum of
 * 3x4 matrix products.
 *
 * Differences from the PSP loop, both deliberate:
 *  - normals ARE skinned (weighted sum of the bone rotations applied
 *    to the bind-pose normal, then re-normalized). The PSP lit with
 *    static bind-pose normals instead; the GLES2 shader expects an
 *    animated normal.
 *  - yaw is a parameter (PSP folds MODEL_FRONT_YAW + facing); the
 *    Android renderer rotates via the uModel matrix instead, so the
 *    demo passes yaw = 0 and lets the model matrix do the turning.
 *
 * No OpenGL here: the caller uploads the output array. All buffers are
 * allocated once (jfa_skin_alloc) and reused every frame.
 */

#ifndef JFA_SKIN_H
#define JFA_SKIN_H

#include "jfa_loader.h"

/*
 * Output vertex - MUST keep the exact memory layout of FBXVertex in
 * fbx_loader.h (position[3], uv[2], normal[3]) so the existing GLES2
 * attribute setup and shader can consume it unchanged.
 */
typedef struct {
	float position[3];
	float uv[2];
	float normal[3];
} JFASkinVertex;

typedef struct {
	JFASkinVertex *v;      /* nv output vertices, reused every frame */
	size_t nv;
} JFASkinned;

/*
 * Compute the character normalization from the bind-pose bbox:
 *   *out_scale = target_height / bbox extent  (0 extent -> 1)
 *   out_center = { mid X, min Y, mid Z }      (PSP cxm, cym, czm)
 */
void jfa_skin_setup(const JFACharacter *c, float target_height,
		    float *out_scale, float out_center[3]);

/* Allocate the output vertex array once. Returns 0 on OOM. */
int jfa_skin_alloc(JFASkinned *s, size_t nv);

/* Free the output array (safe on a zeroed struct). */
void jfa_skin_free(JFASkinned *s);

/*
 * Frame index for time t (seconds since the clip started): nearest
 * sampled frame at the clip's own fps, clamped to the last frame
 * (matching the PSP's integer frame stepping).
 */
int jfa_skin_frame_index(const JFAClip *clip, double t);

/*
 * Skin one frame.
 *   clip_index  which clip of c to pose
 *   frame       frame index within that clip (see jfa_skin_frame_index)
 *   scale       character scale (jfa_skin_setup)
 *   center      character center (jfa_skin_setup)
 *   world       world-space translation added after centering (PSP wx/wy/wz)
 *
 * Writes positions and normals into out->v; the uv field is left
 * untouched (fill it once at init; the PSP does the same).
 */
void jfa_skin_frame(const JFACharacter *c, int clip_index, int frame,
		    float scale, const float center[3], const float world[3],
		    JFASkinned *out);

#endif /* JFA_SKIN_H */
