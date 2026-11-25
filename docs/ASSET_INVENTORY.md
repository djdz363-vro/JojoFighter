# Asset inventory

All files below were copied byte-for-byte from the old branches (Git blob
hashes are unchanged). "Origin" is the path in the archive tags.

## Characters

### model0 (main fighter, Mixamo-rigged)

| File | Origin | Notes |
|------|--------|-------|
| `assets/characters/model0/model0.obj` | `main:model0.obj` | Static mesh split into `head`, `torso`, `arm_l`, `arm_r`, `leg_l`, `leg_r` objects; +Z front, Y up; 2,824 vertices |
| `assets/characters/model0/model0.png` | `main:model0.png` | 1024×512 texture |
| `assets/characters/model0/source/model0.fbx` | `android-prototype:Android/app/src/main/assets/model0.fbx` | Skinned FBX source of the character |
| `assets/characters/model0/source/Mma Idle.fbx` | `android-prototype:Android/app/src/main/assets/Mma Idle.fbx` | Mixamo "Mma Idle" export; re-bakes `model0_idle.jfa` within 1e-3 tolerance (verified 2026-11-25) |

Baked skeletal animations (JFA1 format, see [legacy/JFA1.md](legacy/JFA1.md)).
All clips share the same 33-bone Mixamo skeleton (`mixamorig:*`) and the same
8,952-vertex triangle-soup mesh.

| Clip | Frames | FPS | Duration |
|------|-------:|----:|---------:|
| `model0_idle.jfa` | 151 | 30 | 5.00 s |
| `model0_walk.jfa` | 61 | 60 | 1.00 s |
| `model0_backwalk.jfa` | 27 | 30 | 0.87 s |
| `model0_guard.jfa` | 80 | 60 | 1.32 s |
| `model0_punch_left.jfa` | 53 | 30 | 1.73 s |
| `model0_punch_right.jfa` | 91 | 30 | 3.00 s |
| `model0_kick.jfa` | 70 | 30 | 2.30 s |
| `model0_hit.jfa` | 43 | 30 | 1.40 s |
| `model0_knockdown.jfa` | 87 | 30 | 2.87 s |
| `model0_getup.jfa` | 77 | 30 | 2.53 s |

Origin: root of `main` (identical copies also lived in
`Android/app/src/main/assets/animations/` on the Android branches).
An earlier 176-frame / 60 fps version of `model0_guard.jfa` (added in `20fa200`,
replaced three hours later in `d2e04f1`) was superseded and is not copied here;
it remains reachable through the archive tags.

Only the idle clip has its FBX source in the repository; the other nine clips
exist only in baked JFA1 form.

### model1

| File | Origin | Notes |
|------|--------|-------|
| `assets/characters/model1/model1.obj` | `main:model1.obj` | Static mesh, same 6-part split; 2,252 vertices |
| `assets/characters/model1/model1.png` | `main:model1.png` | 1024×512 texture |

### model2

| File | Origin | Notes |
|------|--------|-------|
| `assets/characters/model2/model2.obj` | `main:model2.obj` | Static mesh, same 6-part split; 1,520 vertices. Units are centimetres (≈160 at head height), unlike model0/model1 (metres) |
| `assets/characters/model2/model2.png` | `main:model2.png` | 1024×512 texture |

model1 and model2 have no rig, no FBX source and no animations in the repository.

## Categories with no assets in the repository

Searched across every branch: no audio (sound effects, music, voice), no stage
or background art, no fonts, and no UI graphics exist. The old games drew
stages and UI procedurally in code. These must be sourced or created for the remake.

## Tools

| File | Purpose |
|------|---------|
| `tools/asset-pipeline/jfa_convert.c` | `--info` validates/prints a JFA file; `--bake` converts an animated, skinned FBX into JFA1 |
| `tools/asset-pipeline/jfa_compare.py` | Section-by-section comparison of two JFA1 files |
| `tools/asset-pipeline/validate_jfa.sh` | Builds the converter, checks every clip, re-bakes idle and compares |
| `tools/asset-pipeline/inspect_fbx.c` | Prints animation duration and sample skinned vertices of `Mma Idle.fbx` |
| `third_party/ufbx/` | ufbx FBX loader (MIT / public domain), used by the tools above |
| `.github/workflows/validate-assets.yml` | Runs `validate_jfa.sh` in CI on branch pushes that change assets or tools (see [CI.md](CI.md)) |

## Intentionally not kept

| Item | Reason |
|------|--------|
| `Android/app/src/main/assets/model0.bmp` | Pixel-identical to `model0.png` (verified); generated conversion |
| `Android/app/src/main/res/mipmap-*/ic_launcher.png` | Byte-identical to SDL2's default template icons, not original art |
| Duplicate copies of `model0.obj/png` and the JFA clips under `Android/app/src/main/assets/` | Same blobs as the root copies already preserved |
| `Android/app/jni/SDL/` (≈1,800 files) | Vendored SDL2 library for the old Android build |
| SDL Android Java glue, Gradle wrapper and build files | Old Android build only |
