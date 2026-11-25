/*
 * jfa_convert.c - standalone JFA1 animation converter for JojoFighter.
 *
 * Reconstructs the tool that produced the original model0_*.jfa files:
 * it loads an animated, skinned FBX with the vendored ufbx library and
 * bakes the animation into the JFA1 binary format consumed by the game's
 * JFA loader.
 *
 * Usage:
 *   jfa_convert --info <file.jfa>
 *       Validate a JFA file and print its contents (header, skeleton,
 *       mesh statistics, animation timing).
 *
 *   jfa_convert --bake <input.fbx> [-o <output.jfa>] [--fps <N>]
 *                               [--frames <N>] [--flags <N>] [--raw-weights]
 *       Bake the first animation of an FBX file into a JFA file.
 *       Default fps is 30, default frame count is round(duration * fps) + 1
 *       (inclusive sampling, matches the original files).
 *
 * Bake conventions (reverse-engineered and numerically validated against
 * model0_idle.jfa, see docs/JFA1.md):
 *   - Mesh is expanded to a triangle soup in FBX face order, so the index
 *     buffer is the identity sequence 0..nv-1.
 *   - Vertex positions and UVs are the raw bind-pose values from the FBX
 *     (no UV flip; the runtime loader flips V itself). Normals are NOT taken
 *     from the FBX: they are flat per-face normals computed as
 *     normalize(cross(b - a, c - a)), matching the original files.
 *   - Up to 4 bone influences per corner (ufbx sorts weights descending),
 *     re-normalized to sum to 1, padded with bone 0 / weight 0.
 *   - Per frame, per bone, a row-major 4x4 GLOBAL SKINNING matrix
 *     (world/bone evaluated x inverse bind) is stored:
 *       - bones with a skin cluster: ufbx evaluated cluster->geometry_to_world
 *       - leaf bones without a cluster: evaluated node_to_world
 *           multiplied by the inverse of the bind-pose node_to_world
 *   - Bone table is the skeleton in depth-first order from the root bone.
 *
 * Build:
 *   cc -std=c99 -O2 -I third_party/ufbx \
 *       tools/asset-pipeline/jfa_convert.c third_party/ufbx/ufbx.c -lm -o jfa_convert
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ufbx.h"

/* Limits copied from the runtime JFA loader in the PSP main.c so that every
 * file this tool writes is guaranteed loadable. */
#define JFA_MAX_BONES 64
#define JFA_MAX_VERTS 12000
#define JFA_MAX_FRAMES 240
#define JFA_MAX_INDICES (JFA_MAX_VERTS * 3)
#define JFA_MAX_NAME 48

static const char JFA_MAGIC[4] = { 'J', 'F', 'A', '1' };

/* ------------------------------------------------------------------ */
/* Little-endian byte writers (format is defined as little-endian)     */
/* ------------------------------------------------------------------ */

static void write_u32(FILE *f, uint32_t v)
{
	unsigned char b[4] = {
		(unsigned char)(v & 0xff),
		(unsigned char)((v >> 8) & 0xff),
		(unsigned char)((v >> 16) & 0xff),
		(unsigned char)((v >> 24) & 0xff),
	};
	if (fwrite(b, 1, 4, f) != 4) {
		fprintf(stderr, "error: write failed\n");
		exit(1);
	}
}

static void write_f32(FILE *f, float x)
{
	uint32_t v;
	memcpy(&v, &x, 4);
	write_u32(f, v);
}

/* ------------------------------------------------------------------ */
/* Little-endian byte readers                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	unsigned char *data;
	size_t size;
} Buffer;

static uint32_t read_u32(const Buffer *b, size_t off)
{
	return (uint32_t)b->data[off] |
	       ((uint32_t)b->data[off + 1] << 8) |
	       ((uint32_t)b->data[off + 2] << 16) |
	       ((uint32_t)b->data[off + 3] << 24);
}

static float read_f32(const Buffer *b, size_t off)
{
	uint32_t v = read_u32(b, off);
	float x;
	memcpy(&x, &v, 4);
	return x;
}

static int read_s32(const Buffer *b, size_t off)
{
	return (int)read_u32(b, off);
}

static void fail(const char *msg, long v)
{
	fprintf(stderr, "error: %s (%ld)\n", msg, v);
	exit(1);
}

/* ------------------------------------------------------------------ */
/* --info: validate and dump a JFA file                               */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t version, nb, nv, ni, nf, fps, flags;
} JFAHeader;

static void jfa_info(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "error: cannot open %s\n", path);
		exit(1);
	}
	Buffer b = { 0 };
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	b.data = malloc((size_t)sz);
	b.size = (size_t)sz;
	if (!b.data || fread(b.data, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "error: cannot read %s\n", path);
		exit(1);
	}
	fclose(f);

	int problems = 0;
#define CHECK(cond, msg)                                                     \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("  PROBLEM: %s\n", msg);                      \
			problems++;                                          \
		}                                                            \
	} while (0)

	if (b.size < 32 || memcmp(b.data, JFA_MAGIC, 4) != 0) {
		fprintf(stderr, "error: %s is not a JFA1 file\n", path);
		exit(1);
	}
	JFAHeader h;
	h.version = read_u32(&b, 4);
	h.nb = read_u32(&b, 8);
	h.nv = read_u32(&b, 12);
	h.ni = read_u32(&b, 16);
	h.nf = read_u32(&b, 20);
	h.fps = read_u32(&b, 24);
	h.flags = read_u32(&b, 28);

	printf("JFA file: %s (%ld bytes)\n", path, sz);
	printf("  version=%u bones=%u vertices=%u indices=%u frames=%u fps=%u flags=0x%x\n",
	       h.version, h.nb, h.nv, h.ni, h.nf, h.fps, h.flags);

	CHECK(h.version == 1, "unsupported version (expected 1)");
	CHECK(h.nb >= 1 && h.nb <= JFA_MAX_BONES, "bone count out of range 1..64");
	CHECK(h.nv >= 1 && h.nv <= JFA_MAX_VERTS, "vertex count out of range 1..12000");
	CHECK(h.ni >= 1 && h.ni <= JFA_MAX_INDICES, "index count out of range 1..36000");
	CHECK(h.nf >= 1 && h.nf <= JFA_MAX_FRAMES, "frame count out of range 1..240");
	CHECK(h.fps >= 1, "fps must be nonzero");

	size_t bone_off = 32;
	for (uint32_t i = 0; i < h.nb; i++) {
		if (bone_off + 8 > b.size) {
			fprintf(stderr, "error: truncated bone table\n");
			exit(1);
		}
		int32_t parent = read_s32(&b, bone_off);
		uint32_t len = read_u32(&b, bone_off + 4);
		bone_off += 8;
		if (len == 0 || len >= JFA_MAX_NAME || bone_off + len > b.size) {
			fprintf(stderr, "error: bad bone name length at bone %u\n", i);
			exit(1);
		}
		char name[JFA_MAX_NAME];
		memcpy(name, b.data + bone_off, len);
		name[len] = 0;
		bone_off += len;
		printf("  bone %2u: parent=%3d  %s\n", i, parent, name);
		CHECK(parent >= -1 && parent < (int32_t)h.nb, "bone parent out of range");
		if (parent >= (int32_t)i)
			printf("  PROBLEM: bone %u parent %d is not an earlier bone\n", i, parent), problems++;
	}

	size_t vert_off = bone_off;
	size_t vert_end = vert_off + (size_t)h.nv * 64;
	size_t idx_off = vert_end;
	size_t idx_end = idx_off + (size_t)h.ni * 4;
	size_t mat_off = idx_end;
	size_t mat_end = mat_off + (size_t)h.nf * h.nb * 16 * 4;
	CHECK(vert_end <= b.size, "file too small for vertex data");
	if (mat_end != b.size)
		printf("  PROBLEM: expected file size %zu, actual %ld\n", mat_end, sz), problems++;

	/* Mesh statistics */
	float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
	uint32_t *bone_count = calloc(h.nb, sizeof(uint32_t));
	float wsum_min = 1e30f, wsum_max = 0.0f;
	int unweighted = 0, weight_problems = 0;
	if (vert_end <= b.size && bone_count) {
		for (uint32_t v = 0; v < h.nv; v++) {
			size_t vo = vert_off + (size_t)v * 64;
			float p[3], w[4];
			uint32_t bi[4];
			for (int k = 0; k < 3; k++) p[k] = read_f32(&b, vo + 4 * (size_t)k);
			for (int k = 0; k < 4; k++) bi[k] = read_u32(&b, vo + 32 + 4 * (size_t)k);
			for (int k = 0; k < 4; k++) w[k] = read_f32(&b, vo + 48 + 4 * (size_t)k);
			for (int k = 0; k < 3; k++) {
				if (p[k] < mn[k]) mn[k] = p[k];
				if (p[k] > mx[k]) mx[k] = p[k];
			}
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				if (w[k] > 0.0f) {
					sum += w[k];
					if (bi[k] >= h.nb) weight_problems++;
					else bone_count[bi[k]]++;
				}
			}
			if (sum <= 0.0f) unweighted++;
			else {
				if (sum < wsum_min) wsum_min = sum;
				if (sum > wsum_max) wsum_max = sum;
				if (fabsf(sum - 1.0f) > 0.001f) weight_problems++;
			}
		}
	}
	printf("  mesh bbox: x[%g..%g] y[%g..%g] z[%g..%g]\n",
	       (double)mn[0], (double)mx[0], (double)mn[1], (double)mx[1],
	       (double)mn[2], (double)mx[2]);
	printf("  weight sums range [%g..%g]; vertices with no weights: %d\n",
	       (double)wsum_min, (double)wsum_max, unweighted);
	if (weight_problems)
		printf("  PROBLEM: %d weight problems (bone index out of range or sum != 1)\n", weight_problems),
		       problems++;

	/* Per-bone influence counts (compact summary) */
	printf("  bones with weighted vertices: ");
	int any = 0;
	for (uint32_t i = 0; i < h.nb; i++)
		if (bone_count[i] > 0) {
			printf("%u:%u ", i, bone_count[i]);
			any = 1;
		}
	printf("%s\n", any ? "" : "(none)");

	/* Index buffer: identity or not (identity = triangle soup) */
	int identity = 1, oob = 0;
	if (idx_end <= b.size) {
		for (uint32_t i = 0; i < h.ni; i++) {
			uint32_t ix = read_u32(&b, idx_off + (size_t)i * 4);
			if (ix != i) identity = 0;
			if (ix >= h.nv) oob++;
		}
		printf("  index buffer: %s%s\n", identity ? "identity (triangle soup)" : "general",
		       oob ? " (contains out-of-bounds indices!)" : "");
		if (oob) problems++;
	} else {
		printf("  index buffer: (data missing)\n");
	}

	if (mat_end <= b.size) {
		int bad_row3 = 0;
		for (uint32_t i = 0; i < h.nf * h.nb; i++) {
			size_t mo = mat_off + (size_t)i * 64;
			if (read_f32(&b, mo + 48) != 0.0f || read_f32(&b, mo + 52) != 0.0f ||
			    read_f32(&b, mo + 56) != 0.0f || read_f32(&b, mo + 60) != 1.0f)
				bad_row3++;
		}
		if (bad_row3)
			printf("  PROBLEM: %d matrices have a non-standard bottom row\n", bad_row3), problems++;
	}

	if (problems == 0) printf("  OK: no structural problems\n");
	else printf("  FAILED: %d problem(s)\n", problems);
	free(bone_count);
	free(b.data);
#undef CHECK
	if (problems) exit(1);
}

/* ------------------------------------------------------------------ */
/* --bake: FBX -> JFA1                                                */
/* ------------------------------------------------------------------ */

typedef struct {
	ufbx_node *node;             /* skeleton node (bind pose) */
	int parent;                  /* index of parent bone, -1 for root */
	ufbx_skin_cluster *cluster;  /* NULL for leaf bones */
	ufbx_matrix inv_bind;        /* inverse bind node_to_world (leaves) */
	const char *name;
} Bone;

static Bone bones[JFA_MAX_BONES];
static uint32_t nbones = 0;

static ufbx_skin_deformer *g_skin = NULL;

static ufbx_skin_cluster *cluster_for_node(const ufbx_node *node)
{
	if (!g_skin) return NULL;
	for (size_t i = 0; i < g_skin->clusters.count; i++) {
		ufbx_skin_cluster *cl = g_skin->clusters.data[i];
		if (cl->bone_node && cl->bone_node->element_id == node->element_id)
			return cl;
	}
	return NULL;
}

static int node_is_bone(const ufbx_node *node)
{
	return node->bone != NULL;
}

static void collect_bones(ufbx_node *node, int parent)
{
	if (nbones >= JFA_MAX_BONES) {
		fprintf(stderr, "error: skeleton has more than %d bones\n", JFA_MAX_BONES);
		exit(1);
	}
	if (!node_is_bone(node)) {
		/* Not a bone (e.g. a mesh node inside the skeleton): skip it but
		 * keep traversing its children. */
		for (size_t i = 0; i < node->children.count; i++)
			collect_bones(node->children.data[i], parent);
		return;
	}
	Bone *b = &bones[nbones];
	b->node = node;
	b->parent = parent;
	b->cluster = cluster_for_node(node);
	b->name = node->name.data ? node->name.data : "";
	b->inv_bind = ufbx_matrix_invert(&node->node_to_world);
	int self = (int)nbones;
	nbones++;
	for (size_t i = 0; i < node->children.count; i++)
		collect_bones(node->children.data[i], self);
}

static void bake(const char *input, const char *output, uint32_t fps,
		 long frames_arg, uint32_t flags, int raw_weights)
{
	ufbx_error err;
	ufbx_load_opts opts = { 0 };
	opts.generate_missing_normals = true;

	ufbx_scene *scene = ufbx_load_file(input, &opts, &err);
	if (!scene) {
		char msg[512];
		ufbx_format_error(msg, sizeof(msg), &err);
		fprintf(stderr, "error: FBX load failed:\n%s\n", msg);
		exit(1);
	}
	if (scene->meshes.count == 0) {
		fprintf(stderr, "error: FBX contains no mesh\n");
		exit(1);
	}
	ufbx_mesh *mesh = scene->meshes.data[0];
	if (mesh->skin_deformers.count == 0) {
		fprintf(stderr, "error: mesh '%s' has no skin deformer (unskinned FBX cannot be baked)\n",
			mesh->name.data ? mesh->name.data : "");
		exit(1);
	}
	g_skin = mesh->skin_deformers.data[0];

	/* ---- skeleton: find the root bone and collect depth-first ---- */
	ufbx_node *root = NULL;
	for (size_t i = 0; i < g_skin->clusters.count; i++) {
		ufbx_node *n = g_skin->clusters.data[i]->bone_node;
		if (!n) {
			fprintf(stderr, "error: skin cluster %zu has no bone node\n", i);
			exit(1);
		}
		while (n->parent && cluster_for_node(n->parent))
			n = n->parent;
		if (!root)
			root = n;
		else if (root != n) {
			fprintf(stderr, "error: skeleton has multiple roots ('%s' and '%s'); "
					"multi-root rigs are not supported\n",
				root->name.data ? root->name.data : "?",
				n->name.data ? n->name.data : "?");
			exit(1);
		}
	}
	if (!root) {
		fprintf(stderr, "error: skin deformer has no clusters\n");
		exit(1);
	}
	collect_bones(root, -1);

	/* every cluster bone must be part of the collected skeleton */
	for (size_t i = 0; i < g_skin->clusters.count; i++) {
		ufbx_node *n = g_skin->clusters.data[i]->bone_node;
		int found = 0;
		for (uint32_t b = 0; b < nbones; b++)
			if (bones[b].node->element_id == n->element_id) found = 1;
		if (!found) {
			fprintf(stderr, "error: bone '%s' is not in the skeleton rooted at '%s'\n",
				n->name.data ? n->name.data : "?",
				root->name.data ? root->name.data : "?");
			exit(1);
		}
	}

	/* ---- animation timing ---- */
	if (scene->anim_stacks.count == 0) {
		fprintf(stderr, "error: FBX contains no animation (cannot bake a static scene)\n");
		exit(1);
	}
	ufbx_anim_stack *stack = scene->anim_stacks.data[0];
	double duration = stack->time_end - stack->time_begin;
	if (duration <= 0.0) {
		fprintf(stderr, "error: animation '%s' has zero length\n",
			stack->name.data ? stack->name.data : "?");
		exit(1);
	}
	uint32_t nframes;
	if (frames_arg > 0) {
		nframes = (uint32_t)frames_arg;
	} else {
		nframes = (uint32_t)floor(duration * (double)fps + 0.5) + 1;
	}

	/* ---- enforce runtime loader limits ---- */
	if (nbones < 1 || nbones > JFA_MAX_BONES)
		fail("bone count out of range", (long)nbones);
	if (fps == 0) {
		fprintf(stderr, "error: fps must be nonzero\n");
		exit(1);
	}
	if (nframes < 1 || nframes > JFA_MAX_FRAMES)
		fail("frame count out of range (increase --frames limit or fps)", (long)nframes);

	/* ---- vertex soup ---- */
	size_t max_corners = mesh->num_triangles * 3;
	if (max_corners < 1 || max_corners > JFA_MAX_INDICES) {
		fprintf(stderr, "error: mesh triangle corner count %zu out of range 1..%d\n",
			max_corners, JFA_MAX_INDICES);
		exit(1);
	}
	uint32_t *tri = malloc(max_corners * sizeof(uint32_t));
	if (!tri) {
		fprintf(stderr, "error: out of memory\n");
		exit(1);
	}

	typedef struct {
		float p[3], n[3], uv[2];
		uint32_t bi[4];
		float bw[4];
	} OutVert;

	size_t corner_cap = mesh->num_triangles * 3;
	OutVert *verts = malloc(corner_cap * sizeof(OutVert));
	if (!verts) {
		fprintf(stderr, "error: out of memory\n");
		exit(1);
	}
	size_t nv = 0;

	for (size_t fi = 0; fi < mesh->faces.count; fi++) {
		ufbx_face face = mesh->faces.data[fi];
		if (face.num_indices < 3) continue;
		uint32_t ntri = ufbx_triangulate_face(tri, mesh->max_face_triangles * 3, mesh, face);
		for (uint32_t t = 0; t < ntri; t++) {
			/* The original JFA files store FLAT per-face normals computed as
			 * normalize(cross(b - a, c - a)) from the baked bind-pose
			 * triangle positions; the FBX normals are not used. The runtime
			 * JFA lighting relies on this. */
			uint32_t c0 = tri[t * 3 + 0], c1 = tri[t * 3 + 1], c2 = tri[t * 3 + 2];
			ufbx_vec3 pa = ufbx_get_vertex_vec3(&mesh->vertex_position, c0);
			ufbx_vec3 pb = ufbx_get_vertex_vec3(&mesh->vertex_position, c1);
			ufbx_vec3 pc = ufbx_get_vertex_vec3(&mesh->vertex_position, c2);
			float ux = (float)pb.x - (float)pa.x, uy = (float)pb.y - (float)pa.y, uz = (float)pb.z - (float)pa.z;
			float vx = (float)pc.x - (float)pa.x, vy = (float)pc.y - (float)pa.y, vz = (float)pc.z - (float)pa.z;
			float nx = uy * vz - uz * vy;
			float ny = uz * vx - ux * vz;
			float nz = ux * vy - uy * vx;
			float nl = sqrtf(nx * nx + ny * ny + nz * nz);
			if (nl > 0.0f) {
				nx /= nl; ny /= nl; nz /= nl;
			}
			for (int k = 0; k < 3; k++) {
				uint32_t corner = tri[t * 3 + k];
				if (nv >= corner_cap) {
					fprintf(stderr, "error: internal corner overflow\n");
					exit(1);
				}
				OutVert *v = &verts[nv++];
				ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, corner);
				ufbx_vec2 uv;
				uv.x = 0; uv.y = 0;
				if (mesh->vertex_uv.exists)
					uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, corner);
				v->p[0] = (float)p.x; v->p[1] = (float)p.y; v->p[2] = (float)p.z;
				v->n[0] = nx; v->n[1] = ny; v->n[2] = nz;
				v->uv[0] = (float)uv.x; v->uv[1] = (float)uv.y;

				/* bone influences: ufbx keeps per-vertex weights sorted
				 * by decreasing weight; take the first 4 and renormalize */
				uint32_t vi = mesh->vertex_position.indices.data[corner];
				ufbx_skin_vertex sv = g_skin->vertices.data[vi];
				float sum = 0.0f;
				uint32_t take = sv.num_weights > 4 ? 4 : (uint32_t)sv.num_weights;
				for (uint32_t w = 0; w < 4; w++) {
					v->bi[w] = 0;
					v->bw[w] = 0.0f;
				}
				for (uint32_t w = 0; w < take; w++) {
					ufbx_skin_weight sw = g_skin->weights.data[sv.weight_begin + w];
					ufbx_skin_cluster *cl = g_skin->clusters.data[sw.cluster_index];
					int bone = -1;
					for (uint32_t bb = 0; bb < nbones; bb++)
						if (bones[bb].node->element_id == cl->bone_node->element_id)
							bone = (int)bb;
					if (bone < 0) {
						fprintf(stderr, "error: vertex influenced by bone '%s' "
							"outside the skeleton\n",
							cl->bone_node->name.data ? cl->bone_node->name.data : "?");
						exit(1);
					}
					v->bi[w] = (uint32_t)bone;
					v->bw[w] = (float)sw.weight;
					sum += v->bw[w];
				}
				if (!raw_weights && sum > 0.0f) {
					for (uint32_t w = 0; w < 4; w++) v->bw[w] /= sum;
				}
			}
		}
	}
	if (nv < 1 || nv > JFA_MAX_VERTS) {
		fprintf(stderr, "error: baked vertex count %zu out of range 1..%d\n", nv, JFA_MAX_VERTS);
		exit(1);
	}

	/* ---- write output ---- */
	FILE *f = fopen(output, "wb");
	if (!f) {
		fprintf(stderr, "error: cannot create %s\n", output);
		exit(1);
	}
	write_u32(f, (uint32_t)(JFA_MAGIC[0] | (JFA_MAGIC[1] << 8) | (JFA_MAGIC[2] << 16) | ((uint32_t)JFA_MAGIC[3] << 24)));
	write_u32(f, 1);            /* version */
	write_u32(f, nbones);
	write_u32(f, (uint32_t)nv);
	write_u32(f, (uint32_t)nv); /* identity index buffer */
	write_u32(f, nframes);
	write_u32(f, fps);
	write_u32(f, flags);

	for (uint32_t b = 0; b < nbones; b++) {
		const char *nm = bones[b].name;
		size_t len = strlen(nm);
		if (len == 0 || len >= JFA_MAX_NAME) {
			fprintf(stderr, "error: bone name length %zu out of range 1..%d ('%s')\n",
				len, JFA_MAX_NAME - 1, nm);
			exit(1);
		}
		write_u32(f, (uint32_t)bones[b].parent);
		write_u32(f, (uint32_t)len);
		if (fwrite(nm, 1, len, f) != len) {
			fprintf(stderr, "error: write failed\n");
			exit(1);
		}
	}

	for (size_t i = 0; i < nv; i++) {
		for (int k = 0; k < 3; k++) write_f32(f, verts[i].p[k]);
		for (int k = 0; k < 3; k++) write_f32(f, verts[i].n[k]);
		for (int k = 0; k < 2; k++) write_f32(f, verts[i].uv[k]);
		for (int k = 0; k < 4; k++) write_u32(f, verts[i].bi[k]);
		for (int k = 0; k < 4; k++) write_f32(f, verts[i].bw[k]);
	}

	for (uint32_t i = 0; i < nv; i++) write_u32(f, (uint32_t)i); /* identity */

	/* ---- bake the matrices ---- */
	ufbx_evaluate_opts eopts = { 0 };
	eopts.evaluate_skinning = true;

	for (uint32_t k = 0; k < nframes; k++) {
		double t = stack->time_begin + (double)k / (double)fps;
		if (t > stack->time_end) t = stack->time_end;
		ufbx_scene *ev = ufbx_evaluate_scene(scene, stack->anim, t, &eopts, &err);
		if (!ev) {
			fprintf(stderr, "error: animation evaluation failed at frame %u\n", k);
			exit(1);
		}
		ufbx_mesh *emesh = ev->meshes.data[0];
		ufbx_skin_deformer *eskin = emesh->skin_deformers.data[0];

		for (uint32_t b = 0; b < nbones; b++) {
			ufbx_matrix m;
			if (bones[b].cluster) {
				/* evaluated cluster with the same element_id */
				ufbx_skin_cluster *cl = NULL;
				for (size_t i = 0; i < eskin->clusters.count; i++) {
					ufbx_skin_cluster *c = eskin->clusters.data[i];
					if (c->bone_node && c->bone_node->element_id == bones[b].node->element_id) {
						cl = c;
						break;
					}
				}
				if (!cl) {
					fprintf(stderr, "error: evaluated scene is missing bone '%s'\n",
						bones[b].name);
					exit(1);
				}
				m = cl->geometry_to_world;
			} else {
				ufbx_node *en = ufbx_find_node(ev, bones[b].name);
				if (!en) {
					fprintf(stderr, "error: evaluated scene is missing leaf bone '%s'\n",
						bones[b].name);
					exit(1);
				}
				m = ufbx_matrix_mul(&en->node_to_world, &bones[b].inv_bind);
			}
			write_f32(f, (float)m.m00); write_f32(f, (float)m.m01); write_f32(f, (float)m.m02); write_f32(f, (float)m.m03);
			write_f32(f, (float)m.m10); write_f32(f, (float)m.m11); write_f32(f, (float)m.m12); write_f32(f, (float)m.m13);
			write_f32(f, (float)m.m20); write_f32(f, (float)m.m21); write_f32(f, (float)m.m22); write_f32(f, (float)m.m23);
			write_f32(f, 0.0f); write_f32(f, 0.0f); write_f32(f, 0.0f); write_f32(f, 1.0f);
		}
		ufbx_free_scene(ev);
	}

	if (fclose(f) != 0) {
		fprintf(stderr, "error: write failed\n");
		exit(1);
	}

	printf("baked %s -> %s\n", input, output);
	printf("  bones=%u vertices=%zu indices=%zu frames=%u fps=%u duration=%.3fs\n",
	       nbones, nv, nv, nframes, fps, duration);
	printf("  skeleton root: %s\n", bones[0].name);

	free(tri);
	free(verts);
	ufbx_free_scene(scene);
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
	printf("usage:\n"
	       "  jfa_convert --info <file.jfa>\n"
	       "  jfa_convert --bake <input.fbx> [-o <output.jfa>] [--fps <N>]\n"
	       "              [--frames <N>] [--flags <N>] [--raw-weights]\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 2;
	}
	if (strcmp(argv[1], "--info") == 0) {
		if (argc != 3) {
			usage();
			return 2;
		}
		jfa_info(argv[2]);
		return 0;
	}
	if (strcmp(argv[1], "--bake") == 0) {
		const char *input = NULL, *output = "out.jfa";
		uint32_t fps = 30, flags = 1;
		long frames = 0;
		int raw_weights = 0;
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
				output = argv[++i];
			} else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
				fps = (uint32_t)atoi(argv[++i]);
			} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
				frames = atol(argv[++i]);
			} else if (strcmp(argv[i], "--flags") == 0 && i + 1 < argc) {
				flags = (uint32_t)strtoul(argv[++i], NULL, 0);
			} else if (strcmp(argv[i], "--raw-weights") == 0) {
				raw_weights = 1;
			} else if (!input) {
				input = argv[i];
			} else {
				fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]);
				return 2;
			}
		}
		if (!input) {
			usage();
			return 2;
		}
		bake(input, output, fps, frames, flags, raw_weights);
		return 0;
	}
	usage();
	return 2;
}
