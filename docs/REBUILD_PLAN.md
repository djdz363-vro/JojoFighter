# JOJO: The Last Stand — Rebuild plan

**Project identity:** JOJO: The Last Stand (formerly JoJo Fighters). The GitHub repository name and historical references remain unchanged unless the owner explicitly requests otherwise.

## Starting point (2026-11-25)

- Active rebuild branch: `restart/jojo-fighters`, created from the former `main` history (`eeb4482`).
- Confirmed platform order: **Android first, PC later**.
- Development workflow preference: develop using both an Android phone and a laptop, with the same project synchronized through GitHub.
- No game code, engine, programming language or build system is present on the active rebuild branch.
- Preserved: 3 character meshes with textures, 2 FBX sources and 10 baked animation clips for model0 ([ASSET_INVENTORY.md](ASSET_INVENTORY.md)), a working FBX → JFA1 asset pipeline, and the legacy reference docs in [legacy/](legacy/).
- Historical implementations are recoverable from the `archive/*` tags (see the root README).

## Platform and development workflow

### Confirmed decisions

1. **Android is the primary platform.** PC is planned for a later phase.
2. **Development should be possible on both the user's Android phone and laptop.** Use one canonical GitHub repository for version control and synchronization.
3. The project should remain practical to build, test and maintain across both devices. Keep engine/editor versions and project settings consistent.

### Proposed, not yet approved

- **Engine:** Godot 4 is the current candidate.
- **Language:** GDScript is the current candidate.
- **Rendering:** Godot Compatibility renderer is a candidate to test for older hardware and broad device support; final choice depends on actual tests.
- **Workflow:** Use the Godot Android Editor for suitable phone-based tasks and the desktop Godot editor on the laptop for larger or more demanding tasks.
- **Learning:** The user has no prior Godot experience. Provide step-by-step guidance and explain necessary editor workflows; do not assume the user can independently navigate or program in Godot.

### Compatibility gate before engine approval

The user's laptop was previously reported as Windows 8.1 (64-bit), Intel Core i3-4005U, with no dedicated GPU. This environment must be rechecked because editor and renderer requirements vary by Godot version.

Before selecting a specific Godot 4 release or committing to the engine:

1. Verify which Godot 4 desktop editor version can run reliably on the actual laptop and operating system.
2. Install and open the corresponding Godot Android Editor on the phone, and verify its practical usability for this project.
3. Create a minimal 3D test project and confirm it opens and runs on both devices.
4. Verify Android export and installation on the phone.
5. Confirm the same project can be synchronized safely through GitHub without conflicting edits or machine-specific generated files.

If either device cannot support the chosen workflow, reassess the version or workflow before production begins. Do not treat Godot 4 + GDScript as a final decision until this gate passes and the user confirms it.

## Historical implementations (reference only)

1. **PSP implementation** (former `main`): one ~3,600-line `main.c` using pspgu/pspgum and libpng, built with the PSPSDK Makefile and a PSP CI workflow.
2. **Android prototype** (former `android-prototype` and `freebuff/…` branches): SDL2 + OpenGL ES in C via ndk-build/Gradle. Milestones M1 (JFA1 format and converter), M2 (loader, CPU and GPU skinning), M3.1–M3.3 (movement, attacks, two fighters, hit detection, hit reaction, measured contact windows). See [legacy/CHANGELOG.md](legacy/CHANGELOG.md).

Lessons worth carrying over are recorded in [legacy/BUGS_AND_ERRORS.md](legacy/BUGS_AND_ERRORS.md).

## Open decisions

These must be decided and recorded in [architecture/](architecture/) before writing game code:

1. Engine or framework and implementation language. Godot 4 + GDScript is under evaluation, not yet approved.
2. Exact engine/editor version and renderer, after the phone-and-laptop compatibility gate.
3. Whether the JFA1 format stays as the runtime animation format, or the FBX sources are imported directly / re-exported to another format (glTF, …). Nine of the ten clips have no FBX source in the repository; their original Mixamo sources would need to be re-downloaded if JFA1 is dropped.
4. Scale convention (model2 is in centimetres, model0/model1 in metres) and whether model1/model2 will be rigged.
5. Combat model: frame data, hitbox/hurtbox representation, input buffering. [legacy/phase0-contact-measurements.txt](legacy/phase0-contact-measurements.txt) gives measured active frames and reach for the existing attack clips.
6. Source of audio, stage, UI and font assets (none exist yet).
7. Licensing of the project itself and of the character assets (see [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).
8. Practical GitHub synchronization workflow for editing on both devices, including conflict avoidance and generated-file exclusions.

## Suggested order of work

1. Settle the engine/framework, language, exact version, renderer and animation asset strategy; write architecture decision records.
2. Pass the phone-and-laptop compatibility gate with a minimal project that builds and runs on Android.
3. Establish a safe GitHub workflow for synchronizing the project between both devices.
4. Import the preserved assets into the new pipeline and verify one character renders and plays every available clip.
5. Only then start gameplay: movement, then attacks, then hit detection, using the measured contact data as a starting point.
6. Resolve the items in [../migration-review/](../migration-review/).
