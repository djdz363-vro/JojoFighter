# Legacy reference material

These documents describe the **old** JoJo Fighters prototype. They are kept
because they contain measured data and format knowledge that the remake can
reuse. Source paths mentioned inside them (for example `Android/app/jni/src/...`,
`tools/jfa_*.c`, root-level `model0_*.jfa`) refer to the old layout and exist
only in the archive tags, not in this branch.

| File | Origin | Why it is kept |
|------|--------|----------------|
| [JFA1.md](JFA1.md) | `docs/JFA1.md` on `freebuff/58d654f…` | Byte-level spec of the JFA1 animation format used by every preserved `.jfa` clip; needed to read or convert them |
| [phase0-contact-measurements.txt](phase0-contact-measurements.txt) | `docs/phase0-contact-measurements.txt` on `freebuff/58d654f…` | Measured per-frame limb extension/height and contact windows for punch_left, punch_right and kick; useful for hitbox and frame-data design |
| [CHANGELOG.md](CHANGELOG.md) | `docs/CHANGELOG.md` on `android-prototype` | Milestone history of the prototype (M1–M3.3) and what was tested |
| [BUGS_AND_ERRORS.md](BUGS_AND_ERRORS.md) | `docs/BUGS_AND_ERRORS.md` on `android-prototype` | Bugs and pitfalls found in the prototype; lessons for the remake |

Path translation to this branch:

| Old path | New path |
|----------|----------|
| `model0_*.jfa`, `Android/app/src/main/assets/animations/*.jfa` | `assets/characters/model0/animations/` |
| `model0.obj/png`, `model1.*`, `model2.*` | `assets/characters/<model>/` |
| `Android/app/src/main/assets/model0.fbx`, `Mma Idle.fbx` | `assets/characters/model0/source/` |
| `tools/jfa_convert.c`, `jfa_compare.py`, `validate_jfa.sh` | `tools/asset-pipeline/` |
| `Android/app/jni/ufbx/` | `third_party/ufbx/` (+ `tools/asset-pipeline/inspect_fbx.c`) |
| `tools/jfa_contact_analysis.c` | `migration-review/` |
