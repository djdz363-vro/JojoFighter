/*
 * jfa_loader.c - JFA1 skeletal-animation runtime loader (Android).
 *
 * Stage A: parse + validate only. See jfa_loader.h and docs/JFA1.md.
 *
 * Design notes:
 *  - Asset access uses SDL_LoadFile (via the Android asset manager),
 *    the same mechanism the existing FBX loader uses.
 *  - All reads go through a bounds-checked cursor; every length
 *    computation uses overflow-checked arithmetic before allocation.
 *  - The loader never writes to or moves the original files; it only
 *    reads them into memory.
 *  - The only SDL call made outside file loading is SDL_LogWarn for the
 *    tolerated mesh-mismatch case; all failures are returned through
 *    the caller-provided error buffer.
 */

#include "jfa_loader.h"

#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *jfa_clip_suffix[JFA_CLIP_COUNT] = {
	"idle", "walk", "backwalk", "guard", "punch_left",
	"punch_right", "kick", "hit", "knockdown", "getup"
};

/* ------------------------------------------------------------------ */
/* Error buffer helper                                                 */
/* ------------------------------------------------------------------ */

static void err_set(char *err, size_t errlen, const char *clip,
		    const char *fmt, ...)
{
	va_list ap;

	if (!err || errlen == 0)
		return;
	if (clip && *clip)
		snprintf(err, errlen, "jfa: %s: ", clip);
	else
		snprintf(err, errlen, "jfa: ");
	size_t len = strlen(err);
	va_start(ap, fmt);
	vsnprintf(err + len, errlen - len, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Overflow-safe size arithmetic                                       */
/* ------------------------------------------------------------------ */

static int size_mul(size_t a, size_t b, size_t *out)
{
	if (a != 0 && b > (size_t)-1 / a)
		return 0;
	*out = a * b;
	return 1;
}

static int size_add(size_t a, size_t b, size_t *out)
{
	if (b > (size_t)-1 - a)
		return 0;
	*out = a + b;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Bounds-checked little-endian reader                                 */
/* ------------------------------------------------------------------ */

typedef struct {
	const unsigned char *d;
	size_t size;
	size_t pos;
} Rd;

/* True if n more bytes are available. pos <= size is an invariant. */
static int rd_need(const Rd *r, size_t n)
{
	return n <= r->size - r->pos;
}

static int rd_u32(Rd *r, uint32_t *out)
{
	if (!rd_need(r, 4))
		return 0;
	*out = (uint32_t)r->d[r->pos] |
	       ((uint32_t)r->d[r->pos + 1] << 8) |
	       ((uint32_t)r->d[r->pos + 2] << 16) |
	       ((uint32_t)r->d[r->pos + 3] << 24);
	r->pos += 4;
	return 1;
}

static int rd_i32(Rd *r, int32_t *out)
{
	uint32_t v;
	if (!rd_u32(r, &v))
		return 0;
	*out = (int32_t)v;
	return 1;
}

static int rd_f32(Rd *r, float *out)
{
	uint32_t v;
	if (!rd_u32(r, &v))
		return 0;
	memcpy(out, &v, 4);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Per-clip parse (one JFA1 file)                                      */
/* ------------------------------------------------------------------ */

/* Parsed result of one file before it is merged into the character. */
typedef struct {
	uint32_t nb, nv, ni, nf, fps, flags;
	int32_t parent[JFA_MAX_BONES];
	char bone_name[JFA_MAX_BONES][JFA_MAX_NAME];
	JFAVertex *v;
	uint32_t *indices;
	float *mat; /* nf * nb * 12 */
} JFAFile;

static void jfa_file_clear(JFAFile *f)
{
	free(f->v);
	free(f->indices);
	free(f->mat);
	memset(f, 0, sizeof(*f));
}

/*
 * Validate and parse `data` (a whole JFA1 file) into `f`.
 * Returns 0 and writes a message on failure; `f` is left freed.
 */
static int jfa_parse_clip(const char *clip, const unsigned char *data,
			  size_t size, JFAFile *f, char *err, size_t errlen)
{
	Rd r = { data, size, 0 };
	size_t expect;
	uint32_t version;

	memset(f, 0, sizeof(*f));

	/* ---- header ---- */
	if (!rd_need(&r, 32)) {
		err_set(err, errlen, clip, "file too small for header (%zu bytes)",
			size);
		goto bad;
	}
	if (memcmp(r.d, "JFA1", 4) != 0) {
		err_set(err, errlen, clip, "bad magic (not a JFA1 file)");
		goto bad;
	}
	r.pos = 4;
	if (!rd_u32(&r, &version) || version != 1) {
		err_set(err, errlen, clip, "unsupported version %u (expected 1)",
			(unsigned)version);
		goto bad;
	}
	if (!rd_u32(&r, &f->nb) || !rd_u32(&r, &f->nv) ||
	    !rd_u32(&r, &f->ni) || !rd_u32(&r, &f->nf) ||
	    !rd_u32(&r, &f->fps) || !rd_u32(&r, &f->flags))
		goto bad;
	if (f->nb < 1 || f->nb > JFA_MAX_BONES) {
		err_set(err, errlen, clip, "bone count %u out of range 1..%d",
			(unsigned)f->nb, JFA_MAX_BONES);
		goto bad;
	}
	if (f->nv < 1 || f->nv > JFA_MAX_VERTS) {
		err_set(err, errlen, clip, "vertex count %u out of range 1..%d",
			(unsigned)f->nv, JFA_MAX_VERTS);
		goto bad;
	}
	if (f->ni < 1 || f->ni > JFA_MAX_INDICES) {
		err_set(err, errlen, clip, "index count %u out of range 1..%d",
			(unsigned)f->ni, JFA_MAX_INDICES);
		goto bad;
	}
	if (f->nf < 1 || f->nf > JFA_MAX_FRAMES) {
		err_set(err, errlen, clip, "frame count %u out of range 1..%d",
			(unsigned)f->nf, JFA_MAX_FRAMES);
		goto bad;
	}
	if (f->fps < 1 || f->fps > 1000) {
		err_set(err, errlen, clip, "fps %u out of range 1..1000",
			(unsigned)f->fps);
		goto bad;
	}
	if (f->flags != 1) {
		err_set(err, errlen, clip, "unsupported flags 0x%x (expected 0x1)",
			(unsigned)f->flags);
		goto bad;
	}

	/* ---- bone table ---- */
	for (uint32_t b = 0; b < f->nb; b++) {
		int32_t parent;
		uint32_t len;
		if (!rd_i32(&r, &parent) || !rd_u32(&r, &len)) {
			err_set(err, errlen, clip, "truncated bone table at bone %u",
				b);
			goto bad;
		}
		if (parent < -1 || parent >= (int32_t)f->nb) {
			err_set(err, errlen, clip,
				"bone %u parent %d out of range -1..%u",
				b, (int)parent, (unsigned)(f->nb - 1));
			goto bad;
		}
		/* Parents must reference earlier bones (DFS preorder). */
		if (parent >= (int32_t)b) {
			err_set(err, errlen, clip,
				"bone %u parent %d is not an earlier bone", b,
				(int)parent);
			goto bad;
		}
		if (len < 1 || len >= JFA_MAX_NAME) {
			err_set(err, errlen, clip,
				"bone %u name length %u out of range 1..%d",
				b, (unsigned)len, JFA_MAX_NAME - 1);
			goto bad;
		}
		if (!rd_need(&r, len)) {
			err_set(err, errlen, clip,
				"truncated bone name at bone %u", b);
			goto bad;
		}
		memcpy(f->bone_name[b], r.d + r.pos, len);
		f->bone_name[b][len] = 0;
		f->parent[b] = parent;
		r.pos += len;
	}

	/* ---- vertex data ---- */
	if (!size_mul((size_t)f->nv, sizeof(JFAVertex), &expect) ||
	    !size_add(r.pos, expect, &expect) || expect > size) {
		err_set(err, errlen, clip, "file too small for vertex data");
		goto bad;
	}
	{
		size_t alloc;
		if (!size_mul((size_t)f->nv, sizeof(JFAVertex), &alloc)) {
			err_set(err, errlen, clip, "vertex allocation overflow");
			goto bad;
		}
		f->v = malloc(alloc);
		if (!f->v) {
			err_set(err, errlen, clip, "out of memory (%zu bytes)",
				alloc);
			goto bad;
		}
	}
	for (uint32_t i = 0; i < f->nv; i++) {
		JFAVertex *v = &f->v[i];
		float wsum = 0.0f;
		for (int k = 0; k < 3; k++)
			if (!rd_f32(&r, &v->p[k]))
				goto trunc_vert;
		for (int k = 0; k < 3; k++)
			if (!rd_f32(&r, &v->n[k]))
				goto trunc_vert;
		for (int k = 0; k < 2; k++)
			if (!rd_f32(&r, &v->uv[k]))
				goto trunc_vert;
		for (int k = 0; k < 4; k++)
			if (!rd_u32(&r, &v->bi[k]))
				goto trunc_vert;
		for (int k = 0; k < 4; k++)
			if (!rd_f32(&r, &v->bw[k]))
				goto trunc_vert;
		for (int k = 0; k < 4; k++) {
			if (v->bi[k] >= f->nb) {
				err_set(err, errlen, clip,
					"vertex %u bone index %u out of range (bones=%u)",
					i, (unsigned)v->bi[k], (unsigned)f->nb);
				goto bad;
			}
			if (v->bw[k] != 0.0f)
				wsum += v->bw[k];
		}
		if (wsum != 0.0f && (wsum < 0.999f || wsum > 1.001f)) {
			err_set(err, errlen, clip,
				"vertex %u weight sum %.4f is not ~1", i,
				(double)wsum);
			goto bad;
		}
		continue;
	trunc_vert:
		err_set(err, errlen, clip, "truncated vertex data at vertex %u",
			i);
		goto bad;
	}

	/* ---- index buffer ---- */
	{
		size_t alloc;
		if (!size_mul((size_t)f->ni, sizeof(uint32_t), &alloc) ||
		    !size_add(r.pos, alloc, &expect) || expect > size) {
			err_set(err, errlen, clip,
				"file too small for index data");
			goto bad;
		}
		f->indices = malloc(alloc);
		if (!f->indices) {
			err_set(err, errlen, clip, "out of memory (%zu bytes)",
				alloc);
			goto bad;
		}
		for (uint32_t i = 0; i < f->ni; i++) {
			uint32_t ix;
			if (!rd_u32(&r, &ix)) {
				err_set(err, errlen, clip,
					"truncated index data at index %u", i);
				goto bad;
			}
			if (ix >= f->nv) {
				err_set(err, errlen, clip,
					"index %u value %u out of range (vertices=%u)",
					i, (unsigned)ix, (unsigned)f->nv);
				goto bad;
			}
			f->indices[i] = ix;
		}
	}

	/* ---- animation matrices (frame-major, nf * nb * 4x4) ---- */
	{
		size_t nf16, alloc, matsize;
		if (!size_mul((size_t)f->nf, (size_t)f->nb, &nf16) ||
		    !size_mul(nf16, 16 * sizeof(float), &matsize) ||
		    !size_add(r.pos, matsize, &expect) || expect > size) {
			err_set(err, errlen, clip,
				"file too small for animation matrices");
			goto bad;
		}
		if (expect != size) {
			err_set(err, errlen, clip,
				"trailing bytes after matrices (expected %zu, file %zu)",
				expect, size);
			goto bad;
		}
		if (!size_mul(nf16, 12 * sizeof(float), &alloc)) {
			err_set(err, errlen, clip, "matrix allocation overflow");
			goto bad;
		}
		f->mat = malloc(alloc);
		if (!f->mat) {
			err_set(err, errlen, clip, "out of memory (%zu bytes)",
				alloc);
			goto bad;
		}
		for (size_t m = 0; m < nf16; m++) {
			float row3[4];
			float *dst = &f->mat[m * 12];
			for (int k = 0; k < 12; k++)
				if (!rd_f32(&r, &dst[k])) {
					err_set(err, errlen, clip,
						"truncated matrix data");
					goto bad;
				}
			for (int k = 0; k < 3; k++)
				if (!rd_f32(&r, &row3[k])) {
					err_set(err, errlen, clip,
						"truncated matrix data");
					goto bad;
				}
			if (!rd_f32(&r, &row3[3]))
				goto bad;
			if (row3[0] != 0.0f || row3[1] != 0.0f ||
			    row3[2] != 0.0f || row3[3] != 1.0f) {
				err_set(err, errlen, clip,
					"matrix %zu has non-standard bottom row",
					m);
				goto bad;
			}
		}
	}

	return 1;

bad:
	jfa_file_clear(f);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Character-level loading                                             */
/* ------------------------------------------------------------------ */

static int clip_path(char *buf, size_t bufsz, const char *dir,
		     const char *suffix)
{
	int n = snprintf(buf, bufsz, "%s/model0_%s.jfa", dir, suffix);
	return n > 0 && n < (int)bufsz;
}

/* Read one asset file fully via SDL (asset manager on Android). */
static int read_asset(const char *path, unsigned char **out, size_t *outlen,
		      char *err, size_t errlen)
{
	size_t size = 0;
	void *data = SDL_LoadFile(path, &size);

	if (!data) {
		err_set(err, errlen, NULL, "cannot read '%s' (%s)", path,
			SDL_GetError());
		return 0;
	}
	*out = (unsigned char *)data;
	*outlen = size;
	return 1;
}

void jfa_free(JFACharacter *c)
{
	if (!c)
		return;
	for (int i = 0; i < JFA_CLIP_COUNT; i++)
		free(c->clips[i].mat);
	free(c->v);
	free(c->indices);
	memset(c, 0, sizeof(*c));
}

int jfa_load_all(JFACharacter *c, const char *asset_dir, char *err,
		 size_t errlen)
{
	JFAFile cur;
	unsigned char *data = NULL;
	size_t size = 0;
	char path[512];
	int ok = 0;

	memset(c, 0, sizeof(*c));
	memset(&cur, 0, sizeof(cur));
	if (errlen > 0)
		err[0] = 0;

	for (int ci = 0; ci < JFA_CLIP_COUNT; ci++) {
		const char *suffix = jfa_clip_suffix[ci];

		if (!clip_path(path, sizeof(path), asset_dir, suffix)) {
			err_set(err, errlen, suffix, "asset path too long");
			goto fail;
		}
		free(data);
		data = NULL;
		if (!read_asset(path, &data, &size, err, errlen))
			goto fail;
		if (!jfa_parse_clip(suffix, data, size, &cur, err, errlen))
			goto fail;

		if (ci == JFA_IDLE) {
			/* First clip defines the shared skeleton + mesh. */
			c->nb = (int)cur.nb;
			c->nv = (int)cur.nv;
			c->ni = (int)cur.ni;
			for (uint32_t b = 0; b < cur.nb; b++) {
				c->parent[b] = cur.parent[b];
				memcpy(c->bone_name[b], cur.bone_name[b],
				       JFA_MAX_NAME);
			}
			size_t vmesh, imesh;
			if (!size_mul((size_t)cur.nv, sizeof(JFAVertex),
				      &vmesh) ||
			    !size_mul((size_t)cur.ni, sizeof(uint32_t),
				      &imesh)) {
				err_set(err, errlen, suffix,
					"mesh copy size overflow");
				goto fail;
			}
			c->v = malloc(vmesh);
			c->indices = malloc(imesh);
			if (!c->v || !c->indices) {
				err_set(err, errlen, suffix, "out of memory");
				goto fail;
			}
			memcpy(c->v, cur.v, vmesh);
			memcpy(c->indices, cur.indices, imesh);
			for (uint32_t i = 0; i < cur.nv; i++)
				for (int k = 0; k < 3; k++) {
					float x = c->v[i].p[k];
					if (x < c->minv[k])
						c->minv[k] = x;
					if (x > c->maxv[k])
						c->maxv[k] = x;
				}
		} else {
			/* Skeleton must be identical to the first clip. */
			if (cur.nb != (uint32_t)c->nb) {
				err_set(err, errlen, suffix,
					"bone count %u differs from idle (%d)",
					(unsigned)cur.nb, c->nb);
				goto fail;
			}
			for (uint32_t b = 0; b < cur.nb; b++) {
				if (cur.parent[b] != c->parent[b] ||
				    memcmp(cur.bone_name[b], c->bone_name[b],
					   JFA_MAX_NAME) != 0) {
					err_set(err, errlen, suffix,
						"bone %u differs from idle (%s vs %s)",
						b, cur.bone_name[b],
						c->bone_name[b]);
					goto fail;
				}
			}
			/* Mesh must be dimension-compatible; byte-equality
			 * is only warned about (clips may be re-exports). */
			if (cur.nv != (uint32_t)c->nv ||
			    cur.ni != (uint32_t)c->ni) {
				err_set(err, errlen, suffix,
					"mesh dims %u/%u differ from idle (%d/%d)",
					(unsigned)cur.nv, (unsigned)cur.ni,
					c->nv, c->ni);
				goto fail;
			}
			if (memcmp(c->v, cur.v,
				   (size_t)c->nv * sizeof(JFAVertex)) != 0)
				SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
					    "jfa: %s: mesh data differs from idle (re-export tolerated)",
					    suffix);
		}

		/* Copy the animation matrices. */
		{
			size_t nf12;
			if (!size_mul((size_t)cur.nf, (size_t)cur.nb * 12 *
				      sizeof(float), &nf12)) {
				err_set(err, errlen, suffix, "size overflow");
				goto fail;
			}
			c->clips[ci].frames = (int)cur.nf;
			c->clips[ci].fps = cur.fps;
			c->clips[ci].mat = malloc(nf12);
			if (!c->clips[ci].mat) {
				err_set(err, errlen, suffix, "out of memory");
				goto fail;
			}
			memcpy(c->clips[ci].mat, cur.mat, nf12);
		}

		SDL_Log("jfa: loaded %s: %u bones, %u verts, %u indices, %u frames @ %u fps",
			path, (unsigned)cur.nb, (unsigned)cur.nv,
			(unsigned)cur.ni, (unsigned)cur.nf, (unsigned)cur.fps);

		/* Release this clip's parse buffers; the same `cur` struct is
		 * reused for the next clip and would otherwise leak them. */
		jfa_file_clear(&cur);
	}

	c->ready = 1;
	ok = 1;
	goto done;

fail:
	jfa_free(c);
done:
	jfa_file_clear(&cur);
	free(data);
	return ok;
}
