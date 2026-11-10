#include <pspkernel.h>
#include <psputils.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <png.h>

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

static unsigned int __attribute__((aligned(64))) list[524288];

typedef struct { uint32_t color; float x, y, z; } Vtx;
typedef struct { float u, v; uint32_t color; float x, y, z; } TVtx;
typedef struct { float x, y, z; } V3;

#define VT3 (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define VT2 (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define VTT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

static V3 v3(float x, float y, float z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }

/* characters: 0 = Brawler, 1 = Speedster, 2 = Heavy */
static const int cspd[3]  = {3, 4, 2};
static const int cpun[3]  = {10, 8, 12};
static const int ckick[3] = {15, 12, 17};
static const int cjmp[3]  = {12, 14, 10};
static const char *cname[3] = {"BRAWLER", "SPEEDSTER", "HEAVY"};
static const char *cdesc[3] = {"BALANCED", "FAST + JUMPY", "SLOW + STRONG"};
static const int cfx[3] = {80, 220, 360};
static const uint32_t cskin[3] = {0xFF78ACE0, 0xFF96BEEC, 0xFF648CC8};
static const uint32_t bootc[3] = {0xFFF0F0F0, 0xFF202020, 0xFF303030};

/* difficulty: 0 easy, 1 normal, 2 hard */
static const int ecdb[3]  = {70, 40, 22};
static const int ecdr[3]  = {40, 40, 20};
static const int eblk[3]  = {40, 20, 10};
static const int edmg[3]  = {70, 100, 120};
static const char *lvname[3] = {"EASY", "NORMAL", "HARD"};
static const uint32_t lvcol[3] = {0xFF30FF30, 0xFF00E0FF, 0xFF3030FF};

/* moves: 1 light punch, 2 heavy punch, 3 light kick, 4 heavy kick */
static const int mdur[5]   = {0, 15, 25, 19, 31};   /* total frames (each move is 5 frames = 0.09 s longer) */
static const int mstart[5] = {0, 5, 10, 7, 14};     /* frame where the blow lands */
static const int mact[5]   = {0, 7, 12, 9, 14};     /* how many frames the blow stays dangerous */
static const int mkb[5]    = {0, 8, 20, 12, 30};
static const int mshk[5]   = {0, 3, 7, 4, 11};
static const int mmtr[5]   = {0, 6, 12, 7, 13};
static const int mhs[5]    = {0, 4, 9, 6, 11};   /* HIT-STOP frames per move */

static int movedmg(int type, int ch) {
    int base = (type <= 2) ? cpun[ch] : ckick[ch];
    int pct = (type == 1 || type == 3) ? 70 : 150;
    int d = base * pct / 100;
    return d < 1 ? 1 : d;
}

/* how far apart (sideways) two fighters can be and still get hit: quick moves are "straight" */
static float ztol(int t) { return (t == 1 || t == 3) ? 0.55f : 1.2f; }

/* how far (sideways) an attack aimed along 'aim' misses a target when the line has turned to 'ang' */
static float latoff(int a, int b, float diff) {
    float d = (float)((a > b) ? (a - b) : (b - a)) / 20.0f;
    return fabsf(d * sinf(diff));
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
    if (x < -200) return -200;      /* the ring wall is the real limit, not the ends of the line */
    if (x > 700) return 700;
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
   TEXTURES: 12 images, 64x64. Built in code, and replaced by PNG files
   (head0.png ... stand.png) found next to EBOOT.PBP.
   ===================================================================== */
#define TEXSZ 64
#define NTEX 12
enum { T_HEAD0 = 0, T_BODY0 = 3, T_LEGS0 = 6, T_GROUND = 9, T_BUILD = 10, T_STAND = 11 };

static uint32_t __attribute__((aligned(16))) texdata[NTEX][TEXSZ * TEXSZ];
static const char *texname[NTEX] = {
    "head0.png", "head1.png", "head2.png",
    "body0.png", "body1.png", "body2.png",
    "legs0.png", "legs1.png", "legs2.png",
    "ground.png", "building.png", "stand.png"
};
static int ncustom = 0;
static char basedir[160] = "";
static int cur_tex = -2;

static void bind_tex(int t) {
    if (t == cur_tex) return;
    cur_tex = t;
    if (t < 0) { sceGuDisable(GU_TEXTURE_2D); return; }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, TEXSZ, TEXSZ, TEXSZ, texdata[t]);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
}

static unsigned int tseed = 777;
static int trnd(void) {
    tseed = tseed * 1664525u + 1013904223u;
    return (int)((tseed >> 16) & 0xFFFF);
}

static uint32_t tpx(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static uint32_t tadj(uint32_t c, int d) {
    return tpx((int)(c & 255) + d, (int)((c >> 8) & 255) + d, (int)((c >> 16) & 255) + d);
}

static void tput(int t, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= TEXSZ || y >= TEXSZ) return;
    texdata[t][y * TEXSZ + x] = c;
}

static void tfill(int t, int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            tput(t, x + i, y + j, c);
}

/* face texture: covers the FRONT HALF of the head.
   Edges and the top are hair, so the back of the head looks right. */
static void gen_head(int t, uint32_t skin, uint32_t hair, int kind) {
    uint32_t white = tpx(240, 240, 240), dark = tpx(24, 18, 28);
    for (int y = 0; y < TEXSZ; y++) {
        for (int x = 0; x < TEXSZ; x++) {
            uint32_t c = tadj(skin, trnd() % 9 - 4);
            if (y < 16 || (y < 46 && (x < 7 || x > 56))) c = tadj(hair, trnd() % 13 - 6);
            tput(t, x, y, c);
        }
    }
    /* eyes */
    tfill(t, 17, 29, 8, 5, white);
    tfill(t, 39, 29, 8, 5, white);
    tfill(t, 21, 30, 4, 4, dark);
    tfill(t, 39, 30, 4, 4, dark);
    /* nose */
    tfill(t, 30, 36, 4, 5, tadj(skin, -30));
    if (kind == 0) {            /* brawler: calm brows, smirk */
        tfill(t, 16, 26, 10, 2, dark);
        tfill(t, 38, 26, 10, 2, dark);
        tfill(t, 27, 46, 10, 2, tpx(150, 50, 50));
        tfill(t, 37, 45, 2, 1, tpx(150, 50, 50));
    } else if (kind == 1) {     /* speedster: sharp brows, grin */
        tfill(t, 16, 25, 5, 2, dark);
        tfill(t, 21, 27, 5, 2, dark);
        tfill(t, 43, 25, 5, 2, dark);
        tfill(t, 38, 27, 5, 2, dark);
        tfill(t, 26, 45, 12, 4, tpx(120, 30, 40));
        tfill(t, 27, 45, 10, 1, white);
    } else {                    /* heavy: thick brows, frown, scar */
        tfill(t, 15, 24, 11, 3, dark);
        tfill(t, 38, 24, 11, 3, dark);
        tfill(t, 27, 47, 10, 2, dark);
        tfill(t, 26, 48, 2, 1, dark);
        tfill(t, 36, 48, 2, 1, dark);
        tfill(t, 47, 34, 1, 9, tpx(170, 90, 80));
    }
}

static void gen_body(int t, uint32_t base, uint32_t accent, int kind) {
    for (int y = 0; y < TEXSZ; y++) {
        for (int x = 0; x < TEXSZ; x++) {
            uint32_t c = tadj(base, trnd() % 11 - 5);
            if (((x + y) & 7) == 0) c = tadj(c, -8);
            tput(t, x, y, c);
        }
    }
    if (kind == 0) {            /* jacket: zipper, collar, hem, emblem */
        tfill(t, 31, 0, 2, TEXSZ, tadj(base, -70));
        for (int y = 2; y < TEXSZ; y += 4) tfill(t, 29, y, 6, 1, accent);
        tfill(t, 0, 0, TEXSZ, 6, accent);
        tfill(t, 0, 57, TEXSZ, 7, accent);
        for (int dy = -6; dy <= 6; dy++) {
            int w = 6 - (dy < 0 ? -dy : dy);
            tfill(t, 44 - w, 24 + dy, w * 2 + 1, 1, accent);
        }
    } else if (kind == 1) {     /* racing stripes */
        tfill(t, 25, 0, 3, TEXSZ, accent);
        tfill(t, 36, 0, 3, TEXSZ, accent);
        tfill(t, 0, 0, TEXSZ, 4, accent);
        tfill(t, 0, 60, TEXSZ, 4, accent);
    } else {                    /* armor plates with rivets */
        for (int y = 0; y < TEXSZ; y++) {
            for (int x = 0; x < TEXSZ; x++) {
                if (x % 16 == 0 || y % 16 == 0) tput(t, x, y, tadj(base, -60));
                else if (x % 16 == 1 || y % 16 == 1) tput(t, x, y, tadj(base, 25));
            }
        }
        for (int gy = 0; gy < 4; gy++)
            for (int gx = 0; gx < 4; gx++)
                tfill(t, gx * 16 + 6, gy * 16 + 6, 3, 3, accent);
    }
}

static void gen_legs(int t, uint32_t base, uint32_t accent) {
    for (int y = 0; y < TEXSZ; y++)
        for (int x = 0; x < TEXSZ; x++)
            tput(t, x, y, tadj(base, trnd() % 9 - 4));
    tfill(t, 0, 0, 2, TEXSZ, tadj(base, -35));
    tfill(t, 32, 0, 1, TEXSZ, tadj(base, -25));
    tfill(t, 0, 24, TEXSZ, 6, tadj(base, -20));
    tfill(t, 0, 44, TEXSZ, 2, tadj(accent, -30));
    tfill(t, 0, 50, TEXSZ, 14, accent);
}

static void gen_ground(int t) {
    /* dark polished marble with gold tile lines (Tekken-style arena floor) */
    for (int y = 0; y < TEXSZ; y++) {
        for (int x = 0; x < TEXSZ; x++) {
            uint32_t c = tpx(74, 40, 22);
            c = tadj(c, trnd() % 13 - 6);
            if (((x / 32) + (y / 32)) & 1) c = tadj(c, 14);
            if ((x + y * 2) % 41 < 2) c = tadj(c, 22);
            if (x % 32 < 2 || y % 32 < 2) c = tpx(214, 160, 64);
            tput(t, x, y, c);
        }
    }
    for (int i = 0; i < 30; i++) tput(t, trnd() % TEXSZ, trnd() % TEXSZ, tpx(240, 200, 110));
}

static void gen_build(int t) {
    /* warm sandstone wall with carved panels */
    for (int y = 0; y < TEXSZ; y++) {
        for (int x = 0; x < TEXSZ; x++) {
            uint32_t c = tpx(132, 70, 38);
            c = tadj(c, trnd() % 9 - 4);
            if (x % 32 < 2) c = tpx(214, 160, 64);
            if (y % 32 < 2) c = tpx(176, 116, 52);
            if ((x % 32) > 6 && (x % 32) < 26 && (y % 32) > 6 && (y % 32) < 26) c = tadj(c, -16);
            tput(t, x, y, c);
        }
    }
}

static void gen_stand(int t) {
    for (int y = 0; y < TEXSZ; y++) {
        for (int x = 0; x < TEXSZ; x++) {
            int v = ((x + y) / 5) & 1;
            uint32_t c = v ? tpx(250, 250, 255) : tpx(205, 205, 225);
            c = tadj(c, trnd() % 13 - 6);
            if ((y % 16) < 2) c = tpx(255, 255, 255);
            tput(t, x, y, c);
        }
    }
}

static void gen_all(void) {
    gen_head(T_HEAD0 + 0, tpx(224, 172, 120), tpx(110, 70, 40), 0);
    gen_head(T_HEAD0 + 1, tpx(236, 190, 150), tpx(250, 210, 60), 1);
    gen_head(T_HEAD0 + 2, tpx(200, 140, 100), tpx(40, 30, 30), 2);
    gen_body(T_BODY0 + 0, tpx(224, 160, 48), tpx(250, 250, 250), 0);
    gen_body(T_BODY0 + 1, tpx(190, 40, 40), tpx(255, 255, 255), 1);
    gen_body(T_BODY0 + 2, tpx(48, 170, 64), tpx(170, 170, 180), 2);
    gen_legs(T_LEGS0 + 0, tpx(40, 60, 150), tpx(240, 240, 240));
    gen_legs(T_LEGS0 + 1, tpx(40, 30, 40), tpx(200, 40, 40));
    gen_legs(T_LEGS0 + 2, tpx(70, 80, 70), tpx(240, 130, 30));
    gen_ground(T_GROUND);
    gen_build(T_BUILD);
    gen_stand(T_STAND);
}

/* ---------- PNG loading / saving (any PNG size is shrunk to 64x64) ---------- */
static int load_png_at(int t, const char *path) {
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
    size_t sz = PNG_IMAGE_SIZE(img);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) {
        png_image_free(&img);
        return 0;
    }
    if (!png_image_finish_read(&img, NULL, buf, 0, NULL)) {
        free(buf);
        png_image_free(&img);
        return 0;
    }
    for (int y = 0; y < TEXSZ; y++) {
        int sy = y * (int)img.height / TEXSZ;
        for (int x = 0; x < TEXSZ; x++) {
            int sx = x * (int)img.width / TEXSZ;
            const uint8_t *p = buf + ((size_t)sy * img.width + sx) * 4;
            texdata[t][y * TEXSZ + x] = 0xFF000000u | ((uint32_t)p[2] << 16) |
                                        ((uint32_t)p[1] << 8) | (uint32_t)p[0];
        }
    }
    free(buf);
    png_image_free(&img);
    return 1;
}

static int load_png(int t, const char *name) {
    char path[220];
    snprintf(path, sizeof(path), "%s%s", basedir, name);
    if (load_png_at(t, path)) return 1;
    snprintf(path, sizeof(path), "ms0:/PSP/GAME/JojoFighter/%s", name);
    if (load_png_at(t, path)) return 1;
    return load_png_at(t, name);
}

static int save_png_at(int t, const char *path) {
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    img.width = TEXSZ;
    img.height = TEXSZ;
    img.format = PNG_FORMAT_RGBA;
    return png_image_write_to_file(&img, path, 0, texdata[t], 0, NULL) ? 1 : 0;
}

static int export_all(void) {
    int n = 0;
    for (int t = 0; t < NTEX; t++) {
        char path[220];
        snprintf(path, sizeof(path), "%s%s", basedir, texname[t]);
        if (save_png_at(t, path)) { n++; continue; }
        snprintf(path, sizeof(path), "ms0:/PSP/GAME/JojoFighter/%s", texname[t]);
        if (save_png_at(t, path)) n++;
    }
    return n;
}

/* =====================================================================
   2D drawing (screen pixels) - used for UI
   ===================================================================== */
static void rect(int x, int y, int w, int h, uint32_t c) {
    bind_tex(-1);
    Vtx *v = (Vtx *)sceGuGetMemory(2 * sizeof(Vtx));
    v[0].color = c; v[0].x = (float)x;       v[0].y = (float)y;       v[0].z = 0;
    v[1].color = c; v[1].x = (float)(x + w); v[1].y = (float)(y + h); v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, VT2, 2, 0, v);
}

static void disc(int cx, int cy, int r, uint32_t c) {
    bind_tex(-1);
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
    bind_tex(-1);
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    Vtx *v = (Vtx *)sceGuGetMemory(4 * sizeof(Vtx));
    v[0].color = c; v[0].x = x0 + nx; v[0].y = y0 + ny; v[0].z = 0;
    v[1].color = c; v[1].x = x0 - nx; v[1].y = y0 - ny; v[1].z = 0;
    v[2].color = c; v[2].x = x1 - nx; v[2].y = y1 - ny; v[2].z = 0;
    v[3].color = c; v[3].x = x1 + nx; v[3].y = y1 + ny; v[3].z = 0;
    sceGuDrawArray(GU_TRIANGLE_FAN, VT2, 4, 0, v);
}

/* =====================================================================
   Text: tiny 5x7 pixel font drawn by the GPU
   ===================================================================== */
static const char CS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!+-.:/><()";
static const unsigned char FONT[47][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* + */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, /* : */
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10}, /* / */
    {0x10,0x08,0x04,0x02,0x04,0x08,0x10}, /* > */
    {0x01,0x02,0x04,0x08,0x04,0x02,0x01}, /* < */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}  /* ) */
};

static const unsigned char *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (int i = 0; CS[i]; i++) if (CS[i] == c) return FONT[i];
    return FONT[0];
}

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

static void flush_text(void) {
    if (ntx == 0) return;
    bind_tex(-1);
    sceGuDisable(GU_DEPTH_TEST);

    int total = 0;
    for (int i = 0; i < ntx; i++) {
        for (int c = 0; txq[i].s[c]; c++) {
            const unsigned char *g = glyph(txq[i].s[c]);
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++)
                    if (g[r] & (0x10 >> b)) total++;
        }
    }
    if (total == 0) { ntx = 0; return; }

    Vtx *v = (Vtx *)sceGuGetMemory(total * 2 * sizeof(Vtx));
    int k = 0;
    for (int i = 0; i < ntx; i++) {
        for (int c = 0; txq[i].s[c]; c++) {
            const unsigned char *g = glyph(txq[i].s[c]);
            int x0 = (txq[i].col + c) * 7;
            int y0 = txq[i].row * 8 + 1;
            for (int r = 0; r < 7; r++) {
                for (int b = 0; b < 5; b++) {
                    if (!(g[r] & (0x10 >> b))) continue;
                    int x = x0 + b;
                    if (x >= SCR_W) continue;
                    v[k].color = txq[i].color; v[k].x = (float)x;       v[k].y = (float)(y0 + r);       v[k].z = 0; k++;
                    v[k].color = txq[i].color; v[k].x = (float)(x + 1); v[k].y = (float)(y0 + r + 1);   v[k].z = 0; k++;
                }
            }
        }
    }
    if (k > 0) sceGuDrawArray(GU_SPRITES, VT2, k, 0, v);
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
static int g_flat = 0;     /* 1 = no shading (boxes) */
static int g_flash = 0;    /* 1 = hit flash: draw solid white, no texture */
static uint32_t g_stand = 0xFFE030A0;   /* tint for Stand bodies */
static V3 cam_eye, cam_ctr;

/* The fight is played along a "line" (pixels). The line can turn around the floor:
   g_la = its angle, (g_ox, g_oz) = where its origin is in the world.  At angle 0 this is
   exactly the old side-view mapping. */
static float g_ox = 0.0f, g_oz = 0.0f, g_la = 0.0f;
static V3 wp(float px_, float py_, float z) {
    float x = (px_ - 240.0f) / 20.0f;
    float c = cosf(g_la), sn = sinf(g_la);
    return v3(g_ox + c * x - sn * z, ((float)GROUND - py_) / 20.0f, g_oz + sn * x + c * z);
}
/* a point 'r' to the right and 'u' up from c, as seen from the side-on camera */
static V3 wr(V3 c, float r, float u) {
    return v3(c.x + cosf(g_la) * r, c.y + u, c.z + sinf(g_la) * r);
}
/* a point offset from o by (lx along the line, ly up, lz toward the camera) */
static V3 lo(V3 o, float lx, float ly, float lz) {
    float c = cosf(g_la), sn = sinf(g_la);
    return v3(o.x + c * lx - sn * lz, o.y + ly, o.z + sn * lx + c * lz);
}

static uint32_t shadec(uint32_t c, float f) {
    if (f > 1.0f) f = 1.0f;
    if (f < 0.0f) f = 0.0f;
    uint32_t r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255;
    r = (uint32_t)(r * f); g = (uint32_t)(g * f); b = (uint32_t)(b * f);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static float lit(float nx, float ny, float nz) {
    float d = nx * -0.348f + ny * 0.795f + nz * 0.497f;
    return d < 0.0f ? 0.0f : d;
}

#define PV(P) do { v[k].color = col; v[k].x = (P)->x; v[k].y = (P)->y; v[k].z = (P)->z; k++; } while (0)

/* c[0..3] = one end, c[4..7] = other end */
static void prism(const V3 *c, uint32_t color) {
    static const int F[6][4] = {{0,1,2,3},{4,5,6,7},{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7}};
    float mx = 0, my = 0, mz = 0;
    for (int i = 0; i < 8; i++) { mx += c[i].x; my += c[i].y; mz += c[i].z; }
    mx /= 8; my /= 8; mz /= 8;

    bind_tex(-1);
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
        if (!g_flat) sh = 0.45f + 0.55f * lit(nx, ny, nz);
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

static void tvx(TVtx *v, int *k, float u, float vv, uint32_t c, float x, float y, float z) {
    v[*k].u = u; v[*k].v = vv; v[*k].color = c;
    v[*k].x = x; v[*k].y = y; v[*k].z = z;
    (*k)++;
}

static void disc3(float cx, float cy, float cz, float r, uint32_t c) {
    bind_tex(-1);
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

static float g_shz = 0.0f;   /* world z of the shadow being drawn */
static void shadow(float wx, float height) {
    bind_tex(-1);
    float sz = 1.3f / (1.0f + height * 0.35f);
    uint32_t c = 0xFF0A0A14;
    float ca = cosf(g_la), sa = sinf(g_la);
    Vtx *v = (Vtx *)sceGuGetMemory(4 * sizeof(Vtx));
    float us[4] = {-sz, sz, sz, -sz};
    float vs[4] = {0.9f, 0.9f, -0.9f, -0.9f};
    for (int i = 0; i < 4; i++) {
        v[i].color = c;
        v[i].x = wx + ca * us[i] - sa * vs[i];
        v[i].y = 0.10f;
        v[i].z = g_shz + sa * us[i] + ca * vs[i];
    }
    sceGuDrawArray(GU_TRIANGLE_FAN, VT3, 4, 0, v);
}

/* ---------- round shapes ---------- */
#define MAXSEG 14
#define MAXRING 8

/* ellipsoid. Texture u runs over the FRONT HALF (the face side); yaw turns it. */
static void ell(float cx, float cy, float cz, float rx, float ry, float rz, float yaw,
                int tex, uint32_t col, int seg, int ring) {
    static TVtx grid[(MAXRING + 1) * (MAXSEG + 1)];
    float clat[MAXRING + 1], slat[MAXRING + 1], cp[MAXSEG + 1], sp[MAXSEG + 1];
    if (seg > MAXSEG) seg = MAXSEG;
    if (ring > MAXRING) ring = MAXRING;
    int t = g_flash ? -1 : tex;
    float yc = cosf(yaw), ys = sinf(yaw);

    for (int i = 0; i <= ring; i++) {
        float la = 1.5707963f - i * 3.1415927f / ring;
        clat[i] = cosf(la); slat[i] = sinf(la);
    }
    for (int j = 0; j <= seg; j++) {
        float pr = ((float)j / seg - 0.5f) * 6.2831853f;
        cp[j] = cosf(pr); sp[j] = sinf(pr);
    }
    for (int i = 0; i <= ring; i++) {
        for (int j = 0; j <= seg; j++) {
            float pr = ((float)j / seg - 0.5f) * 6.2831853f;
            float sa = sp[j] * yc + cp[j] * ys;
            float ca = cp[j] * yc - sp[j] * ys;
            float nx = clat[i] * sa, ny = slat[i], nz = clat[i] * ca;
            float u = 0.5f + pr / 3.1415927f;
            if (u < 0.0f) u = 0.0f;
            if (u > 1.0f) u = 1.0f;
            uint32_t c = g_flash ? 0xFFFFFFFF : shadec(col, 0.5f + 0.5f * lit(nx, ny, nz));
            TVtx *g = &grid[i * (seg + 1) + j];
            g->u = u; g->v = (float)i / ring; g->color = c;
            g->x = cx + nx * rx; g->y = cy + ny * ry; g->z = cz + nz * rz;
        }
    }
    int nv = seg * ring * 6;
    TVtx *v = (TVtx *)sceGuGetMemory(nv * sizeof(TVtx));
    int k = 0;
    for (int i = 0; i < ring; i++) {
        for (int j = 0; j < seg; j++) {
            TVtx *a = &grid[i * (seg + 1) + j];
            TVtx *b = &grid[i * (seg + 1) + j + 1];
            TVtx *c2 = &grid[(i + 1) * (seg + 1) + j + 1];
            TVtx *d = &grid[(i + 1) * (seg + 1) + j];
            v[k++] = *a; v[k++] = *b; v[k++] = *c2;
            v[k++] = *a; v[k++] = *c2; v[k++] = *d;
        }
    }
    bind_tex(t);
    sceGuDrawArray(GU_TRIANGLES, VTT, nv, 0, v);
}

/* 8-sided tapered tube from a to b (radius ra at a, rb at b). dz squashes depth. */
static void tube(V3 a, V3 b, float ra, float rb, float dz, int tex, uint32_t col) {
    static float tcs[9], tsn[9];
    static int tinit = 0;
    if (!tinit) {
        for (int s = 0; s <= 8; s++) { tcs[s] = cosf(s * 0.7853982f); tsn[s] = sinf(s * 0.7853982f); }
        tinit = 1;
    }
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    float n1x = 1.0f, n1y = 0.0f;
    if (len > 0.001f) { n1x = -dy / len; n1y = dx / len; }
    int t = g_flash ? -1 : tex;
    TVtx rg[2][9];
    for (int s = 0; s <= 8; s++) {
        float nx = tcs[s] * n1x, ny = tcs[s] * n1y, nz = tsn[s];
        uint32_t c = g_flash ? 0xFFFFFFFF : shadec(col, 0.5f + 0.5f * lit(nx, ny, nz));
        float u = s / 8.0f + 0.25f;
        rg[0][s].u = u; rg[0][s].v = 0.0f; rg[0][s].color = c;
        rg[0][s].x = a.x + nx * ra; rg[0][s].y = a.y + ny * ra; rg[0][s].z = a.z + nz * ra * dz;
        rg[1][s].u = u; rg[1][s].v = 1.0f; rg[1][s].color = c;
        rg[1][s].x = b.x + nx * rb; rg[1][s].y = b.y + ny * rb; rg[1][s].z = b.z + nz * rb * dz;
    }
    int nv = 48;
    TVtx *v = (TVtx *)sceGuGetMemory(nv * sizeof(TVtx));
    int k = 0;
    for (int s = 0; s < 8; s++) {
        v[k++] = rg[0][s]; v[k++] = rg[0][s + 1]; v[k++] = rg[1][s + 1];
        v[k++] = rg[0][s]; v[k++] = rg[1][s + 1]; v[k++] = rg[1][s];
    }
    bind_tex(t);
    sceGuDrawArray(GU_TRIANGLES, VTT, nv, 0, v);
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
    sceGumUpdateMatrix();
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
        V3 C = wp(sparks[i].x, sparks[i].y, 1.3f);
        float l2 = L * 0.66f;
        limb(wr(C, -L, 0.0f), wr(C, L, 0.0f), 0.18f, 0.18f, c);
        limb(wr(C, 0.0f, -L), wr(C, 0.0f, L), 0.18f, 0.18f, c);
        limb(wr(C, -l2, -l2), wr(C, l2, l2), 0.18f, 0.18f, c);
        limb(wr(C, -l2, l2), wr(C, l2, -l2), 0.18f, 0.18f, c);
        box(C.x, C.y, C.z, 0.3f, 0.3f, 0.3f, 0xFFFFFFFF);
    }
    g_flat = 0;
}

/* =====================================================================
   IMPACT EFFECTS: hit-stop, particles, shockwave rings, screen flash
   ===================================================================== */
typedef struct { float x, y, vx, vy; int t, tmax; uint32_t c; float sz; } Part;
static Part parts[48];
typedef struct { float x, y; int t, tmax; float r; uint32_t c; } Wave;
static Wave waves[4];

static int hitstop = 0;    /* frames left of freeze */
static int hitmax = 1;     /* length of the current freeze */
static int hitvic = 0;     /* who got hit: 1 = enemy, 2 = player */
static int hitamp = 0;     /* how hard the victim vibrates (pixels) */
static int flashscr = 0;   /* white screen flash */

/* x,y = impact point (old pixel coords). dir = attacker facing (+1/-1).
   power: 0 tiny, 1 light, 3 heavy, 4 K.O.  victim: 1 enemy, 2 player. */
static void hitfx(int x, int y, int dir, int power, int block, int victim, int stop) {
    static const uint32_t hot[4]  = {0xFF30D0FF, 0xFFFFFFFF, 0xFF30A0FF, 0xFF2828FF};
    static const uint32_t cool[3] = {0xFFFFE0A0, 0xFFFFFFFF, 0xFFE0C080};

    add_spark(x, y, block ? 2 : (power >= 3 ? 1 : 0));

    int n = block ? 4 : 5 + power * 4;
    for (int i = 0; i < 48 && n > 0; i++) {
        if (parts[i].t > 0) continue;
        n--;
        float a = ((float)(rnd() % 200) / 100.0f - 1.0f) * 1.3f;
        float sp = 1.2f + (float)(rnd() % 100) / 100.0f * (1.5f + (float)power);
        parts[i].x = (float)x;
        parts[i].y = (float)y;
        parts[i].vx = (float)dir * cosf(a) * sp;
        parts[i].vy = sinf(a) * sp - 0.8f;
        parts[i].tmax = 16 + rnd() % 14 + power * 3;
        parts[i].t = parts[i].tmax;
        parts[i].sz = (block ? 1.2f : 1.5f) + (float)power * 0.5f + (float)(rnd() % 10) / 10.0f;
        parts[i].c = block ? cool[rnd() % 3] : hot[rnd() % 4];
    }

    if (!block && power >= 1) {
        for (int i = 0; i < 4; i++) {
            if (waves[i].t > 0) continue;
            waves[i].x = (float)x;
            waves[i].y = (float)y;
            waves[i].tmax = 10 + power * 2;
            waves[i].t = waves[i].tmax;
            waves[i].r = (power >= 3) ? 3.4f : 1.8f;
            waves[i].c = (power >= 3) ? 0xFFFFFFFF : 0xFF30D0FF;
            break;
        }
    }

    if (stop > 0) {
        if (stop > hitstop) { hitstop = stop; hitmax = stop; }
        hitvic = victim;
        hitamp = block ? 1 : ((power >= 3) ? 3 : 2);
    }
    if (!block && power >= 3 && flashscr < 3) flashscr = 3;
}

static void update_fx(void) {
    for (int i = 0; i < 48; i++) {
        if (parts[i].t <= 0) continue;
        parts[i].x += parts[i].vx;
        parts[i].y += parts[i].vy;
        parts[i].vy += 0.18f;
        parts[i].vx *= 0.97f;
        if (parts[i].y > GROUND - 2) { parts[i].y = GROUND - 2; parts[i].vy *= -0.4f; }
        parts[i].t--;
    }
    for (int i = 0; i < 4; i++) if (waves[i].t > 0) waves[i].t--;
}

static void clear_fx(void) {
    for (int i = 0; i < 48; i++) parts[i].t = 0;
    for (int i = 0; i < 4; i++) waves[i].t = 0;
    for (int i = 0; i < 8; i++) sparks[i].t = 0;
    hitstop = 0; hitmax = 1; hitvic = 0; hitamp = 0; flashscr = 0;
}

static void draw_fx(void) {
    bind_tex(-1);

    /* particles: small camera-facing squares that fade out */
    int cnt = 0;
    for (int i = 0; i < 48; i++) if (parts[i].t > 0) cnt++;
    if (cnt > 0) {
        Vtx *v = (Vtx *)sceGuGetMemory(cnt * 6 * sizeof(Vtx));
        int k = 0;
        for (int i = 0; i < 48; i++) {
            if (parts[i].t <= 0) continue;
            V3 C = wp(parts[i].x, parts[i].y, 1.4f);
            float life = (float)parts[i].t / (float)parts[i].tmax;
            float s = parts[i].sz * 0.06f * (0.4f + 0.6f * life);
            float af = life * 2.0f;
            if (af > 1.0f) af = 1.0f;
            uint32_t a = (uint32_t)(255.0f * af);
            uint32_t col = (a << 24) | (parts[i].c & 0x00FFFFFF);
            {
                V3 q0 = wr(C, -s, -s), q1 = wr(C, s, -s), q2 = wr(C, s, s), q3 = wr(C, -s, s);
                qv(v, &k, q0.x, q0.y, q0.z, col); qv(v, &k, q1.x, q1.y, q1.z, col); qv(v, &k, q2.x, q2.y, q2.z, col);
                qv(v, &k, q0.x, q0.y, q0.z, col); qv(v, &k, q2.x, q2.y, q2.z, col); qv(v, &k, q3.x, q3.y, q3.z, col);
            }
        }
        sceGuDrawArray(GU_TRIANGLES, VT3, k, 0, v);
    }

    /* shockwave rings */
    for (int i = 0; i < 4; i++) {
        Wave *w = &waves[i];
        if (w->t <= 0) continue;
        float prog = 1.0f - (float)w->t / (float)w->tmax;
        float r = w->r * (0.2f + 0.8f * prog);
        float th = 0.08f + 0.22f * (1.0f - prog);
        uint32_t a = (uint32_t)(230.0f * (1.0f - prog));
        uint32_t col = (a << 24) | (w->c & 0x00FFFFFF);
        V3 C = wp(w->x, w->y, 1.2f);
        int n = 20;
        Vtx *v = (Vtx *)sceGuGetMemory((n + 1) * 2 * sizeof(Vtx));
        for (int s = 0; s <= n; s++) {
            float an = s * 6.2831853f / n;
            float cs = cosf(an), sn = sinf(an);
            V3 in0 = wr(C, cs * (r - th), sn * (r - th));
            V3 out0 = wr(C, cs * (r + th), sn * (r + th));
            v[s * 2].color = col;
            v[s * 2].x = in0.x; v[s * 2].y = in0.y; v[s * 2].z = in0.z;
            v[s * 2 + 1].color = col;
            v[s * 2 + 1].x = out0.x; v[s * 2 + 1].y = out0.y; v[s * 2 + 1].z = out0.z;
        }
        sceGuDrawArray(GU_TRIANGLE_STRIP, VT3, (n + 1) * 2, 0, v);
    }
}

/* ---------- 3D stage: golden palace hall (Tekken-style arena) ---------- */
#define STAGE_CX (0.0f)     /* centre of the fighting ring (world x) - same as the middle of the fight line */

static void pillar(float cx, float cz, float r, float y0, float y1, uint32_t col) {
    bind_tex(-1);
    int n = 10;
    Vtx *v = (Vtx *)sceGuGetMemory((unsigned int)(n + 1) * 2 * sizeof(Vtx));
    for (int i = 0; i <= n; i++) {
        float a = (float)i * 6.2831853f / (float)n;
        float nx = cosf(a), nz = sinf(a);
        uint32_t c = shadec(col, 0.35f + 0.65f * lit(nx, 0.0f, nz));
        v[i * 2].color = c;
        v[i * 2].x = cx + nx * r; v[i * 2].y = y0; v[i * 2].z = cz + nz * r;
        v[i * 2 + 1].color = c;
        v[i * 2 + 1].x = cx + nx * r; v[i * 2 + 1].y = y1; v[i * 2 + 1].z = cz + nz * r;
    }
    sceGuDrawArray(GU_TRIANGLE_STRIP, VT3, (n + 1) * 2, 0, v);
}

/* flat ring lying on the floor */
static void floor_ring(float cx, float cz, float r0, float r1, float y, uint32_t c) {
    bind_tex(-1);
    int n = 48;
    Vtx *v = (Vtx *)sceGuGetMemory((unsigned int)(n + 1) * 2 * sizeof(Vtx));
    for (int i = 0; i <= n; i++) {
        float a = (float)i * 6.2831853f / (float)n;
        float ca = cosf(a), sa = sinf(a);
        v[i * 2].color = c;
        v[i * 2].x = cx + ca * r0; v[i * 2].y = y; v[i * 2].z = cz + sa * r0;
        v[i * 2 + 1].color = c;
        v[i * 2 + 1].x = cx + ca * r1; v[i * 2 + 1].y = y; v[i * 2 + 1].z = cz + sa * r1;
    }
    sceGuDrawArray(GU_TRIANGLE_STRIP, VT3, (n + 1) * 2, 0, v);
}

#define ARENA_R   38.0f   /* the round arena wall */
#define PIL_R     33.0f   /* ring of pillars in front of the wall */
#define NPIL      12

static float acs[49], asn[49];     /* cos/sin of 48 steps around a circle */
static int ainit = 0;

/* tall ring-shaped band (a round wall piece): c0 at the bottom, c1 at the top */
static void arena_band(float r, float y0, float y1, uint32_t c0, uint32_t c1) {
    bind_tex(-1);
    int n = 48;
    Vtx *v = (Vtx *)sceGuGetMemory((unsigned int)(n + 1) * 2 * sizeof(Vtx));
    for (int i = 0; i <= n; i++) {
        float x = STAGE_CX + acs[i] * r, z = asn[i] * r;
        v[i * 2].color = c0;
        v[i * 2].x = x; v[i * 2].y = y0; v[i * 2].z = z;
        v[i * 2 + 1].color = c1;
        v[i * 2 + 1].x = x; v[i * 2 + 1].y = y1; v[i * 2 + 1].z = z;
    }
    sceGuDrawArray(GU_TRIANGLE_STRIP, VT3, (n + 1) * 2, 0, v);
}

/* The arena is a ROUND palace hall, so it looks right from every angle
   while the camera circles around the fighters. */
static void draw_stage(int frame) {
    g_flat = 0;
    g_flash = 0;
    bind_tex(-1);

    if (!ainit) {
        for (int i = 0; i <= 48; i++) {
            float a = (float)i * 6.2831853f / 48.0f;
            acs[i] = cosf(a); asn[i] = sinf(a);
        }
        ainit = 1;
    }

    /* domed ceiling: glowing red at the rim, dark at the top */
    {
        Vtx *v = (Vtx *)sceGuGetMemory(48 * 3 * sizeof(Vtx));
        int k = 0;
        uint32_t lo = 0xFF1C4CB8, hi = 0xFF060818;
        for (int i = 0; i < 48; i++) {
            qv(v, &k, STAGE_CX + acs[i] * ARENA_R,     17.0f, asn[i] * ARENA_R,     lo);
            qv(v, &k, STAGE_CX + acs[i + 1] * ARENA_R, 17.0f, asn[i + 1] * ARENA_R, lo);
            qv(v, &k, STAGE_CX, 40.0f, 0.0f, hi);
        }
        sceGuDrawArray(GU_TRIANGLES, VT3, k, 0, v);
    }

    /* round sandstone wall */
    {
        TVtx *v = (TVtx *)sceGuGetMemory(48 * 6 * sizeof(TVtx));
        int k = 0;
        uint32_t wc = 0xFFC8C8C8;
        for (int i = 0; i < 48; i++) {
            float x0 = STAGE_CX + acs[i] * ARENA_R,     z0 = asn[i] * ARENA_R;
            float x1 = STAGE_CX + acs[i + 1] * ARENA_R, z1 = asn[i + 1] * ARENA_R;
            float u0 = (float)i * (40.0f / 48.0f), u1 = (float)(i + 1) * (40.0f / 48.0f);
            tvx(v, &k, u0, 0.0f, wc, x0, 0.0f,  z0);
            tvx(v, &k, u1, 0.0f, wc, x1, 0.0f,  z1);
            tvx(v, &k, u1, 2.7f, wc, x1, 16.0f, z1);
            tvx(v, &k, u0, 0.0f, wc, x0, 0.0f,  z0);
            tvx(v, &k, u1, 2.7f, wc, x1, 16.0f, z1);
            tvx(v, &k, u0, 2.7f, wc, x0, 16.0f, z0);
        }
        bind_tex(T_BUILD);
        sceGuDrawArray(GU_TRIANGLES, VTT, k, 0, v);
        bind_tex(-1);
    }

    /* glowing arched windows between the pillars */
    {
        Vtx *v = (Vtx *)sceGuGetMemory(NPIL * 12 * sizeof(Vtx));
        int k = 0;
        for (int i = 0; i < NPIL; i++) {
            float a = ((float)i + 0.5f) * 6.2831853f / (float)NPIL;
            float ca = cosf(a), sa = sinf(a);
            float rr = ARENA_R - 0.15f;
            float cx = STAGE_CX + ca * rr, cz = sa * rr;
            float tx = -sa, tz = ca;                 /* along the wall */
            int fl = ((frame / 6) + i * 3) % 11 == 0;
            uint32_t lo = fl ? 0xFF3090FF : 0xFF2078F0;
            uint32_t hi = 0xFF1030A0;
            float fw = 3.4f, gw = 3.0f;
            /* dark frame */
            qv(v, &k, cx - tx * fw, 3.2f, cz - tz * fw, 0xFF081030);
            qv(v, &k, cx + tx * fw, 3.2f, cz + tz * fw, 0xFF081030);
            qv(v, &k, cx + tx * fw, 13.8f, cz + tz * fw, 0xFF081030);
            qv(v, &k, cx - tx * fw, 3.2f, cz - tz * fw, 0xFF081030);
            qv(v, &k, cx + tx * fw, 13.8f, cz + tz * fw, 0xFF081030);
            qv(v, &k, cx - tx * fw, 13.8f, cz - tz * fw, 0xFF081030);
            /* glow (a little closer to the room) */
            float gx = cx - ca * 0.1f, gz = cz - sa * 0.1f;
            qv(v, &k, gx - tx * gw, 3.6f, gz - tz * gw, lo);
            qv(v, &k, gx + tx * gw, 3.6f, gz + tz * gw, lo);
            qv(v, &k, gx + tx * gw, 13.2f, gz + tz * gw, hi);
            qv(v, &k, gx - tx * gw, 3.6f, gz - tz * gw, lo);
            qv(v, &k, gx + tx * gw, 13.2f, gz + tz * gw, hi);
            qv(v, &k, gx - tx * gw, 13.2f, gz - tz * gw, hi);
        }
        sceGuDrawArray(GU_TRIANGLES, VT3, k, 0, v);
    }

    /* gold base and cornice all the way around the wall */
    arena_band(ARENA_R - 0.05f, 0.0f, 1.0f, 0xFF2078B0, 0xFF2078B0);
    arena_band(ARENA_R - 0.05f, 15.8f, 17.0f, 0xFF30A8E0, 0xFF30A8E0);

    /* ring of pillars */
    for (int i = 0; i < NPIL; i++) {
        float a = (float)i * 6.2831853f / (float)NPIL;
        float x = STAGE_CX + cosf(a) * PIL_R, z = sinf(a) * PIL_R;
        box(x, 0.5f, z, 2.8f, 1.0f, 2.8f, 0xFF30A8E0);
        pillar(x, z, 1.0f, 1.0f, 14.2f, 0xFF10188A);
        pillar(x, z, 1.12f, 3.2f, 3.8f, 0xFF30A8E0);
        pillar(x, z, 1.12f, 9.0f, 9.6f, 0xFF30A8E0);
        box(x, 14.6f, z, 3.0f, 0.9f, 3.0f, 0xFF30A8E0);
    }

    /* hanging lights (little flicker) */
    for (int i = 0; i < 6; i++) {
        float a = (float)i * 1.0471976f + 0.5f;
        float x = STAGE_CX + cosf(a) * 20.0f, z = sinf(a) * 20.0f;
        float fl = 0.5f * sinf((float)frame * 0.13f + (float)i * 1.7f);
        float s = 1.0f + 0.12f * fl;
        box(x, 14.0f, z, 0.12f, 6.0f, 0.12f, 0xFF30A8E0);
        box(x, 11.0f, z, 1.1f * s, 1.1f * s, 1.1f * s, 0xFF40A0FF);
        box(x, 11.0f, z, 0.6f, 0.6f, 0.6f, 0xFF80F0FF);
    }

    /* floor: polished marble disc */
    {
        const int rn = 3, sn = 24;
        const float rr[4] = {0.0f, 13.0f, 26.0f, ARENA_R + 0.5f};
        TVtx *v = (TVtx *)sceGuGetMemory((unsigned int)(rn * sn * 6) * sizeof(TVtx));
        int k = 0;
        for (int r = 0; r < rn; r++) {
            for (int s = 0; s < sn; s++) {
                float ax0 = acs[s * 2], az0 = asn[s * 2];
                float ax1 = acs[s * 2 + 2], az1 = asn[s * 2 + 2];
                float x00 = STAGE_CX + ax0 * rr[r],     z00 = az0 * rr[r];
                float x01 = STAGE_CX + ax1 * rr[r],     z01 = az1 * rr[r];
                float x10 = STAGE_CX + ax0 * rr[r + 1], z10 = az0 * rr[r + 1];
                float x11 = STAGE_CX + ax1 * rr[r + 1], z11 = az1 * rr[r + 1];
                uint32_t c = ((r + s) & 1) ? 0xFFD8D8D8 : 0xFFFFFFFF;
                tvx(v, &k, x00 / 3.0f, z00 / 3.0f, c, x00, 0.0f, z00);
                tvx(v, &k, x10 / 3.0f, z10 / 3.0f, c, x10, 0.0f, z10);
                tvx(v, &k, x11 / 3.0f, z11 / 3.0f, c, x11, 0.0f, z11);
                tvx(v, &k, x00 / 3.0f, z00 / 3.0f, c, x00, 0.0f, z00);
                tvx(v, &k, x11 / 3.0f, z11 / 3.0f, c, x11, 0.0f, z11);
                tvx(v, &k, x01 / 3.0f, z01 / 3.0f, c, x01, 0.0f, z01);
            }
        }
        bind_tex(T_GROUND);
        sceGuDrawArray(GU_TRIANGLES, VTT, k, 0, v);
        bind_tex(-1);
    }

    /* the fighting ring: gold rings and spokes */
    floor_ring(STAGE_CX, 0.0f, 0.0f, 14.2f, 0.04f, 0x70102060);
    floor_ring(STAGE_CX, 0.0f, 14.2f, 15.2f, 0.06f, 0xFF30B8F0);
    floor_ring(STAGE_CX, 0.0f, 11.4f, 11.8f, 0.06f, 0xFF2090D0);
    floor_ring(STAGE_CX, 0.0f, 3.0f, 3.5f, 0.06f, 0xFF30B8F0);
    floor_ring(STAGE_CX, 0.0f, 25.0f, 25.5f, 0.05f, 0xFF2090D0);
    {
        Vtx *v = (Vtx *)sceGuGetMemory(8 * 6 * sizeof(Vtx));
        int k = 0;
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 0.7853982f;
            float ca = cosf(a), sa = sinf(a);
            float r0 = 3.5f, r1 = 11.4f, hw = 0.14f;
            float px0 = STAGE_CX + ca * r0, pz0 = sa * r0;
            float px1 = STAGE_CX + ca * r1, pz1 = sa * r1;
            float ox = -sa * hw, oz = ca * hw;
            uint32_t c = 0xFF2090D0;
            qv(v, &k, px0 - ox, 0.06f, pz0 - oz, c); qv(v, &k, px0 + ox, 0.06f, pz0 + oz, c); qv(v, &k, px1 + ox, 0.06f, pz1 + oz, c);
            qv(v, &k, px0 - ox, 0.06f, pz0 - oz, c); qv(v, &k, px1 + ox, 0.06f, pz1 + oz, c); qv(v, &k, px1 - ox, 0.06f, pz1 - oz, c);
        }
        sceGuDrawArray(GU_TRIANGLES, VT3, k, 0, v);
    }

    /* soft light beams from the ceiling, in a circle around the ring */
    {
        Vtx *v = (Vtx *)sceGuGetMemory(6 * 6 * sizeof(Vtx));
        int k = 0;
        for (int i = 0; i < 6; i++) {
            float a = (float)i * 1.0471976f;
            float ca = cosf(a), sa = sinf(a);
            float x = STAGE_CX + ca * 22.0f, z = sa * 22.0f;
            float tx = -sa, tz = ca;
            float pulse = 0.5f + 0.5f * sinf((float)frame * 0.02f + (float)i * 2.0f);
            uint32_t bot = ((uint32_t)(0x1C + (int)(pulse * 0x14)) << 24) | 0x0070D8FF;
            uint32_t top = 0x0070D8FF;
            qv(v, &k, x - tx * 0.8f, 20.0f, z - tz * 0.8f, top);
            qv(v, &k, x + tx * 0.8f, 20.0f, z + tz * 0.8f, top);
            qv(v, &k, x + tx * 4.0f, 0.0f,  z + tz * 4.0f,  bot);
            qv(v, &k, x - tx * 0.8f, 20.0f, z - tz * 0.8f, top);
            qv(v, &k, x + tx * 4.0f, 0.0f,  z + tz * 4.0f,  bot);
            qv(v, &k, x - tx * 4.0f, 0.0f,  z - tz * 4.0f,  bot);
        }
        sceGuDepthMask(GU_TRUE);    /* see-through beams must not hide the fighters */
        sceGuDrawArray(GU_TRIANGLES, VT3, k, 0, v);
        sceGuDepthMask(GU_FALSE);
    }
    bind_tex(-1);
}

/* =====================================================================
   OBJ MODEL LOADER + RENDERER
   Put model0.obj / model0.png (Brawler), model1.* (Speedster) and
   model2.* (Heavy) next to EBOOT.PBP. Missing files = procedural body.
   ===================================================================== */
#define MODEL_MAX        3          /* model0 = Brawler, model1 = Speedster, model2 = Heavy */
#define MTEX             256        /* model textures are shrunk to 256x256 */
#define MAX_TRIS         4500       /* triangle limit per model */
#define HOLD_FRAMES      8          /* UP/DOWN released before this = quick tap (side-step); held longer = jump / duck */
#define DUCK_FRAMES      3          /* hold DOWN this long and you duck (jump still needs HOLD_FRAMES) */
#define BODY_GAP         34         /* fighters can't get closer than this (pixels): they are solid */
#define DOUBLETAP        14         /* tap, let go, press again within this many frames = side-walk */
#define RING_LIMIT       13.4f      /* fighters can't leave the round ring */
#define CAM_BASE         6.9f       /* fight camera distance when the fighters are close (smaller = closer) */
#define MODEL_HEIGHT     4.6f       /* models are scaled to this height (bigger = bigger fighters) */
#define MODEL_FRONT_YAW  1.5707963f /* turns a Blender-default model to face the side.
                                       If your fighter faces the wrong way, try -1.5707963f */
#define MODEL_TURN       0.28f      /* how far fighters turn toward the camera */
#define NPART            6
enum { P_HEAD = 0, P_TORSO, P_ARML, P_ARMR, P_LEGL, P_LEGR };

typedef struct {
    int ntri;
    float *pos;   /* 3 floats per vertex */
    float *nrm;   /* 3 floats per vertex */
    float *uv;    /* 2 floats per vertex */
    unsigned char *part;      /* which body part each triangle belongs to */
    int split;                /* 1 = the OBJ has named parts (head, torso, arm_l ...) */
    float piv[NPART][2];      /* joint position of each part: x (forward), y (up) */
    float elb[NPART][2];      /* elbow / knee position of each arm and leg */
    unsigned char *wq;        /* per vertex: 0 = above the elbow/knee ... 4 = fully below it */
} Model;

/* swing angle (radians) of every body part around its joint, plus body lean */
typedef struct {
    float a[NPART];   /* swing of the whole part around its joint */
    float b[NPART];   /* elbow / knee bend of the lower half (arms: + = fist up, legs: - = heel back) */
} Pose;

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

/* case-insensitive "starts with" for part names */
static int name_is(const char *p, const char *want) {
    while (*want) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != *want) return 0;
        p++; want++;
    }
    return 1;
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
    unsigned char *pa = (unsigned char *)malloc((size_t)MAX_TRIS);
    int iv = 0, it = 0, in = 0, tris = 0, cp = P_TORSO, isplit = 0;
    if (!fv || !ft || !fn || !op || !on || !ou || !pa) goto done;

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
        } else if (p[0] == 'o' && p[1] == ' ') {
            const char *nm = p + 2;
            while (*nm == ' ') nm++;
            if      (name_is(nm, "head"))  { cp = P_HEAD;  isplit = 1; }
            else if (name_is(nm, "torso")) { cp = P_TORSO; isplit = 1; }
            else if (name_is(nm, "arm_l")) { cp = P_ARML;  isplit = 1; }
            else if (name_is(nm, "arm_r")) { cp = P_ARMR;  isplit = 1; }
            else if (name_is(nm, "leg_l")) { cp = P_LEGL;  isplit = 1; }
            else if (name_is(nm, "leg_r")) { cp = P_LEGR;  isplit = 1; }
            else                           { cp = P_TORSO; }
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
                pa[tris] = (unsigned char)cp;
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
    /* joints: head = bottom of the head, arms/legs = top of the limb */
    for (int pi = 0; pi < NPART; pi++) {
        float mn = 1e30f, mx = -1e30f;
        int cnt = 0;
        for (int t = 0; t < tris; t++) {
            if (pa[t] != pi) continue;
            for (int j = 0; j < 3; j++) {
                float y = op[(t * 3 + j) * 3 + 1];
                if (y < mn) mn = y;
                if (y > mx) mx = y;
                cnt++;
            }
        }
        models[id].piv[pi][0] = 0.0f;
        models[id].piv[pi][1] = MODEL_HEIGHT * 0.5f;
        if (cnt == 0 || pi == P_TORSO) continue;
        float band = (mx - mn) * 0.12f;
        float sx = 0.0f, sy2 = 0.0f;
        int n2 = 0;
        for (int t = 0; t < tris; t++) {
            if (pa[t] != pi) continue;
            for (int j = 0; j < 3; j++) {
                float x = op[(t * 3 + j) * 3];
                float y = op[(t * 3 + j) * 3 + 1];
                int inband = (pi == P_HEAD) ? (y <= mn + band) : (y >= mx - band);
                if (inband) { sx += x; sy2 += y; n2++; }
            }
        }
        if (n2 > 0) {
            models[id].piv[pi][0] = sx / (float)n2;
            models[id].piv[pi][1] = sy2 / (float)n2;
        }
    }
    /* elbows and knees: a bit above the middle of each arm and leg */
    {
        unsigned char *wq = (unsigned char *)malloc((size_t)tris * 3);
        for (int pi = 0; pi < NPART; pi++) {
            models[id].elb[pi][0] = models[id].piv[pi][0];
            models[id].elb[pi][1] = models[id].piv[pi][1];
        }
        if (wq) {
            memset(wq, 0, (size_t)tris * 3);
            for (int pi = P_ARML; pi <= P_LEGR; pi++) {
                float mn = 1e30f;
                int cnt = 0;
                for (int t = 0; t < tris; t++) {
                    if (pa[t] != pi) continue;
                    for (int j = 0; j < 3; j++) {
                        float y = op[(t * 3 + j) * 3 + 1];
                        if (y < mn) mn = y;
                        cnt++;
                    }
                }
                float top = models[id].piv[pi][1];
                float len = top - mn;
                if (cnt == 0 || len < 0.4f) continue;
                float ey = top - len * 0.48f;
                float bw = len * 0.10f;
                float sx = 0.0f;
                int n2 = 0;
                for (int t = 0; t < tris; t++) {
                    if (pa[t] != pi) continue;
                    for (int j = 0; j < 3; j++) {
                        float y = op[(t * 3 + j) * 3 + 1];
                        if (y > ey - bw && y < ey + bw) { sx += op[(t * 3 + j) * 3]; n2++; }
                    }
                }
                models[id].elb[pi][0] = (n2 > 0) ? sx / (float)n2 : models[id].piv[pi][0];
                models[id].elb[pi][1] = ey;
                for (int t = 0; t < tris; t++) {
                    if (pa[t] != pi) continue;
                    for (int j = 0; j < 3; j++) {
                        float y = op[(t * 3 + j) * 3 + 1];
                        float w = (ey + bw - y) / (2.0f * bw);
                        if (w < 0.0f) w = 0.0f;
                        if (w > 1.0f) w = 1.0f;
                        wq[t * 3 + j] = (unsigned char)(w * 4.0f + 0.5f);
                    }
                }
            }
        }
        models[id].wq = wq;
    }
    models[id].ntri = tris;
    models[id].pos = op;
    models[id].nrm = on;
    models[id].uv = ou;
    models[id].part = pa;
    models[id].split = isplit;
    op = NULL; on = NULL; ou = NULL; pa = NULL;   /* the model now owns these */
    ok = 1;

done:
    free(buf);
    free(fv); free(ft); free(fn);
    free(op); free(on); free(ou); free(pa);
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
   lean turns it about the hips (+ = forward), yaw about the up axis, sy squashes it.
   pose (or NULL) swings the head, arms and legs around their joints. */
static void model_draw(int id, float wx, float wy, float wz, float yaw, float lean,
                       float sy, uint32_t tint, int flash, const Pose *pose) {
    Model *m = &models[id];
    int nv = m->ntri * 3;
    TVtx *v = (TVtx *)sceGuGetMemory((unsigned int)nv * sizeof(TVtx));
    float cl = cosf(lean), sl = sinf(lean);
    float cyw = cosf(yaw), syw = sinf(yaw);
    const float piv = MODEL_HEIGHT * 0.5f;
    float pc[NPART], ps[NPART];
    for (int k = 0; k < NPART; k++) {
        float an = (pose && m->split) ? pose->a[k] : 0.0f;
        pc[k] = cosf(an); ps[k] = sinf(an);
    }
    /* elbow / knee bend, in 5 steps from the upper half (0) to the lower half (4) */
    float ec[NPART][5], es[NPART][5];
    for (int k = 0; k < NPART; k++) {
        for (int q = 0; q < 5; q++) {
            float bn = (pose && m->split && m->wq) ? pose->b[k] * (float)q * 0.25f : 0.0f;
            if (bn == 0.0f) { ec[k][q] = 1.0f; es[k][q] = 0.0f; }
            else { ec[k][q] = cosf(bn); es[k][q] = sinf(bn); }
        }
    }

    for (int i = 0; i < nv; i++) {
        float x = m->pos[i * 3], y = m->pos[i * 3 + 1], z = m->pos[i * 3 + 2];
        float nx = m->nrm[i * 3], ny = m->nrm[i * 3 + 1], nz = m->nrm[i * 3 + 2];

        /* swing the body part around its joint (forward = +x) */
        if (pose && m->split) {
            int k = m->part[i / 3];
            if (k != P_TORSO) {
                int q = m->wq ? (int)m->wq[i] : 0;
                if (q > 0) {                       /* bend at the elbow / knee first */
                    float rx = x - m->elb[k][0], ry = y - m->elb[k][1];
                    x = m->elb[k][0] + rx * ec[k][q] - ry * es[k][q];
                    y = m->elb[k][1] + rx * es[k][q] + ry * ec[k][q];
                    float tx2 = nx * ec[k][q] - ny * es[k][q];
                    ny = nx * es[k][q] + ny * ec[k][q];
                    nx = tx2;
                }
                float dx = x - m->piv[k][0], dy = y - m->piv[k][1];
                x = m->piv[k][0] + dx * pc[k] - dy * ps[k];
                y = m->piv[k][1] + dx * ps[k] + dy * pc[k];
                float tx = nx * pc[k] - ny * ps[k];
                ny = nx * ps[k] + ny * pc[k];
                nx = tx;
            }
        }
        y *= sy;

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

/* progress (0..1) of the attack being drawn; main() sets it before drawing a fighter */
static float g_prog = 0.0f;
static int g_crouch = 0;      /* 1 = ducking */
static int g_dash = 0;        /* 1 = dashing forward, 2 = hopping back */
static float g_yawadd = 0.0f; /* extra turn so fighters face each other when side-stepping */
static float g_walk_phase = 0.0f; /* stride phase advanced by actual travel distance */
static float g_walk_blend = 0.0f; /* fades gait in/out instead of snapping to neutral */
static int g_walk_mode = 0; /* 0 = forward/back gait, 1 = lateral side-walk */

/* 0 at rest, 1 when the blow lands (th = point of impact, 0..0.5) */
static float strike(float t, float th) {
    float a = th * 0.55f;
    if (t < a) return -0.35f * (t / a);
    if (t < th) return -0.35f + 1.35f * ((t - a) / (th - a));
    if (t < th + 0.12f) return 1.0f;
    float r = (t - th - 0.12f) / (1.0f - th - 0.12f);
    return (r >= 1.0f) ? 0.0f : 1.0f - r;
}

/* returns 1 if a model was drawn (so the procedural body is skipped) */
static int draw_model_fighter(int x, int y, int face, int flash, int atk, int block,
                              int walk, int frame, int style, float z) {
    if (style < 0 || style >= MODEL_MAX || models[style].ntri == 0) return 0;
    int air = (y < GROUND - 80);
    float lean = 0.0f, sy = 1.0f, shift = 0.0f, bob = 0.0f;
    Pose ps;
    float t = (float)frame;
    /* the arm/leg nearest the camera is the character's right one when facing right */
    int near_arm = (face > 0) ? P_ARMR : P_ARML;
    int far_arm  = (face > 0) ? P_ARML : P_ARMR;
    int near_leg = (face > 0) ? P_LEGR : P_LEGL;
    int far_leg  = (face > 0) ? P_LEGL : P_LEGR;

    /* fighting stance: boxer's guard - elbows bent, fists up by the chin, soft knees */
    float br = sinf(t * 0.09f);
    for (int k = 0; k < NPART; k++) { ps.a[k] = 0.0f; ps.b[k] = 0.0f; }
    ps.a[near_arm] = 0.40f + 0.03f * br;  ps.b[near_arm] = 2.0f;
    ps.a[far_arm]  = 0.30f - 0.03f * br;  ps.b[far_arm]  = 2.2f;
    ps.a[near_leg] = 0.16f;  ps.b[near_leg] = -0.14f;
    ps.a[far_leg]  = -0.16f; ps.b[far_leg]  = -0.10f;
    bob = 0.008f * br;

    if (atk > 0) {
        float p = g_prog;
        float se, s;
        if (atk == 1) {            /* light punch: quick jab, the fist shoots out of the guard */
            s = strike(p, (float)mstart[1] / (float)mdur[1]); se = s > 0.0f ? s : 0.0f;
            ps.a[near_arm] = 0.40f + s * 1.15f;
            ps.b[near_arm] = 2.0f * (1.0f - se);
            lean = 0.16f * se; shift = 0.30f * se;
        } else if (atk == 2) {     /* heavy punch: big swing with the far arm */
            s = strike(p, (float)mstart[2] / (float)mdur[2]); se = s > 0.0f ? s : 0.0f;
            ps.a[far_arm]  = 0.30f + s * 1.25f;
            ps.b[far_arm]  = 2.2f * (1.0f - se);
            ps.a[near_arm] = 0.55f - 0.30f * se;
            ps.a[P_HEAD]   = -0.10f * se;
            lean = 0.34f * se; shift = 0.65f * se;
        } else if (atk == 3) {     /* light kick: knee comes up, then the foot snaps out */
            s = strike(p, (float)mstart[3] / (float)mdur[3]); se = s > 0.0f ? s : 0.0f;
            ps.a[near_leg] = 0.16f + s * 1.15f;
            ps.b[near_leg] = -0.90f * (1.0f - se);
            ps.a[far_leg]  = -0.10f; ps.b[far_leg] = -0.12f;
            lean = -0.16f * se; shift = 0.25f * se;
        } else {                   /* heavy kick: high far-leg kick */
            s = strike(p, (float)mstart[4] / (float)mdur[4]); se = s > 0.0f ? s : 0.0f;
            ps.a[far_leg]  = -0.16f + s * 1.75f;
            ps.b[far_leg]  = -1.00f * (1.0f - se);
            ps.a[near_leg] = 0.10f; ps.b[near_leg] = -0.15f;
            lean = -0.30f * se; shift = 0.45f * se;
        }
    } else if (block) {            /* guard: both forearms up in front of the face */
        ps.a[near_arm] = 0.75f; ps.b[near_arm] = 1.95f;
        ps.a[far_arm]  = 0.85f; ps.b[far_arm]  = 2.00f;
        ps.a[P_HEAD]   = -0.08f;
        ps.a[near_leg] = 0.28f; ps.b[near_leg] = -0.20f;
        ps.a[far_leg]  = -0.22f; ps.b[far_leg] = -0.20f;
        lean = -0.08f; sy = 0.94f;
    } else if (air) {              /* jump: knees up, fists up */
        ps.a[near_leg] = 0.65f;  ps.b[near_leg] = -1.00f;
        ps.a[far_leg]  = -0.25f; ps.b[far_leg]  = -0.50f;
        lean = 0.05f; sy = 1.03f;
    } else if (g_dash == 1) {      /* dash forward */
        ps.a[near_leg] = 0.85f;  ps.b[near_leg] = -0.40f;
        ps.a[far_leg]  = -0.65f; ps.b[far_leg]  = -0.30f;
        ps.a[near_arm] = -0.50f; ps.b[near_arm] = 0.90f;
        ps.a[far_arm]  = -0.30f; ps.b[far_arm]  = 0.60f;
        lean = 0.34f; shift = 0.2f;
    } else if (g_dash == 2) {      /* hop back */
        ps.a[near_leg] = -0.45f; ps.b[near_leg] = -0.30f;
        ps.a[far_leg]  = 0.55f;  ps.b[far_leg]  = -0.50f;
        ps.a[near_arm] = 0.60f;  ps.b[near_arm] = 2.00f;
        ps.a[far_arm]  = 0.70f;  ps.b[far_arm]  = 2.10f;
        lean = -0.22f; bob = 0.18f;
    } else if (walk || g_walk_blend > 0.01f) {
        /*
         * Distance-driven gait: the phase advances with real displacement,
         * not the global render frame. This keeps foot cadence tied to speed
         * and lets the last half-step settle naturally when movement stops.
         */
        float g = sinf(g_walk_phase);
        float gait = g_walk_blend;
        if (g_walk_mode) {
            /* Side-walk: compact knee/hip action with a lateral weight shift,
               rather than the exaggerated forward/back running stride. */
            ps.a[near_leg] = 0.12f * g * gait;
            ps.a[far_leg]  = -0.12f * g * gait;
            ps.b[near_leg] = 0.62f * g * gait;
            ps.b[far_leg]  = -0.62f * g * gait;
            ps.a[near_arm] = 0.38f + 0.16f * g * gait;
            ps.a[far_arm]  = 0.32f - 0.16f * g * gait;
            lean = 0.035f * g * gait;
            bob = fabsf(g) * 0.035f * gait;
        } else {
            ps.a[near_leg] = 0.55f * g * gait;
            ps.a[far_leg]  = -0.55f * g * gait;
            ps.b[near_leg] = (g < 0.0f) ? 0.90f * g * gait : 0.0f;
            ps.b[far_leg]  = (g > 0.0f) ? -0.90f * g * gait : 0.0f;
            ps.a[near_arm] = 0.40f - 0.10f * g * gait;
            ps.a[far_arm]  = 0.30f + 0.10f * g * gait;
            lean = 0.06f * gait;
            bob = fabsf(g) * 0.06f * gait;
        }
    }

    if (g_crouch) {                /* ducking: low, guard up */
        sy = 0.68f;
        if (atk == 0) {
            ps.a[near_leg] = 0.55f; ps.b[near_leg] = -0.60f;
            ps.a[far_leg]  = -0.35f; ps.b[far_leg] = -0.50f;
            ps.a[near_arm] = 0.55f; ps.b[near_arm] = 2.00f;
            ps.a[far_arm]  = 0.45f; ps.b[far_arm]  = 2.20f;
            ps.a[P_HEAD] = -0.1f;
        }
        lean += 0.10f;
    }

    if (flash) {                   /* getting hit: snap back */
        lean = -0.24f;
        ps.a[P_HEAD] = 0.35f;
        ps.a[near_arm] = -0.5f; ps.b[near_arm] = 0.5f;
        ps.a[far_arm]  = -0.3f; ps.b[far_arm]  = 0.5f;
        ps.a[near_leg] = 0.1f;  ps.b[near_leg] = -0.10f;
        ps.a[far_leg]  = -0.1f; ps.b[far_leg]  = -0.10f;
    }

    /* feet stay planted: cancel the body lean on the legs that are standing */
    if (atk == 3)      ps.a[far_leg] += lean;
    else if (atk == 4) ps.a[near_leg] += lean;
    else { ps.a[near_leg] += lean; ps.a[far_leg] += lean; }

    V3 p = wp((float)x + 20.0f, (float)(y + 80), z);        /* feet position */
    float yaw = (face < 0 ? 3.1415927f : 0.0f) - MODEL_TURN * (float)face + g_yawadd;
    model_draw(style, p.x + shift * (float)face * cosf(g_la), p.y + bob, p.z + shift * (float)face * sinf(g_la),
               yaw, lean, sy, 0xFFFFFFFF, flash, &ps);
    return 1;
}

static int draw_model_fallen(int x, int style) {
    if (style < 0 || style >= MODEL_MAX || models[style].ntri == 0) return 0;
    V3 p = wp((float)x + 43.0f, (float)GROUND, 0.0f);
    model_draw(style, p.x, p.y - (MODEL_HEIGHT * 0.5f - 0.6f), p.z, -g_la, -1.5707963f, 1.0f, 0xFFFFFFFF, 0, NULL);
    return 1;
}

/* =====================================================================
   3D fighter (round shapes + textures)
   atk: 0 none, 1 light punch, 2 heavy punch, 3 light kick, 4 heavy kick
   style: 0 brawler, 1 speedster, 2 heavy, 3 stand
   ===================================================================== */
static void draw_fighter(int x, int y, int face, int flash, int atk, int block,
                         int walk, int frame, int style, float z) {
    if (draw_model_fighter(x, y, face, flash, atk, block, walk, frame, style, z)) return;

    float cx = (float)x + 20.0f, ty = (float)y;
    int air = (y < GROUND - 80);
    int stand = (style == 3);
    int ts = stand ? 0 : style;
    int th = stand ? T_STAND : T_HEAD0 + ts;
    int tb = stand ? T_STAND : T_BODY0 + ts;
    int tl = stand ? T_STAND : T_LEGS0 + ts;
    uint32_t tc = stand ? g_stand : 0xFFFFFFFF;
    uint32_t skin = stand ? g_stand : cskin[ts];
    uint32_t boot = stand ? g_stand : bootc[ts];
    float yaw = face * 0.55f + g_yawadd;
    float big = (style == 2) ? 1.0f : 0.0f;
    V3 p, hd;

    g_flash = flash;
    g_flat = flash;

#define CR(v) (g_crouch ? 80.0f - (80.0f - (float)(v)) * 0.68f : (float)(v))   /* ducking squashes the body */
#define J(dx, dy) wp(cx + (float)(dx), ty + CR(dy), z)

    /* head */
    hd = J(0, 10);
    ell(hd.x, hd.y, hd.z, 0.55f, 0.58f, 0.55f, yaw, th, tc, 14, 8);

    /* torso */
    tube(J(0, 20), J(0, 50), 0.46f + 0.26f * big, 0.34f + 0.21f * big, 0.72f, tb, tc);

    /* arms */
    int fx, fy, bx, by;
    if (atk == 1)      { fx = face * 38; fy = 30; bx = -face * 10; by = 42; }
    else if (atk == 2) { fx = face * 58; fy = 26; bx = -face * 16; by = 34; }
    else if (block)    { fx = face * 16; fy = 16; bx = face * 8;   by = 14; }
    else               { fx = face * 14; fy = 42; bx = -face * 12; by = 42; }
    {
        float ar = 0.19f + 0.07f * big;
        tube(J(0, 28), J(fx, fy), ar, ar * 0.8f, 1.0f, tb, tc);
        tube(J(0, 28), J(bx, by), ar, ar * 0.8f, 1.0f, tb, tc);
    }
    p = J(fx, fy); ell(p.x, p.y, p.z, 0.21f, 0.21f, 0.21f, 0.0f, -1, skin, 6, 4);
    p = J(bx, by); ell(p.x, p.y, p.z, 0.21f, 0.21f, 0.21f, 0.0f, -1, skin, 6, 4);

    /* legs */
    int l1x, l1y, l2x, l2y;
    float fallbackGait = sinf(g_walk_phase) * g_walk_blend;
    int gaitStep = (int)(10.0f * fallbackGait);
    int liftNear = (gaitStep < 0) ? -gaitStep / 2 : 0;
    int liftFar  = (gaitStep > 0) ? gaitStep / 2 : 0;
    if (atk == 3)      { l1x = face * 50; l1y = 64; l2x = -face * 6; l2y = 80; }
    else if (atk == 4) { l1x = face * 66; l1y = 42; l2x = -face * 8; l2y = 80; }
    else if (air)      { l1x = 10; l1y = 70; l2x = -10; l2y = 72; }
    else if (walk || g_walk_blend > 0.01f) {
        l1x = 8 + gaitStep;  l1y = 80 - liftNear;
        l2x = -8 - gaitStep; l2y = 80 - liftFar;
    } else               { l1x = 8; l1y = 80; l2x = -8; l2y = 80; }
    {
        float lr = 0.24f + 0.08f * big;
        tube(J(0, 50), J(l1x, l1y), lr, lr * 0.72f, 1.0f, tl, tc);
        tube(J(0, 50), J(l2x, l2y), lr, lr * 0.72f, 1.0f, tl, tc);
    }
    p = J(l1x + face * 4, l1y - 1);
    ell(p.x, p.y, p.z, 0.34f + 0.06f * big, 0.17f, 0.26f, 0.0f, -1, boot, 8, 4);
    p = J(l2x + face * 4, l2y - 1);
    ell(p.x, p.y, p.z, 0.34f + 0.06f * big, 0.17f, 0.26f, 0.0f, -1, boot, 8, 4);

    /* ---- extras ---- */
    if (style == 0) {
        /* cap + brim */
        ell(hd.x, hd.y + 0.28f, hd.z, 0.62f, 0.36f, 0.62f, yaw, -1, 0xFFF0F0F0, 10, 6);
        ell(hd.x + sinf(yaw) * 0.5f, hd.y + 0.15f, hd.z + cosf(yaw) * 0.5f,
            0.40f, 0.06f, 0.40f, 0.0f, -1, 0xFFF0F0F0, 10, 4);
        /* scarf + fluttering tail */
        p = J(0, 21);
        ell(p.x, p.y, p.z, 0.62f, 0.15f, 0.52f, 0.0f, -1, 0xFF2020E0, 10, 4);
        {
            float wob = (float)(((frame / 5) % 2) * 3);
            tube(J(-face * 9, 22), J(-face * 12, 36.0f + wob), 0.15f, 0.10f, 0.4f, -1, 0xFF2020E0);
        }
        /* belt */
        p = J(0, 48);
        ell(p.x, p.y, p.z, 0.44f, 0.10f, 0.38f, 0.0f, -1, 0xFF00D0FF, 10, 4);
    } else if (style == 1) {
        /* spiky hair */
        tube(lo(hd, -0.25f, 0.38f, 0.0f), lo(hd, -0.55f, 1.0f, 0.0f),
             0.17f, 0.02f, 0.8f, -1, 0xFF30D0FF);
        tube(lo(hd, 0.0f, 0.45f, 0.0f), lo(hd, 0.0f, 1.2f, 0.0f),
             0.20f, 0.02f, 0.8f, -1, 0xFF30D0FF);
        tube(lo(hd, 0.25f, 0.38f, 0.0f), lo(hd, 0.55f, 1.0f, 0.0f),
             0.17f, 0.02f, 0.8f, -1, 0xFF30D0FF);
        /* headband */
        ell(hd.x, hd.y + 0.15f, hd.z, 0.57f, 0.07f, 0.57f, 0.0f, -1, 0xFFFFFFFF, 12, 3);
        /* belt */
        p = J(0, 48);
        ell(p.x, p.y, p.z, 0.42f, 0.10f, 0.36f, 0.0f, -1, 0xFFFFFFFF, 10, 4);
    } else if (style == 2) {
        /* shoulder pads */
        p = J(-13, 25); ell(p.x, p.y, p.z, 0.50f, 0.30f, 0.46f, 0.0f, -1, 0xFF909090, 8, 5);
        p = J(13, 25);  ell(p.x, p.y, p.z, 0.50f, 0.30f, 0.46f, 0.0f, -1, 0xFF909090, 8, 5);
        /* mohawk */
        for (int m = -1; m <= 1; m++) {
            float tip = (m == 0) ? 1.15f : 0.95f;
            tube(lo(hd, 0.0f, 0.45f, m * 0.3f), lo(hd, 0.0f, tip, m * 0.3f),
                 0.14f, 0.02f, 1.0f, -1, 0xFF2080FF);
        }
        /* big belt + buckle */
        p = J(0, 48);
        ell(p.x, p.y, p.z, 0.66f, 0.15f, 0.55f, 0.0f, -1, 0xFF30D0FF, 10, 4);
        { V3 q = lo(p, 0.0f, 0.0f, 0.5f); ell(q.x, q.y, q.z, 0.20f, 0.14f, 0.08f, 0.0f, -1, 0xFF00FFFF, 6, 4); }
    } else {
        /* Stand: horns + chest gem */
        tube(lo(hd, -0.28f, 0.45f, 0.0f), lo(hd, -0.40f, 1.0f, 0.0f),
             0.13f, 0.02f, 0.8f, -1, 0xFFFFFF80);
        tube(lo(hd, 0.28f, 0.45f, 0.0f), lo(hd, 0.40f, 1.0f, 0.0f),
             0.13f, 0.02f, 0.8f, -1, 0xFFFFFF80);
        p = J(0, 32);
        { V3 q = lo(p, 0.0f, 0.0f, 0.35f); ell(q.x, q.y, q.z, 0.18f, 0.18f, 0.10f, 0.0f, -1, 0xFF00FFFF, 6, 4); }
    }
    g_flat = 0;
    g_flash = 0;
#undef J
#undef CR
}

static void draw_fallen(int x, int style) {
    if (draw_model_fallen(x, style)) return;
    g_flat = 0;
    g_flash = 0;
    uint32_t w = 0xFFFFFFFF;
    V3 hd = wp((float)x + 8.0f, (float)GROUND - 10.0f, 0.0f);
    ell(hd.x, hd.y, hd.z, 0.55f, 0.55f, 0.55f, 0.0f, T_HEAD0 + style, w, 12, 8);
    tube(wp(x + 18.0f, GROUND - 8.0f, 0), wp(x + 55.0f, GROUND - 8.0f, 0),
         0.40f, 0.34f, 0.8f, T_BODY0 + style, w);
    tube(wp(x + 55.0f, GROUND - 8.0f, 0), wp(x + 75.0f, GROUND - 14.0f, 0),
         0.22f, 0.17f, 1.0f, T_LEGS0 + style, w);
    tube(wp(x + 55.0f, GROUND - 8.0f, 0), wp(x + 75.0f, GROUND - 2.0f, 0),
         0.22f, 0.17f, 1.0f, T_LEGS0 + style, w);
    tube(wp(x + 25.0f, GROUND - 8.0f, 0), wp(x + 35.0f, GROUND - 22.0f, 0),
         0.18f, 0.15f, 1.0f, T_BODY0 + style, w);
}

static void draw_shield(float xpx, float ypx) {
    V3 p = wp(xpx, ypx, 0.0f);
    g_flat = 1;
    {
        float bw = 0.3f, bd = 1.5f;
        if (fabsf(sinf(g_la)) > 0.7f) { bw = 1.5f; bd = 0.3f; }
        box(p.x, p.y, p.z, bw, 2.5f, bd, 0xFF00E0FF);
    }
    g_flat = 0;
}

/* =====================================================================
   Frame control
   ===================================================================== */
static void begin_frame(void) {
    sceGuStart(GU_DIRECT, list);
    cur_tex = -2;
    sceGuClearColor(0xFF2A0C06);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuEnable(GU_DEPTH_TEST);
}

static void end_frame(void) {
    flush_text();
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
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
/* ---------- ring wall: the fighter at the wall stops, the line swings around the OTHER fighter
   (the enemy is never dragged along).  prevd = line distance (pixels) of the last frame. ---------- */
static void ring_core(int *px, int *ex, float *ox, float *oz, float *ang, int prevd) {
    float c = cosf(*ang), s = sinf(*ang);
    float xp = ((float)*px + 20.0f - 240.0f) / 20.0f;
    float xe = ((float)*ex + 20.0f - 240.0f) / 20.0f;
    float Px = *ox + c * xp, Pz = *oz + s * xp;
    float Ex = *ox + c * xe, Ez = *oz + s * xe;
    float rpx = Px - STAGE_CX, rex = Ex - STAGE_CX;
    float rp = sqrtf(rpx * rpx + Pz * Pz), re = sqrtf(rex * rex + Ez * Ez);
    int pout = (rp > RING_LIMIT), eout = (re > RING_LIMIT);
    if (!pout && !eout) return;

    if (pout && eout) {                      /* both at the wall: slide the whole line back */
        for (int it = 0; it < 3; it++) {
            for (int k = 0; k < 2; k++) {
                float fx = (k == 0) ? Px : Ex, fz = (k == 0) ? Pz : Ez;
                float rx = fx - STAGE_CX;
                float rr = sqrtf(rx * rx + fz * fz);
                if (rr > RING_LIMIT) {
                    float k2 = (rr - RING_LIMIT) / rr;
                    float sx = rx * k2, sz = fz * k2;
                    *ox -= sx; *oz -= sz;
                    Px -= sx; Pz -= sz; Ex -= sx; Ez -= sz;
                }
            }
        }
        return;
    }

    /* one fighter (M) is at the wall, the other (F) stays exactly where he is */
    float Fx, Fz, Mx, Mz;
    if (pout) { Fx = Ex; Fz = Ez; Mx = Px; Mz = Pz; }
    else      { Fx = Px; Fz = Pz; Mx = Ex; Mz = Ez; }
    float dl = (float)((*px > *ex) ? (*px - *ex) : (*ex - *px));
    if (dl > (float)prevd) dl = (float)prevd;          /* can't walk (or be knocked) farther out of the ring */
    float d = dl / 20.0f;
    float R = RING_LIMIT;
    float fxr = Fx - STAGE_CX, fzr = Fz;
    float D = sqrtf(fxr * fxr + fzr * fzr);
    float nx, nz;                                      /* new place of M, relative to the ring centre */
    float mxr = Mx - STAGE_CX;
    if (D < 0.05f) {
        float ml = sqrtf(mxr * mxr + Mz * Mz);
        if (ml < 1e-4f) ml = 1.0f;
        nx = mxr / ml * R; nz = Mz / ml * R;
    } else {
        float ux = fxr / D, uz = fzr / D;
        float a = (R * R - d * d + D * D) / (2.0f * D);
        float h2 = R * R - a * a;
        if (h2 < 0.0f) {
            /*
             * The requested separation cannot fit on the ring. Keep M on the
             * nearest boundary point to its current position. The old fallback
             * selected the opposite side and could teleport a fighter.
             */
            float ml = sqrtf(mxr * mxr + Mz * Mz);
            if (ml < 1e-4f) {
                nx = ux * R;
                nz = uz * R;
            } else {
                nx = mxr * (R / ml);
                nz = Mz * (R / ml);
            }
        } else {
            float h = sqrtf(h2);
            float side = mxr * (-uz) + Mz * ux;        /* keep the solution on M's own side */
            if (side < 0.0f) h = -h;
            nx = ux * a - uz * h;
            nz = uz * a + ux * h;
        }
    }
    float nMx = STAGE_CX + nx, nMz = nz;
    float ddx = nMx - Fx, ddz = nMz - Fz;
    int dn = (int)(sqrtf(ddx * ddx + ddz * ddz) * 20.0f + 0.5f);
    if (pout) { int sg = (*px >= *ex) ? 1 : -1; *px = *ex + sg * dn; Px = nMx; Pz = nMz; }
    else      { int sg = (*ex >= *px) ? 1 : -1; *ex = *px + sg * dn; Ex = nMx; Ez = nMz; }

    /* rebuild the line from the two world positions (the angle keeps turning smoothly) */
    float Vx = Ex - Px, Vz = Ez - Pz;
    if (Vx * Vx + Vz * Vz > 1e-4f) {
        float na = (*px < *ex) ? atan2f(Vz, Vx) : atan2f(-Vz, -Vx);
        float da = na - *ang;
        while (da > 3.1415927f) da -= 6.2831853f;
        while (da < -3.1415927f) da += 6.2831853f;
        *ang += da;
    }
    float nc = cosf(*ang), ns = sinf(*ang);
    float nxp = ((float)*px + 20.0f - 240.0f) / 20.0f;
    *ox = Px - nc * nxp;
    *oz = Pz - ns * nxp;
}

/* slide the line's numbers so the two fighters are always centred on it: both sides of the
   line then have the same room, and the numbers never drift away */
static void ring_fix(int *px, int *ex, float *ox, float *oz, float *ang, int prevd) {
    ring_core(px, ex, ox, oz, ang, prevd);
    int sh = (*px + *ex + 40) / 2 - 240;
    if (sh != 0) {
        *px -= sh; *ex -= sh;
        *ox += cosf(*ang) * (float)sh / 20.0f;
        *oz += sinf(*ang) * (float)sh / 20.0f;
    }
}

int main(int argc, char **argv) {
    setup_callbacks();

    /* folder that holds EBOOT.PBP (and the PNG textures and OBJ models) */
    if (argc > 0 && argv != NULL && argv[0] != NULL) {
        strncpy(basedir, argv[0], sizeof(basedir) - 1);
        basedir[sizeof(basedir) - 1] = 0;
        char *sl = strrchr(basedir, '/');
        if (sl) sl[1] = 0; else basedir[0] = 0;
    }

    /* textures: build in code, then replace with any PNG files found */
    gen_all();
    for (int t = 0; t < NTEX; t++) ncustom += load_png(t, texname[t]);
    obj_load_all();
    sceKernelDcacheWritebackAll();

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
    int msgt = 0, msgn = 0;

    /* player */
    int px = 100, py = GROUND - 80, vy = 0, pface = 1;
    float pvelx = 0.0f, pfracx = 0.0f; /* smoothed horizontal movement + sub-pixel carry */
    int php = MAXHP, pflash = 0, pmeter = 0;
    int patk = 0, pt = 0, phit = 0;
    int psp = 0, pblock = 0, pwalk = 0;
    float ang = 0.0f, eaim = 0.0f;    /* angle of the fighting line / where the CPU is aiming */
    float ox = 0.0f, oz = 0.0f;       /* world position of the fighting line */
    float paim = 0.0f;                /* where YOU are aiming (locked while you attack) */
    int eside = 0, esdir = 1, escd = 0;   /* CPU side-step: frames left, direction, cooldown */
    int pdash = 0, pdashdir = 1, pinv = 0, pcrouch = 0, pjd = 0, pdrift = 0, pchain = 0;
    int ptapl = 99, ptapr = 99;       /* frames since the last left / right tap (for dashing) */
    int ukt = -1, dkt = -1;           /* frames UP / DOWN has been held (-1 = not tracking) */
    int utap = 99, dtap = 99;         /* frames since the last quick UP / DOWN tap */
    int dhold = 0;                    /* 1 = DOWN held long enough to duck */
    int pdprev = 240;                 /* distance between the fighters last frame (pixels) */
    int pside = 0, pswdir = 0, pswhold = 0;   /* side-step burst frames, direction, side-walk (2nd tap held) */
    float porbitvel = 0.0f;                  /* smoothed angular velocity for orbit movement */
    float pspiralcarry = 0.0f;               /* fractional inward travel during side-walk */
    float pstep_phase = 0.0f, pwalk_blend = 0.0f; /* player stride clock and gait fade */

    /* enemy */
    int ex = 340, eface = -1, ehp = MAXHP, eflash = 0, emeter = 0;
    int eatk = 0, et = 0, ecd = 60, ehit = 0;
    int eblock = 0, ebcd = 0, ewalk = 0, esp = 0;
    float estep_phase = 0.0f, ewalk_blend = 0.0f; /* CPU stride clock and gait fade */

    int ko = 0, pwon = 0, pwins = 0, ewins = 0, timeup = 0;
    int pcombo = 0, pcc = 0, ecombo = 0, ecc = 0;
    int shake = 0, rtime = ROUNDTIME;
    int frame = 0;
    unsigned int ibuf = 0;   /* buttons pressed during a hit-stop freeze */
    char buf[64];
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
            if (pressed & PSP_CTRL_SELECT) {
                msgn = export_all();
                msgt = 240;
            }
            if (msgt > 0) msgt--;
            if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
                ec = (pc + 1 + (frame & 1)) % 3;
                pwins = 0; ewins = 0;
                newround = 1;
                paused = 0;
                state = 1;
            }

            set_camera(v3(sinf(frame * 0.012f) * 1.5f, 3.0f, 13.0f), v3(0.0f, 2.3f, 0.0f));
            draw_stage(frame);
            for (int i = 0; i < 3; i++) {
                float wx = (cfx[i] + 20 - 240) / 20.0f;
                shadow(wx, 0.0f);
                draw_fighter(cfx[i], GROUND - 80, 1, 0, 0, 0, pc == i, frame, i, 0.0f);
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

            if (msgt > 0) {
                if (msgn > 0) sprintf(buf, "SAVED %d TEXTURE FILES", msgn);
                else          sprintf(buf, "COULD NOT SAVE FILES");
                textx(240, 25, msgn > 0 ? 0xFF30FF30 : 0xFF3030FF, buf);
            }
            textx(240, 27, 0xFFD0D0D0, "UP/DOWN: LEVEL   SQUARE: MODE");
            textx(240, 28, 0xFFD0D0D0, "SQ/TRI PUNCH  X/O KICK  L BLOCK  R STAND");
            textx(240, 29, 0xFFD0D0D0, "HOLD UP JUMP  HOLD DOWN DUCK  TAP UP/DN CIRCLE");
            sprintf(buf, "PNG: %d/12   OBJ: %d/3", ncustom, nmodels);
            textx(240, 30, 0xFFB0B0B0, buf);
            textx(240, 31, 0xFFB0B0B0, "SELECT: SAVE TEMPLATE PNGS");
            {
                int n = (int)strlen(basedir);
                const char *tail = (n > 44) ? basedir + (n - 44) : basedir;
                textx(240, 32, 0xFF808080, tail);
            }
        }

        /* ================= FIGHT ================= */
        else if (state == 1) {
            int frozen = 0;   /* 1 while a hit-stop freeze is running */

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
                    px = 100; py = GROUND - 80; vy = 0; pface = 1; pvelx = 0.0f; pfracx = 0.0f;
                    php = MAXHP; pflash = 0; pmeter = training ? 100 : 0;
                    patk = 0; pt = 0; phit = 0; psp = 0; pblock = 0; pwalk = 0; pstep_phase = 0.0f; pwalk_blend = 0.0f;
                    pdprev = 240;
                    ang = 0.0f; eaim = 0.0f; paim = 0.0f; ox = 0.0f; oz = 0.0f; eside = 0; escd = 0; pdash = 0; pinv = 0; pcrouch = 0; pjd = 0; pdrift = 0; pchain = 0; porbitvel = 0.0f; pspiralcarry = 0.0f;
                    ptapl = 99; ptapr = 99; utap = 99; dtap = 99; ukt = -1; dkt = -1; dhold = 0; pswhold = 0; pside = 0; pswdir = 0;
                    ex = 340; eface = -1; ehp = MAXHP; eflash = 0; emeter = 0;
                    eatk = 0; et = 0; ecd = 60; ehit = 0; eblock = 0; ebcd = 0; ewalk = 0; esp = 0; estep_phase = 0.0f; ewalk_blend = 0.0f;
                    ko = 0; timeup = 0;
                    pcombo = 0; pcc = 0; ecombo = 0; ecc = 0;
                    shake = 0; rtime = ROUNDTIME;
                    ibuf = 0;
                    clear_fx();
                }

                /* ---- hit-stop: freeze the fight, keep drawing ---- */
                frozen = (hitstop > 0);
                if (frozen) {
                    hitstop--;
                    ibuf |= pressed;          /* remember buttons pressed during the freeze */
                } else {
                    pressed |= ibuf;          /* ...and use them as soon as it ends */
                    ibuf = 0;
                    for (int i = 0; i < 8; i++) if (sparks[i].t > 0) sparks[i].t--;
                    update_fx();
                }

                if (frozen) {
                    /* time stands still */
                } else if (ko == 0) {
                    if (pcc > 0 && --pcc == 0) pcombo = 0;
                    if (ecc > 0 && --ecc == 0) ecombo = 0;
                    if (rtime > 0) rtime--;

                    int busy = (patk != 0 || psp > 0);
                    int onground = (py >= GROUND - 80);
                    int dirp = 0;
                    if (pad.Buttons & PSP_CTRL_LEFT)  dirp = -1;
                    if (pad.Buttons & PSP_CTRL_RIGHT) dirp = 1;
                    if (ptapl < 99) ptapl++;
                    if (ptapr < 99) ptapr++;
                    if (utap < 99) utap++;
                    if (dtap < 99) dtap++;
                    if (pinv > 0) pinv--;

                    /* always face the opponent (like Tekken) when on the ground and not attacking */
                    if (!busy && onground && ex != px) pface = (ex > px) ? 1 : -1;

                    /* block (hold L) */
                    pblock = 0;
                    if (!busy && onground && pdash == 0 && (pad.Buttons & PSP_CTRL_LTRIGGER)) pblock = 1;

                    /* UP / DOWN: quick tap = side-step (a short circle), hold = jump (UP) or duck (DOWN),
                       tap then press again and hold = side-walk */
                    {
                        int can = (!busy && onground && !pblock && pdash == 0 && psp == 0);
                        if (!can) { ukt = -1; dkt = -1; }
                        if (can && (pressed & PSP_CTRL_UP)) {
                            if (utap < DOUBLETAP) { pswdir = -1; pswhold = 1; ukt = -1; utap = 99; }
                            else ukt = 0;
                        }
                        if (ukt >= 0) {
                            if (pad.Buttons & PSP_CTRL_UP) {
                                if (++ukt >= HOLD_FRAMES) {          /* held: jump */
                                    vy = -9; /* roughly half a fighter-height, with the existing gravity */
                                    pjd = dirp * cspd[pc];
                                    ukt = -1;
                                }
                            } else {                                  /* let go early: side-step */
                                pswdir = -1; pside = 8; pswhold = 0; utap = 0;
                                ukt = -1;
                            }
                        }
                        if (can && (pressed & PSP_CTRL_DOWN)) {
                            if (dtap < DOUBLETAP) { pswdir = 1; pswhold = 1; dkt = -1; dtap = 99; }
                            else dkt = 0;
                        }
                        if (dkt >= 0) {
                            if (pad.Buttons & PSP_CTRL_DOWN) {
                                if (++dkt >= HOLD_FRAMES) { dhold = 1; dkt = -1; }   /* held: duck */
                            } else {
                                pswdir = 1; pside = 8; pswhold = 0; dtap = 0;
                                dkt = -1;
                            }
                        }
                        if (!(pad.Buttons & PSP_CTRL_DOWN)) dhold = 0;
                    }

                    /* duck (hold DOWN): punches fly over your head */
                    pcrouch = (onground && !pblock && pdash == 0 && psp == 0 && pswdir == 0 && (dhold || dkt >= DUCK_FRAMES)) ? 1 : 0;

                    /* double-tap left/right = dash (hopping back dodges for a moment) */
                    if (!busy && onground && !pblock && !pcrouch && pdash == 0) {
                        if (pressed & PSP_CTRL_RIGHT) {
                            if (ptapr < 16) { pdash = 10; pdashdir = 1; ptapr = 99; } else ptapr = 0;
                        }
                        if (pressed & PSP_CTRL_LEFT) {
                            if (ptapl < 16) { pdash = 10; pdashdir = -1; ptapl = 99; } else ptapl = 0;
                        }
                        if (pdash > 0 && pdashdir != pface) pinv = 8;
                    }

                    /* movement: accelerate and decelerate instead of snapping to full speed.
                       Fractional carry keeps slow movement smooth on the PSP's integer screen coordinates. */
                    {
                        float target = 0.0f;
                        if (!onground) {
                            target = (float)pjd;                    /* keep the jump's chosen direction */
                        } else if (pdash > 0) {
                            target = (float)(pdashdir * ((pdashdir == pface) ? 6 : 5));
                            pdash--;
                        } else if (!pblock && !pcrouch && psp == 0 && patk == 0 && dirp != 0) {
                            int sp = cspd[pc];
                            if (dirp != pface) sp = (sp * 3 + 3) / 4; /* backward movement is slower */
                            target = (float)(dirp * sp);
                        }

                        /* Faster response when reversing direction; softer easing when starting/stopping. */
                        {
                            float rate = 0.28f;
                            if (target == 0.0f) rate = 0.22f;
                            else if (pvelx * target < 0.0f) rate = 0.42f;
                            pvelx += (target - pvelx) * rate;
                        }
                        if (target == 0.0f && fabsf(pvelx) < 0.08f) pvelx = 0.0f;
                        pfracx += pvelx;
                        {
                            int step = (int)pfracx;
                            pfracx -= (float)step;
                            px += step;
                        }
                        pwalk = (fabsf(pvelx) > 0.35f || target != 0.0f) ? 1 : 0;
                    }
                    {
                        int oldpx = px;
                        px = clampx(px);
                        if (px != oldpx) { pvelx = 0.0f; pfracx = 0.0f; }
                    }

                    /*
                     * Tekken-style orbit controller. Quick taps create a short
                     * sidestep; holding the second tap keeps orbiting. Angular
                     * speed ramps in and out instead of snapping.
                     */
                    {
                        float orbitTarget = 0.0f;
                        int orbitAllowed = (onground && patk == 0 && psp == 0 && !pblock);
                        if (!orbitAllowed) {
                            pswdir = 0; pside = 0; pswhold = 0;
                        } else if (pswdir != 0) {
                            if (pside > 0) {
                                orbitTarget = (float)(pswdir * pface) * 0.145f;
                                pside--;
                                pwalk = 1;
                            } else if (pswhold &&
                                (pad.Buttons & ((pswdir < 0) ? PSP_CTRL_UP : PSP_CTRL_DOWN))) {
                                orbitTarget = (float)(pswdir * pface) * 0.075f;
                                pwalk = 1;
                            } else {
                                pswdir = 0;
                                pswhold = 0;
                            }
                        }

                        porbitvel += (orbitTarget - porbitvel) *
                                     ((orbitTarget == 0.0f) ? 0.18f : 0.22f);
                        if (fabsf(porbitvel) < 0.0008f && orbitTarget == 0.0f)
                            porbitvel = 0.0f;

                        if (fabsf(porbitvel) > 0.0001f) {
                            float dw = (float)((px > ex) ? (px - ex) : (ex - px)) / 20.0f;
                            if (dw < 2.2f) dw = 2.2f;
                            float na = ang - porbitvel / dw;
                            float xe = (float)(ex + 20 - 240) / 20.0f;
                            ox += (cosf(ang) - cosf(na)) * xe;
                            oz += (sinf(ang) - sinf(na)) * xe;
                            ang = na;
                            /* Spiral inward slowly: roughly 0.25 screen pixels/frame.
                               This closes the gap over a long side-walk instead of
                               keeping the player on a fixed-radius circle. */
                            if (pswdir != 0 && (pside > 0 || pswhold)) {
                                pspiralcarry += 0.25f;
                                if (pspiralcarry >= 1.0f) {
                                    int inward = (int)pspiralcarry;
                                    pspiralcarry -= (float)inward;
                                    if (px < ex) px += inward;
                                    else if (px > ex) px -= inward;
                                }
                            } else {
                                pspiralcarry *= 0.85f;
                            }
                            pwalk = 1;
                        }
                    }

                    /*
                     * Advance the player's gait from movement distance.
                     * Orbit angular speed is converted to a modest equivalent
                     * stride speed so side-walking also animates while px stays
                     * almost stationary on screen.
                     */
                    {
                        float stride = fabsf(pvelx) + fabsf(porbitvel) * 14.0f;
                        if (onground && patk == 0 && !pblock && !pcrouch) {
                            pstep_phase += stride * 0.10f;
                            if (pstep_phase >= 6.2831853f)
                                pstep_phase = fmodf(pstep_phase, 6.2831853f);
                        }
                        {
                            float gaitTarget = (pwalk && onground && patk == 0 &&
                                                !pblock && !pcrouch) ? 1.0f : 0.0f;
                            pwalk_blend += (gaitTarget - pwalk_blend) * (gaitTarget > pwalk_blend ? 0.18f : 0.12f);
                            if (pwalk_blend < 0.005f) pwalk_blend = 0.0f;
                        }
                    }

                    /* you face the opponent unless you are in the middle of an attack */
                    if (patk == 0 && psp == 0) paim = ang;

                    vy += 1;
                    py += vy;
                    if (py > GROUND - 80) { py = GROUND - 80; vy = 0; pjd = 0; }

                    /* Airborne fighters remain solid: jumping cannot vault through
                       or land on the far side of the opponent. */
                    if (py < GROUND - 80) {
                        int airGap = ex - px;
                        if (airGap > 0 && airGap < BODY_GAP) {
                            px = ex - BODY_GAP;
                            pvelx = 0.0f; pfracx = 0.0f;
                        } else if (airGap < 0 && airGap > -BODY_GAP) {
                            px = ex + BODY_GAP;
                            pvelx = 0.0f; pfracx = 0.0f;
                        }
                    }

                    /* start attacks (also chain into the next hit right after one connects) */
                    {
                        int t = 0;
                        if (pressed & PSP_CTRL_SQUARE)        t = 1;
                        else if (pressed & PSP_CTRL_TRIANGLE) t = 2;
                        else if (pressed & PSP_CTRL_CROSS)    t = 3;
                        else if (pressed & PSP_CTRL_CIRCLE)   t = 4;
                        int canchain = (patk > 0 && phit > 0 && psp == 0 && pchain < 3 &&
                                        (mdur[patk] - pt) >= mstart[patk] + 2);
                        if (t && !pblock && (!busy || canchain)) {
                            if (busy) pchain++; else pchain = 0;
                            patk = t; pt = mdur[t]; phit = 0;
                            if (pdash > 0 && pdashdir == pface) pdrift = 5;   /* dash + hit = lunge */
                            else if (!busy) pdrift = 0;
                            pdash = 0;
                        } else if (!busy && !pblock && (pressed & PSP_CTRL_RTRIGGER) && pmeter >= 100) {
                            psp = 45; pmeter = 0;
                        }
                    }

                    /* player attack */
                    if (patk > 0) {
                        if (pdrift > 0) {
                            px = clampx(px + pface * pdrift);
                            if ((frame & 1) == 0) pdrift--;
                        }
                        int el = mdur[patk] - pt;
                        if (!phit && el >= mstart[patk] && el < mstart[patk] + mact[patk]) {
                            int bx2, by2, bw2, bh2;
                            atkbox(patk, px, py, pface, &bx2, &by2, &bw2, &bh2);
                            if (overlap(bx2, by2, bw2, bh2, ex, GROUND - 80, 40, 80) &&
                                latoff(px, ex, ang - paim) > ztol(patk)) {
                                phit = -1;                      /* the CPU stepped out of the way */
                            } else if (overlap(bx2, by2, bw2, bh2, ex, GROUND - 80, 40, 80)) {
                                phit = 1;
                                int d = movedmg(patk, pc);
                                if (eblock > 0) {
                                    int c = d / 5; if (c < 1) c = 1;
                                    ehp -= c; emeter += 2;
                                    hitfx((eface == 1) ? ex + 44 : ex - 4, by2 + bh2 / 2,
                                          pface, 0, 1, 1, mhs[patk] / 2);
                                    shake = 2;
                                } else {
                                    ehp -= d; eflash = 6; pmeter += mmtr[patk]; emeter += 8;
                                    ex = clampx(ex + mkb[patk] * pface);
                                    hitfx(ex + 20, by2 + bh2 / 2, pface,
                                          (patk == 2 || patk == 4) ? 3 : 1, 0, 1, mhs[patk]);
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
                        if (psp % 6 == 0 && overlap(ax, py, 80, 80, ex, GROUND - 80, 40, 80) && latoff(px, ex, ang - paim) <= 1.6f) {
                            if (eblock > 0) {
                                ehp -= 1;
                                hitfx((eface == 1) ? ex + 44 : ex - 4, py + 20 + rnd() % 40,
                                      pface, 0, 1, 1, 0);
                            } else {
                                ehp -= 4; eflash = 4;
                                ex = clampx(ex + 4 * pface);
                                hitfx(ex + 20, py + 15 + rnd() % 50, pface, 0, 0, 1, 2);
                                shake = 4;
                                pcombo = (pcc > 0) ? pcombo + 1 : 1;
                                pcc = 70; ecombo = 0; ecc = 0;
                            }
                        }
                        psp--;
                    }

                    /* ---------- enemy ---------- */
                    if (eatk == 0 && esp == 0) {
                        float tr = 0.09f;                         /* the CPU turns fast, on every level */
                        float dzz = ang - eaim;                   /* the CPU turns to follow you */
                        if (dzz > tr) eaim += tr; else if (dzz < -tr) eaim -= tr; else eaim = ang;
                    }
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
                        if (escd > 0) escd--;

                        /* the CPU sometimes circles out of the way of your attack (harder levels do it more) */
                        int ecirc = 0;
                        if (eside > 0 && eatk == 0 && eblock == 0 && esp == 0) {
                            float dw = (float)dist / 20.0f;
                            if (dw < 2.2f) dw = 2.2f;
                            float na = ang + (float)(eface * esdir) * 0.17f / dw;
                            float xp = (float)(px + 20 - 240) / 20.0f;       /* turn around the player */
                            ox += (cosf(ang) - cosf(na)) * xp;
                            oz += (sinf(ang) - sinf(na)) * xp;
                            ang = na; eaim = ang;
                            eside--; ecirc = 1;
                        } else {
                            eside = 0;
                            if (lvl > 0 && escd == 0 && eatk == 0 && eblock == 0 && esp == 0 &&
                                patk > 0 && phit == 0 && dist < 110 && rnd() % (lvl == 1 ? 45 : 20) == 0) {
                                eside = 8; esdir = (rnd() % 2) ? 1 : -1; escd = 120;
                            }
                        }

                        if ((patk > 0 || psp > 0) && dist < 110 && eblock == 0 && ebcd == 0 &&
                            eatk == 0 && esp == 0) {
                            if (rnd() % eblk[lvl] == 0) { eblock = 25; ebcd = 90; }
                        }

                        if (esp == 0 && eatk == 0 && eblock == 0 && emeter >= 100 && dist < 100) {
                            esp = 45; emeter = 0;
                        }

                        if (!ecirc && eatk == 0 && eblock == 0 && esp == 0) {
                            if (dist > 60) {
                                ex += eface * cspd[ec];      /* its own speed, on every level */
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
                        ewalk = (ex != exprev) || ecirc;
                        {
                            float enemyStride = fabsf((float)(ex - exprev)) +
                                                (ecirc ? 1.4f : 0.0f);
                            if (eatk == 0 && eblock == 0 && esp == 0) {
                                estep_phase += enemyStride * 0.10f;
                                if (estep_phase >= 6.2831853f)
                                    estep_phase = fmodf(estep_phase, 6.2831853f);
                            }
                            {
                                float gaitTarget = (ewalk && eatk == 0 &&
                                                    eblock == 0 && esp == 0) ? 1.0f : 0.0f;
                                ewalk_blend += (gaitTarget - ewalk_blend) * 0.18f;
                                if (ewalk_blend < 0.005f) ewalk_blend = 0.0f;
                            }
                        }

                        /* enemy attack */
                        if (eatk > 0) {
                            int el = mdur[eatk] - et;
                            /* heavy blows follow you a little while they wind up (a sidewalk won't dodge them) */
                            if ((eatk == 2 || eatk == 4) && el < mstart[eatk]) {
                                float dzh = ang - eaim;
                                if (dzh > 0.012f) eaim += 0.012f; else if (dzh < -0.012f) eaim -= 0.012f; else eaim = ang;
                            }
                            if (!ehit && el >= mstart[eatk] && el < mstart[eatk] + mact[eatk]) {
                                int bx2, by2, bw2, bh2;
                                atkbox(eatk, ex, GROUND - 80, eface, &bx2, &by2, &bw2, &bh2);
                                if (pinv > 0 || latoff(px, ex, ang - eaim) > ztol(eatk)) {
                                    ehit = 1;                    /* dodged: the blow whiffs */
                                } else if (overlap(bx2, by2, bw2, bh2, px, pcrouch ? py + 36 : py, 40, pcrouch ? 44 : 80)) {
                                    ehit = 1;
                                    int d = movedmg(eatk, ec) * edmg[lvl] / 100;
                                    if (d < 1) d = 1;
                                    if (pblock && pface == -eface) {
                                        int c = d / 5; if (c < 1) c = 1;
                                        php -= c; pmeter += 3; emeter += 2;
                                        hitfx((pface == 1) ? px + 44 : px - 4, by2 + bh2 / 2,
                                              eface, 0, 1, 2, mhs[eatk] / 2);
                                        shake = 2;
                                    } else {
                                        php -= d; pflash = 6; pmeter += 8; emeter += mmtr[eatk];
                                        px = clampx(px + mkb[eatk] * eface);
                                        hitfx(px + 20, by2 + bh2 / 2, eface,
                                              (eatk == 2 || eatk == 4) ? 3 : 1, 0, 2, mhs[eatk]);
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
                            if (esp % 6 == 0 && overlap(ax, GROUND - 80, 80, 80, px, py, 40, 80) && latoff(px, ex, ang - eaim) <= 1.6f && pinv == 0) {
                                if (pblock && pface == -eface) {
                                    php -= 1;
                                    hitfx((pface == 1) ? px + 44 : px - 4, py + 20 + rnd() % 40,
                                          eface, 0, 1, 2, 0);
                                } else {
                                    php -= (4 * edmg[lvl] + 50) / 100;
                                    pflash = 4;
                                    px = clampx(px + 4 * eface);
                                    hitfx(px + 20, py + 15 + rnd() % 50, eface, 0, 0, 2, 2);
                                    shake = 4;
                                    ecombo = (ecc > 0) ? ecombo + 1 : 1;
                                    ecc = 70; pcombo = 0; pcc = 0;
                                }
                            }
                            esp--;
                        }
                    }

                    /* Solid bodies: grounded fighters push each other apart.
                       Airborne collision is handled above so jumping cannot cross over. */
                    if (py >= GROUND - 80) {
                        int dd = ex - px;
                        int sg = (dd > 0) ? 1 : (dd < 0) ? -1 : (pface >= 0 ? 1 : -1);
                        int sepn = dd * sg;
                        if (sepn < BODY_GAP) {
                            int o = BODY_GAP - sepn;
                            int po = (o + 1) / 2, eo = o - po;
                            if (po > 8) po = 8;
                            if (eo > 8) eo = 8;
                            px -= sg * po;
                            ex += sg * eo;
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
                        pdash = 0; pcrouch = 0; pdrift = 0; pinv = 0; ukt = -1; dkt = -1; dhold = 0; pswhold = 0; pside = 0; pswdir = 0;
                        py = GROUND - 80; vy = 0;
                        pcombo = 0; ecombo = 0; pcc = 0; ecc = 0;
                        if (!timeup) {
                            shake = 14;
                            /* big K.O. freeze: loser flashes white, then falls */
                            if (pwon) {
                                hitfx(ex + 20, GROUND - 40, pface, 4, 0, 1, 26);
                                eflash = 10;
                            } else {
                                hitfx(px + 20, GROUND - 40, eface, 4, 0, 2, 26);
                                pflash = 10;
                            }
                            flashscr = 10;
                        }
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
                g_ox = ox; g_oz = oz; g_la = ang;
                /* keep both fighters inside the ring: the one at the wall stops, the enemy is never dragged */
                ring_fix(&px, &ex, &ox, &oz, &ang, pdprev);
                pdprev = (px > ex) ? (px - ex) : (ex - px);
                g_ox = ox; g_oz = oz; g_la = ang;
                V3 mid = wp((float)(px + ex + 40) * 0.5f, (float)GROUND, 0.0f);
                float sep = (float)((px > ex) ? (px - ex) : (ex - px)) / 20.0f;
                float dist = CAM_BASE + sep * 0.5f;      /* close up, pulls back when far apart */
                float jmp = (float)(GROUND - 80 - py) / 20.0f;
                if (jmp < 0.0f) jmp = 0.0f;
                float camy = 2.45f + jmp * 0.30f;
                float shx = 0.0f, shy = 0.0f;
                /* camera punches in during a hit-stop freeze */
                if (hitstop > 0 && hitmax > 0)
                    dist -= ((hitmax > 6) ? 1.0f : 0.5f) * (float)hitstop / (float)hitmax;
                if (shake > 0) {
                    shx = ((frame & 1) ? 1.0f : -1.0f) * shake * 0.04f;
                    shy = ((frame & 2) ? 1.0f : -1.0f) * shake * 0.03f;
                    if (!paused && !frozen) shake--;   /* shake holds still while frozen */
                }
                /* the camera stands on the side of the line and turns with it (like Tekken) */
                float ca = cosf(ang), sa = sinf(ang);
                set_camera(v3(mid.x - sa * dist + ca * shx, camy + 0.15f + shy, mid.z + ca * dist + sa * shx),
                           v3(mid.x, camy - 0.15f, mid.z));
            }
            draw_stage(frame);

            /* the victim vibrates during the freeze */
            int vibx = (hitstop > 0) ? ((frame & 1) ? hitamp : -hitamp) : 0;
            int dpx = px + ((hitvic == 2) ? vibx : 0);
            int dex = ex + ((hitvic == 1) ? vibx : 0);

            V3 pwp = wp((float)px + 20.0f, (float)GROUND, 0.0f);
            V3 ewp = wp((float)ex + 20.0f, (float)GROUND, 0.0f);
            float pwx = pwp.x;
            float ewx = ewp.x;

            /* player side */
            g_shz = pwp.z;
            if (ko > 0 && hitstop == 0 && !pwon && !timeup) {
                draw_fallen(px, pc);
            } else if (ko > 0 && !pwon && timeup) {
                shadow(pwx, 0.0f);
                draw_fighter(px, py, pface, 0, 0, 0, 0, frame, pc, 0.0f);
            } else {
                shadow(pwx, (GROUND - 80 - py) / 20.0f);
                if (psp > 0) {
                    g_stand = 0xFFE030A0;
                    g_yawadd = -paim;
                    draw_fighter(px + pface * 35, py, pface, 0, ((psp / 3) % 2) ? 1 : 0,
                                 0, 0, frame, 3, -0.9f);
                    g_yawadd = 0.0f;
                }
                g_prog = (patk > 0 && mdur[patk] > 0) ? 1.0f - (float)pt / (float)mdur[patk] : 0.0f;
                g_crouch = pcrouch;
                g_dash = (pdash > 0) ? ((pdashdir == pface) ? 1 : 2) : 0;
                g_yawadd = -paim;
                g_walk_phase = pstep_phase;
                g_walk_blend = pwalk_blend;
                g_walk_mode = (fabsf(porbitvel) > 0.002f || pswdir != 0) ? 1 : 0;
                draw_fighter(dpx, py, pface, pflash > 0, patk, pblock, pwalk, frame, pc, 0.0f);
                g_crouch = 0; g_dash = 0; g_yawadd = 0.0f; g_walk_mode = 0;
                if (pblock) draw_shield((pface == 1) ? dpx + 44.0f : dpx - 4.0f, py + 40.0f);
            }

            /* enemy side */
            g_shz = ewp.z;
            if (ko > 0 && hitstop == 0 && pwon && !timeup) {
                draw_fallen(ex, ec);
            } else if (ko > 0 && pwon && timeup) {
                shadow(ewx, 0.0f);
                draw_fighter(ex, GROUND - 80, eface, 0, 0, 0, 0, frame, ec, 0.0f);
            } else {
                shadow(ewx, 0.0f);
                if (esp > 0) {
                    g_stand = 0xFF40E060;
                    g_yawadd = -eaim;
                    draw_fighter(ex + eface * 35, GROUND - 80, eface, 0, ((esp / 3) % 2) ? 1 : 0,
                                 0, 0, frame, 3, -0.9f);
                    g_yawadd = 0.0f;
                }
                g_prog = (eatk > 0 && mdur[eatk] > 0) ? 1.0f - (float)et / (float)mdur[eatk] : 0.0f;
                g_yawadd = -eaim;
                g_walk_phase = estep_phase;
                g_walk_blend = ewalk_blend;
                draw_fighter(dex, GROUND - 80, eface, eflash > 0, eatk, eblock > 0, ewalk, frame, ec, 0.0f);
                g_yawadd = 0.0f;
                if (eblock > 0) draw_shield((eface == 1) ? dex + 44.0f : dex - 4.0f, GROUND - 40.0f);
            }

            draw_sparks();
            draw_fx();
            g_shz = 0.0f;
            g_ox = 0.0f; g_oz = 0.0f; g_la = 0.0f;

            /* ---- UI (2D) ---- */
            sceGuDisable(GU_DEPTH_TEST);

            /* white screen flash on heavy hits and K.O. */
            if (flashscr > 0) {
                rect(0, 0, W, H, ((uint32_t)(flashscr * 14) << 24) | 0x00FFFFFF);
                if (!paused) flashscr--;
            }

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
                    project(pwx, (GROUND - py) / 20.0f + 1.2f, pwp.z, &sx2, &sy2);
                    textx(sx2, sy2 / 8, 0xFFE030A0, "ORA ORA ORA!");
                }
                if (esp > 0 && (frame / 4) % 2 == 0) {
                    project(ewx, 5.2f, ewp.z, &sx2, &sy2);
                    textx(sx2, sy2 / 8, 0xFF40E060, "MUDA MUDA!");
                }
            }

            /* K.O. / TIME UP banner (shows after the K.O. freeze ends) */
            if (ko > 0 && hitstop == 0) {
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
                rect(110, 30, 260, 214, 0xFF000000);
                rect(110, 30, 260, 2, 0xFF00E0FF);
                rect(110, 242, 260, 2, 0xFF00E0FF);
                rect(110, 30, 2, 214, 0xFF00E0FF);
                rect(368, 30, 2, 214, 0xFF00E0FF);
                textx(240, 4, 0xFF00E0FF, "MOVE LIST");
                textl(16, 6,  0xFFFFFFFF, "SQ/TRI    PUNCH (LIGHT/HEAVY)");
                textl(16, 8,  0xFFFFFFFF, "X/O       KICK (LIGHT/HEAVY)");
                textl(16, 10, 0xFFFFFFFF, "UP HOLD   JUMP + HIT IN AIR");
                textl(16, 12, 0xFFFFFFFF, "DOWN HOLD DUCK UNDER PUNCHES");
                textl(16, 14, 0xFFFFFFFF, "FWD,FWD   DASH FORWARD");
                textl(16, 16, 0xFFFFFFFF, "BACK,BACK HOP BACK (DODGES)");
                textl(16, 18, 0xFFFFFFFF, "UP TAP    CIRCLE (TAP,TAP+HOLD)");
                textl(16, 20, 0xFFFFFFFF, "DOWN TAP  CIRCLE OTHER WAY");
                textl(16, 22, 0xFFFFFFFF, "HIT,HIT   CHAIN AFTER A HIT");
                textl(16, 24, 0xFFFFFFFF, "L (HOLD)  BLOCK");
                textl(16, 26, 0xFFFFFFFF, "R         STAND RUSH (FULL BAR)");
                textx(240, 28, 0xFF30FF30, "PRESS X TO GO BACK");
            }
        }

        /* ================= MATCH OVER ================= */
        else {
            mt++;
            if (mt > 40 && (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START))) {
                state = 0;
            }

            set_camera(v3(sinf(frame * 0.01f) * 2.0f, 3.0f, 9.0f), v3(0.0f, 2.3f, 0.0f));
            draw_stage(frame);
            if (matchwon) {
                shadow(0.0f, 0.0f);
                draw_fighter(220, GROUND - 80, 1, 0, 0, 0, 0, frame, pc, 0.0f);
            } else {
                draw_fallen(210, pc);
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
