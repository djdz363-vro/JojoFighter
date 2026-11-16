# JFA1 — JojoFighter Animation format

JFA1 is the binary skeletal-animation format consumed by the JFA loader in the
game (see the `JFA1` section of the runtime loader). One `.jfa` file contains
**one mesh, one skeleton, and one animation clip** (a "clip").

All integers and floats are **little-endian**. Floats are 32-bit (`float`).

## File layout

```
offset  size  field
0       4     magic           "JFA1" (bytes 0x4A 0x46 0x41 0x31)
4       4     version         uint32, must be 1
8       4     nb              uint32, bone count          (runtime limit: 1..64)
12      4     nv              uint32, vertex count        (runtime limit: 1..12000)
16      4     ni              uint32, index count         (runtime limit: 1..36000, <= 65535 per index)
20      4     nf              uint32, frame count         (runtime limit: 1..240)
24      4     fps             uint32, sample rate         (must be nonzero)
28      4     flags           uint32, semantics unknown; always 1 in existing files
```

After the header follow, in order:

1. **Bone table** (`nb` entries)
2. **Vertex data** (`nv` entries of 64 bytes)
3. **Index data** (`ni` × uint32)
4. **Animation matrices** (`nf × nb` matrices of 16 floats = 64 bytes each)

The runtime loader additionally requires that every non-idle clip of a
character has a bone table **byte-identical** to the character's `idle` clip
(same bone order, same names, same parent indices) and the same `nb/nv/ni`.

## Bone table

Per bone:

```
int32   parent    index of the parent bone, -1 for the root
uint32  nameLen   name length in bytes (1..47)
char[]  name      UTF-8, not NUL-terminated in the file
```

Bones are stored in **depth-first preorder** starting from the root bone
(Mixamo rigs: `mixamorig:Hips` first, arms before legs). The converter
enumerates the skeleton this way, and the runtime relies on stable bone order
across a character's clips.

## Vertex data (64 bytes per vertex)

```
float p[3]      bind-pose position (meters, Y-up)
float n[3]      bind-pose normal
float uv[2]     raw FBX UV (the runtime loader flips V itself: v' = 1 - v)
uint32 bi[4]    bone indices (unused slots: 0)
float  bw[4]    bone weights (unused slots: 0.0)
```

Conventions (matching the original files):

- The mesh is stored as a **triangle soup in FBX face order**, and the index
  buffer is the **identity sequence** `0..nv-1`. Therefore `nv` is always
  `triangle count * 3` and `nv == ni`.
- **Normals are flat per-face normals**, computed as
  `normalize(cross(b - a, c - a))` from the baked triangle positions; the
  FBX's own normals are *not* stored. All 3 corners of a triangle share the
  same normal. (The runtime lights with these static bind-pose normals.)
- Weights are the top-4 influences sorted by **decreasing weight** and
  **normalized to sum to 1** (the runtime does not renormalize). Vertices
  influenced by fewer than 4 bones are padded with `bi=0, bw=0`.

## Animation matrices

`nf × nb` row-major 4×4 matrices, stored frame-major (all bones of frame 0,
then frame 1, ...). Each matrix is the **global skinning matrix** for that
bone in that frame:

```
world-space transform of the bone  x  inverse bind transform
```

so that `posed = M x bind_pose_vertex` directly. The bottom row of each
matrix is `(0, 0, 0, 1)`; the runtime only reads the first 12 floats.

## Provenance & reconstruction

The original converter was never committed; the existing `model0_*.jfa` files
were uploaded pre-baked. The format was reconstructed by inspecting the files
and validated numerically: re-baking `Android/app/src/main/assets/Mma Idle.fbx`
(the original source export of character 0's idle, 5.0 s at 30 fps = 151
frames) with `tools/jfa_convert.c` reproduces the original `model0_idle.jfa`
**structurally byte-identically** (header, bone table, UVs, index buffer, file
size) with float differences bounded by:

| section          | max abs difference | note                                              |
|------------------|--------------------|---------------------------------------------------|
| vertex positions | 1.2e-07            | available FBX is a re-export of the original source |
| vertex normals   | 5.7e-07            | derived from positions                            |
| vertex weights   | 1.2e-07            | source re-export noise                            |
| matrices         | 2.8e-05            | accumulated evaluation noise                      |
| **posed mesh**   | **2.1e-05 m**      | max per-vertex distance over all 151 frames       |

i.e. the re-bake is visually identical (2.1e-05 m ≈ 21 µm on a ~1.7 m
character). Byte-identical output is impossible without the *original* source
FBX, because the FBX currently in `assets/` differs from it at the 1e-07
level. Run the validation yourself:

```sh
sh tools/validate_jfa.sh            # strict (reports differences, exit 1)
sh tools/validate_jfa.sh --tol 1e-3 # pass if all float deltas <= 0.001
```

## Tool usage

```sh
# build (from repo root)
cc -std=c99 -O2 -I Android/app/jni/ufbx \
    tools/jfa_convert.c Android/app/jni/ufbx/ufbx.c -lm -o build/jfa_convert

# inspect / validate any JFA file (strict structural checks, exit 1 on error)
build/jfa_convert --info model0_idle.jfa

# bake an animated skinned FBX into a JFA clip
build/jfa_convert --bake <input.fbx> --fps 30 -o model0_newclip.jfa

# compare two files section by section (quantified differences)
python3 tools/jfa_compare.py original.jfa baked.jfa [--tol 1e-3]
```

`--bake` enforces all runtime loader limits (64 bones, 12000 vertices,
240 frames, indices <= 65535, bone names < 48 bytes) and rejects multi-root
skeletons, unskinned meshes, static FBXs, and out-of-range data with explicit
errors.

## Producing JFA clips for characters 1 and 2

1. Rig the character (mesh sources: `model1.obj`, `model2.obj` — these carry
   no skeleton, so the rig must come from Blender/Mixamo) and export/download
   one **animated, skinned FBX per clip** from Mixamo.
2. Ensure the skeleton uses the same bone naming (`mixamorig:*`) and that the
   *idle* clip is generated first: it defines the character's skeleton.
3. Bake every clip with the same rig and the same fps:
   `jfa_convert --bake model1_idle.fbx --fps 30 -o model1_idle.jfa`, etc.
4. Validate the set: every clip must pass `--info`, and all clips of a
   character must share the identical bone table (`jfa_compare.py` on two
   clips' bone tables, or the loader will refuse them at runtime).
5. Character textures are the existing `model1.png` / `model2.png`.
