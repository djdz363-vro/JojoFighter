# Continuous integration

## Active workflow on this branch

| Workflow | File | Trigger |
|----------|------|---------|
| Validate JFA Assets | `.github/workflows/validate-assets.yml` | Branch pushes touching `assets/characters/**`, `tools/asset-pipeline/**`, `third_party/ufbx/**` or the workflow file itself; manual run |

It builds the JFA converter, checks every preserved clip and re-bakes the idle
clip. It does not build a game and uploads no artifacts. Tag pushes never
trigger it.

## Legacy workflows (old implementation)

The old PSP and Android workflows still exist in the archived commits
(`main`, `android-prototype`, `freebuff/…` and the `archive/*` tags). GitHub
runs a push-triggered workflow using the file from the pushed commit, and
these files used a bare `on: push:` with no branch or tag filter. As a result,
pushing any tag that points at an old commit (for example the `archive/*`
safety tags on 2026-11-25) started full PSP and APK builds.

Because archived commits must not be modified, these workflows are
**disabled at the repository level** (Actions → workflow → "Disable workflow"),
which applies to every branch and tag:

| Workflow | File | State | Why |
|----------|------|-------|-----|
| Build PSP EBOOT | `.github/workflows/build.yml` | disabled | Builds the old PSP game on any push, including tags |
| Build Android APK | `.github/workflows/android.yml` | disabled | Builds the old Android APK on any push, including tags |
| Validate JFA Assets (legacy) | `.github/workflows/validate-jfa.yml` | disabled | Old-layout asset check, replaced by `validate-assets.yml`; also ran on tag pushes |
| Build Android GPU Variant APKs | `.github/workflows/android-variants.yml` | active | Manual (`workflow_dispatch`) only; cannot run from a push or tag |
| Inspect FBX Models | `.github/workflows/inspect-fbx.yml` | active | Manual only; cannot run from a push or tag |

To build the old game deliberately, re-enable the workflow, run it manually
on the archived ref, then disable it again:

```sh
gh workflow enable build.yml        # or android.yml
gh workflow run build.yml --ref archive/pre-rebuild-2026-11-25
gh workflow disable build.yml
```

Note: while a legacy workflow is enabled, pushing to an old branch or tag will
also start it.

## Rules for new workflows

- Always restrict `push` triggers with `branches:` (and `paths:` where useful)
  so that tag pushes do not start builds by accident.
- Release builds, if added later, should use an explicit tag pattern
  (for example `tags: ['v*']`), never a bare `on: push:`.
