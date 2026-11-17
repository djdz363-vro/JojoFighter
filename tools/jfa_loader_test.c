/*
 * jfa_loader_test.c - desktop test harness for the Android JFA1 loader.
 *
 * Compiles the REAL Android loader (Android/app/jni/src/jfa_loader.c)
 * against real SDL2, so the SDL_LoadFile asset path is exercised
 * exactly as on-device (on desktop it reads regular files).
 *
 * Positive: loads all 10 clips from
 *   Android/app/src/main/assets/animations/
 * and checks frame counts, fps, bone/vertex/index counts and skeleton
 * identity against the values recorded in docs/JFA1.md / M1.
 *
 * Negative: writes corrupted copies of the idle clip into a temp
 * directory (bad magic, truncation, bad parent, bad bone index, bad
 * weights, out-of-range index, bad fps/flags, trailing bytes, missing
 * clips) and requires a clean failure with a meaningful message.
 *
 * Exit code 0 = all tests passed.
 */

#include "../Android/app/jni/src/jfa_loader.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define ASSETS "Android/app/src/main/assets/animations"
#define TMPDIR "/tmp/opencode/jfa_loader_test"

static int failures = 0;

static void check(int cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
	if (!cond)
		failures++;
}

static unsigned char *read_file(const char *path, size_t *outlen)
{
	FILE *f = fopen(path, "rb");
	unsigned char *d;
	long sz;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
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

static int write_file(const char *path, const unsigned char *d, size_t n)
{
	FILE *f = fopen(path, "wb");
	size_t w;

	if (!f)
		return 0;
	w = fwrite(d, 1, n, f);
	fclose(f);
	return w == n;
}

/* ------------------------------------------------------------------ */
/* Positive: load the real asset set                                   */
/* ------------------------------------------------------------------ */

typedef struct {
	const char *suffix;
	int frames;
	uint32_t fps;
} Expect;

static const Expect expected[JFA_CLIP_COUNT] = {
	{ "idle", 151, 30 },	{ "walk", 61, 60 },
	{ "backwalk", 27, 30 },	{ "guard", 80, 60 },
	{ "punch_left", 53, 30 }, { "punch_right", 91, 30 },
	{ "kick", 70, 30 },	{ "hit", 43, 30 },
	{ "knockdown", 87, 30 },	{ "getup", 77, 30 },
};

static void positive_tests(void)
{
	JFACharacter c;
	char err[512];

	printf("== positive: load all 10 clips from %s ==\n", ASSETS);
	memset(&c, 0, sizeof(c));
	int ok = jfa_load_all(&c, ASSETS, err, sizeof(err));
	if (!ok) {
		printf("  [FAIL] jfa_load_all failed: %s\n", err);
		failures++;
		return;
	}
	printf("  [OK] jfa_load_all succeeded\n");

	check(c.ready == 1, "character marked ready");
	check(c.nb == 33, "skeleton has 33 bones");
	check(c.nv == 8952, "shared mesh has 8952 soup vertices");
	check(c.ni == 8952, "shared mesh has 8952 indices");

	int skeleton_ok = (c.parent[0] == -1) &&
			  (strcmp(c.bone_name[0], "mixamorig:Hips") == 0) &&
			  (strcmp(c.bone_name[6], "mixamorig:HeadTop_End") == 0) &&
			  (strcmp(c.bone_name[32], "mixamorig:RightToe_End") == 0);
	check(skeleton_ok, "bone table matches M1 reference (parents + names)");

	for (int i = 0; i < JFA_CLIP_COUNT; i++) {
		char what[256];
		snprintf(what, sizeof(what), "%s: %d frames @ %u fps",
			 expected[i].suffix, c.clips[i].frames,
			 (unsigned)c.clips[i].fps);
		check(c.clips[i].frames == expected[i].frames &&
			      c.clips[i].fps == expected[i].fps,
		      what);
	}

	int finite = 1;
	for (int i = 0; i < JFA_CLIP_COUNT && finite; i++) {
		const JFAClip *cl = &c.clips[i];
		size_t n = (size_t)cl->frames * (size_t)c.nb * 12;
		for (size_t m = 0; m < n; m++) {
			float x = cl->mat[m];
			if (!(x == x) || x > 1e9f || x < -1e9f) { /* NaN/huge */
				finite = 0;
				break;
			}
		}
	}
	check(finite, "all matrix elements finite and bounded");

	int differ = memcmp(c.clips[JFA_IDLE].mat, c.clips[JFA_KICK].mat,
			    33 * 12 * sizeof(float)) != 0;
	check(differ, "idle and kick frame 0 poses differ");

	jfa_free(&c);
	check(c.ready == 0 && c.v == NULL, "jfa_free resets the character");
}

/* ------------------------------------------------------------------ */
/* Negative: corrupted single-clip directories                         */
/* ------------------------------------------------------------------ */

/*
 * Mutation offsets for model0_idle.jfa (verified at runtime below):
 *   header:            0..31
 *   bone0 entry:       parent@32, nameLen@36 (14), name@40
 *   bone table size:   927 bytes (33 entries: 8 + name length each)
 *   vertex section:    32 + 927 = 959
 *   index section:     959 + 8952*64
 *   matrix section:    index section + 8952*4
 */
#define IDLE_BONETABLE 927
#define IDLE_VERTOFF (32 + IDLE_BONETABLE)
#define IDLE_IDXOFF (IDLE_VERTOFF + 8952 * 64)

static void mut_magic(unsigned char *d, size_t *size)
{
	(void)size;
	d[0] ^= 0xff;
}
static void mut_truncate(unsigned char *d, size_t *size)
{
	(void)d;
	*size = 5000;
}
static void mut_parent(unsigned char *d, size_t *size)
{
	(void)size;
	/* bone1 parent at 32 + (8 + 14): set to 5 (>= own index 1) */
	uint32_t p = 5;
	memcpy(d + 32 + 22, &p, 4);
}
static void mut_boneidx(unsigned char *d, size_t *size)
{
	(void)size;
	uint32_t p = 99; /* >= 33 bones */
	memcpy(d + IDLE_VERTOFF + 32, &p, 4);
}
static void mut_weights(unsigned char *d, size_t *size)
{
	(void)size;
	/* vertex0 bw[0] at IDLE_VERTOFF+48: halve it so the sum != 1 */
	float w;
	memcpy(&w, d + IDLE_VERTOFF + 48, 4);
	w *= 0.5f;
	memcpy(d + IDLE_VERTOFF + 48, &w, 4);
}
static void mut_index(unsigned char *d, size_t *size)
{
	(void)size;
	uint32_t p = 999999; /* >= 8952 vertices */
	memcpy(d + IDLE_IDXOFF, &p, 4);
}
static void mut_fps(unsigned char *d, size_t *size)
{
	(void)size;
	uint32_t p = 0;
	memcpy(d + 24, &p, 4);
}
static void mut_flags(unsigned char *d, size_t *size)
{
	(void)size;
	uint32_t p = 7;
	memcpy(d + 28, &p, 4);
}
static void mut_frames(unsigned char *d, size_t *size)
{
	(void)size;
	uint32_t p = 241; /* > JFA_MAX_FRAMES */
	memcpy(d + 20, &p, 4);
}

typedef struct {
	const char *name;
	void (*mutate)(unsigned char *d, size_t *size);
	int append_garbage; /* append 8 bytes instead of in-place mutation */
	const char *expect; /* substring expected in the error message */
} NegTest;

static void run_negative(const NegTest *t, const unsigned char *idle,
			 size_t idle_size)
{
	JFACharacter c;
	char err[512];
	char path[512];
	unsigned char *d = malloc(idle_size + 16);
	size_t size = idle_size;

	printf("== negative: %s ==\n", t->name);
	memcpy(d, idle, idle_size);
	if (t->append_garbage) {
		memset(d + idle_size, 0xab, 8);
		size = idle_size + 8;
	} else {
		t->mutate(d, &size);
	}

	snprintf(path, sizeof(path), "%s/%s", TMPDIR, t->name);
	mkdir(TMPDIR, 0777);
	mkdir(path, 0777);
	snprintf(path, sizeof(path), "%s/%s/model0_idle.jfa", TMPDIR, t->name);
	if (!write_file(path, d, size)) {
		printf("  [FAIL] cannot write temp file\n");
		failures++;
		free(d);
		return;
	}
	free(d);

	memset(&c, 0, sizeof(c));
	snprintf(path, sizeof(path), "%s/%s", TMPDIR, t->name);
	int ok = jfa_load_all(&c, path, err, sizeof(err));
	if (ok) {
		printf("  [FAIL] expected failure but load succeeded\n");
		failures++;
		jfa_free(&c);
		return;
	}
	printf("  error: %s\n", err);
	int matched = strstr(err, t->expect) != NULL;
	check(matched, "load rejected");
	check(!c.ready && c.v == NULL, "character left freed after failure");
}

static void negative_tests(void)
{
	size_t idle_size;
	unsigned char *idle = read_file(ASSETS "/model0_idle.jfa", &idle_size);

	if (!idle) {
		printf("  [FAIL] cannot read %s\n", ASSETS "/model0_idle.jfa");
		failures++;
		return;
	}

	/* verify the hardcoded mutation offsets before relying on them:
	 * the LAST bone name must end exactly at the vertex section */
	check(memcmp(idle + IDLE_VERTOFF - 22, "mixamorig:RightToe_End", 22) == 0,
	      "mutation offsets: last bone name ends at vertex section");
	check(idle_size == 928607, "idle file size matches M1 record");

	const NegTest tests[] = {
		{ "bad_magic", mut_magic, 0, "bad magic" },
		{ "truncated", mut_truncate, 0, "too small" },
		{ "bad_parent", mut_parent, 0, "not an earlier bone" },
		{ "bad_bone_index", mut_boneidx, 0, "bone index" },
		{ "bad_weights", mut_weights, 0, "weight sum" },
		{ "bad_index", mut_index, 0, "out of range" },
		{ "bad_fps", mut_fps, 0, "fps" },
		{ "bad_flags", mut_flags, 0, "flags" },
		{ "trailing_bytes", NULL, 1, "trailing bytes" },
		{ "too_many_frames", mut_frames, 0, "frame count" },
	};

	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
		run_negative(&tests[i], idle, idle_size);

	/* missing clips: valid idle present, walk/backwalk/... absent */
	printf("== negative: missing clips ==\n");
	{
		char path[512];
		JFACharacter c;
		char err[512];

		snprintf(path, sizeof(path), "%s/missing", TMPDIR);
		mkdir(path, 0777);
		snprintf(path, sizeof(path), "%s/missing/model0_idle.jfa",
			 TMPDIR);
		write_file(path, idle, idle_size);

		memset(&c, 0, sizeof(c));
		snprintf(path, sizeof(path), "%s/missing", TMPDIR);
		int ok = jfa_load_all(&c, path, err, sizeof(err));
		check(!ok, "missing clips cause failure");
		check(strstr(err, "walk") != NULL,
		      "error names the first missing clip (walk)");
		jfa_free(&c);
	}

	free(idle);
}

int main(void)
{
	positive_tests();
	negative_tests();

	printf("\n%s (%d failure%s)\n",
	       failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
