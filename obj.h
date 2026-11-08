/* obj.h - OBJ model loader + renderer (included from main.c) */
#ifndef OBJ_H
#define OBJ_H

#define MODEL_MAX        3          /* model0 = Brawler, model1 = Speedster, model2 = Heavy */
#define MTEX             256        /* model textures are shrunk to 256x256 */
#define MAX_TRIS         3000       /* triangle limit per model */
#define MODEL_HEIGHT     4.0f       /* models are scaled to this height (fighters are ~4) */
#define MODEL_FRONT_YAW  1.5707963f /* turns a Blender-default model to face the side.
                                       If your fighter faces the wrong way, try -1.5707963f */
#define MODEL_TURN       0.45f      /* 3/4 view toward the camera */

typedef struct {
    int ntri;
    float *pos;   /* 3 floats per vertex */
    float *nrm;   /* 3 floats per vertex */
    float *uv;    /* 2 floats per vertex */
} Model;

static Model models[MODEL_MAX];
static int nmodels = 0;
static uint32_t __attribute__((aligned(16))) mtex[MODEL_MAX][MTEX * MTEX];

/* read a whole file: tries next to EBOOT.PBP, then the memory stick, then the plain name */
static char *read_try(const char *name, long *outn) {
    char path[220];
    char *b = NULL;
    for (int k = 0; k < 3 && !b; k++) {
        if (k == 0)      snprintf(path, sizeof(path), "%s%s", basedir, name);
        else if (k == 1) snprintf(path, sizeof(path), "ms0:/PSP/GAME/JojoFighter/%s", name);
        else             snprintf(path, sizeof(path), "%s", name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 16L * 1024 * 1024) {
            b = (char *)malloc((size_t)n + 1);
            if (b) {
                long r = (long)fread(b, 1, (size_t)n, f);
                b[r] = 0;
                *outn = r;
            }
        }
        fclose(f);
    }
    return b;
}

/* parses one face corner: "v", "v/t", "v//n" or "v/t/n" */
static int parse_idx(const char **sp, int *vi, int *ti, int *ni) {
    const char *s = *sp;
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '\n' || *s == '\r') return 0;
    long a = strtol(s, &e, 10);
    if (e == s) return 0;
    *vi = (int)a; *ti = 0; *ni = 0;
    s = e;
    if (*s == '/') {
        s++;
        if (*s != '/') {
            long b = strtol(s, &e, 10);
            if (e != s) { *ti = (int)b; s = e; }
        }
        if (*s == '/') {
            s++;
            long c = strtol(s, &e, 10);
            if (e != s) { *ni = (int)c; s = e; }
        }
    }
    *sp = s;
    return 1;
}

static int obj_load(int id, const char *name) {
    long n = 0;
    char *buf = read_try(name, &n);
    if (!buf) return 0;

    int ok = 0;
    int cv = 0, ct = 0, cn = 0;
    for (char *p = buf; *p; ) {
        if (p[0] == 'v' && p[1] == ' ') cv++;
        else if (p[0] == 'v' && p[1] == 't' && p[2] == ' ') ct++;
        else if (p[0] == 'v' && p[1] == 'n' && p[2] == ' ') cn++;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }

    float *fv = (float *)malloc((size_t)(cv + 1) * 3 * sizeof(float));
    float *ft = (float *)malloc((size_t)(ct + 1) * 2 * sizeof(float));
    float *fn = (float *)malloc((size_t)(cn + 1) * 3 * sizeof(float));
    float *op = (float *)malloc((size_t)MAX_TRIS * 9 * sizeof(float));
    float *on = (float *)malloc((size_t)MAX_TRIS * 9 * sizeof(float));
    float *ou = (float *)malloc((size_t)MAX_TRIS * 6 * sizeof(float));
    if (!fv || !ft || !fn || !op || !on || !ou) goto done;

    int iv = 0, it = 0, in = 0, tris = 0;
    for (char *p = buf; *p; ) {
        if (p[0] == 'v' && p[1] == ' ') {
            char *e = p + 2;
            for (int k = 0; k < 3; k++) fv[iv * 3 + k] = (float)strtod(e, &e);
            iv++;
        } else if (p[0] == 'v' && p[1] == 't' && p[2] == ' ') {
            char *e = p + 3;
            for (int k = 0; k < 2; k++) ft[it * 2 + k] = (float)strtod(e, &e);
            it++;
        } else if (p[0] == 'v' && p[1] == 'n' && p[2] == ' ') {
            char *e = p + 3;
            for (int k = 0; k < 3; k++) fn[in * 3 + k] = (float)strtod(e, &e);
            in++;
        } else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
            int vi[16], ti[16], ni[16], nc = 0;
            const char *s = p + 1;
            while (nc < 16 && parse_idx(&s, &vi[nc], &ti[nc], &ni[nc])) nc++;
            for (int k = 1; k + 1 < nc && tris < MAX_TRIS; k++) {
                int c[3];
                float pp[3][3], nn[3][3], uu[3][2];
                int hasn[3], good = 1;
                c[0] = 0; c[1] = k; c[2] = k + 1;
                for (int j = 0; j < 3; j++) {
                    int a = vi[c[j]], b = ti[c[j]], d = ni[c[j]];
                    if (a < 0) a = iv + a + 1;
                    if (b < 0) b = it + b + 1;
                    if (d < 0) d = in + d + 1;
                    if (a < 1 || a > iv) { good = 0; break; }
                    memcpy(pp[j], &fv[(a - 1) * 3], 3 * sizeof(float));
                    if (b >= 1 && b <= it) {
                        uu[j][0] = ft[(b - 1) * 2];
                        uu[j][1] = 1.0f - ft[(b - 1) * 2 + 1];   /* OBJ v goes up, texture v goes down */
                    } else {
                        uu[j][0] = 0.0f; uu[j][1] = 0.0f;
                    }
                    hasn[j] = (d >= 1 && d <= in);
                    if (hasn[j]) memcpy(nn[j], &fn[(d - 1) * 3], 3 * sizeof(float));
                    else { nn[j][0] = 0.0f; nn[j][1] = 1.0f; nn[j][2] = 0.0f; }
                }
                if (!good) continue;
                if (!hasn[0] || !hasn[1] || !hasn[2]) {
                    float e1x = pp[1][0] - pp[0][0], e1y = pp[1][1] - pp[0][1], e1z = pp[1][2] - pp[0][2];
                    float e2x = pp[2][0] - pp[0][0], e2y = pp[2][1] - pp[0][1], e2z = pp[2][2] - pp[0][2];
                    float fx = e1y * e2z - e1z * e2y;
                    float fy = e1z * e2x - e1x * e2z;
                    float fz = e1x * e2y - e1y * e2x;
                    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
                    if (fl < 1e-8f) { fx = 0.0f; fy = 1.0f; fz = 0.0f; }
                    else { fx /= fl; fy /= fl; fz /= fl; }
                    for (int j = 0; j < 3; j++) { nn[j][0] = fx; nn[j][1] = fy; nn[j][2] = fz; }
                }
                for (int j = 0; j < 3; j++) {
                    int o = tris * 3 + j;
                    op[o * 3] = pp[j][0]; op[o * 3 + 1] = pp[j][1]; op[o * 3 + 2] = pp[j][2];
                    on[o * 3] = nn[j][0]; on[o * 3 + 1] = nn[j][1]; on[o * 3 + 2] = nn[j][2];
                    ou[o * 2] = uu[j][0]; ou[o * 2 + 1] = uu[j][1];
                }
                tris++;
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (tris < 1) goto done;

    /* scale to MODEL_HEIGHT, put the feet at y = 0, center left/right, turn to face +X */
    {
        int nv = tris * 3;
        float mnx = 1e30f, mny = 1e30f, mnz = 1e30f, mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
        for (int i = 0; i < nv; i++) {
            float x = op[i * 3], y = op[i * 3 + 1], z = op[i * 3 + 2];
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (y < mny) mny = y;
            if (y > mxy) mxy = y;
            if (z < mnz) mnz = z;
            if (z > mxz) mxz = z;
        }
        float hgt = mxy - mny;
        float s = (hgt > 1e-6f) ? MODEL_HEIGHT / hgt : 1.0f;
        float cxm = (mnx + mxx) * 0.5f, czm = (mnz + mxz) * 0.5f;
        float cf = cosf(MODEL_FRONT_YAW), sf = sinf(MODEL_FRONT_YAW);
        for (int i = 0; i < nv; i++) {
            float x = (op[i * 3] - cxm) * s;
            float y = (op[i * 3 + 1] - mny) * s;
            float z = (op[i * 3 + 2] - czm) * s;
            op[i * 3]     = x * cf + z * sf;
            op[i * 3 + 1] = y;
            op[i * 3 + 2] = -x * sf + z * cf;
            float nx = on[i * 3], ny = on[i * 3 + 1], nz = on[i * 3 + 2];
            float nl = sqrtf(nx * nx + ny * ny + nz * nz);
            if (nl < 1e-6f) { nx = 0.0f; ny = 1.0f; nz = 0.0f; }
            else { nx /= nl; ny /= nl; nz /= nl; }
            on[i * 3]     = nx * cf + nz * sf;
            on[i * 3 + 1] = ny;
            on[i * 3 + 2] = -nx * sf + nz * cf;
        }
    }
    models[id].ntri = tris;
    models[id].pos = op;
    models[id].nrm = on;
    models[id].uv = ou;
    op = on = ou = NULL;   /* the model now owns these */
    ok = 1;

done:
    free(buf);
    free(fv); free(ft); free(fn);
    free(op); free(on); free(ou);
    return ok;
}

/* ---------- model texture (any PNG up to 1024x1024 is shrunk to 256x256) ---------- */
static int load_png_sz_at(uint32_t *dst, int sz, const char *path) {
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) {
        png_image_free(&img);
        return 0;
    }
    img.format = PNG_FORMAT_RGBA;
    if (img.width == 0 || img.height == 0 || img.width > 1024 || img.height > 1024) {
        png_image_free(&img);
        return 0;
    }
    uint8_t *pix = (uint8_t *)malloc(PNG_IMAGE_SIZE(img));
    if (!pix) {
        png_image_free(&img);
        return 0;
    }
    if (!png_image_finish_read(&img, NULL, pix, 0, NULL)) {
        free(pix);
        png_image_free(&img);
        return 0;
    }
    int w = (int)img.width, h = (int)img.height;
    for (int y = 0; y < sz; y++) {
        int y0 = y * h / sz, y1 = (y + 1) * h / sz;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int x = 0; x < sz; x++) {
            int x0 = x * w / sz, x1 = (x + 1) * w / sz;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            unsigned r = 0, g = 0, b = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++) {
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *q = pix + ((size_t)yy * w + xx) * 4;
                    r += q[0]; g += q[1]; b += q[2]; cnt++;
                }
            }
            if (cnt == 0) cnt = 1;
            dst[y * sz + x] = 0xFF000000u | ((b / cnt) << 16) | ((g / cnt) << 8) | (r / cnt);
        }
    }
    free(pix);
    png_image_free(&img);
    return 1;
}

static int load_png_sz(uint32_t *dst, int sz, const char *name) {
    char path[220];
    snprintf(path, sizeof(path), "%s%s", basedir, name);
    if (load_png_sz_at(dst, sz, path)) return 1;
    snprintf(path, sizeof(path), "ms0:/PSP/GAME/JojoFighter/%s", name);
    if (load_png_sz_at(dst, sz, path)) return 1;
    return load_png_sz_at(dst, sz, name);
}

static void obj_load_all(void) {
    char nm[32];
    for (int i = 0; i < MODEL_MAX; i++) {
        snprintf(nm, sizeof(nm), "model%d.obj", i);
        if (!obj_load(i, nm)) continue;
        nmodels++;
        snprintf(nm, sizeof(nm), "model%d.png", i);
        if (!load_png_sz(mtex[i], MTEX, nm)) {
            for (int k = 0; k < MTEX * MTEX; k++) mtex[i][k] = cskin[i];   /* plain color */
        }
    }
}

static void bind_mtex(int i) {
    if (cur_tex == 100 + i) return;
    cur_tex = 100 + i;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, MTEX, MTEX, MTEX, mtex[i]);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
}

/* draws a model at world position (wx, wy, wz) = its feet.
   lean turns it about the hips (+ = forward), yaw about the up axis, sy squashes it. */
static void model_draw(int id, float wx, float wy, float wz, float yaw, float lean,
                       float sy, uint32_t tint, int flash) {
    Model *m = &models[id];
    int nv = m->ntri * 3;
    TVtx *v = (TVtx *)sceGuGetMemory((unsigned int)nv * sizeof(TVtx));
    float cl = cosf(lean), sl = sinf(lean);
    float cyw = cosf(yaw), syw = sinf(yaw);
    const float piv = 2.0f;

    for (int i = 0; i < nv; i++) {
        float x = m->pos[i * 3], y = m->pos[i * 3 + 1] * sy, z = m->pos[i * 3 + 2];
        float nx = m->nrm[i * 3], ny = m->nrm[i * 3 + 1], nz = m->nrm[i * 3 + 2];

        float yy = y - piv;                         /* lean about the hips */
        float x1 = x * cl + yy * sl;
        float y1 = -x * sl + yy * cl + piv;
        float nx1 = nx * cl + ny * sl;
        float ny1 = -nx * sl + ny * cl;

        float x2 = x1 * cyw + z * syw;              /* turn left/right */
        float z2 = -x1 * syw + z * cyw;
        float nx2 = nx1 * cyw + nz * syw;
        float nz2 = -nx1 * syw + nz * cyw;

        float sh = 0.45f + 0.55f * lit(nx2, ny1, nz2);
        v[i].u = m->uv[i * 2];
        v[i].v = m->uv[i * 2 + 1];
        v[i].color = flash ? 0xFFFFFFFF : shadec(tint, sh);
        v[i].x = wx + x2;
        v[i].y = wy + y1;
        v[i].z = wz + z2;
    }
    if (flash) bind_tex(-1); else bind_mtex(id);
    sceGuDrawArray(GU_TRIANGLES, VTT, nv, 0, v);
}

/* returns 1 if a model was drawn (so the procedural body is skipped) */
static int draw_model_fighter(int x, int y, int face, int flash, int atk, int block,
                              int walk, int frame, int style, float z) {
    if (style < 0 || style >= MODEL_MAX || models[style].ntri == 0) return 0;
    int air = (y < GROUND - 80);
    float lean = 0.0f, sy = 1.0f, shift = 0.0f, bob = 0.0f;

    if (atk == 1)      { lean = 0.22f;  shift = 0.35f; }   /* light punch: lean in */
    else if (atk == 2) { lean = 0.38f;  shift = 0.70f; }   /* heavy punch: big lunge */
    else if (atk == 3) { lean = -0.18f; shift = 0.30f; }   /* light kick: lean back */
    else if (atk == 4) { lean = -0.32f; shift = 0.50f; }   /* heavy kick: lean way back */
    else if (block)    { lean = -0.10f; sy = 0.93f; }      /* guard: sink a little */
    else if (walk)     { lean = 0.07f;  bob = fabsf(sinf(frame * 0.35f)) * 0.12f; }
    if (air && atk == 0) { lean = 0.05f; sy = 1.04f; }     /* stretch in the air */

    V3 p = wp((float)x + 20.0f, (float)(y + 80), z);        /* feet position */
    float yaw = (face < 0 ? 3.1415927f : 0.0f) - MODEL_TURN * (float)face;
    model_draw(style, p.x + shift * (float)face, p.y + bob, p.z, yaw, lean, sy, 0xFFFFFFFF, flash);
    return 1;
}

static int draw_model_fallen(int x, int style) {
    if (style < 0 || style >= MODEL_MAX || models[style].ntri == 0) return 0;
    V3 p = wp((float)x + 43.0f, (float)GROUND, 0.0f);
    model_draw(style, p.x, p.y - 1.5f, p.z, 0.0f, -1.5707963f, 1.0f, 0xFFFFFFFF, 0);
    return 1;
}

#endif
