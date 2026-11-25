/*
 * jfa_reaction_test.c - desktop unit tests for M3.3 Stage 3b-B
 * (opponent hit reaction).
 *
 * The reaction state and its helpers are file-static next to the attack
 * logic in the Android app, so this harness compiles the REAL
 * Android/app/jni/src/main.c (with its entry point renamed) instead of
 * copying the logic. Startup code - OpenGL, SDL window, asset loading -
 * is never called and is discarded at link time by
 * -ffunction-sections/-fdata-sections/--gc-sections, so no GL or SDL
 * library is required.
 *
 * Build (from the repo root):
 *   cc -std=gnu99 -O1 -I tools/host_stubs \
 *      -I Android/app/jni/SDL/include -I Android/app/jni/ufbx \
 *      -ffunction-sections -fdata-sections tools/jfa_reaction_test.c \
 *      -lm -Wl,--gc-sections -o build/jfa_reaction_test
 *
 * Coverage:
 *   1. start: trigger activates, sample carries the hit clip and time
 *   2. completion: returns 0 exactly at frames/fps, clears the state and
 *      leaves the caller's sample untouched
 *   3. restart-on-rehit: a trigger during a reaction restarts the timer
 *   4. reset: clears every field to a defined, in-range value
 *   5. invalid clip data: bad indices, frames <= 0, fps <= 0, NULL
 *      arguments and a backwards clock can never produce a stuck
 *      reaction, a negative sample time or a division by zero
 *
 * Exit code 0 = all tests passed.
 */

/* Rename the app's entry point so this file can own main(). */
#define main jfa_android_app_main
#include "../Android/app/jni/src/main.c"
#undef main

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
    if (!cond)
        failures++;
}

static void check_near(double got, double want, const char *what)
{
    double d = got - want;

    if (d < 0.0)
        d = -d;
    check(d <= 1e-12, what);
}

/* Two valid clips: idle 30 frames @ 30 fps, hit 15 frames @ 30 fps. */
static JFAClip clips[JFA_CLIP_COUNT];

static void clips_init(void)
{
    memset(clips, 0, sizeof(clips));
    clips[JFA_IDLE].frames = 30;
    clips[JFA_IDLE].fps = 30;
    clips[JFA_HIT].frames = 15;
    clips[JFA_HIT].fps = 30;
}

static void test_clip_seconds(void)
{
    puts("clip duration");

    check_near(jfa_clip_seconds(&clips[JFA_HIT]), 0.5, "15 frames @ 30 fps = 0.5 s");
    clips[JFA_HIT].frames = 151;
    check_near(jfa_clip_seconds(&clips[JFA_HIT]), 151.0 / 30.0,
               "151 frames @ 30 fps matches frames/fps");
    clips[JFA_HIT].frames = 15;

    clips[JFA_IDLE].frames = 0;
    check_near(jfa_clip_seconds(&clips[JFA_IDLE]), 0.0, "frames = 0 -> 0 s");
    clips[JFA_IDLE].frames = -5;
    check_near(jfa_clip_seconds(&clips[JFA_IDLE]), 0.0, "negative frames -> 0 s");
    clips[JFA_IDLE].frames = 30;
    {
        uint32_t saved = clips[JFA_IDLE].fps;
        clips[JFA_IDLE].fps = 0;
        check_near(jfa_clip_seconds(&clips[JFA_IDLE]), 0.0,
                   "fps = 0 -> 0 s (no division by zero)");
        clips[JFA_IDLE].fps = saved;
    }
    check_near(jfa_clip_seconds(NULL), 0.0, "NULL clip -> 0 s");
}

static void test_start(void)
{
    JFAReaction r;
    JFAAnimSample s;

    puts("reaction start");

    jfa_reaction_reset(&r);
    memset(&s, 0, sizeof(s));
    check(jfa_reaction_select(&r, 10.0, clips, &s) == 0,
          "no sample before a trigger");
    check(r.active == 0, "reaction inactive after reset");

    check(jfa_reaction_trigger(&r, 10.0, JFA_HIT, clips) == 1,
          "trigger accepted for a valid hit clip");
    check(r.active == 1, "reaction active after trigger");
    check(r.clip == JFA_HIT, "reaction remembers the hit clip");

    check(jfa_reaction_select(&r, 10.0, clips, &s) == 1,
          "sample at the trigger instant is a reaction sample");
    check(s.clip == JFA_HIT, "sample clip is the hit clip");
    check_near(s.time, 0.0, "sample time starts at 0");

    check(jfa_reaction_select(&r, 10.1, clips, &s) == 1,
          "sample mid-reaction");
    check_near(s.time, 0.1, "sample time is now - start");
    check(r.active == 1, "reaction stays active mid-clip");
}

static void test_completion(void)
{
    JFAReaction r;
    JFAAnimSample s;

    puts("reaction completion");

    jfa_reaction_reset(&r);
    jfa_reaction_trigger(&r, 2.0, JFA_HIT, clips);
    memset(&s, 0, sizeof(s));

    check(jfa_reaction_select(&r, 2.0 + 0.5 - 1e-6, clips, &s) == 1,
          "still active just before frames/fps");
    check_near(s.time, 0.5 - 1e-6, "last sample time is just before duration");

    s.clip = JFA_KICK;
    s.time = 42.0;
    check(jfa_reaction_select(&r, 2.5, clips, &s) == 0,
          "inactive exactly at frames/fps");
    check(r.active == 0, "reaction cleared at the end of the clip");
    check(s.clip == JFA_KICK && s.time == 42.0,
          "caller's sample is untouched when the reaction is over");

    check(jfa_reaction_select(&r, 2.6, clips, &s) == 0,
          "stays inactive after completion");
}

static void test_restart(void)
{
    JFAReaction r;
    JFAAnimSample s;

    puts("restart on re-hit");

    jfa_reaction_reset(&r);
    jfa_reaction_trigger(&r, 5.0, JFA_HIT, clips);
    jfa_reaction_select(&r, 5.2, clips, &s);
    check_near(s.time, 0.2, "first hit is 0.2 s in");

    check(jfa_reaction_trigger(&r, 5.2, JFA_HIT, clips) == 1,
          "a second hit during the reaction is accepted");
    check(jfa_reaction_select(&r, 5.3, clips, &s) == 1,
          "restarted reaction is active");
    check_near(s.time, 0.1, "restart resets the timer instead of continuing");
    check(jfa_reaction_select(&r, 5.2 + 0.5, clips, &s) == 0,
          "restarted reaction ends one duration after the re-hit");
}

static void test_reset(void)
{
    JFAReaction r;
    JFAAnimSample s;

    puts("reset");

    jfa_reaction_reset(&r);
    jfa_reaction_trigger(&r, 1.0, JFA_HIT, clips);
    jfa_reaction_reset(&r);

    check(r.active == 0, "reset clears active");
    check(r.clip == JFA_IDLE, "reset parks the clip at a valid idle index");
    check_near(r.start, 0.0, "reset clears the timer");
    check(jfa_reaction_select(&r, 1.1, clips, &s) == 0,
          "no sample after reset");

    jfa_reaction_reset(&r);
    check(r.active == 0, "reset is idempotent");
    jfa_reaction_reset(NULL);
    check(1, "reset(NULL) is safe");
}

static void test_invalid(void)
{
    JFAReaction r;
    JFAAnimSample s;

    puts("invalid clip data and defensive inputs");

    jfa_reaction_reset(&r);
    check(jfa_reaction_trigger(&r, 0.0, -1, clips) == 0,
          "negative clip index is refused");
    check(jfa_reaction_trigger(&r, 0.0, JFA_CLIP_COUNT, clips) == 0,
          "clip index == JFA_CLIP_COUNT is refused");
    check(r.active == 0, "refused triggers leave the reaction inactive");

    clips[JFA_HIT].frames = 0;
    check(jfa_reaction_trigger(&r, 0.0, JFA_HIT, clips) == 0,
          "frames = 0 is refused");
    clips[JFA_HIT].frames = -3;
    check(jfa_reaction_trigger(&r, 0.0, JFA_HIT, clips) == 0,
          "negative frames is refused");
    clips[JFA_HIT].frames = 15;
    {
        uint32_t saved = clips[JFA_HIT].fps;
        clips[JFA_HIT].fps = 0;
        check(jfa_reaction_trigger(&r, 0.0, JFA_HIT, clips) == 0,
              "fps = 0 is refused");
        clips[JFA_HIT].fps = saved;
    }

    check(jfa_reaction_trigger(NULL, 0.0, JFA_HIT, clips) == 0,
          "trigger(NULL) is safe");
    check(jfa_reaction_trigger(&r, 0.0, JFA_HIT, NULL) == 0,
          "trigger without clip data is refused");
    check(jfa_reaction_select(NULL, 0.0, clips, &s) == 0,
          "select(NULL) is safe");
    check(jfa_reaction_select(&r, 0.0, clips, NULL) == 0,
          "select without output is safe");

    /* An already-active reaction whose clip data becomes invalid must be
     * dropped on the next update instead of sampling garbage. */
    jfa_reaction_trigger(&r, 0.0, JFA_HIT, clips);
    r.clip = 999;
    check(jfa_reaction_select(&r, 0.0, clips, &s) == 0,
          "out-of-range active clip is dropped");
    check(r.active == 0, "stale reaction is cleared");

    jfa_reaction_trigger(&r, 0.0, JFA_HIT, clips);
    r.clip = -1;
    check(jfa_reaction_select(&r, 0.0, clips, &s) == 0,
          "negative active clip is dropped");
    check(r.active == 0, "negative clip clears the reaction");

    jfa_reaction_trigger(&r, 0.0, JFA_HIT, clips);
    clips[JFA_HIT].fps = 0;
    check(jfa_reaction_select(&r, 0.0, clips, &s) == 0,
          "active reaction with fps = 0 is dropped, not divided by zero");
    check(r.active == 0, "zero-fps reaction is cleared");
    clips[JFA_HIT].fps = 30;

    /* A clock that went backwards must not yield a negative sample time. */
    jfa_reaction_reset(&r);
    jfa_reaction_trigger(&r, 5.0, JFA_HIT, clips);
    check(jfa_reaction_select(&r, 4.5, clips, &s) == 1,
          "backwards clock still returns a sample");
    check_near(s.time, 0.0, "backwards clock is clamped to frame 0");
    check(r.active == 1, "backwards clock does not end the reaction");
}

int main(void)
{
    puts("== M3.3 Stage 3b-B reaction tests ==");
    clips_init();

    test_clip_seconds();
    test_start();
    test_completion();
    test_restart();
    test_reset();
    test_invalid();

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    puts("\nall reaction checks passed");
    return 0;
}
