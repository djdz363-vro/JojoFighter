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

static void rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= H) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= W) continue;
            vram[j * STRIDE + i] = color;
        }
    }
}

int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    int px = 100, py = GROUND - 80, vy = 0;
    int punch = 0;          /* frames left of punch */
    int hit_done = 0;
    int ex = 340, ehp = 100, flash = 0;
    SceCtrlData pad;
    unsigned int prev = 0;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;

        if (pad.Buttons & PSP_CTRL_LEFT)  px -= 3;
        if (pad.Buttons & PSP_CTRL_RIGHT) px += 3;
        if (px < 0) px = 0;
        if (px > W - 40) px = W - 40;

        if ((pressed & PSP_CTRL_CROSS) && py >= GROUND - 80) vy = -12;
        vy += 1;
        py += vy;
        if (py > GROUND - 80) { py = GROUND - 80; vy = 0; }

        if ((pressed & PSP_CTRL_SQUARE) && punch == 0) { punch = 12; hit_done = 0; }

        if (punch > 0) {
            int fx = px + 40, fy = py + 20;
            if (!hit_done && fx + 30 > ex && fx < ex + 40 && fy + 15 > GROUND - 80 && fy < GROUND) {
                ehp -= 10; flash = 6; hit_done = 1;
                if (ehp <= 0) ehp = 100;
            }
            punch--;
        }
        if (flash > 0) flash--;

        /* draw */
        rect(0, 0, W, H, 0xFF402010);               /* sky */
        rect(0, GROUND, W, H - GROUND, 0xFF204020); /* ground */
        rect(px, py, 40, 80, 0xFFE0A030);           /* player */
        if (punch > 0) rect(px + 40, py + 20, 30, 15, 0xFF30E0FF);
        rect(ex, GROUND - 80, 40, 80, flash ? 0xFFFFFFFF : 0xFF3030C0); /* dummy */
        rect(W - 220, 10, 200, 14, 0xFF000000);     /* enemy health bar */
        rect(W - 220, 10, ehp * 2, 14, 0xFF30C030);

        sceDisplayWaitVblankStart();
    }
    return 0;
}
