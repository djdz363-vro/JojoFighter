# JOJO: The Last Stand — Clean Rebuild

**JOJO: The Last Stand** is the official current name of this project. The repository and historical commits still use the former name, **JoJo Fighters**.

This branch (`restart/jojo-fighters`) is the clean starting point for a complete rebuild. The previous implementations (a PSP homebrew build and an SDL2/OpenGL ES Android prototype written in C) have been removed from this branch. Only reusable assets, asset-processing tools, licenses and reference documentation were kept.

**There is no game code in this branch yet.** The confirmed platform order is **Android first, PC later**. The engine/framework, programming language and animation pipeline are not yet finalized; those decisions must be agreed and documented in [`docs/architecture/`](docs/architecture/) before gameplay code is written. The durable project handoff is [`docs/PROJECT_CONTEXT.md`](docs/PROJECT_CONTEXT.md).

## Repository layout

| Path | Contents |
|------|----------|
| [`assets/`](assets/) | Preserved original character models, textures, FBX sources and baked animations |
| [`tools/asset-pipeline/`](tools/asset-pipeline/) | Standalone FBX → JFA1 converter, JFA comparison/validation scripts, FBX inspector |
| [`third_party/ufbx/`](third_party/ufbx/) | Vendored ufbx FBX loader (used only by the asset tools) |
| [`docs/`](docs/) | Project context, asset inventory, rebuild plan, CI and architecture notes |
| [`docs/legacy/`](docs/legacy/) | Historical reference material from the former JoJo Fighters prototype |
| [`migration-review/`](migration-review/) | Files kept for review because their reuse value is uncertain |
| [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | Third-party licenses and asset attribution notes |

## Recovering the historical implementations

Nothing was deleted from Git history. The old code is available through these archive tags:

| Tag | Preserved history |
|-----|-------------------|
| `archive/pre-rebuild-2026-11-25` | Former default branch before the rebuild (PSP implementation, `eeb4482`) |
| `archive/android-prototype-2026-11-25` | Android prototype and documentation (`4e2a32e`, M3.3 Stage 3b-B hit reaction) |
| `archive/freebuff-contact-windows-2026-11-25` | Android prototype variant with measured contact windows (`37850da`) |

```sh
git fetch --tags
git switch --detach archive/pre-rebuild-2026-11-25
```

These tags are the preservation points. Historical branch names are not required to recover the archived commits.

## Validating the preserved assets

The only runnable tooling in this branch is the asset pipeline. It needs a C99 compiler and Python 3:

```sh
sh tools/asset-pipeline/validate_jfa.sh --tol 1e-3
```

This rebuilds the JFA converter, prints information for every preserved animation clip, re-bakes the idle clip from its FBX source and compares it with the original file. Output goes to ignored `build/`.

## Next steps

See [`docs/REBUILD_PLAN.md`](docs/REBUILD_PLAN.md). Do not begin gameplay implementation until the engine/language and animation pipeline decisions are agreed and recorded.