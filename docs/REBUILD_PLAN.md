# Rebuild plan

## Starting point (2026-11-25)

- Branch `restart/jojo-fighters`, created from `main` (`eeb4482`).
- No game code, engine, build system or target platform is present.
- Preserved: 3 character meshes with textures, 2 FBX sources and 10 baked
  animation clips for model0 ([ASSET_INVENTORY.md](ASSET_INVENTORY.md)),
  a working FBX → JFA1 asset pipeline, and the legacy reference docs in
  [legacy/](legacy/).
- The old code is recoverable from the `archive/*` tags (see the root README).

## What the old project was (for context only)

1. **PSP build** (`main`): one ~3,600-line `main.c` using pspgu/pspgum and
   libpng, built with the PSPSDK `Makefile` and a pspdev CI workflow.
2. **Android prototype** (`android-prototype`, `freebuff/…`): SDL2 + OpenGL ES
   in C via ndk-build/Gradle. Milestones M1 (JFA1 format and converter),
   M2 (loader, CPU and GPU skinning), M3.1–M3.3 (movement, attacks, two
   fighters, hit detection, hit reaction, measured contact windows).
   See [legacy/CHANGELOG.md](legacy/CHANGELOG.md).

Lessons worth carrying over are recorded in
[legacy/BUGS_AND_ERRORS.md](legacy/BUGS_AND_ERRORS.md).

## Open decisions

These must be decided and recorded in [architecture/](architecture/) before
writing game code:

1. Target platforms (Android, PC, PSP, web, …).
2. Engine or framework, and implementation language.
3. Whether the JFA1 format stays as the runtime animation format, or the FBX
   sources are imported directly / re-exported to another format (glTF, …).
   Nine of the ten clips have no FBX source in the repository; their original
   Mixamo sources would need to be re-downloaded if JFA1 is dropped.
4. Scale convention (model2 is in centimetres, model0/model1 in metres) and
   whether model1/model2 will be rigged.
5. Combat model: frame data, hitbox/hurtbox representation, input buffering.
   [legacy/phase0-contact-measurements.txt](legacy/phase0-contact-measurements.txt)
   gives measured active frames and reach for the existing attack clips.
6. Source of audio, stage, UI and font assets (none exist yet).
7. Licensing of the project itself and of the character assets
   (see [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).

## Suggested order of work

1. Settle decisions 1–3 and write the first architecture records.
2. Add the chosen engine/toolchain and a minimal empty project that builds.
3. Import the preserved assets into the new pipeline and verify one character
   renders and plays every clip.
4. Only then start gameplay: movement, then attacks, then hit detection, using
   the measured contact data as the starting point.
5. Resolve the items in [../migration-review/](../migration-review/).
