# Third-party notices

## ufbx

`third_party/ufbx/` — FBX loader by Samuli Raivio, dual-licensed under the
MIT License or public domain (Unlicense). Full text:
[third_party/ufbx/LICENSE](third_party/ufbx/LICENSE). Used only by the asset
tools in `tools/asset-pipeline/`.

## Character assets

- The model0 skeleton uses Mixamo bone names (`mixamorig:*`), and
  `assets/characters/model0/source/Mma Idle.fbx` is a Mixamo animation export.
  Mixamo content is provided by Adobe under the Adobe/Mixamo terms of use;
  confirm those terms cover your intended distribution.
- The origin and authorship of `model0`, `model1` and `model2` meshes and
  textures are not recorded anywhere in the repository history (they were added
  through "Add files via upload" commits). **Their license must be confirmed by
  the project owner.**
- JoJo's Bizarre Adventure is the property of Hirohiko Araki / Shueisha.
  This is a fan project; any use of related characters or names is subject to
  the rights holders.

## Removed with the old implementation

SDL2 (zlib license) was vendored under `Android/app/jni/SDL/` in the Android
prototype. It is no longer part of this branch; its license remains in the
archive tags.

## Project license

No license file existed in the original repository and none has been added.
Choosing one is an open decision ([docs/REBUILD_PLAN.md](docs/REBUILD_PLAN.md)).
