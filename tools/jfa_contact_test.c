/*
 * jfa_contact_test.c - desktop tests for the contact-window module
 * (Android/app/jni/src/jfa_contact.c), for the Phase B combat timing that now
 * gates strikes with it, and regression checks that damage, health, reach,
 * cone and the one-hit-per-activation rule are unchanged.
 *
 * The window data and queries are compiled directly from the real Android
 * sources. The strike gate and damage path are file-static next to the attack
 * logic in the app, so this harness also compiles the REAL
 * Android/app/jni/src/main.c (with its entry point renamed) - the same
 * approach as tools/jfa_reaction_test.c.
 * Startup code (OpenGL, SDL window, asset loading) is never called and is
 * discarded at link time by -ffunction-sections/-fdata-sections/--gc-sections,
 * so no GL or SDL library is required.
 *
 * Build (from the repo root):
 *   cc -std=gnu99 -O1 -Wall -Wextra -I Android/app/jni/src \
 *      -I tools/host_stubs -I Android/app/jni/SDL/include \
 *      -I Android/app/jni/ufbx -ffunction-sections -fdata-sections \
 *      tools/jfa_contact_test.c Android/app/jni/src/jfa_contact.c \
 *      Android/app/jni/src/jfa_skin.c \
 *      -lm -Wl,--gc-sections -o build/jfa_contact_test
 *   build/jfa_contact_test
 *
 * Coverage:
 *   1. table values: every window equals the Phase 0 measurement
 *      (punch_left f12..f17; punch_right f29..f31 and f42..f46; kick f32..f36),
 *      clips without measured contacts report none
 *   2. lookup: invalid clip indices and out-of-range window indices
 *   3. staleness guard: a window only matches the clip data it was measured on
 *      (frame count and fps), and only sane frame ranges
 *   4. seconds conversion: [first/fps, (last+1)/fps), NULL out-params allowed
 *   5. frame hits: inside/outside each window, the gap between punch_right's
 *      two contacts, and defensive inputs (negative/past-the-end time, NaN,
 *      NULL, bad data, unmeasured clips)
 *   6. windows within their clip: ordered, non-overlapping, inside the clip
 *   7. Phase B strike gate: the real jfa_attack_in_strike_window() opens on
 *      the measured contact frames of left punch, right punch and kick, and no
 *      longer on the old 30%..70% fractions
 *   8. Phase B damage path: jfa_attack_hits() + jfa_attack_try_damage() apply
 *      exactly one 10-damage event per activation for all three attacks,
 *      including punch_right's two contact windows and repeated activations
 *   9. Phase B fallback: stale clip data (different frame count or fps) and
 *      invalid data fall back to the legacy 30%..70% window safely
 *
 * Exit code 0 = all tests passed.
 */

/* Rename the app's entry point so this file can own main(). */
#define main jfa_android_app_main
#include "../Android/app/jni/src/main.c"
#undef main

#include "../Android/app/jni/src/jfa_contact.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
    if (!cond)
        failures++;
}

static void check_near(double got, double want, double tol, const char *what)
{
    double d = got - want;

    if (d < 0.0)
        d = -d;
    check(d <= tol, what);
}

/* The real clip data the windows were measured on. */
static JFAClip testClips[JFA_CLIP_COUNT];

static void clips_init(void)
{
    memset(testClips, 0, sizeof(testClips));
    testClips[JFA_IDLE].frames = 151;
    testClips[JFA_WALK].frames = 31;
    testClips[JFA_BACKWALK].frames = 31;
    testClips[JFA_GUARD].frames = 41;
    testClips[JFA_PUNCH_LEFT].frames = 53;
    testClips[JFA_PUNCH_RIGHT].frames = 91;
    testClips[JFA_KICK].frames = 70;
    testClips[JFA_HIT].frames = 15;
    testClips[JFA_KNOCKDOWN].frames = 61;
    testClips[JFA_GETUP].frames = 71;
    for (int i = 0; i < JFA_CLIP_COUNT; i++)
        testClips[i].fps = 30;
}

/* ---------------------------------------------------------------- */
/* 1. measured values                                                */
/* ---------------------------------------------------------------- */

static void test_measured_values(void)
{
    const JFAContactWindow *w;

    puts("measured windows match the Phase 0 log");

    check(jfa_contact_count(JFA_PUNCH_LEFT) == 1, "punch_left has one contact window");
    w = jfa_contact_window(JFA_PUNCH_LEFT, 0);
    check(w != NULL, "punch_left window 0 exists");
    check(w && w->firstFrame == 12, "punch_left window starts at frame 12");
    check(w && w->lastFrame == 17, "punch_left window ends at frame 17");
    check(w && w->frames == 53, "punch_left window records 53 frames");
    check(w && w->fps == 30, "punch_left window records 30 fps");

    check(jfa_contact_count(JFA_PUNCH_RIGHT) == 2, "punch_right has two contact windows");
    w = jfa_contact_window(JFA_PUNCH_RIGHT, 0);
    check(w && w->firstFrame == 29 && w->lastFrame == 31,
          "punch_right first window is frames 29..31");
    check(w && w->frames == 91 && w->fps == 30,
          "punch_right first window records 91 frames @ 30 fps");
    w = jfa_contact_window(JFA_PUNCH_RIGHT, 1);
    check(w && w->firstFrame == 42 && w->lastFrame == 46,
          "punch_right second window is frames 42..46");
    check(w && w->frames == 91 && w->fps == 30,
          "punch_right second window records 91 frames @ 30 fps");

    check(jfa_contact_count(JFA_KICK) == 1, "kick has one contact window");
    w = jfa_contact_window(JFA_KICK, 0);
    check(w && w->firstFrame == 32 && w->lastFrame == 36,
          "kick window is frames 32..36");
    check(w && w->frames == 70, "kick window records 70 frames");

    check(jfa_contact_count(JFA_IDLE) == 0, "idle has no measured contacts");
    check(jfa_contact_count(JFA_WALK) == 0, "walk has no measured contacts");
    check(jfa_contact_count(JFA_BACKWALK) == 0, "backwalk has no measured contacts");
    check(jfa_contact_count(JFA_GUARD) == 0, "guard has no measured contacts");
    check(jfa_contact_count(JFA_HIT) == 0, "hit has no measured contacts");
    check(jfa_contact_count(JFA_KNOCKDOWN) == 0, "knockdown has no measured contacts");
    check(jfa_contact_count(JFA_GETUP) == 0, "getup has no measured contacts");
}

/* ---------------------------------------------------------------- */
/* 2. lookup bounds                                                  */
/* ---------------------------------------------------------------- */

static void test_lookup(void)
{
    puts("lookup bounds");

    check(jfa_contact_count(-1) == 0, "negative clip index has no windows");
    check(jfa_contact_count(JFA_CLIP_COUNT) == 0, "clip index == count has no windows");
    check(jfa_contact_window(-1, 0) == NULL, "window(-1) is NULL");
    check(jfa_contact_window(JFA_CLIP_COUNT, 0) == NULL, "window(count) is NULL");
    check(jfa_contact_window(JFA_PUNCH_LEFT, -1) == NULL, "negative window index is NULL");
    check(jfa_contact_window(JFA_PUNCH_LEFT, 1) == NULL, "window index past the end is NULL");
    check(jfa_contact_window(JFA_PUNCH_RIGHT, 2) == NULL,
          "punch_right third window index is NULL");
    check(jfa_contact_window(JFA_IDLE, 0) == NULL, "unmeasured clip has no window");
}

/* ---------------------------------------------------------------- */
/* 3. staleness guard                                                */
/* ---------------------------------------------------------------- */

static void test_matches(void)
{
    const JFAContactWindow *w = jfa_contact_window(JFA_PUNCH_LEFT, 0);
    JFAContactWindow tmp;

    puts("window/clip data matching");

    check(jfa_contact_window_matches(w, &testClips[JFA_PUNCH_LEFT]) == 1,
          "window matches the clip data it was measured on");
    check(jfa_contact_window_matches(NULL, &testClips[JFA_PUNCH_LEFT]) == 0,
          "NULL window never matches");
    check(jfa_contact_window_matches(w, NULL) == 0, "NULL clip never matches");

    {
        JFAClip c = testClips[JFA_PUNCH_LEFT];

        c.frames = 54;
        check(jfa_contact_window_matches(w, &c) == 0,
              "re-baked clip with a different frame count is refused");
        c.frames = 52;
        check(jfa_contact_window_matches(w, &c) == 0,
              "shorter clip is refused");
        c.frames = 53;
        c.fps = 60;
        check(jfa_contact_window_matches(w, &c) == 0,
              "clip at a different fps is refused");
        c.fps = 30;
        check(jfa_contact_window_matches(w, &c) == 1, "original data matches again");
    }

    tmp = *w;
    tmp.frames = 0;
    check(jfa_contact_window_matches(&tmp, &testClips[JFA_PUNCH_LEFT]) == 0,
          "window with frames = 0 is refused");
    tmp = *w;
    tmp.fps = 0;
    check(jfa_contact_window_matches(&tmp, &testClips[JFA_PUNCH_LEFT]) == 0,
          "window with fps = 0 is refused (no division by zero)");
    tmp = *w;
    tmp.lastFrame = 53;
    check(jfa_contact_window_matches(&tmp, &testClips[JFA_PUNCH_LEFT]) == 0,
          "window ending past the last frame is refused");
    tmp = *w;
    tmp.firstFrame = -1;
    check(jfa_contact_window_matches(&tmp, &testClips[JFA_PUNCH_LEFT]) == 0,
          "negative first frame is refused");
    tmp = *w;
    tmp.firstFrame = 18;
    tmp.lastFrame = 17;
    check(jfa_contact_window_matches(&tmp, &testClips[JFA_PUNCH_LEFT]) == 0,
          "reversed frame range is refused");
    tmp = *w;
    tmp.clip = JFA_CLIP_COUNT;
    check(jfa_contact_window_matches(&tmp, &testClips[JFA_PUNCH_LEFT]) == 0,
          "window with an invalid clip index is refused");
}

/* ---------------------------------------------------------------- */
/* 4. seconds conversion                                             */
/* ---------------------------------------------------------------- */

static void test_span_seconds(void)
{
    double s = -1.0, e = -1.0;

    puts("window -> displayed seconds");

    check(jfa_contact_span_seconds(jfa_contact_window(JFA_PUNCH_LEFT, 0),
                                   &testClips[JFA_PUNCH_LEFT], &s, &e) == 1,
          "punch_left window converts");
    check_near(s, 12.0 / 30.0, 1e-9, "punch_left starts at frame 12 = 0.400 s");
    check_near(e, 18.0 / 30.0, 1e-9, "punch_left ends after frame 17 = 0.600 s");

    check(jfa_contact_span_seconds(jfa_contact_window(JFA_PUNCH_RIGHT, 0),
                                   &testClips[JFA_PUNCH_RIGHT], &s, &e) == 1,
          "punch_right first window converts");
    check_near(s, 29.0 / 30.0, 1e-9, "punch_right first starts at 0.9667 s");
    check_near(e, 32.0 / 30.0, 1e-9, "punch_right first ends at 1.0667 s");

    check(jfa_contact_span_seconds(jfa_contact_window(JFA_PUNCH_RIGHT, 1),
                                   &testClips[JFA_PUNCH_RIGHT], &s, &e) == 1,
          "punch_right second window converts");
    check_near(s, 42.0 / 30.0, 1e-9, "punch_right second starts at 1.400 s");
    check_near(e, 47.0 / 30.0, 1e-9, "punch_right second ends at 1.5667 s");

    check(jfa_contact_span_seconds(jfa_contact_window(JFA_KICK, 0),
                                   &testClips[JFA_KICK], &s, &e) == 1,
          "kick window converts");
    check_near(s, 32.0 / 30.0, 1e-9, "kick starts at 1.0667 s");
    check_near(e, 37.0 / 30.0, 1e-9, "kick ends after frame 36 at 1.2333 s");

    check(jfa_contact_span_seconds(jfa_contact_window(JFA_KICK, 0),
                                   &testClips[JFA_KICK], NULL, NULL) == 1,
          "NULL start/end out-params are allowed");

    s = -7.0;
    {
        JFAClip stale = testClips[JFA_KICK];

        stale.frames = 69;
        check(jfa_contact_span_seconds(jfa_contact_window(JFA_KICK, 0), &stale,
                                       &s, &e) == 0,
              "stale clip data is refused");
        check(s == -7.0, "refusal leaves the out-params untouched");
    }
    check(jfa_contact_span_seconds(NULL, &testClips[JFA_KICK], &s, &e) == 0,
          "NULL window is refused");
}

/* ---------------------------------------------------------------- */
/* 5. frame hits                                                     */
/* ---------------------------------------------------------------- */

static void test_frame_hits(void)
{
    puts("contact-frame queries");

    /* punch_left: frames 12..17 of a 53-frame clip. */
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 0.0) == 0,
          "punch_left start of clip is not a contact");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 0.3999) == 0,
          "punch_left 0.3999 s is still frame 11 (no contact)");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 0.400) == 1,
          "punch_left 0.400 s is frame 12 (contact)");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 0.467) == 1,
          "punch_left measured peak frame 14 is a contact");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 0.5999) == 1,
          "punch_left last frame of the window is a contact");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 0.600) == 0,
          "punch_left 0.600 s is frame 18 (window is exclusive at the end)");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 1.5) == 0,
          "punch_left after the window is not a contact");

    /* punch_right: 29..31 and 42..46 of a 91-frame clip. */
    check(jfa_contact_frame_hits(JFA_PUNCH_RIGHT, testClips, 1.000) == 1,
          "punch_right first peak (frame 30) is a contact");
    check(jfa_contact_frame_hits(JFA_PUNCH_RIGHT, testClips, 1.10) == 0,
          "punch_right frame 33 (retraction) is not a contact");
    check(jfa_contact_frame_hits(JFA_PUNCH_RIGHT, testClips, 1.20) == 0,
          "punch_right between the two contacts is not a contact");
    check(jfa_contact_frame_hits(JFA_PUNCH_RIGHT, testClips, 1.400) == 1,
          "punch_right second window opens at frame 42");
    check(jfa_contact_frame_hits(JFA_PUNCH_RIGHT, testClips, 1.467) == 1,
          "punch_right second peak (frame 44) is a contact");
    check(jfa_contact_frame_hits(JFA_PUNCH_RIGHT, testClips, 1.55) == 1,
          "punch_right last frame of the second window is a contact");
    check(jfa_contact_frame_hits(JFA_PUNCH_RIGHT, testClips, 1.60) == 0,
          "punch_right after the second window is not a contact");

    /* kick: 32..36 of a 70-frame clip. */
    check(jfa_contact_frame_hits(JFA_KICK, testClips, 1.05) == 0,
          "kick frame 31 (before contact) is not a contact");
    check(jfa_contact_frame_hits(JFA_KICK, testClips, 1.0667) == 1,
          "kick window opens at frame 32");
    check(jfa_contact_frame_hits(JFA_KICK, testClips, 1.167) == 1,
          "kick measured peak frame 35 is a contact");
    check(jfa_contact_frame_hits(JFA_KICK, testClips, 1.2333) == 1,
          "kick last frame of the window is a contact");
    check(jfa_contact_frame_hits(JFA_KICK, testClips, 1.24) == 0,
          "kick after the window is not a contact");

    /* Clips with no measured data never report a contact. */
    check(jfa_contact_frame_hits(JFA_HIT, testClips, 0.1) == 0,
          "clip without measured contacts never reports a contact");
    check(jfa_contact_frame_hits(JFA_IDLE, testClips, 1.0) == 0,
          "idle never reports a contact");

    /* Defensive inputs. */
    check(jfa_contact_frame_hits(-1, testClips, 0.45) == 0,
          "negative clip index is refused");
    check(jfa_contact_frame_hits(JFA_CLIP_COUNT, testClips, 0.45) == 0,
          "clip index == count is refused");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, NULL, 0.45) == 0,
          "NULL clip array is refused");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, -1.0) == 0,
          "negative elapsed time is refused");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 53.0 / 30.0) == 0,
          "elapsed at the clip duration is refused");
    check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 99.0) == 0,
          "elapsed past the clip duration is refused");
    {
        double nan_value = 0.0 / 0.0;

        check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, nan_value) == 0,
              "NaN elapsed time is refused");
    }

    {
        JFAClip saved = testClips[JFA_PUNCH_LEFT];

        testClips[JFA_PUNCH_LEFT].frames = 0;
        check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 0.45) == 0,
              "clip with frames = 0 is refused");
        testClips[JFA_PUNCH_LEFT] = saved;
        testClips[JFA_PUNCH_LEFT].fps = 0;
        check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, testClips, 0.45) == 0,
              "clip with fps = 0 is refused (no division by zero)");
        testClips[JFA_PUNCH_LEFT] = saved;
    }

    {
        JFAClip stale = testClips[JFA_PUNCH_LEFT];

        stale.frames = 54;
        check(jfa_contact_frame_hits(JFA_PUNCH_LEFT, &stale, 0.45) == 0,
              "re-baked clip length is refused rather than using stale frames");
    }
    {
        JFAClip stale = testClips[JFA_KICK];

        stale.fps = 60;
        check(jfa_contact_frame_hits(JFA_KICK, &stale, 1.1) == 0,
              "re-baked clip rate is refused rather than using stale frames");
    }
}

/* ---------------------------------------------------------------- */
/* 6. table sanity                                                   */
/* ---------------------------------------------------------------- */

static void test_table_sanity(void)
{
    puts("windows are ordered, disjoint and inside their clips");

    for (int clip = 0; clip < JFA_CLIP_COUNT; clip++) {
        int n = jfa_contact_count(clip);
        int prevLast = -1;

        for (int i = 0; i < n; i++) {
            const JFAContactWindow *w = jfa_contact_window(clip, i);
            double s = 0.0, e = 0.0;
            char what[128];

            snprintf(what, sizeof(what), "clip %d window %d has valid data", clip, i);
            check(jfa_contact_window_matches(w, &testClips[clip]) == 1, what);
            snprintf(what, sizeof(what), "clip %d window %d starts after the previous one", clip, i);
            check(w && w->firstFrame > prevLast, what);
            snprintf(what, sizeof(what), "clip %d window %d is ordered", clip, i);
            check(w && w->firstFrame <= w->lastFrame, what);
            if (w)
                prevLast = w->lastFrame;
            snprintf(what, sizeof(what), "clip %d window %d lies inside the clip", clip, i);
            check(jfa_contact_span_seconds(w, &testClips[clip], &s, &e) == 1 &&
                      s >= 0.0 && e <= (double)testClips[clip].frames /
                                             (double)testClips[clip].fps,
                  what);
        }
        if (n == 0) {
            char what[128];

            snprintf(what, sizeof(what), "clip %d has no measurable window", clip);
            check(jfa_contact_window(clip, 0) == NULL, what);
        }
    }
}

/* ---------------------------------------------------------------- */
/* 7. Phase B: the strike gate uses the measured contact frames      */
/* ---------------------------------------------------------------- */

static JFAAttack active_attack(int clip)
{
    JFAAttack a;

    memset(&a, 0, sizeof(a));
    a.active = 1;
    a.clip = clip;
    a.start = 0.0;
    return a;
}

static void test_combat_timing(void)
{
    JFAAttack a;
    double dur = 53.0 / 30.0;   /* punch_left */

    puts("Phase B: the gate follows the measured contact frames");

    /* Unchanged by Phase B. */
    check(JFA_HIT_DAMAGE == 10, "hit damage is still 10");
    check(JFA_MAX_HEALTH == 100, "max health is still 100");
    check_near((double)JFA_HIT_REACH, 1.2, 1e-6, "hit reach is still 1.2");
    check_near((double)JFA_HIT_COS_HALF_CONE, 0.5, 1e-6,
               "hit cone is still cos(60 deg)");
    /* The old fractions survive only as the explicit fallback window. */
    check_near((double)JFA_HIT_WINDOW_START, 0.30, 1e-12,
               "legacy fallback start is still 30% of the clip");
    check_near((double)JFA_HIT_WINDOW_END, 0.70, 1e-12,
               "legacy fallback end is still 70% of the clip");

    /* Left punch: measured frames 12..17 = 0.400..0.600 s displayed. */
    a = active_attack(JFA_PUNCH_LEFT);
    check(jfa_attack_in_strike_window(&a, testClips, 0.3999) == 0,
          "left punch gate rejects frame 11 (before contact)");
    check(jfa_attack_in_strike_window(&a, testClips, 0.400) == 1,
          "left punch gate accepts frame 12 (first contact frame)");
    check(jfa_attack_in_strike_window(&a, testClips, 0.467) == 1,
          "left punch gate accepts the measured peak frame 14");
    check(jfa_attack_in_strike_window(&a, testClips, 0.5999) == 1,
          "left punch gate accepts the last contact frame 17");
    check(jfa_attack_in_strike_window(&a, testClips, 0.600) == 0,
          "left punch gate rejects frame 18 (contact finished)");
    check(jfa_attack_in_strike_window(&a, testClips, 0.620) == 0,
          "left punch 0.620 s is rejected although the old 30-70% window accepted it");
    check(jfa_attack_in_strike_window(&a, testClips, dur) == 0,
          "gate rejects the end of the clip");

    /* Right punch: measured frames 29..31 and 42..46. */
    a = active_attack(JFA_PUNCH_RIGHT);
    check(jfa_attack_in_strike_window(&a, testClips, 1.000) == 1,
          "right punch gate accepts its first contact (frame 30)");
    check(jfa_attack_in_strike_window(&a, testClips, 1.200) == 0,
          "right punch gate rejects the gap between its two contacts");
    check(jfa_attack_in_strike_window(&a, testClips, 1.400) == 1,
          "right punch gate accepts its second contact (frame 42)");
    check(jfa_attack_in_strike_window(&a, testClips, 1.467) == 1,
          "right punch gate accepts its second peak (frame 44)");
    check(jfa_attack_in_strike_window(&a, testClips, 1.600) == 0,
          "right punch gate closes after the second contact");

    /* Kick: measured frames 32..36. */
    a = active_attack(JFA_KICK);
    check(jfa_attack_in_strike_window(&a, testClips, 1.050) == 0,
          "kick gate rejects frame 31 (before contact)");
    check(jfa_attack_in_strike_window(&a, testClips, 1.0667) == 1,
          "kick gate accepts frame 32 (first contact frame)");
    check(jfa_attack_in_strike_window(&a, testClips, 1.167) == 1,
          "kick gate accepts the measured peak frame 35");
    check(jfa_attack_in_strike_window(&a, testClips, 1.500) == 0,
          "kick 1.500 s is rejected although the old 30-70% window accepted it");

    a.active = 0;
    check(jfa_attack_in_strike_window(&a, testClips, 0.467) == 0,
          "gate rejects when no attack is active");
}

/* ---------------------------------------------------------------- */
/* 8. Phase B: real hit + damage path                                */
/* ---------------------------------------------------------------- */

static void fighters_setup(JFAFighter *attacker, JFAFighter *target)
{
    memset(attacker, 0, sizeof(*attacker));
    memset(target, 0, sizeof(*target));
    attacker->facing = 0.0f;    /* forward = +Z */
    target->pos[1] = 0.5f;      /* 0.5 in front, well inside 1.2 reach */
    jfa_health_init(&target->health);
    jfa_attack_reset(&attacker->attack);
}

/* One complete activation at a 60 Hz tick, calling the same hit/damage pair
 * the app loop calls. Returns the number of damage events applied. */
static int run_activation(int clip, int *hp)
{
    JFAFighter attacker, target;
    double dur = (double)testClips[clip].frames / (double)testClips[clip].fps;
    int events = 0;
    double t;

    fighters_setup(&attacker, &target);
    if (!jfa_attack_trigger(&attacker.attack, 0.0, 0.0, -1, clip))
        return -1;

    for (t = 0.0; t < dur; t += 1.0 / 60.0) {
        if (jfa_attack_hits(&attacker, &target, testClips, t) &&
            jfa_attack_try_damage(&attacker.attack, &target.health,
                                  JFA_HIT_DAMAGE))
            events++;
    }
    if (hp)
        *hp = target.health.health;
    return events;
}

static void test_damage_path(void)
{
    int hp = 0;

    puts("Phase B: one damage event per activation, all three attacks");

    check(run_activation(JFA_PUNCH_LEFT, &hp) == 1,
          "left punch applies exactly one damage event");
    check(hp == 90, "left punch removes exactly 10 health (100 -> 90)");

    check(run_activation(JFA_PUNCH_RIGHT, &hp) == 1,
          "right punch applies exactly one damage event despite two contact windows");
    check(hp == 90, "right punch removes exactly 10 health (100 -> 90)");

    check(run_activation(JFA_KICK, &hp) == 1,
          "kick applies exactly one damage event");
    check(hp == 90, "kick removes exactly 10 health (100 -> 90)");
}

static void test_one_hit_per_activation(void)
{
    JFAFighter attacker, target;
    double dur = 91.0 / 30.0;   /* punch_right */
    int events = 0, refused = 0;
    double t;

    puts("one damage event per activation across both right-punch windows");

    fighters_setup(&attacker, &target);
    check(jfa_attack_trigger(&attacker.attack, 0.0, 0.0, -1,
                             JFA_PUNCH_RIGHT) == 1,
          "right punch activation is accepted");

    for (t = 0.0; t < dur; t += 1.0 / 60.0) {
        if (!refused && t >= 1.0)
            refused = !jfa_attack_trigger(&attacker.attack, t, 0.0, -1,
                                          JFA_PUNCH_RIGHT);
        if (jfa_attack_hits(&attacker, &target, testClips, t) &&
            jfa_attack_try_damage(&attacker.attack, &target.health,
                                  JFA_HIT_DAMAGE))
            events++;
    }

    check(refused == 1, "a second trigger during the same activation is refused");
    check(events == 1, "only one damage event is applied in the whole activation");
    check(target.health.health == 90, "opponent health is 90, not 80");
    check(jfa_attack_in_strike_window(&attacker.attack, testClips, 1.000) == 1,
          "the first contact window was inside the strike window");
    check(jfa_attack_in_strike_window(&attacker.attack, testClips, 1.467) == 1,
          "the second contact window is still inside the strike window");
}

static void test_repeated_activations(void)
{
    JFAFighter attacker, target;
    int events = 0;
    double t;

    puts("repeated activations each apply one damage event");

    fighters_setup(&attacker, &target);
    for (t = 0.0; t < 4.5; t += 1.0 / 60.0) {
        /* Same completion path the app uses: jfa_anim_select ends an attack
         * once its clip has fully played out. */
        jfa_anim_select(&attacker.attack, t, &attacker.clip,
                        &attacker.clipStart, testClips, 0.0f, 0.0f);
        if (!attacker.attack.active)
            jfa_attack_trigger(&attacker.attack, t, 0.0, -1, JFA_PUNCH_LEFT);
        if (jfa_attack_hits(&attacker, &target, testClips, t) &&
            jfa_attack_try_damage(&attacker.attack, &target.health,
                                  JFA_HIT_DAMAGE))
            events++;
    }

    check(events == 3, "three consecutive left punches apply three damage events");
    check(target.health.health == 70, "opponent health is 70 after three punches");
}

/* ---------------------------------------------------------------- */
/* 9. Phase B: stale / invalid data falls back safely                */
/* ---------------------------------------------------------------- */

static void test_stale_and_invalid_fallback(void)
{
    JFAAttack a;
    JFAClip saved;
    JFAFighter attacker, target;

    puts("stale or invalid contact data falls back to the legacy gate");

    /* Different frame count: the measured frames no longer describe the clip. */
    a = active_attack(JFA_PUNCH_LEFT);
    saved = testClips[JFA_PUNCH_LEFT];
    testClips[JFA_PUNCH_LEFT].frames = 54;
    check(jfa_attack_in_strike_window(&a, testClips, 0.467) == 0,
          "stale frame count is not trusted (measured contact time rejected)");
    check(jfa_attack_in_strike_window(&a, testClips, 0.600) == 1,
          "stale frame count falls back to the 30-70% window");
    check(jfa_attack_in_strike_window(&a, testClips, 0.30 * 1.8 - 1e-6) == 0,
          "fallback window still rejects before 30% of the clip");
    check(jfa_attack_in_strike_window(&a, testClips, 0.70 * 1.8 + 1e-3) == 0,
          "fallback window still rejects after 70% of the clip");
    testClips[JFA_PUNCH_LEFT] = saved;

    /* Different fps: same guard. */
    a = active_attack(JFA_KICK);
    saved = testClips[JFA_KICK];
    testClips[JFA_KICK].fps = 60;
    check(jfa_attack_in_strike_window(&a, testClips, 0.500) == 1,
          "stale fps falls back to the 30-70% window");
    check(jfa_attack_in_strike_window(&a, testClips, 1.167) == 0,
          "stale fps no longer uses the measured kick contact");
    testClips[JFA_KICK] = saved;

    /* Invalid clip data never strikes and never divides by zero. */
    a = active_attack(JFA_PUNCH_LEFT);
    saved = testClips[JFA_PUNCH_LEFT];
    testClips[JFA_PUNCH_LEFT].frames = 0;
    check(jfa_attack_in_strike_window(&a, testClips, 0.45) == 0,
          "clip with frames = 0 cannot strike");
    testClips[JFA_PUNCH_LEFT] = saved;
    testClips[JFA_PUNCH_LEFT].fps = 0;
    check(jfa_attack_in_strike_window(&a, testClips, 0.45) == 0,
          "clip with fps = 0 cannot strike (no division by zero)");
    testClips[JFA_PUNCH_LEFT] = saved;

    /* Out-of-range clip indices in the attack state are refused, not read. */
    a = active_attack(JFA_CLIP_COUNT);
    check(jfa_attack_in_strike_window(&a, testClips, 0.45) == 0,
          "attack with clip index == count is refused");
    a = active_attack(-1);
    check(jfa_attack_in_strike_window(&a, testClips, 0.45) == 0,
          "attack with a negative clip index is refused");
    check(jfa_attack_in_strike_window(&a, NULL, 0.45) == 0,
          "NULL clip array is refused");

    /* An invalid attack cannot damage anything through the hit path. */
    fighters_setup(&attacker, &target);
    attacker.attack.active = 1;
    attacker.attack.clip = JFA_CLIP_COUNT;
    check(jfa_attack_hits(&attacker, &target, testClips, 0.45) == 0,
          "hit detection refuses an out-of-range attack clip");
    jfa_attack_reset(&attacker.attack);
    check(jfa_attack_try_damage(&attacker.attack, &target.health,
                                JFA_HIT_DAMAGE) == 0,
          "an inactive attack applies no damage");
    check(target.health.health == 100, "target health is untouched");
}

int main(void)
{
    puts("== contact-window + Phase B combat-timing tests ==");
    clips_init();

    test_measured_values();
    test_lookup();
    test_matches();
    test_span_seconds();
    test_frame_hits();
    test_table_sanity();
    test_combat_timing();
    test_damage_path();
    test_one_hit_per_activation();
    test_repeated_activations();
    test_stale_and_invalid_fallback();

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    puts("\nall contact-window and combat-timing checks passed");
    return 0;
}
