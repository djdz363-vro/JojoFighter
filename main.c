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

static int clampx(int x) {
    if (x < 0) return 0;
    if (x > W - 40) return W - 40;
    return x;
}

int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    int px = 100, py = GROUND - 80, vy = 0, pface = 1;
    int php = 100, pflash = 0;
    int punch = 0, hit_done = 0;

    int ex = 340, eface = -1, ehp = 100, eflash = 0;
    int epunch = 0, ecd = 60, ehit_done = 0;

    SceCtrlData pad;
    unsigned int prev = 0;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;

        /* player movement */
        if (pad.Buttons & PSP_CTRL_LEFT)  { px -= 3; pface = -1; }
        if (pad.Buttons & PSP_CTRL_RIGHT) { px += 3; pface = 1; }
        px = clampx(px);

        if ((pressed & PSP_CTRL_CROSS) && py >= GROUND - 80) vy = -12;
        vy += 1;
        py += vy;
        if (py > GROUND - 80) { py = GROUND - 80; vy = 0; }

        /* player punch */
        if ((pressed & PSP_CTRL_SQUARE) && punch == 0) { punch = 12; hit_done = 0; }
        if (punch > 0) {
            int fx = (pface == 1) ? px + 40 : px - 30;
            int fy = py + 20;
            if (!hit_done && fx + 30 > ex && fx < ex + 40 &&
                fy + 15 > GROUND - 80 && fy < GROUND) {
                ehp -= 10; eflash = 6; hit_done = 1;
                ex = clampx(ex + 15 * pface);
            }
            punch--;
        }

        /* enemy AI */
        int dx = px - ex;
        int dist = dx < 0 ? -dx : dx;
        eface = (dx >= 0) ? 1 : -1;
        if (ecd > 0) ecd--;

        if (epunch == 0) {
            if (dist > 70) {
                ex += eface * 2;                 /* walk toward player */
            } else if (ecd > 0 && dist < 55) {
                ex -= eface;                     /* back away while waiting */
            } else if (ecd == 0) {
                epunch = 14; ehit_done = 0;      /* attack */
                ecd = 40 + (rnd() % 40);
            }
        }
        ex = clampx(ex);

        if (epunch > 0) {
            int fx = (eface == 1) ? ex + 40 : ex - 30;
            int fy = GROUND - 60;
            if (!ehit_done && fx + 30 > px && fx < px + 40 &&
                fy + 15 > py && fy < py + 80) {
                php -= 10; pflash = 6; ehit_done = 1;
                px = clampx(px + 15 * eface);
            }
            epunch--;
        }

        if (eflash > 0) eflash--;
        if (pflash > 0) pflash--;

        /* round reset */
        if (php <= 0 || ehp <= 0) {
            php = 100; ehp = 100;
            px = 100; ex = 340;
            epunch = 0; punch = 0; ecd = 60;
        }

        /* draw */
        rect(0, 0, W, H, 0xFF402010);
        rect(0, GROUND, W, H - GROUND, 0xFF204020);
        rect(px, py, 40, 80, pflash ? 0xFFFFFFFF : 0xFF30A0E0);
        if (punch > 0) rect((pface == 1) ? px + 40 : px - 30, py + 20, 30, 15, 0xFF30E0FF);
        rect(ex, GROUND - 80, 40, 80, eflash ? 0xFFFFFFFF : 0xFF3030C0);
        if (epunch > 0) rect((eface == 1) ? ex + 40 : ex - 30, GROUND - 60, 30, 15, 0xFF2020FF);

        /* health bars */
        rect(20, 10, 200, 14, 0xFF000000);
        rect(20, 10, php * 2, 14, 0xFF30C030);
        rect(W - 220, 10, 200, 14, 0xFF000000);
        rect(W - 220, 10, ehp * 2, 14, 0xFF30C030);

        sceDisplayWaitVblankStart();
    }
    return 0;
}
