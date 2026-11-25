# JOJO: The Last Stand — Rebuild plan

**Project identity:** JOJO: The Last Stand (formerly JoJo Fighters). The GitHub repository name and historical references remain unchanged unless the owner explicitly requests otherwise.

## Starting point (2026-11-25)

- Active rebuild branch: `restart/jojo-fighters`, created from the former `main` history (`eeb4482`).
- Confirmed platform order: **Android first, PC later**.
- No game code, engine, programming language or build system is present on the active rebuild branch.
- Preserved: 3 character meshes with textures, 2 FBX sources and 10 baked animation clips for model0 ([ASSET_INVENTORY.md](ASSET_INVENTORY.md)), a working FBX → JFA1 asset pipeline, and the legacy reference docs in [legacy/](legacy/).
- Historical implementations are recoverable from the `archive/*` tags (see the root README).

## Historical implementations (reference only)

1. **PSP implementation** (former `main`): one ~3,600-line `main.c` using pspgu/pspgum and libpng, built with the PSPSDK Makefile and a PSP CI workflow.
2. **Android prototype** (former `android-prototype` and `freebuff/…` branches): SDL2 + OpenGL ES in C via ndk-build/Gradle. Milestones M1 (JFA1 format and converter), M2 (loader, CPU and GPU skinning), M3.1–M3.3 (movement, attacks, two fighters, hit detection, hit reaction, measured contact windows). See [legacy/CHANGELOG.md](legacy/CHANGELOG.md).

Lessons worth carrying over are recorded in [legacy/BUGS_AND_ERRORS.md](legacy/BUGS_AND_ERRORS.md).

## Open decisions

These must be decided and recorded in [architecture/](architecture/) before writing game code:

1. Engine or framework, and implementation language, selected for Android-first development with PC later.
2. Whether the JFA1 format stays as the runtime animation format, or the FBX sources are imported directly / re-exported to another format (glTF, …). Nine of the ten clips have no FBX source in the repository; their original Mixamo sources would need to be re-downloaded if JFA1 is dropped.
3. Scale convention (model2 is in centimetres, model0/model1 in metres) and whether model1/model2 will be rigged.
4. Combat model: frame data, hitbox/hurtbox representation, input buffering. [legacy/phase0-contact-measurements.txt](legacy/phase0-contact-measurements.txt) gives measured active frames and reach for the existing attack clips.
5. Source of audio, stage, UI and font assets (none exist yet).
6. Licensing of the project itself and of the character assets (see [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).

## Suggested order of work

1. Settle the engine/framework, language and animation asset strategy; write architecture decision records.
2. Add the chosen engine/toolchain and a minimal empty project that builds for Android.
3. Import the preserved assets into the new pipeline and verify one character renders and plays every available clip.
4. Only then start gameplay: movement, then attacks, then hit detection, using the measured contact data as a starting point.
5. Resolve the items in [../migration-review/](../migration-review/).