# Migration review

Files here were kept because they may be useful but cannot be used as-is.
They are **not** part of the active project and are not built by anything.

| File | Origin | Why it needs review |
|------|--------|---------------------|
| `jfa_contact_analysis.c` | `tools/jfa_contact_analysis.c` on `freebuff/58d654f…` (tag `archive/freebuff-contact-windows-2026-11-25`) | Produced [docs/legacy/phase0-contact-measurements.txt](../docs/legacy/phase0-contact-measurements.txt). It measures attack contact frames from the JFA clips, which is useful for designing frame data. It does not compile on this branch because it includes the old runtime's `jfa_loader.h` and `jfa_skin.h`/`jfa_skin.c`. Options: rewrite it against the new engine's animation loader, make it standalone in the asset pipeline, or delete it once the measurements are reproduced. |

To rebuild it exactly as before, check out the archive tag and follow the build
command at the top of the measurement file.
