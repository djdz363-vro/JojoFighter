# JoJo Fighters — Clean Restart

This branch (`restart/jojo-fighters`) is the clean starting point for a complete
rebuild of JoJo Fighters. The previous implementation (a PSP homebrew build and
an SDL2/OpenGL ES Android prototype written in C) has been removed from this
branch. Only reusable assets, asset-processing tools, licenses and reference
documentation were kept.

**There is no game code in this branch yet.** No engine, language or platform
has been chosen for the remake. Those decisions will be made and documented in
[`docs/architecture/`](docs/architecture/) before any gameplay code is written.

## Repository layout

| Path | Contents |
|------|----------|
| [`assets/`](assets/) | Preserved original character models, textures, FBX sources and baked animations |
| [`tools/asset-pipeline/`](tools/asset-pipeline/) | Standalone FBX → JFA1 converter, JFA comparison/validation scripts, FBX inspector |
| [`third_party/ufbx/`](third_party/ufbx/) | Vendored ufbx FBX loader (used only by the asset tools) |
| [`docs/`](docs/) | New documentation: asset inventory, rebuild plan, architecture notes |
| [`docs/legacy/`](docs/legacy/) | Reference material from the old prototype (JFA1 format spec, measured contact frames, changelog, bug log) |
| [`migration-review/`](migration-review/) | Files kept for review because their reuse value is uncertain |
| [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | Third-party licenses and asset attribution notes |

## Recovering the old implementation

Nothing was deleted from Git history. The old code is available through these tags:

| Tag | Points to |
|-----|-----------|
| `archive/pre-rebuild-2026-11-25` | `main` exactly as it was before the restart (PSP build, `eeb4482`) |
| `archive/android-prototype-2026-11-25` | Latest Android prototype + docs (`4e2a32e`, M3.3 Stage 3b-B hit reaction) |
| `archive/freebuff-contact-windows-2026-11-25` | Android prototype variant with measured contact windows (`37850da`) |

```sh
git fetch --tags
git switch --detach archive/pre-rebuild-2026-11-25
```

## Working with the preserved assets

The only runnable tooling in this branch is the asset pipeline. It needs a C99
compiler and Python 3:

```sh
sh tools/asset-pipeline/validate_jfa.sh --tol 1e-3
```

This rebuilds the JFA converter, prints information for every preserved
animation clip, re-bakes the idle clip from its FBX source and compares it with
the original file. Output goes to `build/` (ignored by Git).

## Next steps

See [`docs/REBUILD_PLAN.md`](docs/REBUILD_PLAN.md).
