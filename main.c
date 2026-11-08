#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <stdint.h>

PSP_MODULE_INFO("JojoFighter", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define W 480
#define H 272
#define STRIDE 512
#define GROUND 220

static uint32_t *vram = (uint32_t *)0x44000000;

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

static void rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= H) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= W) continue;
            vram[j * STRIDE + i] = color;
        }
    }
}

static void disc(int cx, int cy, int r, uint32_t color) {
    for (int j = -r; j <= r; j++) {
        for (int i = -r; i <= r; i++) {
            if (i * i + j * j <= r * r) {
                int x = cx + i, y = cy + j;
                if (x >= 0 && x < W && y >= 0 && y < H) vram[y * STRIDE + x] = color;
            }
        }
    }
}

static void line(int x0, int y0, int x1, int y1, int t, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;   /* negative abs */
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        rect(x0 - t / 2, y0 - t / 2, t, t, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int clampx(int x) {
    if (x < 0) return 0;
    if (x > W - 40) return W - 40;
    return x;
}

static int overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

/* atk: 0 none, 1 punch, 2 kick */
static void draw_fighter(int x, int y, int face, uint32_t col,
                         int atk, int block, int walk, int frame) {
    int cx = x + 20;
    int air = (y < GROUND - 80);

    /* head + eye */
    disc(cx, y + 10, 10, col);
    rect(cx + face * 5 - 2, y + 7, 5, 5, 0xFF00FFFF);

    /* body */
    line(cx, y + 20, cx, y + 50, 4, col);

    /* arms */
    int fx, fy, bx, by;
    if (atk == 1)      { fx = cx + face * 50; fy = y + 27; bx = cx - face * 10; by = y + 42; }
    else if (block)    { fx = cx + face * 16; fy = y + 16; bx = cx + face * 8;  by = y + 14; }
    else               { fx = cx + face * 14; fy = y + 42; bx = cx - face * 12; by = y + 42; }
    line(cx, y + 28, fx, fy, 4, col);
    line(cx, y + 28, bx, by, 4, col);

    /* legs */
    int l1x, l1y, l2x, l2y;
    int spread = ((frame / 6) % 2) ? 14 : 4;
    if (atk == 2)  { l1x = cx + face * 62; l1y = y + 57; l2x = cx - face * 6; l2y = y + 80; }
    else if (air)  { l1x = cx + 10; l1y = y + 70; l2x = cx - 10; l2y = y + 72; }
    else if (walk) { l1x = cx + spread; l1y = y + 80; l2x = cx - spread; l2y = y + 80; }
    else           { l1x = cx + 8; l1y = y + 80; l2x = cx - 8; l2y = y + 80; }
    line(cx, y + 50, l1x, l1y, 4, col);
    line(cx, y + 50, l2x, l2y, 4, col);
}

int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    /* player */
    int px = 100, py = GROUND - 80, vy = 0, pface = 1;
    int php = 100, pflash = 0, pmeter = 0;
    int patk = 0, pt = 0, phit = 0;
    int psp = 0, pblock = 0, pwalk = 0;

    /* enemy */
    int ex = 340, eface = -1, ehp = 100, eflash = 0;
    int epunch = 0, ecd = 60, ehit = 0;
    int eblock = 0, ebcd = 0, ewalk = 0;

    int frame = 0;
    SceCtrlData pad;
    unsigned int prev = 0;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;
        frame++;

        int busy = (patk != 0 || psp > 0);
        int onground = (py >= GROUND - 80);

        /* block */
        pblock = 0;
        if (!busy && onground && (pad.Buttons & PSP_CTRL_CIRCLE)) pblock = 1;

        /* movement */
        pwalk = 0;
        if (!busy && !pblock) {
            if (pad.Buttons & PSP_CTRL_LEFT)  { px -= 3; pface = -1; pwalk = 1; }
            if (pad.Buttons & PSP_CTRL_RIGHT) { px += 3; pface = 1;  pwalk = 1; }
            px = clampx(px);
            if ((pressed & PSP_CTRL_CROSS) && onground) vy = -12;
        }
        vy += 1;
        py += vy;
        if (py > GROUND - 80) { py = GROUND - 80; vy = 0; }

        /* start attacks */
        if (!busy && !pblock) {
            if (pressed & PSP_CTRL_SQUARE)        { patk = 1; pt = 12; phit = 0; }
            else if (pressed & PSP_CTRL_TRIANGLE) { patk = 2; pt = 18; phit = 0; }
            else if ((pressed & PSP_CTRL_RTRIGGER) && pmeter >= 100) { psp = 45; pmeter = 0; }
        }

        /* punch / kick */
        if (patk > 0) {
            int aw, ah = 15, ay, dmg, kb;
            if (patk == 1) { aw = 30; ay = py + 20; dmg = 10; kb = 15; }
            else           { aw = 45; ay = py + 50; dmg = 15; kb = 25; }
            int ax = (pface == 1) ? px + 40 : px - aw;
            if (!phit && overlap(ax, ay, aw, ah, ex, GROUND - 80, 40, 80)) {
                phit = 1;
                if (eblock > 0) {
                    ehp -= dmg / 5;
                } else {
                    ehp -= dmg; eflash = 6; pmeter += 10;
                    ex = clampx(ex + kb * pface);
                }
            }
            if (--pt <= 0) patk = 0;
        }

        /* Stand rush */
        if (psp > 0) {
            int ax = (pface == 1) ? px + 40 : px - 80;
            if (psp % 6 == 0 && overlap(ax, py, 80, 80, ex, GROUND - 80, 40, 80)) {
                if (eblock > 0) {
                    ehp -= 1;
                } else {
                    ehp -= 4; eflash = 4;
                    ex = clampx(ex + 4 * pface);
                }
            }
            psp--;
        }

        /* enemy AI */
        int exprev = ex;
        int dx = px - ex;
        int dist = dx < 0 ? -dx : dx;
        eface = (dx >= 0) ? 1 : -1;
        if (ecd > 0) ecd--;
        if (ebcd > 0) ebcd--;
        if (eblock > 0) eblock--;

        if ((patk > 0 || psp > 0) && dist < 110 && eblock == 0 && ebcd == 0 && epunch == 0) {
            if (rnd() % 20 == 0) { eblock = 25; ebcd = 90; }
        }

        if (epunch == 0 && eblock == 0) {
            if (dist > 60) {
                ex += eface * 2;
            } else if (ecd > 0 && dist < 45) {
                ex -= eface;
            } else if (ecd == 0) {
                epunch = 14; ehit = 0;
                ecd = 40 + (rnd() % 40);
            }
        }
        ex = clampx(ex);
        ewalk = (ex != exprev);

        if (epunch > 0) {
            int fx = (eface == 1) ? ex + 40 : ex - 30;
            int fy = GROUND - 60;
            if (!ehit && overlap(fx, fy, 30, 15, px, py, 40, 80)) {
                ehit = 1;
                if (pblock && pface == -eface) {
                    php -= 2; pmeter += 3;
                } else {
                    php -= 10; pflash = 6; pmeter += 8;
                    px = clampx(px + 15 * eface);
                }
            }
            epunch--;
        }

        if (pmeter > 100) pmeter = 100;
        if (eflash > 0) eflash--;
        if (pflash > 0) pflash--;

        /* round reset */
        if (php <= 0 || ehp <= 0) {
            php = 100; ehp = 100; pmeter = 0;
            px = 100; ex = 340;
            patk = 0; psp = 0; epunch = 0; eblock = 0; ecd = 60;
        }

        /* draw */
        rect(0, 0, W, H, 0xFF402010);
        rect(0, GROUND, W, H - GROUND, 0xFF204020);

        /* Stand behind the player */
        if (psp > 0) {
            int sx = px + pface * 35;
            draw_fighter(sx, py, pface, 0xFFE030A0, ((psp / 3) % 2) ? 1 : 0, 0, 0, frame);
        }

        /* player */
        draw_fighter(px, py, pface, pflash ? 0xFFFFFFFF : 0xFF30A0E0,
                     patk, pblock, pwalk, frame);
        if (pblock) rect((pface == 1) ? px + 40 : px - 8, py + 15, 8, 50, 0xFFFFE000);

        /* enemy */
        draw_fighter(ex, GROUND - 80, eface, eflash ? 0xFFFFFFFF : 0xFF3030C0,
                     epunch > 0 ? 1 : 0, eblock > 0, ewalk, frame);
        if (eblock > 0) rect((eface == 1) ? ex + 40 : ex - 8, GROUND - 65, 8, 50, 0xFFFFE000);

        /* health bars */
        rect(20, 10, 200, 14, 0xFF000000);
        rect(20, 10, php * 2, 14, 0xFF30C030);
        rect(W - 220, 10, 200, 14, 0xFF000000);
        rect(W - 220, 10, ehp * 2, 14, 0xFF30C030);

        /* special meter */
        rect(20, 28, 200, 8, 0xFF000000);
        rect(20, 28, pmeter * 2, 8, (pmeter >= 100) ? 0xFFE030A0 : 0xFF00D0FF);

        sceDisplayWaitVblankStart();
    }
    return 0;
}
