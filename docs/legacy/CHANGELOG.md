# JoJoFighter — Changelog

Chronological record of milestones. Newest entries are added at the bottom, in
the order the milestones were completed.

Each entry records the milestone or version, the date when known, added
features, changes and improvements, bug fixes, testing status, and the
relevant commit SHA.

Related documents:

- [BUGS_AND_ERRORS.md](BUGS_AND_ERRORS.md) — confirmed bugs, suspected
  problems and their verification
- [JFA1.md](JFA1.md) — animation format specification

Only what was actually implemented and actually tested is recorded here.
Untested scenarios are named as untested, and device testing is only recorded
as passed once the user reports the result.

## Earlier history (before this changelog)

This changelog starts with M3.3 Stage 3b-B. Earlier milestones (M1 JFA1
format and converter; M2 Stage A loader, Stage B CPU skinning, Stage C GPU
skinning; M3.1 movement, D-pad and facing; M3.2 basic attacks and
face-button controls; M3.3 two-fighter rendering, Stage 3a hit detection and
Stage 3b-A opponent facing) predate it and are not reconstructed here to avoid
inventing details. Their commits remain the authoritative record:

```sh
git log --oneline --reverse
```

## M3.3 Stage 3b-B — opponent hit reaction

- **Date:** 2026-11-24
- **Commit:** `4fbe9a38fb1959ba60df1dfa9a77f3cc22113233` (`M3.3 Stage 3b-B: opponent hit reaction`)
- **Base:** `192aa4a7128eafe338d557b137e651965c3a73f5` (M3.3 Stage 3b-A)
- **Branch:** `android-prototype`

### Added

- Opponent hit reaction: the opponent now plays the `hit` animation when it is
  damaged.
- `tools/jfa_reaction_test.c`, a host test suite that compiles the real
  `Android/app/jni/src/main.c` (entry point renamed) and covers reaction
  start, completion, restart-on-rehit, reset, and invalid clip data.
- `tools/host_stubs/GLES2/gl2.h`, a test-only GLES2 header shim so the host
  test builds without GL or SDL libraries. It is not part of the APK.

### Changes and improvements

- The reaction triggers only after damage was really applied, i.e. after
  `jfa_attack_try_damage()` accepted the hit. A blocked, already-applied or
  out-of-range hit never starts a reaction.
- Repeated hits during a reaction restart it from frame 0, so consecutive
  attacks each produce a fresh reaction instead of being ignored or
  interrupting into a half-played animation.
- When the reaction clip finishes, the reaction clears and the opponent
  returns to its normal animation. The locomotion cursor is deliberately left
  running underneath, so the opponent resumes the idle phase it would have
  had.
- The reaction overrides locomotion only. An attack (unreachable for the
  opponent in this milestone) would outrank it.
- Background/resume: an in-flight reaction is cleared on
  `SDL_APP_WILLENTERBACKGROUND` / `SDL_APP_DIDENTERBACKGROUND`, because
  `animationTime` keeps running while backgrounded and the reaction would
  otherwise resume as a stale pose.
- The final hit at 0 HP plays its reaction once and then returns to idle;
  knockdown and KO are deferred to Stage 3b-C.

### Bug fixes

None. No bug was confirmed or fixed in this milestone.

### Testing status

- **Host tests — passed.** `tools/jfa_reaction_test.c` built with the repo's
  documented command and reported no failed checks (reaction start,
  completion at `frames / fps`, restart-on-rehit, reset, and invalid clip data
  including bad indices, `frames <= 0`, `fps <= 0`, NULL arguments and a
  backwards clock). The pre-existing host suites (loader, skin, GPU, and
  `validate_jfa.sh --tol 1e-3`) also passed.
- **CI — passed.** GitHub Actions workflow `android.yml` built the debug APK
  (`assembleDebug`) successfully and uploaded the `JoJo-Fighter-Android`
  artifact (8.21 MB). The PSP workflow `build.yml` also succeeded.
- **Android device testing — passed, as reported by the user.** The installed
  APK was checked on the device for: launch, opponent reacting to a hit,
  returning to normal afterwards, repeated hits restarting the reaction, and
  background/resume.
- **Not tested:** knockdown/KO behavior (out of scope for this milestone,
  deferred to 3b-C), non-`hit` reaction clips, and reaction behavior for
  fighter 0 (the player does not react in 3b-B).
