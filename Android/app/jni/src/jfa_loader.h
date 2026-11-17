/*
 * jfa_loader.h - JFA1 skeletal-animation runtime loader (Android).
 *
 * Loads the per-character JFA1 clip set (model0_idle.jfa, ...) from the
 * APK assets (via SDL_LoadFile, same access path as the FBX loader) and
 * validates them against the runtime limits enforced by tools/jfa_convert.c.
 *
 * Stage A scope: parsing + validation only. No skinning, no OpenGL.
 *
 * File format: see docs/JFA1.md. Little-endian; header, bone table,
 * vertex data (64 B/vertex), index buffer, then frame-major animation
 * matrices (one row-major 4x4 global skinning matrix per bone per frame;
 * the bottom row is always 0,0,0,1 and is dropped in memory: we store the
 * upper 3x4).
 */

#ifndef JFA_LOADER_H
#define JFA_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* Runtime limits (must match tools/jfa_convert.c and the PSP loader). */
#define JFA_MAX_BONES 64
#define JFA_MAX_VERTS 12000
#define JFA_MAX_FRAMES 240
#define JFA_MAX_INDICES (JFA_MAX_VERTS * 3) /* 36000 */
#define JFA_MAX_NAME 48
#define JFA_MAX_INFLUENCES 4

/* Clip slots, same order as the PSP loader in the PSP main.c. */
enum {
	JFA_IDLE = 0, JFA_WALK, JFA_BACKWALK, JFA_GUARD,
	JFA_PUNCH_LEFT, JFA_PUNCH_RIGHT, JFA_KICK, JFA_HIT,
	JFA_KNOCKDOWN, JFA_GETUP,
	JFA_CLIP_COUNT
};

/* Clip file name suffixes, indexed by the enum above. */
extern const char *jfa_clip_suffix[JFA_CLIP_COUNT];

typedef struct {
	float p[3];            /* bind-pose position */
	float n[3];            /* bind-pose (flat per-face) normal */
	float uv[2];           /* raw file UV; the renderer flips V itself */
	uint32_t bi[4];        /* bone indices */
	float bw[4];           /* bone weights, normalized to sum 1 */
} JFAVertex;

typedef struct {
	int frames;            /* number of sampled frames (1..240) */
	uint32_t fps;          /* sampling rate from the header (1..1000) */
	float *mat;            /* frames * nbones * 12 floats, frame-major:
			      3x4 row-major global skinning matrices;
			      posed = (x*m0..m2 + m3, ...) per bone */
} JFAClip;

typedef struct {
	int ready;             /* 1 after a successful jfa_load_all */
	int nb, nv, ni;        /* bone / vertex / index counts (shared) */
	int parent[JFA_MAX_BONES];
	char bone_name[JFA_MAX_BONES][JFA_MAX_NAME];
	JFAVertex *v;          /* shared bind-pose mesh (nv vertices) */
	uint32_t *indices;     /* ni indices, all < nv */
	JFAClip clips[JFA_CLIP_COUNT];
	float minv[3], maxv[3];/* bind-pose bbox (computed at load) */
} JFACharacter;

/*
 * Load all JFA_CLIP_COUNT clips named "<asset_dir>/model0_<suffix>.jfa".
 * The first clip (idle) provides the shared skeleton and mesh; every
 * other clip must carry the identical bone table and compatible mesh
 * dimensions (a differing mesh byte-for-byte is tolerated with a
 * warning, matching "compatible", since clips may come from re-exports).
 *
 * Returns 1 on success (all clips loaded and validated), 0 on failure
 * with a message written to err. On failure *c is fully freed.
 *
 * asset_dir example: "animations" (relative paths resolve through the
 * Android asset manager via SDL_LoadFile, like the FBX loader).
 */
int jfa_load_all(JFACharacter *c, const char *asset_dir, char *err, size_t errlen);

/* Release all memory owned by c and zero the struct. Safe on zeroed c. */
void jfa_free(JFACharacter *c);

#endif /* JFA_LOADER_H */
