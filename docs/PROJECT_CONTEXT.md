# JOJO: The Last Stand — Project Context

> **Purpose:** Durable handoff document for future contributors and AI assistants.
> Read this file together with the root `README.md` and `docs/REBUILD_PLAN.md`
> before proposing or making project changes. Keep it updated when decisions,
> repository state, or workflow rules change.

## 1. Project identity

- **Current project name:** JOJO: The Last Stand.
- **Former name:** JoJo Fighters (used in historical commits, tags, and some existing documentation).
- **GitHub repository:** `djdz363-vro/JojoFighter`.
- **Active rebuild branch:** `restart/jojo-fighters`.
- Use **JOJO: The Last Stand** in new documentation, discussion, and user-facing project material. Do not rename the GitHub repository unless the owner explicitly requests it.

## 2. Current repository state

This is a clean restart, not a continuation of the old playable prototype.

- The active branch contains no game implementation, selected engine, selected programming language, build system, or confirmed target platform.
- The branch preserves reusable character assets, animation data, asset-processing tools, licenses, and reference documentation.
- The root `README.md` and `docs/REBUILD_PLAN.md` describe the repository layout and rebuild starting point.
- `docs/architecture/README.md` confirms that architecture decisions have not yet been made.
- Do not assume that a previous prototype's technology choices automatically apply to the remake.

### Preserved material

- Character meshes and textures, FBX source files, and baked JFA1 animation clips.
- Standalone FBX-to-JFA1 asset pipeline and validation/comparison tools under `tools/asset-pipeline/`.
- Vendored `ufbx` under `third_party/ufbx/` for the asset tools.
- Legacy format and gameplay reference documentation under `docs/legacy/`.
- Third-party notices and attribution in `THIRD_PARTY_NOTICES.md`.
- Uncertain-reuse files in `migration-review/`.

The rebuild plan records three character meshes with textures, two FBX sources, and ten baked animation clips for model0. Nine of the ten clips do not have FBX sources in this repository; if JFA1 is abandoned, their original Mixamo sources would need to be obtained again.

## 3. Historical implementations (reference only)

The previous implementations remain recoverable from Git history and archive tags. They are not the active implementation.

- **PSP build:** historical `main` implementation, approximately 3,600 lines of C using pspgu/pspgum and libpng, built with PSPSDK.
- **Android prototype:** SDL2 + OpenGL ES in C, using ndk-build/Gradle. It reached milestones covering JFA1, loading, CPU/GPU skinning, movement, attacks, two fighters, hit detection, hit reactions, and measured contact windows.
- Root README lists the archive tags:
  - `archive/pre-rebuild-2026-11-25`
  - `archive/android-prototype-2026-11-25`
  - `archive/freebuff-contact-windows-2026-11-25`

Use these implementations as reference material when useful, not as authority for new architecture. The old PSP code and Android prototype should not be reintroduced wholesale without an explicit design decision.

## 4. Decisions still open

The following decisions are not settled. Do not silently choose them or begin gameplay implementation before documenting the relevant decisions.

1. **Target platforms:** for example Android, PC, PSP, or web.
2. **Engine/framework and language:** select based on target platforms, project scope, maintainability, and available tooling.
3. **Animation/runtime asset pipeline:** decide whether JFA1 remains the runtime format, or whether FBX sources are imported/re-exported to another format such as glTF. Account for the missing source FBX files for nine clips.
4. **Character scale and rigging:** the rebuild plan notes model2 is in centimetres while model0/model1 are in metres; determine a consistent convention and whether model1/model2 will be rigged.
5. **Combat architecture:** frame data, hitbox/hurtbox representation, input buffering, attack priority, hit reactions, knockdown, and other combat rules.
6. **Audio, stage, UI, and fonts:** no such new-project assets are currently present.
7. **Licensing:** determine project licensing and confirm permissible use/redistribution of character assets. Consult `THIRD_PARTY_NOTICES.md`.

Record consequential choices as short architecture decision records in `docs/architecture/`, using the suggested `NNNN-title.md` format: context, options considered, decision, and consequences.

## 5. Suggested development sequence

Follow the rebuild plan unless the owner explicitly changes priorities:

1. Decide target platforms, engine/language, and animation asset strategy; document the decisions.
2. Add the chosen engine/toolchain and establish a minimal empty project that builds.
3. Import preserved assets and verify that one character renders and plays every available clip.
4. Implement gameplay incrementally: movement, attacks, then hit detection. Use the measured contact data in `docs/legacy/phase0-contact-measurements.txt` as reference, not as unquestionable final design.
5. Review and resolve the files in `migration-review/`.

Do not start with unrelated features or broaden scope before the foundation is verified.

## 6. Asset pipeline and validation

The root README documents the current validation command:

```sh
sh tools/asset-pipeline/validate_jfa.sh --tol 1e-3
```

It requires a C99 compiler and Python 3. It rebuilds the converter, reports preserved clips, re-bakes the idle clip from its FBX source, and compares it with the original. Generated output is placed in ignored `build/`.

Preserve source assets and validation tooling. Do not overwrite or discard original animation data as part of an experiment.

## 7. Working and Git safety rules

These rules reflect the project owner's explicit workflow preferences:

- Explain the proposed change and its scope before taking consequential action.
- Prefer small, understandable steps; the owner prefers a one-step-at-a-time workflow.
- Inspect the current branch, working tree, and relevant files before editing.
- Keep experiments isolated where practical; do not mix unrelated changes.
- Run the relevant tests/validation and report their actual results.
- Never claim that a build, test, commit, push, or file creation succeeded unless the tool or repository confirms it.
- Do not force-push, rewrite history, or overwrite user work.
- Do not commit or push without the owner's explicit authorization for that work. If the owner explicitly asks for a commit and push, do so only when repository permissions and verification allow it.
- If authentication or integration permissions prevent a write, stop and report the exact error. Do not imply that a change was made.
- Do not add an APK delivery step unless the owner specifically asks for an APK. The preferred delivery path for authorized repository work is the repository commit/push workflow, not an unnecessary extra APK handoff.
- Preserve the two historical Android C source copies byte-identically only if that old Android implementation is deliberately revisited; they are not part of the current clean rebuild.

## 8. Explicitly withdrawn request

A previous request to make attack speed and animation 1.5× faster was withdrawn. It is **not** a standing requirement and must not influence future design or implementation recommendations unless the owner explicitly raises it again.

## 9. Documentation maintenance

When project decisions or implementation state change:

- Update this file and the relevant detailed document in the same authorized change.
- Keep current facts separate from historical prototype facts and proposed ideas.
- Record unresolved questions as unresolved; do not convert assumptions into decisions.
- Prefer linking to detailed documentation rather than duplicating long specifications here.

**Source-of-truth order:** current repository files and documented decisions on the active branch first; this context document for continuity and workflow; archived implementations only as historical reference. If sources conflict, inspect the current branch and ask the owner before making a consequential assumption.
