#include <SDL.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "fbx_loader.h"
#include "jfa_skin.h"
#include "jfa_gpu.h"

/* ------------------------------------------------------------------ */
/* JFA character state (Stage B). Loaded once; all per-frame work      */
/* reuses these buffers - no allocation in the render loop.            */
/* ------------------------------------------------------------------ */
static JFACharacter jfa;
static JFASkinned jfaSkin;
static int jfaOK = 0;
static int jfaClip = JFA_IDLE;
static double jfaClipStart = 0.0;   /* animationTime when clip began */
static double jfaClipDur = 0.0;     /* seconds of the current clip */
static float lastMoveDx = 0.0f, lastMoveDz = 0.0f; /* locomotion clip selection */
static float jfaScale = 1.0f;
static float jfaCenter[3] = { 0, 0, 0 };
/* PERF accumulators (like fbx_loader.c): averaged over 60 frames. */
static double jfaAccSkin = 0.0, jfaAccUpload = 0.0, jfaAccDraw = 0.0;
static int jfaAccN = 0;

typedef struct {
    float position[3];
    float uv[2];
    float normal[3];
    float color[3];
} FloorVertex;

typedef struct { float p[3], uv[2], n[3]; } Vertex;
typedef struct { Vertex *v; size_t count, cap; } Mesh;

static const char *vs =
"attribute vec3 aPosition; attribute vec2 aUV; attribute vec3 aNormal; attribute vec3 aColor;"
"uniform mat4 uMVP; uniform mat4 uModel; varying vec2 vUV; varying float vLight; varying vec3 vColor;"
"void main(){ vec3 n=normalize(mat3(uModel)*aNormal);"
"vLight=0.35+0.65*max(dot(n,normalize(vec3(-0.4,0.8,0.6))),0.0);"
"vUV=aUV; vColor=aColor; gl_Position=uMVP*vec4(aPosition,1.0); gl_PointSize=3.0; }";
static const char *fs =
"precision mediump float; uniform sampler2D uTexture; uniform float uTextMode;"
"varying vec2 vUV; varying float vLight; varying vec3 vColor;"
"void main(){ if(uTextMode>0.5) gl_FragColor=vec4(vColor,1.0);"
"else { vec4 t=texture2D(uTexture,vUV); gl_FragColor=vec4(t.rgb*vLight,t.a); } }";

static const unsigned char font[14][7] = {
{14,16,16,14,16,16,16},{30,17,17,30,16,16,16},{15,16,16,14,1,1,30},{0,4,4,0,4,4,0},
{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}};
static int glyph(char c){ if(c=='F')return 0;if(c=='P')return 1;if(c=='S')return 2;if(c==':')return 3;if(c>='0'&&c<='9')return 4+c-'0';return 3; }

static GLuint shader(GLenum type,const char *src){
 GLuint s=glCreateShader(type);glShaderSource(s,1,&src,NULL);glCompileShader(s);GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
 if(!ok){char log[512];glGetShaderInfoLog(s,sizeof(log),NULL,log);SDL_Log("Shader: %s",log);glDeleteShader(s);return 0;}return s;
}
static GLuint programCreate(void){
 GLuint a=shader(GL_VERTEX_SHADER,vs),b=shader(GL_FRAGMENT_SHADER,fs);if(!a||!b)return 0;
 GLuint p=glCreateProgram();glAttachShader(p,a);glAttachShader(p,b);glLinkProgram(p);glDeleteShader(a);glDeleteShader(b);
 GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char log[512];glGetProgramInfoLog(p,sizeof(log),NULL,log);SDL_Log("Link: %s",log);glDeleteProgram(p);return 0;}return p;
}
static void mm(float *o,const float *a,const float *b){
 float r[16];for(int c=0;c<4;c++)for(int y=0;y<4;y++){r[c*4+y]=0;for(int k=0;k<4;k++)r[c*4+y]+=a[k*4+y]*b[c*4+k];}memcpy(o,r,sizeof(r));
}

/* ---- M3.1-B: PSP-style DIGITAL D-pad. Four independent buttons,
 * each tracked by its own finger id, so diagonal holds and
 * independent releases work with multi-touch. ---- */
enum {
    JFA_DIR_UP    = 1 << 0,
    JFA_DIR_DOWN  = 1 << 1,
    JFA_DIR_LEFT  = 1 << 2,
    JFA_DIR_RIGHT = 1 << 3
};
#define JFA_DPAD_NX 0.18f
#define JFA_DPAD_NY 0.78f
#define JFA_DPAD_MIN_PX 64.0f
#define JFA_DPAD_MAX_PX 120.0f
#define JFA_DPAD_EDGE_PX 8.0f
#define JFA_DPAD_SPACING 1.22f
/* Fraction of the full-scale radius: smallest practical center dead zone. */
#define JFA_ANALOG_DEAD 0.12f

/* One shared geometry, expressed in normalized WINDOW coordinates and
 * used for BOTH touch hit-testing and overlay drawing. Because sizes
 * are converted to normalized halves per axis, buttons stay square in
 * pixels and hitboxes always match what is drawn -- even when the
 * OpenGL drawable size differs from the SDL window size. */
typedef struct {
    float cx, cy;   /* normalized cluster center */
    float hx, hy;   /* normalized button half-size */
    float gx, gy;   /* normalized center-to-center spacing */
} JFADpadGeom;

static void jfa_dpad_geom(int vw, int vh, JFADpadGeom *g)
{
    int minDim;
    float s, gapPx, lo, hi, v, margin;

    if (vw < 1 || vh < 1) { vw = 1080; vh = 1920; }
    minDim = (vw < vh) ? vw : vh;
    s = (float)minDim * 0.17f;
    if (s < JFA_DPAD_MIN_PX) s = JFA_DPAD_MIN_PX;
    if (s > JFA_DPAD_MAX_PX) s = JFA_DPAD_MAX_PX;
    gapPx = s * JFA_DPAD_SPACING;

    g->hx = s * 0.5f / (float)vw;
    g->hy = s * 0.5f / (float)vh;
    g->gx = gapPx / (float)vw;
    g->gy = gapPx / (float)vh;

    margin = JFA_DPAD_EDGE_PX / (float)vw;
    lo = g->gx + g->hx + margin;
    hi = 1.0f - lo;
    v = JFA_DPAD_NX;
    g->cx = (lo > hi) ? 0.5f : (v < lo ? lo : (v > hi ? hi : v));

    margin = JFA_DPAD_EDGE_PX / (float)vh;
    lo = g->gy + g->hy + margin;
    hi = 1.0f - lo;
    v = JFA_DPAD_NY;
    g->cy = (lo > hi) ? 0.5f : (v < lo ? lo : (v > hi ? hi : v));
}

typedef struct {
    SDL_FingerID finger[4]; /* -1 = unused; index 0=UP,1=DOWN,2=LEFT,3=RIGHT */
    SDL_FingerID analogFinger; /* -1 = no finger owns analogue control */
    float analogX, analogY;    /* unit direction from cluster center */
} JFADpad;

static void jfa_dpad_reset(JFADpad *d)
{
    for (int i = 0; i < 4; i++)
        d->finger[i] = -1;
    d->analogFinger = -1;
    d->analogX = d->analogY = 0.0f;
}

/* Returns the direction mask under a normalized touch: one button bit,
 * UP|LEFT / UP|RIGHT / DOWN|LEFT / DOWN|RIGHT in a diagonal corner gap,
 * or 0 (neutral). Buttons are checked first and gaps use strict bounds,
 * so no coordinate belongs to two regions. */
static int jfa_dpad_hit(float nx, float ny, int vw, int vh)
{
    JFADpadGeom g;
    const int bits[4] = { JFA_DIR_UP, JFA_DIR_DOWN,
                          JFA_DIR_LEFT, JFA_DIR_RIGHT };
    const float off[4][2] = {
        { 0.0f, -1.0f }, { 0.0f, 1.0f },
        { -1.0f, 0.0f }, { 1.0f, 0.0f }
    };
    jfa_dpad_geom(vw, vh, &g);
    for (int i = 0; i < 4; i++) {
        float bx = g.cx + off[i][0] * g.gx;
        float by = g.cy + off[i][1] * g.gy;
        if (fabsf(nx - bx) <= g.hx && fabsf(ny - by) <= g.hy)
            return bits[i];
    }
    /* The four diagonal corner gaps are the squares between adjacent
     * button rects, (gx-2*hx) x (gy-2*hy) pixels in size. */
    if (nx > g.cx - g.gx + g.hx && nx < g.cx - g.hx &&
        ny > g.cy - g.gy + g.hy && ny < g.cy - g.hy)
        return JFA_DIR_UP | JFA_DIR_LEFT;
    if (nx > g.cx + g.hx && nx < g.cx + g.gx - g.hx &&
        ny > g.cy - g.gy + g.hy && ny < g.cy - g.hy)
        return JFA_DIR_UP | JFA_DIR_RIGHT;
    if (nx > g.cx - g.gx + g.hx && nx < g.cx - g.hx &&
        ny > g.cy + g.hy && ny < g.cy + g.gy - g.hy)
        return JFA_DIR_DOWN | JFA_DIR_LEFT;
    if (nx > g.cx + g.hx && nx < g.cx + g.gx - g.hx &&
        ny > g.cy + g.hy && ny < g.cy + g.gy - g.hy)
        return JFA_DIR_DOWN | JFA_DIR_RIGHT;
    return 0;
}

/* Apply a direction mask to one finger atomically: every requested slot
 * must be free or already owned by this finger; otherwise nothing changes
 * (never steal, never partially assign). On success this finger's old
 * slots are released before the new mask is assigned. */
static int jfa_dpad_assign(JFADpad *d, SDL_FingerID id, int mask)
{
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1 << i)))
            continue;
        if (d->finger[i] >= 0 && d->finger[i] != id)
            return 0;
    }
    for (int i = 0; i < 4; i++) {
        if (d->finger[i] == id)
            d->finger[i] = -1;
    }
    for (int i = 0; i < 4; i++) {
        if (mask & (1 << i))
            d->finger[i] = id;
    }
    return 1;
}

static void jfa_dpad_down(JFADpad *d, SDL_FingerID id,
                          float nx, float ny, int vw, int vh)
{
    int mask = jfa_dpad_hit(nx, ny, vw, vh);
    if (mask)
        jfa_dpad_assign(d, id, mask);
}

static void jfa_dpad_motion(JFADpad *d, SDL_FingerID id,
                            float nx, float ny, int vw, int vh)
{
    int owned = 0;
    /* A finger that never grabbed a direction must not acquire one by
     * merely sliding over the D-pad. */
    for (int i = 0; i < 4; i++) {
        if (d->finger[i] == id) { owned = 1; break; }
    }
    if (!owned)
        return;
    int dest = jfa_dpad_hit(nx, ny, vw, vh);
    /* Sliding through a NEUTRAL gap must NOT drop finger ownership:
     * keep the current direction until another button/gap is entered. */
    if (dest == 0)
        return;
    /* jfa_dpad_assign() keeps the old direction if any requested slot is
     * held by another finger, and otherwise moves all of this finger's
     * slots to the new mask (single button or diagonal pair). */
    jfa_dpad_assign(d, id, dest);
}

static void jfa_dpad_up(JFADpad *d, SDL_FingerID id)
{
    for (int i = 0; i < 4; i++)
        if (d->finger[i] == id)
            d->finger[i] = -1;
}

static void jfa_dpad_cancel(JFADpad *d, SDL_FingerID id)
{
    if (d->analogFinger == id) {
        d->analogFinger = -1;
        d->analogX = d->analogY = 0.0f;
    }
    for (int i = 0; i < 4; i++)
        if (d->finger[i] == id)
            d->finger[i] = -1;
}

static unsigned jfa_dpad_mask(const JFADpad *d)
{
    unsigned mask = 0;
    if (d->finger[0] >= 0) mask |= JFA_DIR_UP;
    if (d->finger[1] >= 0) mask |= JFA_DIR_DOWN;
    if (d->finger[2] >= 0) mask |= JFA_DIR_LEFT;
    if (d->finger[3] >= 0) mask |= JFA_DIR_RIGHT;
    return mask;
}

/* Continuous 360-degree virtual stick built on the existing visual D-pad.
 * Full scale is the distance from the cluster center to a button center;
 * the activation circle is the MINIMAL one that encloses all four old
 * button rects: hypot(half-button, spacing+half-button) in window pixels.
 * Returns 1 and a unit direction for any touch inside that circle, 0 if
 * the touch is outside. The center dead zone returns a zero vector. */
static int jfa_dpad_analog(float nx, float ny, int vw, int vh,
                           float *vx, float *vy)
{
    JFADpadGeom g;
    float ox, oy, len, full, activate, dead;

    jfa_dpad_geom(vw, vh, &g);
    /* Window-pixel offsets: correct for portrait and landscape aspect
     * ratios and consistent with hit-testing (which uses window coords). */
    ox = (nx - g.cx) * (float)vw;
    oy = (ny - g.cy) * (float)vh;
    full = g.gx * (float)vw;              /* == g.gy * vh == 1.22*s px */
    activate = sqrtf((g.hx * (float)vw) * (g.hx * (float)vw) +
                     ((g.gx + g.hx) * (float)vw) *
                     ((g.gx + g.hx) * (float)vw));
    len = sqrtf(ox * ox + oy * oy);
    if (full <= 0.0f || len > activate)
        return 0;
    dead = JFA_ANALOG_DEAD * full;
    if (len <= dead) {
        *vx = *vy = 0.0f;
        return 1;
    }
    *vx = ox / len;
    *vy = oy / len;
    return 1;
}

/* First eligible finger owns the analogue control. Other fingers cannot
 * steal or modify it. A touch outside the control circle is ignored. */
static int jfa_dpad_analog_apply(JFADpad *d, SDL_FingerID id,
                                 float nx, float ny, int vw, int vh)
{
    float vx = 0.0f, vy = 0.0f;

    if (d->analogFinger >= 0 && d->analogFinger != id)
        return 0;
    if (!jfa_dpad_analog(nx, ny, vw, vh, &vx, &vy))
        return 0;
    d->analogFinger = id;
    d->analogX = vx;
    d->analogY = vy;
    return 1;
}

static void jfa_dpad_analog_release(JFADpad *d, SDL_FingerID id)
{
    if (d->analogFinger == id) {
        d->analogFinger = -1;
        d->analogX = d->analogY = 0.0f;
    }
}

/* Reuse the accepted four-button highlight for analogue feedback: light
 * the buttons whose axes are predominant. Drawing itself is untouched. */
static unsigned jfa_dpad_visual_mask(const JFADpad *d)
{
    unsigned mask = jfa_dpad_mask(d);

    if (d->analogFinger >= 0) {
        if (d->analogX <= -0.5f) mask |= JFA_DIR_LEFT;
        if (d->analogX >=  0.5f) mask |= JFA_DIR_RIGHT;
        if (d->analogY <= -0.5f) mask |= JFA_DIR_UP;
        if (d->analogY >=  0.5f) mask |= JFA_DIR_DOWN;
    }
    return mask;
}

/* Per-arm highlight levels: digital slots give 1/0, analogue gives the
 * signed axis component, clamped to 0..1. This is display-only — the
 * movement vector itself remains continuous and unquantized. */
static void jfa_dpad_visual_levels(const JFADpad *d, float out[4])
{
    unsigned mask = jfa_dpad_mask(d);

    out[0] = (mask & JFA_DIR_UP)    ? 1.0f : 0.0f;
    out[1] = (mask & JFA_DIR_DOWN)  ? 1.0f : 0.0f;
    out[2] = (mask & JFA_DIR_LEFT)  ? 1.0f : 0.0f;
    out[3] = (mask & JFA_DIR_RIGHT) ? 1.0f : 0.0f;
    if (d->analogFinger >= 0) {
        float up = -d->analogY, down = d->analogY;
        float left = -d->analogX, right = d->analogX;
        if (up > out[0]) out[0] = up;
        if (down > out[1]) out[1] = down;
        if (left > out[2]) out[2] = left;
        if (right > out[3]) out[3] = right;
        for (int i = 0; i < 4; i++)
            if (out[i] > 1.0f) out[i] = 1.0f;
    }
}

/* Stage scale: all arena dimensions grow by 1.35 and the walls become
 * proportionately taller for a more cube-like room. Camera lifts slightly
 * and pitches down a few degrees while keeping the character framed. */
#define JFA_ARENA_SCALE 1.35f
#define JFA_ARENA_HALF (4.2f * JFA_ARENA_SCALE)
#define JFA_CAMERA_Y (-1.28f)
#define JFA_CAMERA_Z (-3.85f)
#define JFA_CAMERA_PITCH 0.055f

/* ---- M3.2-B: multiple basic attacks. The selected animation clip lives
 * in the attack state; CPU and GPU share one sampled clip/frame. ---- */

/* Locomotion is explicit: IDLE when stationary, WALK forward/lateral,
 * BACKWALK backward. GUARD and all attack/reaction clips are reserved for
 * explicit game states and are NEVER auto-selected. */
static int jfa_locomotion_clip_for(float dx, float dz)
{
    if (fabsf(dx) < 0.001f && fabsf(dz) < 0.001f)
        return JFA_IDLE;
    if (dz < -0.001f) return JFA_WALK;
    if (dz >  0.001f) return JFA_BACKWALK;
    return JFA_WALK;   /* pure sideways movement keeps the walk cycle */
}

typedef struct {
    int active;              /* 1 while the attack clip is playing */
    int clip;                /* selected attack clip index */
    double start;            /* animationTime when triggered */
    double savedElapsed;     /* locomotion elapsed time to restore */
    SDL_FingerID finger;     /* owner touch, -1 for keyboard */
} JFAAttack;

typedef struct {
    int clip;                /* clip index to sample this frame */
    double time;             /* seconds into that clip */
} JFAAnimSample;

static int jfa_attack_trigger(JFAAttack *a, double now,
                              double locoElapsed, SDL_FingerID finger,
                              int clip)
{
    if (a->active)
        return 0;            /* no interruption and no queue */
    a->active = 1;
    a->clip = clip;
    a->start = now;
    a->savedElapsed = locoElapsed;
    a->finger = finger;
    return 1;
}

/* Releasing the owning finger does NOT cancel the animation: it plays to
 * its full duration. Cancellation/reset is handled separately. */
static void jfa_attack_release(JFAAttack *a, SDL_FingerID finger)
{
    if (a->finger == finger)
        a->finger = -1;
}

static void jfa_attack_reset(JFAAttack *a)
{
    a->active = 0;
    a->clip = JFA_IDLE;
    a->start = 0.0;
    a->savedElapsed = 0.0;
    a->finger = -1;
}

/* Pure clip selector. Mutates only the attack state and the locomotion
 * cursor/start time; all callers (CPU and GPU) use the returned sample. */
static JFAAnimSample jfa_anim_select(JFAAttack *a, double now,
                                     int *locoClip, double *locoStart,
                                     const JFAClip *clips,
                                     float dx, float dz)
{
    JFAAnimSample s;

    if (a->active) {
        const JFAClip *ac = &clips[a->clip];
        double dur = (double)ac->frames / (double)ac->fps;
        double t = now - a->start;
        if (t < dur) {
            s.clip = a->clip;
            s.time = t;
            return s;
        }
        /* Attack finished: restore the locomotion clip and its elapsed time. */
        a->active = 0;
        a->finger = -1;
        *locoStart = now - a->savedElapsed;
    }

    /* Movement selects the locomotion clip; no automatic cycling. */
    {
        int target = jfa_locomotion_clip_for(dx, dz);
        if (target != *locoClip) {
            *locoClip = target;
            *locoStart = now;
        }
    }
    s.clip = *locoClip;
    {
        const JFAClip *lc = &clips[s.clip];
        double dur = (double)lc->frames / (double)lc->fps;
        double elapsed = now - *locoStart;
        if (elapsed >= dur) {
            *locoStart = now;
            elapsed = 0.0;
        }
        s.time = elapsed;
    }
    return s;
}

/* ---- M3.2-A: extra PSP-style controls (visual/input only, no gameplay).
 * Independent finger per button; press/release only changes highlight. ---- */
enum {
    JFA_BTN_MENU = 0, JFA_BTN_FF, JFA_BTN_SELECT, JFA_BTN_START,
    JFA_BTN_L, JFA_BTN_R, JFA_BTN_TRI, JFA_BTN_SQUARE,
    JFA_BTN_CIRCLE, JFA_BTN_X, JFA_BTN_TOPLEFT, JFA_BTN_COUNT
};
typedef struct {
    SDL_FingerID finger[JFA_BTN_COUNT];
} JFAButtons;

static const float jfaButtonX[JFA_BTN_COUNT] = {
    0.50f, 0.65f, 0.73f, 0.81f,
    0.15f, 0.85f, 0.83f, 0.75f,
    0.91f, 0.83f, 0.18f
};
static const float jfaButtonY[JFA_BTN_COUNT] = {
    0.07f, 0.90f, 0.90f, 0.90f,
    0.28f, 0.28f, 0.55f, 0.66f,
    0.66f, 0.77f, 0.07f
};
/* 0 = circle, 1 = rectangle */
static const unsigned char jfaButtonShape[JFA_BTN_COUNT] = {
    0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};

static void jfa_button_extents(int id, int vw, int vh, float *hw, float *hh)
{
    JFADpadGeom g;
    float halfPx;

    jfa_dpad_geom(vw, vh, &g);
    halfPx = g.hx * (float)vw;              /* s/2 in pixels */
    if (id == JFA_BTN_L || id == JFA_BTN_R) {
        *hw = 0.90f * halfPx;               /* wide shoulder trapezoid */
        *hh = 0.40f * halfPx;
    } else {
        /* FF / SELECT / START: compact but comfortably tappable. The same
         * extents are used by the hit test and the outline draw. */
        *hw = 0.40f * halfPx;
        *hh = 0.50f * halfPx;
    }
}

static void jfa_buttons_reset(JFAButtons *b)
{
    for (int i = 0; i < JFA_BTN_COUNT; i++)
        b->finger[i] = -1;
}

static int jfa_buttons_hit(int id, float nx, float ny, int vw, int vh)
{
    float halfPx, dx, dy;

    if (id < 0 || id >= JFA_BTN_COUNT || vw < 1 || vh < 1)
        return -1;
    {
        JFADpadGeom g;
        jfa_dpad_geom(vw, vh, &g);
        halfPx = g.hx * (float)vw;
    }
    dx = (nx - jfaButtonX[id]) * (float)vw;
    dy = (ny - jfaButtonY[id]) * (float)vh;
    if (jfaButtonShape[id] == 0) {
        float r = (id == JFA_BTN_MENU || id == JFA_BTN_TOPLEFT) ?
                  0.50f * halfPx : 0.65f * halfPx;
        return (dx * dx + dy * dy <= r * r) ? id : -1;
    }
    {
        float hw, hh;
        jfa_button_extents(id, vw, vh, &hw, &hh);
        return (fabsf(dx) <= hw && fabsf(dy) <= hh) ? id : -1;
    }
}

static int jfa_buttons_find(float nx, float ny, int vw, int vh)
{
    for (int i = 0; i < JFA_BTN_COUNT; i++)
        if (jfa_buttons_hit(i, nx, ny, vw, vh) == i)
            return i;
    return -1;
}

static int jfa_buttons_down(JFAButtons *b, int id, SDL_FingerID finger)
{
    if (id < 0 || id >= JFA_BTN_COUNT || b->finger[id] >= 0)
        return 0;
    b->finger[id] = finger;
    return 1;
}

static void jfa_buttons_up(JFAButtons *b, SDL_FingerID finger)
{
    for (int i = 0; i < JFA_BTN_COUNT; i++)
        if (b->finger[i] == finger)
            b->finger[i] = -1;
}

/* M3.2-B: face buttons that trigger attacks. CIRCLE stays visual-only. */
static int jfa_attack_clip_for_button(int btn)
{
    if (btn == JFA_BTN_TRI)    return JFA_PUNCH_RIGHT;
    if (btn == JFA_BTN_SQUARE) return JFA_PUNCH_LEFT;
    if (btn == JFA_BTN_X)      return JFA_KICK;
    return -1;
}

static int jfa_attack_clip_for_key(int index)
{
    static const int clips[3] = { JFA_PUNCH_RIGHT, JFA_PUNCH_LEFT, JFA_KICK };
    return (index >= 0 && index < 3) ? clips[index] : -1;
}

/* An attack may only start after this finger successfully claims the
 * button. A failed claim leaves button ownership and attack state alone.
 * Returns 1 only when a new attack was accepted. */
static int jfa_face_button_press(JFAButtons *buttons, JFAAttack *attack,
                                 int btn, SDL_FingerID finger,
                                 double now, double locoElapsed)
{
    int clip;

    if (!jfa_buttons_down(buttons, btn, finger))
        return 0;
    clip = jfa_attack_clip_for_button(btn);
    if (clip < 0)
        return 0;
    return jfa_attack_trigger(attack, now, locoElapsed, finger, clip);
}

/* TEMPORARY device diagnostics: shows raw SDL finger coordinates, the
 * window size used for hit-testing, the GL drawable size, the resolved
 * button and the resulting direction mask. Set JFA_DPAD_DEBUG=0 to
 * silence after the device issue is diagnosed. */
#ifndef JFA_DPAD_DEBUG
#define JFA_DPAD_DEBUG 1
#endif
#if JFA_DPAD_DEBUG
static void jfa_dpad_log(SDL_Window *win, const char *what,
                         SDL_FingerID id, float nx, float ny,
                         int vw, int vh, unsigned mask)
{
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    SDL_Log("JFA DPAD %s finger=%lld xy=(%.3f,%.3f) window=%dx%d "
            "drawable=%dx%d hit=%d mask=%u",
            what, (long long)id, (double)nx, (double)ny, vw, vh,
            dw, dh, jfa_dpad_hit(nx, ny, vw, vh), mask);
}
#define JFA_DPAD_LOG(win, what, id, nx, ny, vw, vh, mask) \
    jfa_dpad_log((win), (what), (id), (nx), (ny), (vw), (vh), (mask))
#else
#define JFA_DPAD_LOG(win, what, id, nx, ny, vw, vh, mask) do {} while (0)
#endif

/* PSP-style D-pad overlay: four outlined directional sections arranged in
 * a cross. Each section is a rectangle with a diagonal-pointed inner edge;
 * their tips form a small diamond gap around the center. Interiors stay
 * transparent and arrows are hollow, matching the PPSSPP reference.
 * Uses only the existing flat-color program path (uTextMode=1) and a
 * dedicated dynamic VBO; no character/GPU skinning state is touched.
 * The shared fragment shader hardcodes alpha=1, so 'translucency' is
 * represented by drawing outlines only. */
static void jfa_dpad_draw(GLuint prog, GLuint vbo, GLint pos, GLint col,
                          GLint mvp, GLint mode, int w, int h,
                          const float levels[4])
{
    float v[1024];
    int n = 0, ntri;
    JFADpadGeom gg;
    const float dirs[4][2] = {
        { 0.0f, -1.0f }, { 0.0f, 1.0f },
        { -1.0f, 0.0f }, { 1.0f, 0.0f }
    };
    /* Geometry comes from WINDOW coordinates (same as hit testing);
     * identity clip space maps directly onto the GL viewport, so
     * drawable-vs-window scaling cannot shift the buttons. */
    jfa_dpad_geom(w, h, &gg);

#define DPV(nx, ny, r, g, b) do { \
    float px_ = (nx), py_ = (ny); \
    /* Defensive clamp: malformed/scaled device coordinates must never
     * produce giant off-screen triangles over the arena. */ \
    if (px_ < 0.0f) px_ = 0.0f; \
    if (px_ > 1.0f) px_ = 1.0f; \
    if (py_ < 0.0f) py_ = 0.0f; \
    if (py_ > 1.0f) py_ = 1.0f; \
    v[n++] = 2.0f * px_ - 1.0f; \
    v[n++] = 1.0f - 2.0f * py_; \
    v[n++] = 0.0f; v[n++] = r; v[n++] = g; v[n++] = b; \
} while (0)

    /* No filled geometry: the section interiors stay transparent. */
    ntri = n / 6;   /* 0 */

    /* Four outlined pentagons: rectangle + diagonal point toward center,
     * using the unchanged button centers and half-sizes. */
    for (int i = 0; i < 4; i++) {
        const float dx = dirs[i][0], dy = dirs[i][1];
        const float ux = -dy, uy = dx;
        const float bx = gg.cx + dx * gg.gx;
        const float by = gg.cy + dy * gg.gy;
        const float hd = fabsf(dx) * gg.hx + fabsf(dy) * gg.hy;
        const float hu = fabsf(ux) * gg.hx + fabsf(uy) * gg.hy;
        const float gd = fabsf(dx) * gg.gx + fabsf(dy) * gg.gy;
        const float lv = fmaxf(0.0f, fminf(1.0f, levels[i]));

        float p[5][2];
        /* outer-left, outer-right, inner-right, center-point, inner-left */
        p[0][0] = bx + dx * hd - ux * hu;  p[0][1] = by + dy * hd - uy * hu;
        p[1][0] = bx + dx * hd + ux * hu;  p[1][1] = by + dy * hd + uy * hu;
        p[2][0] = bx - dx * hd + ux * hu;  p[2][1] = by - dy * hd + uy * hu;
        p[3][0] = bx - dx * 0.84f * gd;    p[3][1] = by - dy * 0.84f * gd;
        p[4][0] = bx - dx * hd - ux * hu;  p[4][1] = by - dy * hd - uy * hu;

        const float r = 0.66f + 0.34f * lv;
        const float g = 0.69f + 0.21f * lv;
        const float b = 0.74f - 0.24f * lv;
        for (int k = 0; k < 5; k++) {
            int t = (k + 1) % 5;
            DPV(p[k][0], p[k][1], r, g, b);
            DPV(p[t][0], p[t][1], r, g, b);
        }

        /* Hollow outward arrow inside the rectangular part. */
        const float tipx = bx + dx * 0.46f * hd;
        const float tipy = by + dy * 0.46f * hd;
        const float bcx = bx - dx * 0.24f * hd;
        const float bcy = by - dy * 0.24f * hd;
        const float b1x = bcx + ux * 0.32f * hu;
        const float b1y = bcy + uy * 0.32f * hu;
        const float b2x = bcx - ux * 0.32f * hu;
        const float b2y = bcy - uy * 0.32f * hu;
        const float ar = 0.74f + 0.26f * lv;
        const float ag = 0.76f + 0.20f * lv;
        const float ab = 0.80f - 0.10f * lv;
        DPV(tipx, tipy, ar, ag, ab);  DPV(b1x, b1y, ar, ag, ab);
        DPV(b1x, b1y, ar, ag, ab);    DPV(b2x, b2y, ar, ag, ab);
        DPV(b2x, b2y, ar, ag, ab);    DPV(tipx, tipy, ar, ag, ab);
    }
#undef DPV

    if (n > (int)(sizeof(v) / sizeof(v[0])))
        return;
    static const float ident[16] = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    glUseProgram(prog);
    glUniformMatrix4fv(mvp, 1, GL_FALSE, ident);
    glUniform1f(mode, 1.0f);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (size_t)n * sizeof(float), v,
                 GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(pos);
    glEnableVertexAttribArray(col);
    glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (void *)0);
    glVertexAttribPointer(col, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLES, 0, ntri);
    glDrawArrays(GL_LINES, ntri, n / 6 - ntri);
    glEnable(GL_DEPTH_TEST);
}


/* M3.2-A: draw the extra PSP-style controls. Separate VBO; game logic is
 * untouched and press state only changes highlight colors. */
static void jfa_buttons_draw(GLuint prog, GLuint vbo, GLint pos, GLint col,
                             GLint mvp, GLint mode, int w, int h,
                             const JFAButtons *buttons)
{
    float v[4096];
    int n = 0, ntri, i, k;

    if (w < 1 || h < 1)
        return;
#define BPV(nx_, ny_, rr_, gg_, bb_) do { \
    float x_ = (nx_), y_ = (ny_); \
    if (x_ < 0.0f) { x_ = 0.0f; } \
    if (x_ > 1.0f) { x_ = 1.0f; } \
    if (y_ < 0.0f) { y_ = 0.0f; } \
    if (y_ > 1.0f) { y_ = 1.0f; } \
    v[n++] = 2.0f * x_ - 1.0f; \
    v[n++] = 1.0f - 2.0f * y_; \
    v[n++] = 0.0f; v[n++] = rr_; v[n++] = gg_; v[n++] = bb_; \
} while (0)

    /* half button size in px, matching jfa_dpad_geom()'s window units */
    {
        JFADpadGeom gg;
        float halfPx, rx, rxSmall, hw, hh;
        jfa_dpad_geom(w, h, &gg);
        halfPx = gg.hx * (float)w;
        rx = 0.65f * halfPx;
        rxSmall = 0.50f * halfPx;
        hw = halfPx;
        hh = 0.50f * halfPx;

        for (i = 0; i < JFA_BTN_COUNT; i++) {
            float cx = jfaButtonX[i], cy = jfaButtonY[i];
            float nxr = (jfaButtonShape[i] == 0 ?
                         (i == JFA_BTN_MENU || i == JFA_BTN_TOPLEFT ? rxSmall : rx) : hw) / (float)w;
            float nyr = (jfaButtonShape[i] == 0 ?
                         (i == JFA_BTN_MENU || i == JFA_BTN_TOPLEFT ? rxSmall : rx) : hh) / (float)h;
            int pressed = buttons->finger[i] >= 0;
            float cr = pressed ? 1.00f : 0.68f;
            float cg = pressed ? 0.90f : 0.70f;
            float cb = pressed ? 0.50f : 0.75f;

            if (jfaButtonShape[i] == 0) {
                for (k = 0; k < 16; k++) {
                    double a0 = 2.0 * 3.14159265358979323846 * k / 16.0;
                    double a1 = 2.0 * 3.14159265358979323846 * (k + 1) / 16.0;
                    BPV(cx + nxr * (float)cos(a0), cy + nyr * (float)sin(a0), cr, cg, cb);
                    BPV(cx + nxr * (float)cos(a1), cy + nyr * (float)sin(a1), cr, cg, cb);
                }
            } else {
                float lhw, lhh;
                jfa_button_extents(i, w, h, &lhw, &lhh);
                float xr = lhw / (float)w, yr = lhh / (float)h;
                BPV(cx-xr, cy-yr, cr,cg,cb); BPV(cx+xr, cy-yr, cr,cg,cb);
                BPV(cx+xr, cy-yr, cr,cg,cb); BPV(cx+xr, cy+yr, cr,cg,cb);
                BPV(cx+xr, cy+yr, cr,cg,cb); BPV(cx-xr, cy+yr, cr,cg,cb);
                BPV(cx-xr, cy+yr, cr,cg,cb); BPV(cx-xr, cy-yr, cr,cg,cb);
            }

            /* simple glyphs, drawn as thin lines */
            if (i == JFA_BTN_MENU) {
                for (k = -1; k <= 1; k++) {
                    float yy = cy + k * nyr * 0.45f;
                    BPV(cx - nxr*0.45f, yy, cr,cg,cb);
                    BPV(cx + nxr*0.45f, yy, cr,cg,cb);
                }
            } else if (i == JFA_BTN_FF) {
                for (k = 0; k < 2; k++) {
                    float xx = cx + (k ? nxr*0.35f : -nxr*0.35f);
                    BPV(xx-nxr*0.18f, cy-nyr*0.45f, cr,cg,cb);
                    BPV(xx+nxr*0.18f, cy, cr,cg,cb);
                    BPV(xx+nxr*0.18f, cy, cr,cg,cb);
                    BPV(xx-nxr*0.18f, cy+nyr*0.45f, cr,cg,cb);
                }
            } else if (i == JFA_BTN_TRI) {
                BPV(cx, cy-nyr*0.55f, cr,cg,cb);
                BPV(cx-nxr*0.50f, cy+nyr*0.45f, cr,cg,cb);
                BPV(cx-nxr*0.50f, cy+nyr*0.45f, cr,cg,cb);
                BPV(cx+nxr*0.50f, cy+nyr*0.45f, cr,cg,cb);
                BPV(cx+nxr*0.50f, cy+nyr*0.45f, cr,cg,cb);
                BPV(cx, cy-nyr*0.55f, cr,cg,cb);
            } else if (i == JFA_BTN_SQUARE) {
                BPV(cx-nxr*0.45f, cy-nyr*0.45f, cr,cg,cb);
                BPV(cx+nxr*0.45f, cy-nyr*0.45f, cr,cg,cb);
                BPV(cx+nxr*0.45f, cy-nyr*0.45f, cr,cg,cb);
                BPV(cx+nxr*0.45f, cy+nyr*0.45f, cr,cg,cb);
                BPV(cx+nxr*0.45f, cy+nyr*0.45f, cr,cg,cb);
                BPV(cx-nxr*0.45f, cy+nyr*0.45f, cr,cg,cb);
                BPV(cx-nxr*0.45f, cy+nyr*0.45f, cr,cg,cb);
                BPV(cx-nxr*0.45f, cy-nyr*0.45f, cr,cg,cb);
            } else if (i == JFA_BTN_CIRCLE || i == JFA_BTN_TOPLEFT) {
                for (k = 0; k < 12; k++) {
                    double a0 = 2.0 * 3.14159265358979323846 * k / 12.0;
                    double a1 = 2.0 * 3.14159265358979323846 * (k + 1) / 12.0;
                    BPV(cx + nxr*0.5f*(float)cos(a0), cy + nyr*0.5f*(float)sin(a0), cr,cg,cb);
                    BPV(cx + nxr*0.5f*(float)cos(a1), cy + nyr*0.5f*(float)sin(a1), cr,cg,cb);
                }
            } else if (i == JFA_BTN_X) {
                BPV(cx-nxr*0.45f, cy-nyr*0.45f, cr,cg,cb);
                BPV(cx+nxr*0.45f, cy+nyr*0.45f, cr,cg,cb);
                BPV(cx+nxr*0.45f, cy-nyr*0.45f, cr,cg,cb);
                BPV(cx-nxr*0.45f, cy+nyr*0.45f, cr,cg,cb);
            }
        }
    }
#undef BPV

    if (n > (int)(sizeof(v) / sizeof(v[0])))
        return;
    ntri = 0;       /* all geometry here is drawn with GL_LINES */
    static const float ident[16] = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    glUseProgram(prog);
    glUniformMatrix4fv(mvp, 1, GL_FALSE, ident);
    glUniform1f(mode, 1.0f);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (size_t)n * sizeof(float), v, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(pos);
    glEnableVertexAttribArray(col);
    glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glVertexAttribPointer(col, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLES, 0, ntri);
    glDrawArrays(GL_LINES, ntri, n / 6 - ntri);
    glEnable(GL_DEPTH_TEST);
}

/* ---- M3.1-A: one movement step (pure math, desktop-testable).
 * pos[0]/pos[1] are X/Z; Y is never touched (character stays grounded).
 * dt is clamped here so a long pause cannot teleport through a wall. */
#define JFA_TURN_RATE 9.0f /* radians/second; smooth yaw toward input */
#define JFA_PI 3.14159265358979323846f
#define JFA_TWO_PI 6.28318530717958647692f
static void jfa_move_step(float pos[2], float *facing, float dx, float dz,
                          double dt, float xLimit, float zBack,
                          float zFront)
{
 const float speed = 3.0f; /* world units per second */
 float len;

 if (dt < 0.0) dt = 0.0;
 if (dt > 0.05) dt = 0.05;
 len = sqrtf(dx * dx + dz * dz);
 if (len > 0.0f) {
     float target, diff, maxStep;
     dx /= len;
     dz /= len;              /* diagonal is not faster than straight */
     target = atan2f(dx, dz);
     /* Shortest signed arc, correct across the -PI/PI seam. */
     diff = atan2f(sinf(target - *facing), cosf(target - *facing));
     maxStep = JFA_TURN_RATE * (float)dt;
     if (diff > maxStep) diff = maxStep;
     if (diff < -maxStep) diff = -maxStep;
     *facing += diff;
     /* Wrap into [-PI, PI] without unbounded accumulation. */
     *facing = fmodf(*facing + JFA_PI, JFA_TWO_PI);
     if (*facing < 0.0f) *facing += JFA_TWO_PI;
     *facing -= JFA_PI;
     pos[0] += dx * speed * (float)dt;
     pos[1] += dz * speed * (float)dt;
 }
 if (pos[0] < -xLimit) pos[0] = -xLimit;
 if (pos[0] >  xLimit) pos[0] =  xLimit;
 if (pos[1] < zBack)   pos[1] = zBack;
 if (pos[1] > zFront)  pos[1] = zFront;
}
static int push(Mesh *m,Vertex v){
 if(m->count==m->cap){size_t n=m->cap?m->cap*2:4096;Vertex *p=(Vertex*)realloc(m->v,n*sizeof(Vertex));if(!p)return 0;m->v=p;m->cap=n;}m->v[m->count++]=v;return 1;
}
static int idx(int x,int n){return x>0?x-1:(x<0?n+x:-1);}

static int loadOBJ(const char *path,Mesh *m){
 size_t len=0;char *buf=(char*)SDL_LoadFile(path,&len);if(!buf){SDL_Log("OBJ read: %s",SDL_GetError());return 0;}
 float *p=NULL,*uv=NULL,*no=NULL;int np=0,nu=0,nn=0,cp=0,cu=0,cn=0;
 float mn[3]={1e30f,1e30f,1e30f},mx[3]={-1e30f,-1e30f,-1e30f};char *outer=NULL;
 for(char *line=strtok_r(buf,"\n",&outer);line;line=strtok_r(NULL,"\n",&outer)){
  if(line[0]=='v'&&line[1]==' '){float x,y,z;if(sscanf(line+2,"%f %f %f",&x,&y,&z)==3){
   if(np+3>cp){cp=cp?cp*2:4096;p=(float*)realloc(p,cp*sizeof(float));}if(!p)goto bad;
   p[np++]=x;p[np++]=y;p[np++]=z;if(x<mn[0])mn[0]=x;if(y<mn[1])mn[1]=y;if(z<mn[2])mn[2]=z;if(x>mx[0])mx[0]=x;if(y>mx[1])mx[1]=y;if(z>mx[2])mx[2]=z;
  }}else if(line[0]=='v'&&line[1]=='t'){float u,v;if(sscanf(line+2,"%f %f",&u,&v)==2){
   if(nu+2>cu){cu=cu?cu*2:4096;uv=(float*)realloc(uv,cu*sizeof(float));}if(!uv)goto bad;uv[nu++]=u;uv[nu++]=v;
  }}else if(line[0]=='v'&&line[1]=='n'){float x,y,z;if(sscanf(line+2,"%f %f %f",&x,&y,&z)==3){
   if(nn+3>cn){cn=cn?cn*2:4096;no=(float*)realloc(no,cn*sizeof(float));}if(!no)goto bad;no[nn++]=x;no[nn++]=y;no[nn++]=z;
  }}else if(line[0]=='f'&&line[1]==' '){
   int pi[8],ti[8],ni[8],count=0;char *inner=NULL;
   for(char *t=strtok_r(line+2," \t\r",&inner);t&&count<8;t=strtok_r(NULL," \t\r",&inner)){
    int a=0,b=0,c=0;if(sscanf(t,"%d/%d/%d",&a,&b,&c)!=3){count=0;break;}pi[count]=idx(a,np/3);ti[count]=idx(b,nu/2);ni[count]=idx(c,nn/3);count++;
   }
   for(int tri=1;tri<count-1;tri++){int ids[3]={0,tri,tri+1};for(int k=0;k<3;k++){
    int q=ids[k],a=pi[q],b=ti[q],c=ni[q];if(a<0||b<0||c<0||a>=np/3||b>=nu/2||c>=nn/3)goto bad;
    Vertex v={{p[a*3],p[a*3+1],p[a*3+2]},{uv[b*2],1.0f-uv[b*2+1]},{no[c*3],no[c*3+1],no[c*3+2]}};
    if(!push(m,v))goto bad;
   }}
  }
 }
 if(!m->count||np<3)goto bad;
 float center[3];for(int j=0;j<3;j++)center[j]=(mn[j]+mx[j])*0.5f;
 float extent=fmaxf(mx[0]-mn[0],fmaxf(mx[1]-mn[1],mx[2]-mn[2]));if(extent<0.0001f)goto bad;
 float scale=2.2f/extent;for(size_t i=0;i<m->count;i++)for(int j=0;j<3;j++)m->v[i].p[j]=(m->v[i].p[j]-center[j])*scale;
 SDL_free(buf);free(p);free(uv);free(no);SDL_Log("OBJ OK: %d verts, %d UVs, %d normals, %u triangle vertices",np/3,nu/2,nn/3,(unsigned)m->count);return 1;
bad: SDL_Log("OBJ parse failed");SDL_free(buf);free(p);free(uv);free(no);free(m->v);memset(m,0,sizeof(*m));return 0;
}
static GLuint loadBMP(const char *path){
 SDL_Surface *s=SDL_LoadBMP(path);if(!s){SDL_Log("BMP load: %s",SDL_GetError());return 0;}
 SDL_Surface *r=SDL_ConvertSurfaceFormat(s,SDL_PIXELFORMAT_RGBA32,0);SDL_FreeSurface(s);if(!r){SDL_Log("BMP convert: %s",SDL_GetError());return 0;}
 GLuint t=0;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,r->w,r->h,0,GL_RGBA,GL_UNSIGNED_BYTE,r->pixels);SDL_Log("Texture OK: %dx%d",r->w,r->h);SDL_FreeSurface(r);return t;
}
static void drawFPS(GLuint prog,GLuint vbo,GLint pos,GLint col,GLint mvp,GLint mode,int w,int h,int fps){
 char str[16];snprintf(str,sizeof(str),"FPS:%d",fps);float a[16*5*7*6];int n=0;float dx=6.0f/w,dy=6.0f/h,x0=1.0f-((float)SDL_strlen(str)*18+12)*2/w,y0=1.0f-24.0f/h;
 for(int ch=0;str[ch];ch++)for(int y=0;y<7;y++)for(int x=0;x<5;x++)if(font[glyph(str[ch])][y]&(1u<<(4-x))){a[n++]=x0+(ch*6+x)*dx;a[n++]=y0-y*dy;a[n++]=0;a[n++]=1;a[n++]=1;a[n++]=1;}
 const float id[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};glDisable(GL_DEPTH_TEST);glUseProgram(prog);glUniformMatrix4fv(mvp,1,GL_FALSE,id);glUniform1f(mode,1);
 glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,n*sizeof(float),a,GL_DYNAMIC_DRAW);glEnableVertexAttribArray(pos);glEnableVertexAttribArray(col);glVertexAttribPointer(pos,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);glVertexAttribPointer(col,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(3*sizeof(float)));glDrawArrays(GL_POINTS,0,n/6);glEnable(GL_DEPTH_TEST);
}
static GLuint createArenaFloor(float y) {
    const float half = 8.0f * JFA_ARENA_SCALE;

    const FloorVertex floorVertices[] = {
        {{-half, y, -half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},
        {{-half, y,  half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},
        {{ half, y,  half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},

        {{-half, y, -half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},
        {{ half, y,  half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},
        {{ half, y, -half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}}
    };

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glBufferData(
        GL_ARRAY_BUFFER,
        sizeof(floorVertices),
        floorVertices,
        GL_STATIC_DRAW
    );

    return vbo;
}

static void addArenaFace(
    FloorVertex *vertices,
    size_t *count,
    const float a[3],
    const float b[3],
    const float c[3],
    const float d[3],
    float r, float g, float blue
) {
    const FloorVertex face[6] = {
        {{a[0], a[1], a[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{b[0], b[1], b[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{c[0], c[1], c[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{a[0], a[1], a[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{c[0], c[1], c[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{d[0], d[1], d[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}}
    };

    for (int i = 0; i < 6; i++) {
        vertices[*count + i] = face[i];
    }

    *count += 6;
}

static void addArenaBox(
    FloorVertex *vertices,
    size_t *count,
    float xMin, float yMin, float zMin,
    float xMax, float yMax, float zMax
) {
    const float points[8][3] = {
        {xMin, yMin, zMin},
        {xMax, yMin, zMin},
        {xMax, yMin, zMax},
        {xMin, yMin, zMax},
        {xMin, yMax, zMin},
        {xMax, yMax, zMin},
        {xMax, yMax, zMax},
        {xMin, yMax, zMax}
    };

    static const int faces[6][4] = {
        {0, 3, 2, 1},
        {4, 5, 6, 7},
        {0, 1, 5, 4},
        {3, 7, 6, 2},
        {0, 4, 7, 3},
        {1, 2, 6, 5}
    };

    static const float colors[6][3] = {
        {0.10f, 0.15f, 0.20f},
        {0.31f, 0.48f, 0.64f},
        {0.18f, 0.27f, 0.36f},
        {0.12f, 0.19f, 0.27f},
        {0.15f, 0.23f, 0.32f},
        {0.23f, 0.35f, 0.47f}
    };

    for (int i = 0; i < 6; i++) {
        addArenaFace(
            vertices, count,
            points[faces[i][0]],
            points[faces[i][1]],
            points[faces[i][2]],
            points[faces[i][3]],
            colors[i][0], colors[i][1], colors[i][2]
        );
    }
}

static GLuint createArenaWalls(
    float groundY,
    float wallHeight,
    float half,
    float thickness
) {
    const float inner = half - thickness;
    const float top = groundY + wallHeight;

    FloorVertex vertices[144];
    size_t count = 0;

    // Back wall
    addArenaBox(vertices, &count,
        -half, groundY, -half,
         half, top, -inner);

    // Front wall
    addArenaBox(vertices, &count,
        -half, groundY, inner,
         half, top, half);

    // Left wall
    addArenaBox(vertices, &count,
        -half, groundY, -inner,
        -inner, top, inner);

    // Right wall
    addArenaBox(vertices, &count,
        inner, groundY, -inner,
        half, top, inner);

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glBufferData(
        GL_ARRAY_BUFFER,
        count * sizeof(FloorVertex),
        vertices,
        GL_STATIC_DRAW
    );

    return vbo;
}


int main(int argc,char **argv){
 SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
 if(SDL_Init(SDL_INIT_VIDEO)!=0){SDL_Log("SDL: %s",SDL_GetError());return 1;}
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,16);
 SDL_Window *win=SDL_CreateWindow("JoJo Fighter - OBJ Test",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,800,450,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
 if(!win){SDL_Log("Window: %s",SDL_GetError());SDL_Quit();return 1;}SDL_GLContext ctx=SDL_GL_CreateContext(win);if(!ctx){SDL_Log("Context: %s",SDL_GetError());SDL_DestroyWindow(win);SDL_Quit();return 1;}SDL_GL_SetSwapInterval(1);
 GLuint prog=programCreate();if(!prog){SDL_GL_DeleteContext(ctx);SDL_DestroyWindow(win);SDL_Quit();return 1;}
FBXModel fbx = {0};
FBXMesh frameMesh = {0};
int fbxOK = 0;
GLuint tex = 0;
GLuint meshVBO = 0, fpsVBO = 0;
float groundY = 0.0f;
float characterTopY = 0.0f;

/* ---- JFA path: load the character once; FBX renderer is the fallback ---- */
{
	char jerr[512] = { 0 };
	jfaOK = jfa_load_all(&jfa, "animations", jerr, sizeof(jerr));
	if (jfaOK) {
		jfa_skin_setup(&jfa, 2.2f, &jfaScale, jfaCenter);
		if (!jfa_skin_alloc(&jfaSkin, (size_t)jfa.nv)) {
			SDL_Log("JFA skin alloc failed - falling back to FBX");
			jfa_free(&jfa);
			jfaOK = 0;
		}
	}
	if (jfaOK) {
		/* UVs never change: fill them once here, not every frame
		 * (same trick as the PSP loader). V is flipped for GLES. */
		for (size_t i = 0; i < (size_t)jfa.nv; i++) {
			jfaSkin.v[i].uv[0] = jfa.v[i].uv[0];
			jfaSkin.v[i].uv[1] = 1.0f - jfa.v[i].uv[1];
		}
		/* posed local space: feet at y=0, height ~2.2 */
		groundY = 0.0f;
		characterTopY = 2.2f;
		SDL_Log("JFA renderer active: %d bones, %d verts, %d clips, scale %.3f",
			jfa.nb, jfa.nv, JFA_CLIP_COUNT, (double)jfaScale);
	} else {
		SDL_Log("JFA load failed (%s) - using FBX fallback",
			jerr[0] ? jerr : "unknown error");
	}
}
if (!jfaOK) {
	fbxOK = fbx_load(&fbx, "Mma Idle.fbx");
	if (fbxOK) fbxOK = fbx_build_mesh(&fbx, 0.0, &frameMesh);
}
tex = loadBMP("model0.bmp");
glGenBuffers(1, &meshVBO);
glGenBuffers(1, &fpsVBO);
GLuint dpadVBO = 0; /* M3.1-B: dedicated D-pad overlay VBO */
glGenBuffers(1, &dpadVBO);
GLuint buttonsVBO = 0; /* M3.2-A: extra PSP-style controls */
glGenBuffers(1, &buttonsVBO);

if (fbxOK && frameMesh.vertex_count > 0) {
    groundY = frameMesh.vertices[0].position[1];
    characterTopY = groundY;

    for (size_t i = 1; i < frameMesh.vertex_count; i++) {
        float y = frameMesh.vertices[i].position[1];

        if (y < groundY) groundY = y;
        if (y > characterTopY) characterTopY = y;
    }
}

float characterHeight = characterTopY - groundY;
if (characterHeight < 0.001f) characterHeight = 2.0f;

GLuint arenaVBO = createArenaFloor(groundY - 0.02f);
GLuint arenaWallsVBO = createArenaWalls(
    groundY - 0.02f,
    JFA_ARENA_HALF,                 /* cube-like wall height */
    JFA_ARENA_HALF,
    characterHeight * 0.08f
);

if (!jfaOK && fbxOK && frameMesh.vertex_count > 0) {
    glBindBuffer(GL_ARRAY_BUFFER, meshVBO);
    glBufferData(
        GL_ARRAY_BUFFER,
        frameMesh.vertex_count * sizeof(FBXVertex),
        frameMesh.vertices,
        GL_DYNAMIC_DRAW
    );
} else if (jfaOK) {
    /* Reserve the VBO once; every frame only calls glBufferSubData. */
    glBindBuffer(GL_ARRAY_BUFFER, meshVBO);
    glBufferData(
        GL_ARRAY_BUFFER,
        (size_t)jfa.nv * sizeof(JFASkinVertex),
        NULL,
        GL_DYNAMIC_DRAW
    );
}
 GLint pos=glGetAttribLocation(prog,"aPosition"),uv=glGetAttribLocation(prog,"aUV"),normal=glGetAttribLocation(prog,"aNormal"),col=glGetAttribLocation(prog,"aColor");
 GLint mvpLoc=glGetUniformLocation(prog,"uMVP"),modelLoc=glGetUniformLocation(prog,"uModel"),texLoc=glGetUniformLocation(prog,"uTexture"),modeLoc=glGetUniformLocation(prog,"uTextMode");

#ifdef JFA_GPU_SKIN
/* ---- Stage C Substage A: GPU skinning infrastructure. Compiled in
 * ONLY with -DJFA_GPU_SKIN (temporary A/B switch); without it this
 * build is byte-for-byte the CPU behavior. Stage A creates the
 * program, checks the vertex-uniform budget at runtime, and uploads
 * the folded 3x4 matrices as 99 vec4 each frame. Vertex attributes
 * and rendering switch come in Stage B -- the CPU path still
 * renders everything below. ---- */
GLuint jfaGpuProg = 0;
GLint jfaGpuBoneLoc = -1;
int jfaGpuOK = 0;
#if !JFA_GPU_RENDER
static int jfaGpuAccN = 0;
#endif
static float jfaBoneStage[JFA_GPU_BONE_VECS * 4]; /* 99 vec4 staging */
/* Substage B: prepared static GPU vertex data. jfaGpuVerts stays NULL
 * on ANY preparation failure - that only disables the (not yet used)
 * GPU draw path later; the CPU renderer keeps rendering regardless. */
static JFAGpuVertex *jfaGpuVerts = NULL;
static GLuint jfaGpuVbo = 0;
/* Substage C: runtime render-mode switch. JFA_GPU_RENDER (0/1) is a
 * compile-time switch, defaulting to 0:
 *   JFA_GPU_SKIN off               -> CPU-only build (no GPU code)
 *   JFA_GPU_SKIN on, RENDER=0      -> prep + matrix validation only;
 *                                     CPU path renders (Stage A mode)
 *   JFA_GPU_SKIN on, RENDER=1      -> GPU path draws when every
 *                                     runtime check passes; any
 *                                     failure falls back to CPU. */
#ifndef JFA_GPU_RENDER
#define JFA_GPU_RENDER 0
#endif
#if JFA_GPU_RENDER
int jfaGpuRenderReady = 0;
GLint jfaGpuPosLoc = -1, jfaGpuUvLoc = -1, jfaGpuNrmLoc = -1;
GLint jfaGpuBoneIdxLoc = -1, jfaGpuBoneWgtLoc = -1;
GLint jfaGpuMvpLoc = -1, jfaGpuModelLoc = -1, jfaGpuTexLoc = -1,
	jfaGpuModeLoc = -1;
static double jfaGpuDrawAcc = 0.0;
static int jfaGpuDrawN = 0;
#endif
{
	GLint maxVUV = 0;
	glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &maxVUV);
	if (!jfaOK) {
		SDL_Log("JFA GPU skin: skipped (no JFA data)");
	} else if (!jfa_gpu_budget_ok(maxVUV, jfa.nb)) {
		SDL_Log("JFA GPU skin: budget fails (maxVUV=%d, bones=%d, need %d) - CPU path",
			maxVUV, jfa.nb, JFA_GPU_BASE_VECS + JFA_GPU_BONE_VECS);
	} else {
		GLuint va=shader(GL_VERTEX_SHADER,jfa_gpu_vs),vb=shader(GL_FRAGMENT_SHADER,fs);
		if (va && vb) {
			jfaGpuProg=glCreateProgram();glAttachShader(jfaGpuProg,va);glAttachShader(jfaGpuProg,vb);
			glLinkProgram(jfaGpuProg);GLint ok=0;glGetProgramiv(jfaGpuProg,GL_LINK_STATUS,&ok);
			if(!ok){char log[512];glGetProgramInfoLog(jfaGpuProg,sizeof(log),NULL,log);SDL_Log("JFA GPU link: %s",log);glDeleteProgram(jfaGpuProg);jfaGpuProg=0;}
		} else {
			SDL_Log("JFA GPU shader compile failed");
		}
		glDeleteShader(va);glDeleteShader(vb);
		if (jfaGpuProg) {
			jfaGpuBoneLoc=glGetUniformLocation(jfaGpuProg,"uBones");
			if (jfaGpuBoneLoc >= 0) {
				jfaGpuOK = 1;
				SDL_Log("JFA GPU program ready: maxVUV=%d, bones=%d, uBones=%d vec4 (Stage A: matrices only, CPU path renders)",
					maxVUV, jfa.nb, jfa.nb * 3);
			} else {
				SDL_Log("JFA GPU: uBones location not found - CPU path");
				glDeleteProgram(jfaGpuProg);jfaGpuProg=0;
			}
		}
	}
}
/* ---- Stage C Substage B: prepare the static GPU vertex data once.
 * Runs during init, AFTER the Stage A program was created and only if
 * jfaGpuOK (GPU budget + program ready). One allocation, one
 * validated conversion, one upload into a dedicated GL_STATIC_DRAW
 * VBO. Every failure path logs, cleans up after itself and leaves the
 * CPU renderer untouched - jfaGpuVerts/jfaGpuVbo are only consumed by
 * a future draw substage, never bound for drawing here. ---- */
if (jfaGpuOK && jfaOK && jfa.nv > 0) {
	const size_t gnv = (size_t)jfa.nv;
	jfaGpuVerts = malloc(gnv * sizeof(JFAGpuVertex));
	if (!jfaGpuVerts) {
		SDL_Log("JFA GPU verts: alloc failed (%zu x %d bytes) - CPU path unaffected",
			gnv, (int)sizeof(JFAGpuVertex));
	} else if (!jfa_gpu_vertices(&jfa, jfaGpuVerts, gnv)) {
		SDL_Log("JFA GPU verts: conversion rejected input - CPU path unaffected");
		free(jfaGpuVerts);
		jfaGpuVerts = NULL;
	} else {
		glGenBuffers(1, &jfaGpuVbo);
		if (jfaGpuVbo == 0) {
			SDL_Log("JFA GPU verts: glGenBuffers returned 0 - preparation failed, CPU path unaffected");
			free(jfaGpuVerts);
			jfaGpuVerts = NULL;
		} else {
			/* Clear any pre-existing GL error so it is not
			 * misattributed to the upload below. */
			while (glGetError() != GL_NO_ERROR) {
			}
			glBindBuffer(GL_ARRAY_BUFFER, jfaGpuVbo);
			glBufferData(GL_ARRAY_BUFFER,
				gnv * sizeof(JFAGpuVertex), jfaGpuVerts,
				GL_STATIC_DRAW);
			const GLenum gerr = glGetError();
			if (gerr != GL_NO_ERROR) {
				SDL_Log("JFA GPU verts: upload failed (GL error 0x%04x) - preparation failed, CPU path unaffected",
					gerr);
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glDeleteBuffers(1, &jfaGpuVbo);
				jfaGpuVbo = 0;
				free(jfaGpuVerts);
				jfaGpuVerts = NULL;
			} else {
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				SDL_Log("JFA GPU verts ready: %d verts, %d bytes each (Stage B: preparation only, no draw)",
					jfa.nv, (int)sizeof(JFAGpuVertex));
			}
		}
	}
}
#if JFA_GPU_RENDER
/* Substage C: query the GPU program's attribute and uniform
 * locations and validate the remaining hardware limits. Any failure
 * leaves jfaGpuRenderReady = 0 -> the CPU path renders. */
if (jfaGpuOK && jfaGpuVerts && jfaGpuVbo) {
	GLint maxAttribs = 0;
	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttribs);
	jfaGpuPosLoc = glGetAttribLocation(jfaGpuProg, "aPosition");
	jfaGpuUvLoc = glGetAttribLocation(jfaGpuProg, "aUV");
	jfaGpuNrmLoc = glGetAttribLocation(jfaGpuProg, "aNormal");
	jfaGpuBoneIdxLoc = glGetAttribLocation(jfaGpuProg, "aBoneIndex");
	jfaGpuBoneWgtLoc = glGetAttribLocation(jfaGpuProg, "aBoneWeight");
	jfaGpuMvpLoc = glGetUniformLocation(jfaGpuProg, "uMVP");
	jfaGpuModelLoc = glGetUniformLocation(jfaGpuProg, "uModel");
	jfaGpuTexLoc = glGetUniformLocation(jfaGpuProg, "uTexture");
	jfaGpuModeLoc = glGetUniformLocation(jfaGpuProg, "uTextMode");
	if (maxAttribs < 5 || jfaGpuPosLoc < 0 || jfaGpuUvLoc < 0 ||
	    jfaGpuNrmLoc < 0 || jfaGpuBoneIdxLoc < 0 ||
	    jfaGpuBoneWgtLoc < 0 || jfaGpuMvpLoc < 0 ||
	    jfaGpuModelLoc < 0 || jfaGpuTexLoc < 0 ||
	    jfaGpuModeLoc < 0) {
		SDL_Log("JFA GPU render: locations/limits fail (maxAttribs=%d, pos=%d, uv=%d, nrm=%d, bidx=%d, bwgt=%d, mvp=%d, model=%d, tex=%d, mode=%d) - CPU path",
			maxAttribs, jfaGpuPosLoc, jfaGpuUvLoc,
			jfaGpuNrmLoc, jfaGpuBoneIdxLoc, jfaGpuBoneWgtLoc,
			jfaGpuMvpLoc, jfaGpuModelLoc, jfaGpuTexLoc,
			jfaGpuModeLoc);
	} else {
		jfaGpuRenderReady = 1;
		SDL_Log("JFA GPU render active: %d verts, attribs %d/%d/%d/%d/%d (Stage C: GPU path draws, CPU is fallback)",
			jfa.nv, jfaGpuPosLoc, jfaGpuUvLoc, jfaGpuNrmLoc,
			jfaGpuBoneIdxLoc, jfaGpuBoneWgtLoc);
	}
}
#endif
#endif
 glEnable(GL_DEPTH_TEST);
glDepthFunc(GL_LEQUAL);

int run = 1;
SDL_Event e;

/* ---- M3.1-A: basic character movement (keyboard, development only).
 * charPos[0]/[1] are X/Z in the arena; Y stays 0 (grounded).
 * charFacing replaces the old static turntable angle ('angle'). ---- */
static float charPos[2] = { 0.0f, 0.0f };
static float charFacing = 0.0f;
static JFAButtons extraButtons;
static Uint64 moveLastCounter = 0;
static int moveTimeValid = 0;
/* M3.1-B: one finger per D-pad button. Touch hit testing uses WINDOW
 * coordinates (SDL_GetWindowSize); rendering keeps drawable pixels. */
static JFADpad dpad = { { -1, -1, -1, -1 } };
static JFAAttack attack = { 0, JFA_IDLE, 0.0, 0.0, -1 };
static int attackKeyPrev[3] = { 0, 0, 0 };  /* J/K/L edge tracking */
static int inputW = 1080, inputH = 1920;

Uint64 start = SDL_GetPerformanceCounter();
Uint64 animationStart = SDL_GetPerformanceCounter();

int frames = 0, fps = 0;
double freq = (double)SDL_GetPerformanceFrequency();
jfa_buttons_reset(&extraButtons);   /* -1 = no finger owns each button */
 while(run){
  {
    /* SDL finger coordinates are normalized over the WINDOW; keep
     * the last valid size if the query fails (e.g. minimized). */
    int iw = 0, ih = 0;
    SDL_GetWindowSize(win, &iw, &ih);
    if (iw > 0 && ih > 0) {
        inputW = iw;
        inputH = ih;
    }
  }
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT || e.type == SDL_APP_TERMINATING) {
        run = 0;
        jfa_dpad_reset(&dpad);
        jfa_attack_reset(&attack);
        jfa_buttons_reset(&extraButtons);
    } else if (e.type == SDL_APP_WILLENTERBACKGROUND ||
               e.type == SDL_APP_DIDENTERBACKGROUND) {
        /* No FINGERUP is guaranteed when Android backgrounds us. */
        jfa_dpad_reset(&dpad);
        jfa_attack_reset(&attack);
        jfa_buttons_reset(&extraButtons);
    }
#if defined(SDL_FINGERCANCELLED)
    else if (e.type == SDL_FINGERCANCELLED) {
        /* Cancellation is per-finger: release only what that finger owns. */
        jfa_dpad_cancel(&dpad, e.tfinger.fingerId);
        jfa_attack_release(&attack, e.tfinger.fingerId);
        jfa_buttons_up(&extraButtons, e.tfinger.fingerId);
    }
#endif
    else if (e.type == SDL_FINGERDOWN) {
        /* PSP-style buttons keep independent finger ownership. Triangle,
         * Square and X also trigger attacks; Circle stays visual-only. */
        int btn = jfa_buttons_find(e.tfinger.x, e.tfinger.y,
                                   inputW, inputH);
        if (btn >= 0) {
            double now = (SDL_GetPerformanceCounter() - animationStart) / freq;
            if (jfa_face_button_press(&extraButtons, &attack, btn,
                                      e.tfinger.fingerId, now,
                                      now - jfaClipStart))
                SDL_Log("JFA attack: finger=%lld clip=%s",
                        (long long)e.tfinger.fingerId,
                        jfa_clip_suffix[jfa_attack_clip_for_button(btn)]);
        }
        /* First touch inside the analogue circle claims continuous
         * control; others fall through to the existing digital path. */
        else if (!jfa_dpad_analog_apply(&dpad, e.tfinger.fingerId,
                                        e.tfinger.x, e.tfinger.y,
                                        inputW, inputH))
            jfa_dpad_down(&dpad, e.tfinger.fingerId,
                          e.tfinger.x, e.tfinger.y, inputW, inputH);
        JFA_DPAD_LOG(win, "DOWN", e.tfinger.fingerId,
                     e.tfinger.x, e.tfinger.y, inputW, inputH,
                     jfa_dpad_visual_mask(&dpad));
    } else if (e.type == SDL_FINGERMOTION) {
        const unsigned before = jfa_dpad_visual_mask(&dpad);
        /* A button owner that slides off its button is visually released. */
        for (int bi = 0; bi < JFA_BTN_COUNT; bi++) {
            if (extraButtons.finger[bi] == e.tfinger.fingerId) {
                if (jfa_buttons_hit(bi, e.tfinger.x, e.tfinger.y,
                                    inputW, inputH) != bi)
                    extraButtons.finger[bi] = -1;
            }
        }
        if (dpad.analogFinger == e.tfinger.fingerId) {
            /* Leaving the circle stops movement but keeps ownership
             * until release; re-entering resumes continuous control. */
            float vx = 0.0f, vy = 0.0f;
            if (!jfa_dpad_analog(e.tfinger.x, e.tfinger.y,
                                 inputW, inputH, &vx, &vy))
                vx = vy = 0.0f;
            dpad.analogX = vx;
            dpad.analogY = vy;
        } else {
            jfa_dpad_motion(&dpad, e.tfinger.fingerId,
                            e.tfinger.x, e.tfinger.y, inputW, inputH);
        }
        const unsigned after = jfa_dpad_visual_mask(&dpad);
        if (after != before)
            JFA_DPAD_LOG(win, "MOVE", e.tfinger.fingerId,
                         e.tfinger.x, e.tfinger.y, inputW, inputH, after);
    } else if (e.type == SDL_FINGERUP) {
        const unsigned before = jfa_dpad_visual_mask(&dpad);
        /* Releasing the attack finger does not cut the clip short. */
        jfa_attack_release(&attack, e.tfinger.fingerId);
        jfa_buttons_up(&extraButtons, e.tfinger.fingerId);
        jfa_dpad_analog_release(&dpad, e.tfinger.fingerId);
        jfa_dpad_up(&dpad, e.tfinger.fingerId);
        if (before != jfa_dpad_visual_mask(&dpad))
            JFA_DPAD_LOG(win, "UP", e.tfinger.fingerId,
                         e.tfinger.x, e.tfinger.y, inputW, inputH,
                         jfa_dpad_visual_mask(&dpad));
    }
  }
  double animationTime =
    (SDL_GetPerformanceCounter() - animationStart) / freq;

  /* ---- M3.1-A: keyboard movement (WASD + arrows), development only ---- */
  {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    /* Desktop aliases: J = PUNCH_RIGHT, K = PUNCH_LEFT, L = KICK.
     * Edge-triggered and routed through the same no-queue attack state. */
    {
        static const int keyScancode[3] = {
            SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L
        };
        for (int ki = 0; ki < 3; ki++) {
            int clip = jfa_attack_clip_for_key(ki);
            int down = keys[keyScancode[ki]] ? 1 : 0;
            if (down && !attackKeyPrev[ki]) {
                if (jfa_attack_trigger(&attack, animationTime,
                                       animationTime - jfaClipStart, -1,
                                       clip))
                    SDL_Log("JFA attack: key=%d clip=%s", ki,
                            jfa_clip_suffix[clip]);
            }
            attackKeyPrev[ki] = down;
        }
    }
    float dx = 0.0f, dz = 0.0f;
    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])  dx -= 1.0f;
    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) dx += 1.0f;
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])    dz -= 1.0f;
    if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])  dz += 1.0f;

    Uint64 counter = SDL_GetPerformanceCounter();
    double dt = moveTimeValid ?
        (double)(counter - moveLastCounter) / freq : 0.0;
    moveLastCounter = counter;
    moveTimeValid = 1;
    /* X: wall inner face (arena half - wall thickness) minus a body
     * radius. Y is untouched (grounded).
     * Z is ASYMMETRIC: the camera sits at world z=0 and sceneTrans
     * places the character at world z = charZ - 3.5. Keep at least
     * 1.15 body heights in front of the camera (near plane 0.2)
     * and no farther than 3 body heights away (still legible),
     * while never passing the arena's back wall. */
    const float arenaHalf = JFA_ARENA_HALF;
    const float wallThickness = characterHeight * 0.08f;
    const float bodyRadius = 0.35f;
    const float xLimit = arenaHalf - wallThickness - bodyRadius;
    const float sceneDist = 3.85f; /* camera pulled back slightly for larger room */
    const float frontZ = sceneDist - characterHeight * 1.15f;
    float backZ = sceneDist - characterHeight * 3.0f;
    if (backZ < -xLimit) backZ = -xLimit;
    if (backZ > frontZ - 0.5f) backZ = frontZ - 0.5f;
    /* Digital D-pad contributes to the same direction vector as the
     * keyboard; jfa_move_step normalizes diagonals and clamps dt. */
    const unsigned dmask = jfa_dpad_mask(&dpad);
    if (dmask & JFA_DIR_UP)    dz -= 1.0f;
    if (dmask & JFA_DIR_DOWN)  dz += 1.0f;
    if (dmask & JFA_DIR_LEFT)  dx -= 1.0f;
    if (dmask & JFA_DIR_RIGHT) dx += 1.0f;
    /* Analogue unit vector is summed with keyboard/digital input; the
     * existing normalization keeps speed constant for any combination. */
    if (dpad.analogFinger >= 0) {
        dx += dpad.analogX;
        dz += dpad.analogY;
    }
    lastMoveDx = dx;
    lastMoveDz = dz;
    jfa_move_step(charPos, &charFacing, dx, dz, dt,
                  xLimit, backZ, frontZ);
  }

if (!jfaOK) {
    /* FBX fallback: rebuild the mesh on the CPU every frame (existing path). */
    fbx_free_mesh(&frameMesh);

    if (fbxOK) {
        fbxOK = fbx_build_mesh(&fbx, animationTime, &frameMesh);
    }
}
  int w,h;SDL_GL_GetDrawableSize(win,&w,&h);if(w<1)w=1;if(h<1)h=1;glViewport(0,0,w,h);glClearColor(.08f,.08f,.12f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  if(tex && (jfaOK || (fbxOK && frameMesh.vertex_count > 0))){
   float c=cosf(charFacing),s=sinf(charFacing);
   /* Fixed scene transform (also used for arena) is kept separate
    * from the character translation so the arena never moves. A small
    * +X pitch tilts the world up, i.e. the camera looks slightly down. */
   float cp = cosf(JFA_CAMERA_PITCH), sp = sinf(JFA_CAMERA_PITCH);
   float sceneRot[16] = {1,0,0,0, 0,cp,sp,0, 0,-sp,cp,0, 0,0,0,1};
   float sceneMove[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,JFA_CAMERA_Y,JFA_CAMERA_Z,1};
   float sceneTrans[16];
   mm(sceneTrans, sceneMove, sceneRot);
   float charTrans[16]={1,0,0,0,0,1,0,0,0,0,1,0,charPos[0],0.0f,charPos[1],1};
   float model[16]={c,0,-s,0,0,1,0,0,s,0,c,0,0,0,0,1};
   float charScene[16],vm[16],pr[16]={0},mvp[16];
   mm(charScene, sceneTrans, charTrans);
   mm(vm, charScene, model);
   float f=1.0f/tanf(1.047198f*0.5f),aspect=(float)w/h;pr[0]=f/aspect;pr[5]=f;pr[10]=-1.002f;pr[11]=-1;pr[14]=-.2002f;mm(mvp,pr,vm);
   float identity[16] = {
    1,0,0,0,
    0,1,0,0,
    0,0,1,0,
    0,0,0,1
};

float floorMVP[16];
mm(floorMVP, pr, sceneTrans); /* arena stays fixed: no character offset */

glUseProgram(prog);
glUniform1f(modeLoc, 1.0f);
glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, floorMVP);
glUniformMatrix4fv(modelLoc, 1, GL_FALSE, identity);

glBindBuffer(GL_ARRAY_BUFFER, arenaVBO);

glEnableVertexAttribArray(pos);
glEnableVertexAttribArray(uv);
glEnableVertexAttribArray(normal);
glEnableVertexAttribArray(col);

glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, position));

glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, uv));

glVertexAttribPointer(normal, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, normal));

glVertexAttribPointer(col, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, color));

glDrawArrays(GL_TRIANGLES, 0, 6);

glBindBuffer(GL_ARRAY_BUFFER, arenaWallsVBO);

glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, position));

glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, uv));

glVertexAttribPointer(normal, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, normal));

glVertexAttribPointer(col, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, color));

glDrawArrays(GL_TRIANGLES, 0, 144);

glUseProgram(prog);glUniform1f(modeLoc,0);
glDisableVertexAttribArray(col);
glVertexAttrib3f(col, 1.0f, 1.0f, 1.0f);
   glUniformMatrix4fv(mvpLoc,1,GL_FALSE,mvp);glUniformMatrix4fv(modelLoc,1,GL_FALSE,model);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,tex);glUniform1i(texLoc,0);
   glBindBuffer(GL_ARRAY_BUFFER, meshVBO);

size_t drawCount = 0;
const JFAClip *jfaCur = NULL;
int sampleClip = jfaClip;   /* clip actually sampled this frame (attack or locomotion) */
int af = 0;
const float worldPos[3] = { 0.0f, 0.0f, 0.0f };
if (jfaOK) {
    /* ---- Animation advance (shared by CPU and GPU paths): the attack
     * state overrides the clip/frame, otherwise locomotion auto-cycles.
     * Both render paths consume sampleClip/af computed exactly here. ---- */
    JFAAnimSample sample = jfa_anim_select(&attack, animationTime,
                                           &jfaClip, &jfaClipStart,
                                           jfa.clips, lastMoveDx, lastMoveDz);
    sampleClip = sample.clip;
    jfaCur = &jfa.clips[sampleClip];
    jfaClipDur = (double)jfaCur->frames / (double)jfaCur->fps;
    af = jfa_skin_frame_index(jfaCur, sample.time);
}
#if JFA_GPU_SKIN && JFA_GPU_RENDER
int gpuDraw = 0;
if (jfaOK && jfaGpuRenderReady) {
    gpuDraw = 1;
    /* ---- Stage C: GPU skinning draw. One draw call from the
     * static jfaGpuVbo; the pose is the same folded matrix set the
     * CPU path uses (Substage A proved the fold bit-identical). ---- */
    Uint64 tGpu0 = SDL_GetPerformanceCounter();
    /* Clear stale GL errors so the post-draw check below is not
     * misattributed. */
    while (glGetError() != GL_NO_ERROR) {
    }
    jfa_gpu_fold(&jfa, sampleClip, af, jfaScale, jfaCenter, worldPos,
        jfaBoneStage);
    glUseProgram(jfaGpuProg);
    glUniformMatrix4fv(jfaGpuMvpLoc, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(jfaGpuModelLoc, 1, GL_FALSE, model);
    glUniform1i(jfaGpuTexLoc, 0);
    glUniform1f(jfaGpuModeLoc, 0.0f);
    glUniform4fv(jfaGpuBoneLoc, jfa.nb * 3, jfaBoneStage);
    glBindBuffer(GL_ARRAY_BUFFER, jfaGpuVbo);
    glEnableVertexAttribArray(jfaGpuPosLoc);
    glEnableVertexAttribArray(jfaGpuUvLoc);
    glEnableVertexAttribArray(jfaGpuNrmLoc);
    glEnableVertexAttribArray(jfaGpuBoneIdxLoc);
    glEnableVertexAttribArray(jfaGpuBoneWgtLoc);
    glVertexAttribPointer(jfaGpuPosLoc, 3, GL_FLOAT, GL_FALSE,
        (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, position));
    glVertexAttribPointer(jfaGpuUvLoc, 2, GL_FLOAT, GL_FALSE,
        (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, uv));
    glVertexAttribPointer(jfaGpuNrmLoc, 3, GL_FLOAT, GL_FALSE,
        (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, normal));
    glVertexAttribPointer(jfaGpuBoneIdxLoc, 4, GL_UNSIGNED_BYTE,
        GL_FALSE, (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, bone));
    glVertexAttribPointer(jfaGpuBoneWgtLoc, 4, GL_FLOAT, GL_FALSE,
        (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, weight));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)jfa.nv);
    /* Attribute enable state is GLOBAL in GLES2: the two extra
     * bone/weight arrays must not leak into the CPU/FBX/floor
     * draws. pos/uv/normal are re-pointed by whichever path draws
     * next, exactly as they already are today. */
    glDisableVertexAttribArray(jfaGpuBoneIdxLoc);
    glDisableVertexAttribArray(jfaGpuBoneWgtLoc);
    glUseProgram(prog); /* restore: FPS text renders with prog */
    glBindBuffer(GL_ARRAY_BUFFER, meshVBO);
    {
        GLenum derr = glGetError();
        if (derr != GL_NO_ERROR) {
            SDL_Log("JFA GPU render failed (GL error 0x%04x) - CPU path for the rest of the session",
                derr);
            jfaGpuRenderReady = 0;
        }
    }
    Uint64 tGpu1 = SDL_GetPerformanceCounter();
    jfaGpuDrawAcc += (double)(tGpu1 - tGpu0);
    if (++jfaGpuDrawN == 60) {
        double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
        SDL_Log("PERF jfa gpu draw %.2f ms (fold+uniforms+attribs+draw)",
            jfaGpuDrawAcc * ms / 60.0);
        jfaGpuDrawAcc = 0.0;
        jfaGpuDrawN = 0;
    }
    drawCount = (size_t)jfa.nv;
} else
#endif
if (jfaOK) {
    /* ---- JFA: CPU skin the current clip frame, then one sub-data upload ---- */
    Uint64 tSkin0 = SDL_GetPerformanceCounter();
    jfa_skin_frame(&jfa, sampleClip, af, jfaScale, jfaCenter, worldPos,
        &jfaSkin);
    Uint64 tSkin1 = SDL_GetPerformanceCounter();

    glBufferSubData(
        GL_ARRAY_BUFFER,
        0,
        (size_t)jfa.nv * sizeof(JFASkinVertex),
        jfaSkin.v
    );
    Uint64 tUpload1 = SDL_GetPerformanceCounter();

    jfaAccSkin += (double)(tSkin1 - tSkin0);
    jfaAccUpload += (double)(tUpload1 - tSkin1);
    jfaAccDraw += (double)(tUpload1 - tSkin0);
    if (++jfaAccN == 60) {
        double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
        SDL_Log("PERF jfa skin %.2f ms, upload %.2f ms, skin+upload %.2f ms",
            jfaAccSkin * ms / 60.0,
            jfaAccUpload * ms / 60.0,
            jfaAccDraw * ms / 60.0);
        jfaAccSkin = jfaAccUpload = jfaAccDraw = 0.0;
        jfaAccN = 0;
    }
#ifdef JFA_GPU_SKIN
#if !JFA_GPU_RENDER
    /* Stage A validation mode (RENDER=0): fold the same pose into
     * the compact vec4 layout and upload it to the (not rendering)
     * GPU program. In RENDER=1 builds the GPU branch above does
     * the fold + upload itself. */
    if (jfaGpuOK) {
        Uint64 tFold0 = SDL_GetPerformanceCounter();
        jfa_gpu_fold(&jfa, sampleClip, af, jfaScale, jfaCenter, worldPos,
            jfaBoneStage);
        glUseProgram(jfaGpuProg);
        glUniform4fv(jfaGpuBoneLoc, jfa.nb * 3, jfaBoneStage);
        glUseProgram(prog);
        if (++jfaGpuAccN == 60) {
            double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
            SDL_Log("PERF jfa gpu fold+upload %.2f ms (Stage A, CPU path renders)",
                (double)(SDL_GetPerformanceCounter() - tFold0) * ms / 60.0);
            jfaGpuAccN = 0;
        }
    }
#endif
#endif
    drawCount = (size_t)jfa.nv;
} else {
    glBufferData(
        GL_ARRAY_BUFFER,
        frameMesh.vertex_count * sizeof(FBXVertex),
        frameMesh.vertices,
        GL_DYNAMIC_DRAW
    );
    drawCount = frameMesh.vertex_count;
}

#if JFA_GPU_SKIN && JFA_GPU_RENDER
if (!gpuDraw)
#endif
{
glEnableVertexAttribArray(pos);
glEnableVertexAttribArray(uv);
glEnableVertexAttribArray(normal);
   glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE,
    sizeof(FBXVertex), (void*)offsetof(FBXVertex, position));

glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE,
    sizeof(FBXVertex), (void*)offsetof(FBXVertex, uv));

glVertexAttribPointer(normal, 3, GL_FLOAT, GL_FALSE,
    sizeof(FBXVertex), (void*)offsetof(FBXVertex, normal));

glDrawArrays(GL_TRIANGLES, 0, (GLsizei)drawCount);
}
  }
  frames++;Uint64 now=SDL_GetPerformanceCounter();if((double)(now-start)/freq>=.5){fps=(int)(frames*freq/(double)(now-start)+.5);frames=0;start=now;}
  drawFPS(prog,fpsVBO,pos,col,mvpLoc,modeLoc,w,h,fps);
  float levels[4];
  jfa_dpad_visual_levels(&dpad, levels);
  jfa_dpad_draw(prog, dpadVBO, pos, col, mvpLoc, modeLoc,
                inputW, inputH, levels); /* overlay only; character path untouched */
  jfa_buttons_draw(prog, buttonsVBO, pos, col, mvpLoc, modeLoc,
                   inputW, inputH, &extraButtons);
  SDL_GL_SwapWindow(win);
 }
 glDeleteBuffers(1, &meshVBO);
glDeleteBuffers(1, &fpsVBO);
glDeleteBuffers(1, &dpadVBO);
glDeleteBuffers(1, &buttonsVBO);
glDeleteBuffers(1, &arenaVBO);
glDeleteBuffers(1, &arenaWallsVBO);
if (tex)
    glDeleteTextures(1, &tex);

glDeleteProgram(prog);

if (jfaOK) {
    jfa_skin_free(&jfaSkin);
    jfa_free(&jfa);
}

#ifdef JFA_GPU_SKIN
/* Stage B cleanup: the GPU VBO and the prepared vertex array are
 * owned independently of the CPU renderer state. */
if (jfaGpuVbo) {
	glDeleteBuffers(1, &jfaGpuVbo);
	jfaGpuVbo = 0;
}
free(jfaGpuVerts);
jfaGpuVerts = NULL;
if (jfaGpuProg) {
	glDeleteProgram(jfaGpuProg);
	jfaGpuProg = 0;
}
#endif

fbx_free_mesh(&frameMesh);
fbx_free(&fbx);

SDL_GL_DeleteContext(ctx);
SDL_DestroyWindow(win);
SDL_Quit();

return 0;
}
