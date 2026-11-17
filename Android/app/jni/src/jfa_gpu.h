/*
 * jfa_gpu.h - GPU skeletal skinning support for the JFA1 runtime (M2 Stage C).
 *
 * Substage A scope (this file):
 *   - the GPU skinning VERTEX SHADER source (GLES2 / GLSL ES 1.00),
 *   - the CPU-side fold of the current frame's pose matrices into the
 *     compact 99-vec4 staging layout consumed by glUniform4fv,
 *   - the vertex-uniform budget check.
 *
 * The fold reuses the exact PSP-compatible "fin[]" mathematics from
 * jfa_skin.c (scale, bind-pose center, world translation folded per
 * bone, yaw = 0), so the matrices sent to the GPU are bit-comparable
 * with the proven CPU path.
 *
 * Matrix layout (both in JFA clip storage and in the staging array):
 *   one bone = 3 rows of vec4 (row-major 3x4):
 *     row r, column c  =  staged[(b*3 + r) * 4 + c]
 *   posed position     =  (dot(row0,P), dot(row1,P), dot(row2,P))
 *                         with P = (p.x, p.y, p.z, 1)
 *   posed normal (pre-normalize) = row0.xyz*n.x + row1.xyz*n.y + row2.xyz*n.z
 *
 * Deliberately NO OpenGL calls here: this module stays pure C so the
 * matrix fold can be unit-tested on the desktop against the CPU
 * skinning reference. The GL side (program creation, limit query,
 * glUniform4fv) lives in main.c behind the JFA_GPU_SKIN compile flag.
 */

#ifndef JFA_GPU_H
#define JFA_GPU_H

#include "jfa_loader.h"

/*
 * Skeleton size the GPU shader is compiled for. The shader declares
 * `uniform vec4 uBones[33]` (3 vec4 per bone). With the 8 vectors the
 * existing vertex stage already consumes (uMVP + uModel), the link
 * requirement is 8 + 3*33 = 107 <= GL_MAX_VERTEX_UNIFORM_VECTORS
 * (spec minimum is 128; Mali-400 reports exactly 128). Skeletons with
 * more bones fail the budget check and stay on the CPU path.
 */
#define JFA_GPU_MAX_BONES 33
#define JFA_GPU_BONE_VECS (JFA_GPU_MAX_BONES * 3)          /* 99 vec4   */
#define JFA_GPU_BASE_VECS 8                                 /* uMVP+uModel */
#define JFA_GPU_FLOATS   (JFA_GPU_BONE_VECS * 4)           /* 396 floats */

/*
 * Vertex-uniform budget check, evaluated BEFORE creating the program:
 *   max_vuv  value of GL_MAX_VERTEX_UNIFORM_VECTORS (queried at runtime)
 *   nb       bone count of the loaded character
 * Returns 1 if the GPU program can link and index every bone.
 */
int jfa_gpu_budget_ok(int max_vuv, int nb);

/*
 * Fold one frame's pose matrices into the GPU staging layout.
 *   clip_index / frame   pose to fold (same semantics as jfa_skin_frame)
 *   scale, center, world character normalization (jfa_skin_setup) and
 *                        world translation; yaw is 0 (uModel rotates)
 *   out                  JFA_GPU_FLOATS floats (nb*3 vec4; rows for
 *                        bones >= nb are left as written last time --
 *                        only upload nb*3 vec4 with glUniform4fv)
 *
 * The per-bone expressions are written in the same order as the
 * fin[] folding in jfa_skin_frame so results compare equal.
 */
void jfa_gpu_fold(const JFACharacter *c, int clip_index, int frame,
		  float scale, const float center[3], const float world[3],
		  float *out);

/*
 * GPU vertex format (Substage B): the static bind-pose mesh converted
 * for the GPU skinning vertex shader. 52 bytes per vertex:
 *   offset  0: position 3 x float   (bind pose)
 *   offset 12: uv      2 x float    (U as-is, V flipped for GLES,
 *                                    matching the CPU renderer's init
 *                                    prefill "1 - v")
 *   offset 20: normal   3 x float   (bind pose)
 *   offset 32: bone     4 x uint8   (validated, clamped to bone 0
 *                                    for zero-weight slots)
 *   offset 36: weight  4 x float    (copied exactly from the file)
 *
 * Vertex attributes for the shader (Substage C):
 *   aPosition/aUV/aNormal   GL_FLOAT at offsets 0/12/20
 *   aBoneIndex              4 x GL_UNSIGNED_BYTE, NON-normalized, offset 32
 *   aBoneWeight             4 x GL_FLOAT, offset 36
 */
typedef struct {
	float position[3];
	float uv[2];
	float normal[3];
	uint8_t bone[4];
	float weight[4];
} JFAGpuVertex;

/*
 * Convert the shared bind-pose mesh into the GPU vertex layout.
 *
 * Validation (rejects with 0, never produces unsafe indices):
 *   - character ready, 1 <= nb <= JFA_GPU_MAX_BONES (u8-safe and
 *     within the shader's uniform array);
 *   - out_cap >= nv;
 *   - every POSITIVE-weight slot's uint32 index < nb (converted to
 *     uint8 only after this check);
 *   - every zero-weight slot's index is forced to 0 (the weight
 *     contributes nothing, so no valid bone is needed);
 *   - all four weights finite and within [0,1], summing to 1 within
 *     1e-4 (the loader's normalization contract).
 *
 * Positions, normals and weights are copied bit-exactly; U is copied
 * bit-exactly and V is flipped (1 - v) to match the CPU renderer.
 */
int jfa_gpu_vertices(const JFACharacter *c, JFAGpuVertex *out,
		     size_t out_cap);

/*
 * GPU skinning vertex shader (GLSL ES 1.00). Skins position AND normal
 * from the compact vec4 matrix array; lighting formula, varyings and
 * constant color match the existing vertex shader (fs is reused as-is).
 *
 * GLSL ES 1.00 notes (verified against the spec, to be confirmed on
 * device):
 *   - uniform arrays in a VERTEX shader may be indexed with any
 *     integer expression (GLSL ES 1.00 appendix A "Indexing"); the
 *     fragment-shader constant-index-expression limit does not apply.
 *   - int(aBoneIndex[k]) is exact: indices arrive as exact integer
 *     floats 0..32 (they are validated/clamped < nb before upload).
 *   - the loop bound is a constant (4), as required for ES 2.0.
 *   - the weight threshold (w > 0.00001) matches the CPU skinning
 *     skip (jfa_skin_frame skips w <= 0.00001) for exact parity.
 */
extern const char *jfa_gpu_vs;

#endif /* JFA_GPU_H */
