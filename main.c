#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <stdint.h>
#include <stdio.h>

PSP_MODULE_INFO("JojoFighter", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define W 480
#define H 272
#define STRIDE 512
#define GROUND 220
#define MAXHP 125
#define ROUNDTIME 3600   /* 60 seconds at 60 fps */

static uint32_t *vram = (uint32_t *)0x44000000;
static int ox = 0, oy = 0;   /* screen shake offset */

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
    x += ox; y += oy;
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= H) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= W) continue;
            vram[j * STRIDE + i] = color;
        }
    }
}

static void disc(int cx, int cy, int r, uint32_t color) {
    cx += ox; cy += oy;
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

static int barw(int hp) {
    if (hp < 0) hp = 0;
    return hp * 200 / MAXHP;
}

/* ---------- text helpers (debug font: 7 px wide, 8 px tall cells) ---------- */
static void textx(int xc, int row, uint32_t color, const char *s) {
    int n = 0;
    while (s[n]) n++;
    int col = (xc - n * 7 / 2) / 7;
    if (col < 0) col = 0;
    if (col > 67) col = 67;
    pspDebugScreenSetTextColor(color);
    pspDebugScreenSetXY(col, row);
    pspDebugScreenPrintf("%s", s);
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

/* ---------- hit sparks ---------- */
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
    for (int i = 0; i < 8; i++) {
        if (sparks[i].t <= 0) continue;
        int k = sparks[i].kind;
        int L = (8 - sparks[i].t) * 2 + 6 + (k == 1 ? 6 : 0);
        uint32_t c = (k == 2) ? 0xFFFFE0A0 : 0xFF30D0FF;
        int x = sparks[i].x, y = sparks[i].y;
        line(x - L, y, x + L, y, 3, c);
        line(x, y - L, x, y + L, 3, c);
        line(x - L * 2 / 3, y - L * 2 / 3, x + L * 2 / 3, y + L * 2 / 3, 3, c);
        line(x - L * 2 / 3, y + L * 2 / 3, x + L * 2 / 3, y - L * 2 / 3, 3, c);
        disc(x, y, 4, 0xFFFFFFFF);
    }
}

/* ---------- stage ---------- */
static const int bh[12] = {60, 90, 50, 110, 70, 80, 55, 100, 65, 85, 45, 95};

static void draw_stage(int frame) {
    rect(0, 0, W, 90, 0xFF2A0C06);
    rect(0, 90, W, 70, 0xFF381410);
    rect(0, 160, W, GROUND - 160, 0xFF481C14);

    /* stars */
    for (int i = 0; i < 30; i++) {
        int x = (i * 97 + 13) % W;
        int y = (i * 41 + 7) % 110 + 8;
        if ((frame / 8 + i) % 5 != 0) rect(x, y, 2, 2, 0xFFFFFFFF);
    }

    /* moon (crescent) */
    disc(400, 55, 20, 0xFFC0F8FF);
    disc(409, 50, 18, 0xFF2A0C06);

    /* skyline */
    for (int b = 0; b < 12; b++) {
        int x = b * 40;
        int h = bh[b];
        rect(x + 2, GROUND - h, 36, h, 0xFF140804);
        for (int wy = GROUND - h + 8; wy < GROUND - 10; wy += 14) {
            for (int wx = x + 8; wx < x + 32; wx += 12) {
                if (((b * 7 + wx / 4 + wy / 3) % 5) < 2)
                    rect(wx, wy, 5, 6, 0xFF40C8F0);
            }
        }
    }

    /* ground */
    rect(0, GROUND, W, H - GROUND, 0xFF204020);
    rect(0, GROUND, W, 3, 0xFF307030);
}

/* atk: 0 none, 1 punch, 2 kick. style: 0 brawler, 1 speedster, 2 heavy, 3 stand */
static void draw_fighter(int x, int y, int face, uint32_t col,
                         int atk, int block, int walk, int frame, int style) {
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

    /* ---- details ---- */
    if (style == 3) {
        rect(cx - 8, y - 6, 4, 8, 0xFFFFFF80);
        rect(cx + 4, y - 6, 4, 8, 0xFFFFFF80);
        disc(cx, y + 32, 3, 0xFF00FFFF);
        return;
    }

    /* hands */
    disc(fx, fy, 3, 0xFFA0D0FF);
    disc(bx, by, 3, 0xFFA0D0FF);

    if (style == 0) {
        /* cap + brim */
        rect(cx - 10, y - 3, 20, 7, 0xFFF0F0F0);
        rect((face == 1) ? cx + 4 : cx - 14, y + 2, 10, 3, 0xFFF0F0F0);
        /* scarf + fluttering tail */
        rect(cx - 6, y + 19, 12, 4, 0xFF2020E0);
        rect(cx - face * 9 - 2, y + 22, 5, 12 + ((frame / 5) % 2) * 3, 0xFF2020E0);
        /* belt */
        rect(cx - 5, y + 47, 10, 3, 0xFF00D0FF);
        /* boots */
        rect(l1x - 3, l1y - 2, 7, 4, 0xFFF0F0F0);
        rect(l2x - 3, l2y - 2, 7, 4, 0xFFF0F0F0);
    } else if (style == 1) {
        /* spiky hair */
        rect(cx - 9, y - 5, 4, 8, 0xFF30D0FF);
        rect(cx - 3, y - 9, 5, 12, 0xFF30D0FF);
        rect(cx + 4, y - 5, 4, 8, 0xFF30D0FF);
        /* headband */
        rect(cx - 10, y + 3, 20, 3, 0xFFFFFFFF);
        /* belt */
        rect(cx - 5, y + 47, 10, 3, 0xFFFFFFFF);
        /* boots */
        rect(l1x - 3, l1y - 2, 7, 4, 0xFF202020);
        rect(l2x - 3, l2y - 2, 7, 4, 0xFF202020);
    } else {
        /* heavy: thick torso, shoulder pads, mohawk, big belt */
        line(cx, y + 22, cx, y + 48, 9, col);
        rect(cx - 15, y + 22, 30, 6, 0xFF909090);
        rect(cx - 2, y - 9, 5, 11, 0xFF2080FF);
        rect(cx - 7, y + 46, 14, 4, 0xFF30D0FF);
        rect(l1x - 4, l1y - 2, 9, 5, 0xFF303030);
        rect(l2x - 4, l2y - 2, 9, 5, 0xFF303030);
    }
}

static void draw_fallen(int x, uint32_t col) {
    disc(x + 8, GROUND - 10, 10, col);
    line(x + 18, GROUND - 8, x + 55, GROUND - 8, 4, col);
    line(x + 55, GROUND - 8, x + 75, GROUND - 14, 4, col);
    line(x + 55, GROUND - 8, x + 75, GROUND - 2, 4, col);
    line(x + 25, GROUND - 8, x + 35, GROUND - 22, 4, col);
}

int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    pspDebugScreenEnableBackColor(0);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    int state = 0;        /* 0 title, 1 fight, 2 match over */
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
    int epunch = 0, ecd = 60, ehit = 0;
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
                state = 1;
            }

            ox = 0; oy = 0;
            draw_stage(frame);
            rect(0, 14, W, 76, 0xFF000000);
            textx(240, 3, 0xFF00E0FF, "JOJO FIGHTER");
            textx(240, 5, 0xFFFFFFFF, "CHOOSE YOUR FIGHTER");

            sprintf(buf, "MODE: %s", training ? "TRAINING" : "VS CPU");
            textx(240, 7, training ? 0xFF30FF30 : 0xFFFFFFFF, buf);
            sprintf(buf, "LEVEL: %s", lvname[lvl]);
            textx(240, 8, lvcol[lvl], buf);
            if ((frame / 30) % 2 == 0)
                textx(240, 9, 0xFF30FF30, "PRESS X TO START");

            for (int i = 0; i < 3; i++) {
                draw_fighter(cfx[i], GROUND - 80, 1, ccol[i], 0, 0, pc == i, frame, i);
                textx(cfx[i] + 20, 29, 0xFFFFFFFF, cname[i]);
                textx(cfx[i] + 20, 30, 0xFFB0B0B0, cdesc[i]);
            }

            /* selection box */
            {
                int sx = cfx[pc] - 15;
                uint32_t sc = 0xFF00E0FF;
                rect(sx, GROUND - 105, 70, 3, sc);
                rect(sx, GROUND + 4, 70, 3, sc);
                rect(sx, GROUND - 105, 3, 112, sc);
                rect(sx + 67, GROUND - 105, 3, 112, sc);
            }

            textx(240, 32, 0xFFD0D0D0, "UP/DOWN: LEVEL   SQUARE: TRAINING");
            textx(240, 33, 0xFFD0D0D0, "SQ PUNCH  TRI KICK  O BLOCK  R STAND");
        }

        /* ================= FIGHT ================= */
        else if (state == 1) {

            if (training && (pressed & PSP_CTRL_START)) state = 0;

            if (newround) {
                newround = 0;
                px = 100; py = GROUND - 80; vy = 0; pface = 1;
                php = MAXHP; pflash = 0; pmeter = training ? 100 : 0;
                patk = 0; pt = 0; phit = 0; psp = 0; pblock = 0; pwalk = 0;
                ex = 340; eface = -1; ehp = MAXHP; eflash = 0; emeter = 0;
                epunch = 0; ecd = 60; ehit = 0; eblock = 0; ebcd = 0; ewalk = 0; esp = 0;
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

                /* block */
                pblock = 0;
                if (!busy && onground && (pad.Buttons & PSP_CTRL_CIRCLE)) pblock = 1;

                /* movement */
                pwalk = 0;
                if (!busy && !pblock) {
                    if (pad.Buttons & PSP_CTRL_LEFT)  { px -= cspd[pc]; pface = -1; pwalk = 1; }
                    if (pad.Buttons & PSP_CTRL_RIGHT) { px += cspd[pc]; pface = 1;  pwalk = 1; }
                    px = clampx(px);
                    if ((pressed & PSP_CTRL_CROSS) && onground) vy = -cjmp[pc];
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
                    if (patk == 1) { aw = 30; ay = py + 20; dmg = cpun[pc];  kb = 15; }
                    else           { aw = 45; ay = py + 50; dmg = ckick[pc]; kb = 25; }
                    int ax = (pface == 1) ? px + 40 : px - aw;
                    if (!phit && overlap(ax, ay, aw, ah, ex, GROUND - 80, 40, 80)) {
                        phit = 1;
                        if (eblock > 0) {
                            ehp -= dmg / 5; emeter += 2;
                            add_spark((eface == 1) ? ex + 44 : ex - 4, ay + 7, 2);
                            shake = 2;
                        } else {
                            ehp -= dmg; eflash = 6; pmeter += 10; emeter += 8;
                            ex = clampx(ex + kb * pface);
                            add_spark(ex + 20, ay + 7, patk == 2 ? 1 : 0);
                            shake = (patk == 2) ? 10 : 6;
                            pcombo = (pcc > 0) ? pcombo + 1 : 1;
                            pcc = 70; ecombo = 0; ecc = 0;
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
                    /* passive dummy */
                    eface = (px >= ex) ? 1 : -1;
                    ewalk = 0; epunch = 0; esp = 0; eblock = 0;
                } else {
                    int exprev = ex;
                    int dx = px - ex;
                    int dist = dx < 0 ? -dx : dx;
                    eface = (dx >= 0) ? 1 : -1;
                    if (ecd > 0) ecd--;
                    if (ebcd > 0) ebcd--;
                    if (eblock > 0) eblock--;

                    if ((patk > 0 || psp > 0) && dist < 110 && eblock == 0 && ebcd == 0 &&
                        epunch == 0 && esp == 0) {
                        if (rnd() % eblk[lvl] == 0) { eblock = 25; ebcd = 90; }
                    }

                    if (esp == 0 && epunch == 0 && eblock == 0 && emeter >= 100 && dist < 100) {
                        esp = 45; emeter = 0;
                    }

                    if (epunch == 0 && eblock == 0 && esp == 0) {
                        if (dist > 60) {
                            ex += eface * espdv[lvl];
                        } else if (ecd > 0 && dist < 45) {
                            ex -= eface;
                        } else if (ecd == 0) {
                            epunch = 14; ehit = 0;
                            ecd = ecdb[lvl] + (rnd() % ecdr[lvl]);
                        }
                    }
                    ex = clampx(ex);
                    ewalk = (ex != exprev);

                    /* enemy punch */
                    if (epunch > 0) {
                        int fx = (eface == 1) ? ex + 40 : ex - 30;
                        int fy = GROUND - 60;
                        if (!ehit && overlap(fx, fy, 30, 15, px, py, 40, 80)) {
                            ehit = 1;
                            if (pblock && pface == -eface) {
                                php -= 2; pmeter += 3; emeter += 2;
                                add_spark((pface == 1) ? px + 44 : px - 4, fy + 7, 2);
                                shake = 2;
                            } else {
                                php -= cpun[ec] * edmg[lvl] / 100;
                                pflash = 6; pmeter += 8; emeter += 10;
                                px = clampx(px + 15 * eface);
                                add_spark(px + 20, fy + 7, 0);
                                shake = 6;
                                ecombo = (ecc > 0) ? ecombo + 1 : 1;
                                ecc = 70; pcombo = 0; pcc = 0;
                            }
                        }
                        epunch--;
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
                    patk = 0; psp = 0; epunch = 0; eblock = 0; esp = 0;
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

            /* ---------------- draw fight ---------------- */
            ox = 0; oy = 0;
            draw_stage(frame);

            if (shake > 0) {
                ox = (frame & 1) ? shake / 2 : -(shake / 2);
                oy = (frame & 2) ? shake / 3 : -(shake / 3);
                shake--;
            }

            /* player side */
            if (ko > 0 && !pwon && !timeup) {
                draw_fallen(px, ccol[pc]);
            } else if (ko > 0 && !pwon && timeup) {
                draw_fighter(px, py, pface, ccol[pc], 0, 0, 0, frame, pc);
            } else {
                if (psp > 0)
                    draw_fighter(px + pface * 35, py, pface, 0xFFE030A0,
                                 ((psp / 3) % 2) ? 1 : 0, 0, 0, frame, 3);
                draw_fighter(px, py, pface, pflash ? 0xFFFFFFFF : ccol[pc],
                             patk, pblock, pwalk, frame, pc);
                if (pblock) rect((pface == 1) ? px + 40 : px - 8, py + 15, 8, 50, 0xFFFFE000);
            }

            /* enemy side */
            if (ko > 0 && pwon && !timeup) {
                draw_fallen(ex, ccol[ec]);
            } else if (ko > 0 && pwon && timeup) {
                draw_fighter(ex, GROUND - 80, eface, ccol[ec], 0, 0, 0, frame, ec);
            } else {
                if (esp > 0)
                    draw_fighter(ex + eface * 35, GROUND - 80, eface, 0xFF40E060,
                                 ((esp / 3) % 2) ? 1 : 0, 0, 0, frame, 3);
                draw_fighter(ex, GROUND - 80, eface, eflash ? 0xFFFFFFFF : ccol[ec],
                             epunch > 0 ? 1 : 0, eblock > 0, ewalk, frame, ec);
                if (eblock > 0) rect((eface == 1) ? ex + 40 : ex - 8, GROUND - 65, 8, 50, 0xFFFFE000);
            }

            draw_sparks();

            /* ---- UI (no shake) ---- */
            ox = 0; oy = 0;

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
                textx(240, 5, 0xFFB0B0B0, "START: EXIT");
            } else {
                /* round wins */
                for (int i = 0; i < pwins && i < 2; i++) disc(26 + i * 14, 46, 4, 0xFF00E0FF);
                for (int i = 0; i < ewins && i < 2; i++) disc(W - 26 - i * 14, 46, 4, 0xFF00E0FF);

                /* round timer */
                int secs = (rtime + 59) / 60;
                buf[0] = '0' + secs / 10;
                buf[1] = '0' + secs % 10;
                buf[2] = 0;
                textx(240, 4, (secs <= 10) ? 0xFF3030FF : 0xFFFFFFFF, buf);
            }

            /* combo counters */
            if (pcombo >= 2) { combo_str(buf, pcombo); textx(60, 8, 0xFF00E0FF, buf); }
            if (ecombo >= 2) { combo_str(buf, ecombo); textx(420, 8, 0xFF00E0FF, buf); }

            /* Stand shouts */
            if (psp > 0 && (frame / 4) % 2 == 0)
                textx(px + 20, (py - 16) / 8, 0xFFE030A0, "ORA ORA ORA!");
            if (esp > 0 && (frame / 4) % 2 == 0)
                textx(ex + 20, (GROUND - 96) / 8, 0xFF40E060, "MUDA MUDA!");

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
        }

        /* ================= MATCH OVER ================= */
        else {
            mt++;
            if (mt > 40 && (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START))) {
                state = 0;
            }

            ox = 0; oy = 0;
            draw_stage(frame);
            rect(0, 70, W, 70, 0xFF000000);

            if (matchwon) {
                textx(240, 10, 0xFF30FF30, "YOU WIN THE MATCH!");
                draw_fighter(220, GROUND - 80, 1, ccol[pc], 0, 0, 0, frame, pc);
            } else {
                textx(240, 10, 0xFF3030FF, "YOU LOST THE MATCH");
                draw_fallen(210, ccol[pc]);
            }
            buf[0] = '0' + pwins; buf[1] = ' '; buf[2] = '-'; buf[3] = ' ';
            buf[4] = '0' + ewins; buf[5] = 0;
            textx(240, 13, 0xFF00E0FF, buf);
            if (mt > 40 && (frame / 30) % 2 == 0)
                textx(240, 28, 0xFF30FF30, "PRESS X FOR TITLE SCREEN");
        }

        sceDisplayWaitVblankStart();
    }
    return 0;
}
