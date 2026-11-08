#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

PSP_MODULE_INFO("JojoFighter", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define BUF_W 512
#define SCR_W 480
#define SCR_H 272
#define W 480
#define H 272
#define GROUND 220
#define MAXHP 125
#define ROUNDTIME 3600   /* 60 seconds at 60 fps */

static unsigned int __attribute__((aligned(64))) list[262144];

typedef struct { uint32_t color; float x, y, z; } Vtx;
typedef struct { float x, y, z; } V3;

#define VT3 (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define VT2 (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

static V3 v3(float x, float y, float z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }

/* characters: 0 = Brawler, 1 = Speedster, 2 = Heavy */
static const int cspd[3]  = {3, 4, 2};
static const int cpun[3]  = {10, 8, 14};
static const int ckick[3] = {15, 12, 20};
static const int cjmp[3]  = {12, 14, 10};
static const uint32_t ccol[3] = {0xFF30A0E0, 0xFF3030C0, 0xFF40C030};
static const char *cname[3] = {"BRAWLER", "SPEEDSTER", "HEAVY"};
static const char *cdesc[3] = {"BALANCED", "FAST + JUMPY", "SLOW + STRONG"};
static const int cfx[3] = {80, 220, 360};

/* difficulty: 0 easy, 1 normal, 2 hard */
static const int espdv[3] = {1, 2, 3};
static const int ecdb[3]  = {70, 40, 22};
static const int ecdr[3]  = {40, 40, 20};
static const int eblk[3]  = {40, 20, 10};
static const int edmg[3]  = {70, 100, 120};
static const char *lvname[3] = {"EASY", "NORMAL", "HARD"};
static const uint32_t lvcol[3] = {0xFF30FF30, 0xFF00E0FF, 0xFF3030FF};

/* moves: 1 light punch, 2 heavy punch, 3 light kick, 4 heavy kick */
static const int mdur[5]   = {0, 10, 20, 14, 26};
static const int mstart[5] = {0, 3, 8, 5, 12};
static const int mkb[5]    = {0, 8, 20, 12, 30};
static const int mshk[5]   = {0, 3, 7, 4, 11};
static const int mmtr[5]   = {0, 6, 12, 7, 13};

static int movedmg(int type, int ch) {
    int base = (type <= 2) ? cpun[ch] : ckick[ch];
    int pct = (type == 1 || type == 3) ? 70 : 150;
    int d = base * pct / 100;
    return d < 1 ? 1 : d;
}

static void atkbox(int type, int x, int y, int face,
                   int *rx, int *ry, int *rw, int *rh) {
    int w, h, o;
    if (type == 1)      { w = 26; h = 14; o = 20; }
    else if (type == 2) { w = 34; h = 16; o = 18; }
    else if (type == 3) { w = 38; h = 14; o = 54; }
    else                { w = 52; h = 18; o = 36; }
    *rw = w; *rh = h; *ry = y + o;
    *rx = (face == 1) ? x + 40 : x - w;
}

static int exit_cb(int a, int b, void *c) { sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp) {
    int id = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int t = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

static unsigned int seed = 12345;
static int rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) & 0x7fff;
}

static int clampx(int x) {
    if (x < 0) return 0;
    if (x > W - 40) return W - 40;
    return x;
}

static int overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static int barw(int hp) {
    if (hp < 0) hp = 0;
    return hp * 200 / MAXHP;
}

/* =====================================================================
   2D drawing (screen pixels) - used for UI
   ===================================================================== */
static void rect(int x, int y, int w, int h, uint32_t c) {
    Vtx *v = (Vtx *)sceGuGetMemory(2 * sizeof(Vtx));
    v[0].color = c; v[0].x = (float)x;       v[0].y = (float)y;       v[0].z = 0;
    v[1].color = c; v[1].x = (float)(x + w); v[1].y = (float)(y + h); v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, VT2, 2, 0, v);
}

static void disc(int cx, int cy, int r, uint32_t c) {
    int n = 24;
    Vtx *v = (Vtx *)sceGuGetMemory((n + 2) * sizeof(Vtx));
    v[0].color = c; v[0].x = (float)cx; v[0].y = (float)cy; v[0].z = 0;
    for (int i = 0; i <= n; i++) {
        float a = i * 6.2831853f / n;
        v[i + 1].color = c;
        v[i + 1].x = cx + cosf(a) * r;
        v[i + 1].y = cy + sinf(a) * r;
        v[i + 1].z = 0;
    }
    sceGuDrawArray(GU_TRIANGLE_FAN, VT2, n + 2, 0, v);
}

static void line(int x0, int y0, int x1, int y1, int t, uint32_t c) {
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) { rect(x0 - t / 2, y0 - t / 2, t, t, c); return; }
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    Vtx *v = (Vtx *)sceGuGetMemory(4 * sizeof(Vtx));
    v[0].color = c; v[0].x = x0 + nx; v[0].y = y0 + ny; v[0].z = 0;
    v[1].color = c; v[1].x = x0 - nx; v[1].y = y0 - ny; v[1].z = 0;
    v[2].color = c; v[2].x = x1 - nx; v[2].y = y1 - ny; v[2].z = 0;
    v[3].color = c; v[3].x = x1 + nx; v[3].y = y1 + ny; v[3].z = 0;
    sceGuDrawArray(GU_TRIANGLE_FAN, VT2, 4, 0, v);
}

/* =====================================================================
   Text (queued, printed after the GPU finishes the frame)
   ===================================================================== */
typedef struct { int col, row; uint32_t color; char s[48]; } Txt;
static Txt txq[96];
static int ntx = 0;

static void qtext(int col, int row, uint32_t color, const char *s) {
    if (ntx >= 96) return;
    if (col < 0) col = 0;
    if (col > 67) col = 67;
    if (row < 0) row = 0;
    if (row > 33) row = 33;
    txq[ntx].col = col; txq[ntx].row = row; txq[ntx].color = color;
    int i = 0;
    while (s[i] && i < 47) { txq[ntx].s[i] = s[i]; i++; }
    txq[ntx].s[i] = 0;
    ntx++;
}

static void textx(int xc, int row, uint32_t color, const char *s) {
    int n = 0;
    while (s[n]) n++;
    qtext((xc - n * 7 / 2) / 7, row, color, s);
}

static void textl(int col, int row, uint32_t color, const char *s) {
    qtext(col, row, color, s);
}

static void flush_text(int offset) {
    pspDebugScreenSetOffset(offset);
    for (int i = 0; i < ntx; i++) {
        pspDebugScreenSetTextColor(txq[i].color);
        pspDebugScreenSetXY(txq[i].col, txq[i].row);
        pspDebugScreenPrintf("%s", txq[i].s);
    }
    ntx = 0;
}

static int put_int(char *b, int v) {
    char t[8];
    int n = 0, k = 0;
    if (v <= 0) t[n++] = '0';
    while (v > 0 && n < 7) { t[n++] = '0' + v % 10; v /= 10; }
    while (n > 0) b[k++] = t[--n];
    return k;
}

static void combo_str(char *buf, int n) {
    int k = put_int(buf, n);
    const char *s = " HITS";
    for (int i = 0; s[i]; i++) buf[k++] = s[i];
    buf[k] = 0;
}

/* =====================================================================
   3D helpers
   World units: 1 unit = 20 old pixels. Ground is y = 0, fighters ~4 tall.
   ===================================================================== */
static int g_flat = 0;   /* 1 = no shading */
static V3 cam_eye, cam_ctr;

static V3 wp(float px_, float py_, float z) {
    return v3((px_ - 240.0f) / 20.0f, ((float)GROUND - py_) / 20.0f, z);
}

static uint32_t shadec(uint32_t c, float f) {
    if (f > 1.0f) f = 1.0f;
    if (f < 0.0f) f = 0.0f;
    uint32_t r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255;
    r = (uint32_t)(r * f); g = (uint32_t)(g * f); b = (uint32_t)(b * f);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

#define PV(P) do { v[k].color = col; v[k].x = (P)->x; v[k].y = (P)->y; v[k].z = (P)->z; k++; } while (0)

/* c[0..3] = one end, c[4..7] = other end */
static void prism(const V3 *c, uint32_t color) {
    static const int F[6][4] = {{0,1,2,3},{4,5,6,7},{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7}};
    float mx = 0, my = 0, mz = 0;
    for (int i = 0; i < 8; i++) { mx += c[i].x; my += c[i].y; mz += c[i].z; }
    mx /= 8; my /= 8; mz /= 8;

    Vtx *v = (Vtx *)sceGuGetMemory(36 * sizeof(Vtx));
    int k = 0;
    for (int f = 0; f < 6; f++) {
        const V3 *a = &c[F[f][0]], *b = &c[F[f][1]], *cc = &c[F[f][2]], *d = &c[F[f][3]];
        float e1x = b->x - a->x, e1y = b->y - a->y, e1z = b->z - a->z;
        float e2x = d->x - a->x, e2y = d->y - a->y, e2z = d->z - a->z;
        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len < 1e-6f) len = 1.0f;
        nx /= len; ny /= len; nz /= len;
        float qx = (a->x + cc->x) * 0.5f - mx;
        float qy = (a->y + cc->y) * 0.5f - my;
        float qz = (a->z + cc->z) * 0.5f - mz;
        if (nx * qx + ny * qy + nz * qz < 0) { nx = -nx; ny = -ny; nz = -nz; }
        float sh = 1.0f;
        if (!g_flat) {
            float dp = nx * -0.348f + ny * 0.795f + nz * 0.497f;
            if (dp < 0) dp = 0;
            sh = 0.45f + 0.55f * dp;
        }
        uint32_t col = shadec(color, sh);
        PV(a); PV(b); PV(cc);
        PV(a); PV(cc); PV(d);
    }
    sceGuDrawArray(GU_TRIANGLES, VT3, 36, 0, v);
}

static void box(float cx, float cy, float cz, float w, float h, float d, uint32_t col) {
    float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
    float y0 = cy - h * 0.5f, y1 = cy + h * 0.5f;
    float z0 = cz - d * 0.5f, z1 = cz + d * 0.5f;
    V3 c[8];
    c[0] = v3(x0, y0, z0); c[1] = v3(x1, y0, z0); c[2] = v3(x1, y0, z1); c[3] = v3(x0, y0, z1);
    c[4] = v3(x0, y1, z0); c[5] = v3(x1, y1, z0); c[6] = v3(x1, y1, z1); c[7] = v3(x0, y1, z1);
    prism(c, col);
}

static void limb(V3 a, V3 b, float t, float d, uint32_t col) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    float nx, ny;
    if (len < 0.001f) { nx = 1.0f; ny = 0.0f; }
    else { nx = -dy / len; ny = dx / len; }
    nx *= t * 0.5f; ny *= t * 0.5f;
    float hz = d * 0.5f;
    V3 c[8];
    c[0] = v3(a.x - nx, a.y - ny, a.z - hz);
    c[1] = v3(a.x + nx, a.y + ny, a.z - hz);
    c[2] = v3(a.x + nx, a.y + ny, a.z + hz);
    c[3] = v3(a.x - nx, a.y - ny, a.z + hz);
    c[4] = v3(b.x - nx, b.y - ny, b.z - hz);
    c[5] = v3(b.x + nx, b.y + ny, b.z - hz);
    c[6] = v3(b.x + nx, b.y + ny, b.z + hz);
    c[7] = v3(b.x - nx, b.y - ny, b.z + hz);
    prism(c, col);
}

static void qv(Vtx *v, int *k, float x, float y, float z, uint32_t c) {
    v[*k].color = c; v[*k].x = x; v[*k].y = y; v[*k].z = z; (*k)++;
}

static void quadxy(Vtx *v, int *k, float x0, float y0, float x1, float y1, float z, uint32_t c) {
    qv(v, k, x0, y0, z, c); qv(v, k, x1, y0, z, c); qv(v, k, x1, y1, z, c);
    qv(v, k, x0, y0, z, c); qv(v, k, x1, y1, z, c); qv(v, k, x0, y1, z, c);
}

static void quadxz(Vtx *v, int *k, float x0, float z0, float x1, float z1, float y, uint32_t c) {
    qv(v, k, x0, y, z0, c); qv(v, k, x1, y, z0, c); qv(v, k, x1, y, z1, c);
    qv(v, k, x0, y, z0, c); qv(v, k, x1, y, z1, c); qv(v, k, x0, y, z1, c);
}

static void disc3(float cx, float cy, float cz, float r, uint32_t c) {
    int n = 24;
    Vtx *v = (Vtx *)sceGuGetMemory((n + 2) * sizeof(Vtx));
    v[0].color = c; v[0].x = cx; v[0].y = cy; v[0].z = cz;
    for (int i = 0; i <= n; i++) {
        float a = i * 6.2831853f / n;
        v[i + 1].color = c;
        v[i + 1].x = cx + cosf(a) * r;
        v[i + 1].y = cy + sinf(a) * r;
        v[i + 1].z = cz;
    }
    sceGuDrawArray(GU_TRIANGLE_FAN, VT3, n + 2, 0, v);
}

static void shadow(float wx, float height) {
    float s = 1.3f / (1.0f + height * 0.35f);
    uint32_t c = 0xFF0C200C;
    Vtx *v = (Vtx *)sceGuGetMemory(4 * sizeof(Vtx));
    v[0].color = c; v[0].x = wx - s; v[0].y = 0.03f; v[0].z = 0.9f;
    v[1].color = c; v[1].x = wx + s; v[1].y = 0.03f; v[1].z = 0.9f;
    v[2].color = c; v[2].x = wx + s; v[2].y = 0.03f; v[2].z = -0.9f;
    v[3].color = c; v[3].x = wx - s; v[3].y = 0.03f; v[3].z = -0.9f;
    sceGuDrawArray(GU_TRIANGLE_FAN, VT3, 4, 0, v);
}

static void set_camera(V3 eye, V3 ctr) {
    cam_eye = eye; cam_ctr = ctr;
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(60.0f, 480.0f / 272.0f, 3.0f, 150.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    ScePspFVector3 e, c, u;
    e.x = eye.x; e.y = eye.y; e.z = eye.z;
    c.x = ctr.x; c.y = ctr.y; c.z = ctr.z;
    u.x = 0.0f; u.y = 1.0f; u.z = 0.0f;
    sceGumLookAt(&e, &c, &u);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
}

/* world point -> screen pixel (for text placement) */
static int project(float x, float y, float z, int *sx, int *sy) {
    float fx = cam_ctr.x - cam_eye.x, fy = cam_ctr.y - cam_eye.y, fz = cam_ctr.z - cam_eye.z;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    fx /= fl; fy /= fl; fz /= fl;
    float rx = -fz, ry = 0.0f, rz = fx;
    float rl = sqrtf(rx * rx + rz * rz);
    rx /= rl; rz /= rl;
    float ux = ry * fz - rz * fy;
    float uy = rz * fx - rx * fz;
    float uz = rx * fy - ry * fx;
    float px_ = x - cam_eye.x, py_ = y - cam_eye.y, pz_ = z - cam_eye.z;
    float xc = px_ * rx + py_ * ry + pz_ * rz;
    float yc = px_ * ux + py_ * uy + pz_ * uz;
    float zc = px_ * fx + py_ * fy + pz_ * fz;
    if (zc < 0.1f) { *sx = 240; *sy = 136; return 0; }
    *sx = (int)(240.0f + 235.6f * xc / zc);
    *sy = (int)(136.0f - 235.6f * yc / zc);
    return 1;
}

/* ---------- hit sparks (stored in old pixel coordinates) ---------- */
typedef struct { int x, y, t, kind; } Spark;   /* kind: 0 hit, 1 big hit, 2 blocked */
static Spark sparks[8];

static void add_spark(int x, int y, int kind) {
    for (int i = 0; i < 8; i++) {
        if (sparks[i].t <= 0) {
            sparks[i].x = x; sparks[i].y = y; sparks[i].t = 8; sparks[i].kind = kind;
            return;
        }
    }
}

static void draw_sparks(void) {
    g_flat = 1;
    for (int i = 0; i < 8; i++) {
        if (sparks[i].t <= 0) continue;
        int k = sparks[i].kind;
        float L = ((8 - sparks[i].t) * 2 + 6 + (k == 1 ? 6 : 0)) / 20.0f;
        uint32_t c = (k == 2) ? 0xFFFFE0A0 : 0xFF30D0FF;
        float x = (sparks[i].x - 240.0f) / 20.0f;
        float y = ((float)GROUND - sparks[i].y) / 20.0f;
        float z = 1.3f;
        float l2 = L * 0.66f;
        limb(v3(x - L, y, z), v3(x + L, y, z), 0.18f, 0.18f, c);
        limb(v3(x, y - L, z), v3(x, y + L, z), 0.18f, 0.18f, c);
        limb(v3(x - l2, y - l2, z), v3(x + l2, y + l2, z), 0.18f, 0.18f, c);
        limb(v3(x - l2, y + l2, z), v3(x + l2, y - l2, z), 0.18f, 0.18f, c);
        box(x, y, z, 0.3f, 0.3f, 0.3f, 0xFFFFFFFF);
    }
    g_flat = 0;
}

/* ---------- 3D stage ---------- */
static void draw_stage(int frame) {
    g_flat = 0;

    /* horizon glow */
    {
        Vtx *v = (Vtx *)sceGuGetMemory(6 * sizeof(Vtx));
        int k = 0;
        uint32_t lo = 0xFF481C14, hi = 0xFF2A0C06;
        qv(v, &k, -160, 0, -52, lo); qv(v, &k, 160, 0, -52, lo); qv(v, &k, 160, 16, -52, hi);
        qv(v, &k, -160, 0, -52, lo); qv(v, &k, 160, 16, -52, hi); qv(v, &k, -160, 16, -52, hi);
        sceGuDrawArray(GU_TRIANGLES, VT3, 6, 0, v);
    }

    /* stars */
    {
        Vtx *v = (Vtx *)sceGuGetMemory(30 * 6 * sizeof(Vtx));
        int k = 0;
        for (int i = 0; i < 30; i++) {
            if ((frame / 8 + i) % 5 == 0) continue;
            float x = -60.0f + (float)((i * 37) % 121);
            float y = 14.0f + (float)((i * 53) % 30);
            quadxy(v, &k, x, y, x + 0.5f, y + 0.5f, -55.0f, 0xFFFFFFFF);
        }
        if (k > 0) sceGuDrawArray(GU_TRIANGLES, VT3, k, 0, v);
    }

    /* moon (crescent) */
    disc3(20.0f, 24.0f, -50.0f, 4.5f, 0xFFC0F8FF);
    disc3(22.2f, 25.2f, -49.5f, 4.0f, 0xFF2A0C06);

    /* ground stripes */
    {
        Vtx *v = (Vtx *)sceGuGetMemory(16 * 6 * sizeof(Vtx));
        int k = 0;
        for (int i = 0; i < 16; i++) {
            float x0 = -48.0f + i * 6.0f;
            quadxz(v, &k, x0, -46.0f, x0 + 6.0f, 26.0f, 0.0f,
                   (i & 1) ? 0xFF285028 : 0xFF204020);
        }
        sceGuDrawArray(GU_TRIANGLES, VT3, k, 0, v);
    }

    /* skyline */
    for (int b = 0; b < 17; b++) {
        float x = -48.0f + b * 6.0f;
        float h = 6.0f + (float)((b * 37) % 11);
        box(x, h * 0.5f, -26.0f, 5.0f, h, 4.0f, 0xFF140804);

        Vtx *v = (Vtx *)sceGuGetMemory(150 * sizeof(Vtx));
        int n = 0;
        for (float y = 1.5f; y < h - 1.0f; y += 2.0f) {
            for (int c = 0; c < 3; c++) {
                float wx = x - 1.5f + c * 1.5f;
                if (((b * 7 + c * 3 + (int)y * 5) % 5) < 2)
                    quadxy(v, &n, wx - 0.3f, y - 0.35f, wx + 0.3f, y + 0.35f, -23.8f, 0xFF40C8F0);
            }
        }
        if (n > 0) sceGuDrawArray(GU_TRIANGLES, VT3, n, 0, v);
    }
}

/* =====================================================================
   3D fighter
   atk: 0 none, 1 light punch, 2 heavy punch, 3 light kick, 4 heavy kick
   style: 0 brawler, 1 speedster, 2 heavy, 3 stand
   ===================================================================== */
static void draw_fighter(int x, int y, int face, uint32_t col,
                         int atk, int block, int walk, int frame, int style, float z) {
    float cx = (float)x + 20.0f, ty = (float)y;
    int air = (y < GROUND - 80);
    V3 p;
    g_flat = (col == 0xFFFFFFFF);

#define J(dx, dy) wp(cx + (float)(dx), ty + (float)(dy), z)

    /* head + eye */
    p = J(0, 10);
    box(p.x, p.y, p.z, 1.0f, 1.0f, 1.0f, col);
    {
        int sf = g_flat;
        g_flat = 1;
        box(p.x + face * 0.22f, p.y + 0.1f, p.z + 0.53f, 0.26f, 0.26f, 0.1f, 0xFF00FFFF);
        g_flat = sf;
    }

    /* torso */
    {
        float tw = (style == 2) ? 1.3f : 0.7f;
        float td = (style == 2) ? 0.9f : 0.55f;
        limb(J(0, 20), J(0, 50), tw, td, col);
    }

    /* arms */
    int fx, fy, bx, by;
    if (atk == 1)      { fx = face * 38; fy = 30; bx = -face * 10; by = 42; }
    else if (atk == 2) { fx = face * 58; fy = 26; bx = -face * 16; by = 34; }
    else if (block)    { fx = face * 16; fy = 16; bx = face * 8;   by = 14; }
    else               { fx = face * 14; fy = 42; bx = -face * 12; by = 42; }
    limb(J(0, 28), J(fx, fy), 0.36f, 0.36f, col);
    limb(J(0, 28), J(bx, by), 0.36f, 0.36f, col);

    /* legs */
    int l1x, l1y, l2x, l2y;
    int spread = ((frame / 6) % 2) ? 14 : 4;
    if (atk == 3)      { l1x = face * 50; l1y = 64; l2x = -face * 6; l2y = 80; }
    else if (atk == 4) { l1x = face * 66; l1y = 42; l2x = -face * 8; l2y = 80; }
    else if (air)      { l1x = 10; l1y = 70; l2x = -10; l2y = 72; }
    else if (walk)     { l1x = spread; l1y = 80; l2x = -spread; l2y = 80; }
    else               { l1x = 8; l1y = 80; l2x = -8; l2y = 80; }
    {
        float lt = (style == 2) ? 0.55f : 0.4f;
        limb(J(0, 50), J(l1x, l1y), lt, lt, col);
        limb(J(0, 50), J(l2x, l2y), lt, lt, col);
    }

    if (style == 3) {
        /* Stand: horns + chest gem */
        p = J(-6, -3); box(p.x, p.y, p.z, 0.2f, 0.45f, 0.25f, 0xFFFFFF80);
        p = J(6, -3);  box(p.x, p.y, p.z, 0.2f, 0.45f, 0.25f, 0xFFFFFF80);
        p = J(0, 32);
        g_flat = 1;
        box(p.x, p.y, p.z + 0.3f, 0.35f, 0.35f, 0.2f, 0xFF00FFFF);
        g_flat = 0;
        return;
    }

    /* hands */
    p = J(fx, fy); box(p.x, p.y, p.z, 0.34f, 0.34f, 0.34f, 0xFFA0D0FF);
    p = J(bx, by); box(p.x, p.y, p.z, 0.34f, 0.34f, 0.34f, 0xFFA0D0FF);

    if (style == 0) {
        /* cap + brim */
        p = J(0, -1);       box(p.x, p.y, p.z, 1.1f, 0.35f, 1.1f, 0xFFF0F0F0);
        p = J(face * 10, 3); box(p.x, p.y, p.z, 0.6f, 0.15f, 0.95f, 0xFFF0F0F0);
        /* scarf + fluttering tail */
        p = J(0, 21);       box(p.x, p.y, p.z, 0.95f, 0.22f, 0.8f, 0xFF2020E0);
        {
            float wob = (float)(((frame / 5) % 2) * 3);
            float th = (12.0f + wob) / 20.0f;
            p = J(-face * 9, 22.0f + (12.0f + wob) * 0.5f);
            box(p.x, p.y, p.z, 0.25f, th, 0.3f, 0xFF2020E0);
        }
        /* belt */
        p = J(0, 48); box(p.x, p.y, p.z, 0.6f, 0.15f, 0.6f, 0xFF00D0FF);
        /* boots */
        p = J(l1x, l1y); box(p.x, p.y, p.z, 0.45f, 0.22f, 0.55f, 0xFFF0F0F0);
        p = J(l2x, l2y); box(p.x, p.y, p.z, 0.45f, 0.22f, 0.55f, 0xFFF0F0F0);
    } else if (style == 1) {
        /* spiky hair */
        p = J(-7, -1);  box(p.x, p.y, p.z, 0.2f, 0.4f, 0.3f, 0xFF30D0FF);
        p = J(0, -3);   box(p.x, p.y, p.z, 0.25f, 0.6f, 0.3f, 0xFF30D0FF);
        p = J(6, -1);   box(p.x, p.y, p.z, 0.2f, 0.4f, 0.3f, 0xFF30D0FF);
        /* headband */
        p = J(0, 4);    box(p.x, p.y, p.z, 1.05f, 0.15f, 1.05f, 0xFFFFFFFF);
        /* belt */
        p = J(0, 48);   box(p.x, p.y, p.z, 0.6f, 0.15f, 0.6f, 0xFFFFFFFF);
        /* boots */
        p = J(l1x, l1y); box(p.x, p.y, p.z, 0.45f, 0.22f, 0.55f, 0xFF202020);
        p = J(l2x, l2y); box(p.x, p.y, p.z, 0.45f, 0.22f, 0.55f, 0xFF202020);
    } else {
        /* heavy: shoulder pads, mohawk, big belt */
        p = J(0, 25);   box(p.x, p.y, p.z, 1.7f, 0.3f, 1.2f, 0xFF909090);
        p = J(0, -3);   box(p.x, p.y, p.z, 0.25f, 0.6f, 0.35f, 0xFF2080FF);
        p = J(0, 48);   box(p.x, p.y, p.z, 0.8f, 0.2f, 0.95f, 0xFF30D0FF);
        p = J(l1x, l1y); box(p.x, p.y, p.z, 0.5f, 0.25f, 0.65f, 0xFF303030);
        p = J(l2x, l2y); box(p.x, p.y, p.z, 0.5f, 0.25f, 0.65f, 0xFF303030);
    }
    g_flat = 0;
#undef J
}

static void draw_fallen(int x, uint32_t col) {
    g_flat = 0;
    V3 hd = wp((float)x + 8.0f, (float)GROUND - 10.0f, 0.0f);
    box(hd.x, hd.y, hd.z, 1.0f, 1.0f, 1.0f, col);
    limb(wp(x + 18.0f, GROUND - 8.0f, 0), wp(x + 55.0f, GROUND - 8.0f, 0), 0.5f, 0.6f, col);
    limb(wp(x + 55.0f, GROUND - 8.0f, 0), wp(x + 75.0f, GROUND - 14.0f, 0), 0.4f, 0.4f, col);
    limb(wp(x + 55.0f, GROUND - 8.0f, 0), wp(x + 75.0f, GROUND - 2.0f, 0), 0.4f, 0.4f, col);
    limb(wp(x + 25.0f, GROUND - 8.0f, 0), wp(x + 35.0f, GROUND - 22.0f, 0), 0.36f, 0.36f, col);
}

static void draw_shield(float xpx, float ypx) {
    V3 p = wp(xpx, ypx, 0.0f);
    g_flat = 1;
    box(p.x, p.y, p.z, 0.3f, 2.5f, 1.5f, 0xFF00E0FF);
    g_flat = 0;
}

/* =====================================================================
   Frame control
   ===================================================================== */
static int drawoff = 0;

static void begin_frame(void) {
    sceGuStart(GU_DIRECT, list);
    sceGuClearColor(0xFF2A0C06);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuEnable(GU_DEPTH_TEST);
}

static void end_frame(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    flush_text(drawoff);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    drawoff = (drawoff == 0) ? 0x88000 : 0;
}

static void init_gu(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)0x88000, BUF_W);
    sceGuDepthBuffer((void *)0x110000, BUF_W);
    sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(0xc350, 0x2710);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/* =====================================================================
   main
   ===================================================================== */
int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    pspDebugScreenEnableBackColor(0);
    init_gu();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    int state = 0;        /* 0 title, 1 fight, 2 match over */
    int paused = 0;       /* 0 no, 1 menu, 2 move list */
    int pcur = 0;         /* pause menu cursor */
    int pc = 0, ec = 1;   /* chosen characters */
    int lvl = 1;          /* difficulty */
    int training = 0;
    int newround = 1;
    int matchwon = 0, mt = 0;

    /* player */
    int px = 100, py = GROUND - 80, vy = 0, pface = 1;
    int php = MAXHP, pflash = 0, pmeter = 0;
    int patk = 0, pt = 0, phit = 0;
    int psp = 0, pblock = 0, pwalk = 0;

    /* enemy */
    int ex = 340, eface = -1, ehp = MAXHP, eflash = 0, emeter = 0;
    int eatk = 0, et = 0, ecd = 60, ehit = 0;
    int eblock = 0, ebcd = 0, ewalk = 0, esp = 0;

    int ko = 0, pwon = 0, pwins = 0, ewins = 0, timeup = 0;
    int pcombo = 0, pcc = 0, ecombo = 0, ecc = 0;
    int shake = 0, rtime = ROUNDTIME;
    int frame = 0;
    char buf[32];
    SceCtrlData pad;
    unsigned int prev = 0;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;
        frame++;

        begin_frame();

        /* ================= TITLE ================= */
        if (state == 0) {
            if ((pressed & PSP_CTRL_LEFT)  && pc > 0) pc--;
            if ((pressed & PSP_CTRL_RIGHT) && pc < 2) pc++;
            if ((pressed & PSP_CTRL_UP)    && lvl < 2) lvl++;
            if ((pressed & PSP_CTRL_DOWN)  && lvl > 0) lvl--;
            if (pressed & PSP_CTRL_SQUARE) training = !training;
            if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
                ec = (pc + 1 + (frame & 1)) % 3;
                pwins = 0; ewins = 0;
                newround = 1;
                paused = 0;
                state = 1;
            }

            set_camera(v3(sinf(frame * 0.012f) * 2.0f, 3.2f, 17.0f), v3(0.0f, 2.2f, 0.0f));
            draw_stage(frame);
            for (int i = 0; i < 3; i++) {
                float wx = (cfx[i] + 20 - 240) / 20.0f;
                shadow(wx, 0.0f);
                draw_fighter(cfx[i], GROUND - 80, 1, ccol[i], 0, 0, pc == i, frame, i, 0.0f);
            }

            sceGuDisable(GU_DEPTH_TEST);
            rect(0, 10, W, 84, 0xFF000000);
            textx(240, 2, 0xFF00E0FF, "JOJO FIGHTER");
            textx(240, 4, 0xFFFFFFFF, "CHOOSE YOUR FIGHTER");

            sprintf(buf, "MODE: %s", training ? "TRAINING" : "VS CPU");
            textx(240, 6, training ? 0xFF30FF30 : 0xFFFFFFFF, buf);
            sprintf(buf, "LEVEL: %s", lvname[lvl]);
            textx(240, 8, lvcol[lvl], buf);
            if ((frame / 30) % 2 == 0)
                textx(240, 10, 0xFF30FF30, "PRESS X TO START");

            for (int i = 0; i < 3; i++) {
                float wx = (cfx[i] + 20 - 240) / 20.0f;
                int gx, gy;
                project(wx, 0.0f, 0.0f, &gx, &gy);
                textx(gx, gy / 8 + 1, 0xFFFFFFFF, cname[i]);
                textx(gx, gy / 8 + 3, 0xFFB0B0B0, cdesc[i]);
            }

            /* selection box */
            {
                float wx = (cfx[pc] + 20 - 240) / 20.0f;
                int ax, ay, bx_, by_;
                uint32_t sc = 0xFF00E0FF;
                project(wx - 1.9f, -0.4f, 0.0f, &ax, &by_);
                project(wx + 1.9f, 5.0f, 0.0f, &bx_, &ay);
                rect(ax, ay, bx_ - ax, 3, sc);
                rect(ax, by_, bx_ - ax, 3, sc);
                rect(ax, ay, 3, by_ - ay, sc);
                rect(bx_ - 3, ay, 3, by_ - ay + 3, sc);
            }

            textx(240, 27, 0xFFD0D0D0, "UP/DOWN: LEVEL   SQUARE: MODE");
            textx(240, 29, 0xFFD0D0D0, "SQ/TRI PUNCH  X/O KICK  UP JUMP  L BLOCK");
        }

        /* ================= FIGHT ================= */
        else if (state == 1) {

            if (paused == 1) {
                if (pressed & PSP_CTRL_UP)   pcur = (pcur + 3) % 4;
                if (pressed & PSP_CTRL_DOWN) pcur = (pcur + 1) % 4;
                if (pressed & PSP_CTRL_START) paused = 0;
                else if (pressed & PSP_CTRL_CROSS) {
                    if (pcur == 0) paused = 0;
                    else if (pcur == 1) { pwins = 0; ewins = 0; newround = 1; paused = 0; }
                    else if (pcur == 2) paused = 2;
                    else { paused = 0; state = 0; }
                }
            } else if (paused == 2) {
                if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_START)) paused = 1;
            } else if (pressed & PSP_CTRL_START) {
                paused = 1; pcur = 0;
            } else {

                if (newround) {
                    newround = 0;
                    px = 100; py = GROUND - 80; vy = 0; pface = 1;
                    php = MAXHP; pflash = 0; pmeter = training ? 100 : 0;
                    patk = 0; pt = 0; phit = 0; psp = 0; pblock = 0; pwalk = 0;
                    ex = 340; eface = -1; ehp = MAXHP; eflash = 0; emeter = 0;
                    eatk = 0; et = 0; ecd = 60; ehit = 0; eblock = 0; ebcd = 0; ewalk = 0; esp = 0;
                    ko = 0; timeup = 0;
                    pcombo = 0; pcc = 0; ecombo = 0; ecc = 0;
                    shake = 0; rtime = ROUNDTIME;
                    for (int i = 0; i < 8; i++) sparks[i].t = 0;
                }

                for (int i = 0; i < 8; i++) if (sparks[i].t > 0) sparks[i].t--;

                if (ko == 0) {
                    if (pcc > 0 && --pcc == 0) pcombo = 0;
                    if (ecc > 0 && --ecc == 0) ecombo = 0;
                    if (rtime > 0) rtime--;

                    int busy = (patk != 0 || psp > 0);
                    int onground = (py >= GROUND - 80);

                    /* block (hold L) */
                    pblock = 0;
                    if (!busy && onground && (pad.Buttons & PSP_CTRL_LTRIGGER)) pblock = 1;

                    /* movement */
                    pwalk = 0;
                    if (!busy && !pblock) {
                        if (pad.Buttons & PSP_CTRL_LEFT)  { px -= cspd[pc]; pface = -1; pwalk = 1; }
                        if (pad.Buttons & PSP_CTRL_RIGHT) { px += cspd[pc]; pface = 1;  pwalk = 1; }
                        px = clampx(px);
                        if ((pressed & PSP_CTRL_UP) && onground) vy = -cjmp[pc];
                    }
                    vy += 1;
                    py += vy;
                    if (py > GROUND - 80) { py = GROUND - 80; vy = 0; }

                    /* start attacks */
                    if (!busy && !pblock) {
                        int t = 0;
                        if (pressed & PSP_CTRL_SQUARE)        t = 1;
                        else if (pressed & PSP_CTRL_TRIANGLE) t = 2;
                        else if (pressed & PSP_CTRL_CROSS)    t = 3;
                        else if (pressed & PSP_CTRL_CIRCLE)   t = 4;
                        if (t) { patk = t; pt = mdur[t]; phit = 0; }
                        else if ((pressed & PSP_CTRL_RTRIGGER) && pmeter >= 100) { psp = 45; pmeter = 0; }
                    }

                    /* player attack */
                    if (patk > 0) {
                        int el = mdur[patk] - pt;
                        if (!phit && el >= mstart[patk]) {
                            int bx2, by2, bw2, bh2;
                            atkbox(patk, px, py, pface, &bx2, &by2, &bw2, &bh2);
                            if (overlap(bx2, by2, bw2, bh2, ex, GROUND - 80, 40, 80)) {
                                phit = 1;
                                int d = movedmg(patk, pc);
                                if (eblock > 0) {
                                    int c = d / 5; if (c < 1) c = 1;
                                    ehp -= c; emeter += 2;
                                    add_spark((eface == 1) ? ex + 44 : ex - 4, by2 + bh2 / 2, 2);
                                    shake = 2;
                                } else {
                                    ehp -= d; eflash = 6; pmeter += mmtr[patk]; emeter += 8;
                                    ex = clampx(ex + mkb[patk] * pface);
                                    add_spark(ex + 20, by2 + bh2 / 2, (patk == 2 || patk == 4) ? 1 : 0);
                                    shake = mshk[patk];
                                    pcombo = (pcc > 0) ? pcombo + 1 : 1;
                                    pcc = 70; ecombo = 0; ecc = 0;
                                }
                            }
                        }
                        if (--pt <= 0) patk = 0;
                    }

                    /* player Stand rush */
                    if (psp > 0) {
                        int ax = (pface == 1) ? px + 40 : px - 80;
                        if (psp % 6 == 0 && overlap(ax, py, 80, 80, ex, GROUND - 80, 40, 80)) {
                            if (eblock > 0) {
                                ehp -= 1;
                                add_spark((eface == 1) ? ex + 44 : ex - 4, py + 20 + rnd() % 40, 2);
                            } else {
                                ehp -= 4; eflash = 4;
                                ex = clampx(ex + 4 * pface);
                                add_spark(ex + 20, py + 15 + rnd() % 50, 0);
                                shake = 4;
                                pcombo = (pcc > 0) ? pcombo + 1 : 1;
                                pcc = 70; ecombo = 0; ecc = 0;
                            }
                        }
                        psp--;
                    }

                    /* ---------- enemy ---------- */
                    if (training) {
                        eface = (px >= ex) ? 1 : -1;
                        ewalk = 0; eatk = 0; esp = 0; eblock = 0;
                    } else {
                        int exprev = ex;
                        int dx = px - ex;
                        int dist = dx < 0 ? -dx : dx;
                        eface = (dx >= 0) ? 1 : -1;
                        if (ecd > 0) ecd--;
                        if (ebcd > 0) ebcd--;
                        if (eblock > 0) eblock--;

                        if ((patk > 0 || psp > 0) && dist < 110 && eblock == 0 && ebcd == 0 &&
                            eatk == 0 && esp == 0) {
                            if (rnd() % eblk[lvl] == 0) { eblock = 25; ebcd = 90; }
                        }

                        if (esp == 0 && eatk == 0 && eblock == 0 && emeter >= 100 && dist < 100) {
                            esp = 45; emeter = 0;
                        }

                        if (eatk == 0 && eblock == 0 && esp == 0) {
                            if (dist > 60) {
                                ex += eface * espdv[lvl];
                            } else if (ecd > 0 && dist < 45) {
                                ex -= eface;
                            } else if (ecd == 0) {
                                int r = rnd() % 100;
                                int t = (r < 40) ? 1 : (r < 60) ? 2 : (r < 85) ? 3 : 4;
                                eatk = t; et = mdur[t]; ehit = 0;
                                ecd = ecdb[lvl] + (rnd() % ecdr[lvl]) + ((t == 2 || t == 4) ? 12 : 0);
                            }
                        }
                        ex = clampx(ex);
                        ewalk = (ex != exprev);

                        /* enemy attack */
                        if (eatk > 0) {
                            int el = mdur[eatk] - et;
                            if (!ehit && el >= mstart[eatk]) {
                                int bx2, by2, bw2, bh2;
                                atkbox(eatk, ex, GROUND - 80, eface, &bx2, &by2, &bw2, &bh2);
                                if (overlap(bx2, by2, bw2, bh2, px, py, 40, 80)) {
                                    ehit = 1;
                                    int d = movedmg(eatk, ec) * edmg[lvl] / 100;
                                    if (d < 1) d = 1;
                                    if (pblock && pface == -eface) {
                                        int c = d / 5; if (c < 1) c = 1;
                                        php -= c; pmeter += 3; emeter += 2;
                                        add_spark((pface == 1) ? px + 44 : px - 4, by2 + bh2 / 2, 2);
                                        shake = 2;
                                    } else {
                                        php -= d; pflash = 6; pmeter += 8; emeter += mmtr[eatk];
                                        px = clampx(px + mkb[eatk] * eface);
                                        add_spark(px + 20, by2 + bh2 / 2, (eatk == 2 || eatk == 4) ? 1 : 0);
                                        shake = mshk[eatk];
                                        ecombo = (ecc > 0) ? ecombo + 1 : 1;
                                        ecc = 70; pcombo = 0; pcc = 0;
                                    }
                                }
                            }
                            if (--et <= 0) eatk = 0;
                        }

                        /* enemy Stand rush */
                        if (esp > 0) {
                            int ax = (eface == 1) ? ex + 40 : ex - 80;
                            if (esp % 6 == 0 && overlap(ax, GROUND - 80, 80, 80, px, py, 40, 80)) {
                                if (pblock && pface == -eface) {
                                    php -= 1;
                                    add_spark((pface == 1) ? px + 44 : px - 4, py + 20 + rnd() % 40, 2);
                                } else {
                                    php -= (4 * edmg[lvl] + 50) / 100;
                                    pflash = 4;
                                    px = clampx(px + 4 * eface);
                                    add_spark(px + 20, py + 15 + rnd() % 50, 0);
                                    shake = 4;
                                    ecombo = (ecc > 0) ? ecombo + 1 : 1;
                                    ecc = 70; pcombo = 0; pcc = 0;
                                }
                            }
                            esp--;
                        }
                    }

                    if (pmeter > 100) pmeter = 100;
                    if (emeter > 100) emeter = 100;
                    if (eflash > 0) eflash--;
                    if (pflash > 0) pflash--;

                    /* training: refill everything, no K.O. */
                    if (training) {
                        rtime = ROUNDTIME;
                        if (pcc == 0 && ecc == 0) {
                            if (ehp < MAXHP) ehp++;
                            if (php < MAXHP) php++;
                        }
                        if (ehp < 1) ehp = 1;
                        if (php < 1) php = 1;
                        if (pmeter < 100) pmeter++;
                    }

                    /* K.O. or time up */
                    if (!training && (php <= 0 || ehp <= 0 || rtime <= 0)) {
                        ko = 150;
                        timeup = (php > 0 && ehp > 0);
                        pwon = (ehp <= php);
                        if (pwon) pwins++; else ewins++;
                        patk = 0; psp = 0; eatk = 0; eblock = 0; esp = 0;
                        pblock = 0; pwalk = 0; ewalk = 0;
                        py = GROUND - 80; vy = 0;
                        pcombo = 0; ecombo = 0; pcc = 0; ecc = 0;
                        if (!timeup) shake = 14;
                    }
                } else {
                    ko--;
                    if (ko == 0 || (ko < 100 && (pressed & PSP_CTRL_CROSS))) {
                        ko = 0;
                        if (pwins >= 2 || ewins >= 2) {
                            matchwon = (pwins >= 2);
                            mt = 0;
                            state = 2;
                        } else {
                            newround = 1;
                        }
                    }
                }
            }

            /* ---------------- draw fight (3D) ---------------- */
            {
                float midpx = ((px + 20) + (ex + 20)) * 0.5f;
                float midx = (midpx - 240.0f) / 20.0f;
                float sep = (float)((px > ex) ? (px - ex) : (ex - px)) / 20.0f;
                float dist = 11.0f + sep * 0.3f;
                float camx = midx * 0.6f;
                float shx = 0.0f, shy = 0.0f;
                if (shake > 0) {
                    shx = ((frame & 1) ? 1.0f : -1.0f) * shake * 0.04f;
                    shy = ((frame & 2) ? 1.0f : -1.0f) * shake * 0.03f;
                    if (!paused) shake--;
                }
                set_camera(v3(camx + shx, 3.6f + shy, dist), v3(camx, 2.4f, 0.0f));
            }
            draw_stage(frame);

            float pwx = (px + 20 - 240) / 20.0f;
            float ewx = (ex + 20 - 240) / 20.0f;

            /* player side */
            if (ko > 0 && !pwon && !timeup) {
                draw_fallen(px, ccol[pc]);
            } else if (ko > 0 && !pwon && timeup) {
                shadow(pwx, 0.0f);
                draw_fighter(px, py, pface, ccol[pc], 0, 0, 0, frame, pc, 0.0f);
            } else {
                shadow(pwx, (GROUND - 80 - py) / 20.0f);
                if (psp > 0)
                    draw_fighter(px + pface * 35, py, pface, 0xFFE030A0,
                                 ((psp / 3) % 2) ? 1 : 0, 0, 0, frame, 3, -0.9f);
                draw_fighter(px, py, pface, pflash ? 0xFFFFFFFF : ccol[pc],
                             patk, pblock, pwalk, frame, pc, 0.0f);
                if (pblock) draw_shield((pface == 1) ? px + 44.0f : px - 4.0f, py + 40.0f);
            }

            /* enemy side */
            if (ko > 0 && pwon && !timeup) {
                draw_fallen(ex, ccol[ec]);
            } else if (ko > 0 && pwon && timeup) {
                shadow(ewx, 0.0f);
                draw_fighter(ex, GROUND - 80, eface, ccol[ec], 0, 0, 0, frame, ec, 0.0f);
            } else {
                shadow(ewx, 0.0f);
                if (esp > 0)
                    draw_fighter(ex + eface * 35, GROUND - 80, eface, 0xFF40E060,
                                 ((esp / 3) % 2) ? 1 : 0, 0, 0, frame, 3, -0.9f);
                draw_fighter(ex, GROUND - 80, eface, eflash ? 0xFFFFFFFF : ccol[ec],
                             eatk, eblock > 0, ewalk, frame, ec, 0.0f);
                if (eblock > 0) draw_shield((eface == 1) ? ex + 44.0f : ex - 4.0f, GROUND - 40.0f);
            }

            draw_sparks();

            /* ---- UI (2D) ---- */
            sceGuDisable(GU_DEPTH_TEST);

            /* health bars */
            rect(20, 10, 200, 14, 0xFF000000);
            rect(20, 10, barw(php), 14, 0xFF30C030);
            rect(W - 220, 10, 200, 14, 0xFF000000);
            rect(W - 220, 10, barw(ehp), 14, 0xFF30C030);

            /* special meters */
            rect(20, 28, 200, 8, 0xFF000000);
            rect(20, 28, pmeter * 2, 8, (pmeter >= 100) ? 0xFFE030A0 : 0xFF00D0FF);
            rect(W - 220, 28, 200, 8, 0xFF000000);
            rect(W - 220, 28, emeter * 2, 8, (emeter >= 100) ? 0xFF40E060 : 0xFF00D0FF);

            if (training) {
                textx(240, 4, 0xFF30FF30, "TRAINING");
                textx(240, 6, 0xFFB0B0B0, "START: PAUSE");
            } else {
                for (int i = 0; i < pwins && i < 2; i++) disc(26 + i * 14, 46, 4, 0xFF00E0FF);
                for (int i = 0; i < ewins && i < 2; i++) disc(W - 26 - i * 14, 46, 4, 0xFF00E0FF);

                int secs = (rtime + 59) / 60;
                buf[0] = '0' + secs / 10;
                buf[1] = '0' + secs % 10;
                buf[2] = 0;
                textx(240, 4, (secs <= 10) ? 0xFF3030FF : 0xFFFFFFFF, buf);
            }

            /* combo counters */
            if (pcombo >= 2) { combo_str(buf, pcombo); textx(60, 8, 0xFF00E0FF, buf); }
            if (ecombo >= 2) { combo_str(buf, ecombo); textx(420, 8, 0xFF00E0FF, buf); }

            /* Stand shouts (placed above the fighter's head) */
            {
                int sx2, sy2;
                if (psp > 0 && (frame / 4) % 2 == 0) {
                    project(pwx, (GROUND - py) / 20.0f + 1.2f, 0.0f, &sx2, &sy2);
                    textx(sx2, sy2 / 8, 0xFFE030A0, "ORA ORA ORA!");
                }
                if (esp > 0 && (frame / 4) % 2 == 0) {
                    project(ewx, 5.2f, 0.0f, &sx2, &sy2);
                    textx(sx2, sy2 / 8, 0xFF40E060, "MUDA MUDA!");
                }
            }

            /* K.O. / TIME UP banner */
            if (ko > 0) {
                rect(0, 85, W, 100, 0xFF000000);
                if (timeup) {
                    textx(240, 15, 0xFFFFFFFF, "TIME UP!");
                } else {
                    /* K */
                    line(180, 98, 180, 146, 8, 0xFF0030FF);
                    line(180, 122, 215, 98, 8, 0xFF0030FF);
                    line(180, 122, 215, 146, 8, 0xFF0030FF);
                    rect(226, 140, 8, 8, 0xFF0030FF);
                    /* O */
                    disc(268, 122, 25, 0xFF0030FF);
                    disc(268, 122, 14, 0xFF000000);
                    rect(306, 140, 8, 8, 0xFF0030FF);
                }
                if (pwon) textx(240, 21, 0xFF30FF30, "YOU WIN");
                else      textx(240, 21, 0xFF3030FF, "YOU LOSE");
            }

            /* ---- pause overlay ---- */
            if (paused == 1) {
                const char *opt[4];
                opt[0] = "RESUME";
                opt[1] = training ? "RESET POSITIONS" : "RESTART MATCH";
                opt[2] = "MOVE LIST";
                opt[3] = "QUIT TO TITLE";
                rect(0, 0, W, H, 0x99000000);
                rect(130, 46, 220, 160, 0xFF000000);
                rect(130, 46, 220, 2, 0xFF00E0FF);
                rect(130, 204, 220, 2, 0xFF00E0FF);
                rect(130, 46, 2, 160, 0xFF00E0FF);
                rect(348, 46, 2, 160, 0xFF00E0FF);
                textx(240, 8, 0xFF00E0FF, "PAUSED");
                for (int i = 0; i < 4; i++) {
                    if (i == pcur) sprintf(buf, "> %s <", opt[i]);
                    else           sprintf(buf, "%s", opt[i]);
                    textx(240, 11 + i * 2, (i == pcur) ? 0xFF00E0FF : 0xFFFFFFFF, buf);
                }
                textx(240, 21, 0xFFB0B0B0, "UP/DOWN + X");
            } else if (paused == 2) {
                rect(0, 0, W, H, 0x99000000);
                rect(110, 30, 260, 200, 0xFF000000);
                rect(110, 30, 260, 2, 0xFF00E0FF);
                rect(110, 228, 260, 2, 0xFF00E0FF);
                rect(110, 30, 2, 200, 0xFF00E0FF);
                rect(368, 30, 2, 200, 0xFF00E0FF);
                textx(240, 5, 0xFF00E0FF, "MOVE LIST");
                textl(18, 8,  0xFFFFFFFF, "SQUARE    LIGHT PUNCH");
                textl(18, 10, 0xFFFFFFFF, "TRIANGLE  HEAVY PUNCH");
                textl(18, 12, 0xFFFFFFFF, "X         LIGHT KICK");
                textl(18, 14, 0xFFFFFFFF, "CIRCLE    HEAVY KICK");
                textl(18, 16, 0xFFFFFFFF, "UP        JUMP");
                textl(18, 18, 0xFFFFFFFF, "L (HOLD)  BLOCK");
                textl(18, 20, 0xFFFFFFFF, "R         STAND RUSH");
                textx(240, 22, 0xFFB0B0B0, "(NEEDS A FULL METER)");
                textx(240, 24, 0xFF30D0FF, "HEAVY = MORE DAMAGE, SLOWER");
                textx(240, 26, 0xFF30FF30, "PRESS X TO GO BACK");
            }
        }

        /* ================= MATCH OVER ================= */
        else {
            mt++;
            if (mt > 40 && (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START))) {
                state = 0;
            }

            set_camera(v3(sinf(frame * 0.01f) * 3.0f, 3.4f, 13.0f), v3(0.0f, 2.2f, 0.0f));
            draw_stage(frame);
            if (matchwon) {
                shadow(0.0f, 0.0f);
                draw_fighter(220, GROUND - 80, 1, ccol[pc], 0, 0, 0, frame, pc, 0.0f);
            } else {
                draw_fallen(210, ccol[pc]);
            }

            sceGuDisable(GU_DEPTH_TEST);
            rect(0, 40, W, 60, 0xFF000000);
            if (matchwon) textx(240, 6, 0xFF30FF30, "YOU WIN THE MATCH!");
            else          textx(240, 6, 0xFF3030FF, "YOU LOST THE MATCH");
            buf[0] = '0' + pwins; buf[1] = ' '; buf[2] = '-'; buf[3] = ' ';
            buf[4] = '0' + ewins; buf[5] = 0;
            textx(240, 9, 0xFF00E0FF, buf);
            if (mt > 40 && (frame / 30) % 2 == 0)
                textx(240, 27, 0xFF30FF30, "PRESS X FOR TITLE SCREEN");
        }

        end_frame();
    }
    return 0;
}
