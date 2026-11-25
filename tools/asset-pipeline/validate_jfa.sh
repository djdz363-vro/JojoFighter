#!/bin/sh
# validate_jfa.sh - build tools/asset-pipeline/jfa_convert and validate the re-bake of
# model0_idle.jfa against the original file.
#
# The re-baked file is written to build/jfa/ (never over the original).
#
# Usage: sh tools/asset-pipeline/validate_jfa.sh [--tol E]
#   --tol E is forwarded to tools/asset-pipeline/jfa_compare.py as an allowed maximum
#   float difference (default: require byte-identical output).

set -e
cd "$(dirname "$0")/../.."

OUT=build/jfa
mkdir -p "$OUT"

echo "== building tools/asset-pipeline/jfa_convert =="
${CC:-cc} -std=c99 -O2 -I third_party/ufbx \
	tools/asset-pipeline/jfa_convert.c third_party/ufbx/ufbx.c -lm -o "$OUT/jfa_convert"

echo
echo "== --info on all existing JFA files =="
for f in assets/characters/model0/animations/model0_*.jfa; do
	"$OUT/jfa_convert" --info "$f" | tail -3
done

echo
echo "== re-baking idle animation =="
# The original model0_idle.jfa was baked from a Mixamo export of the same
# character at 30 fps (151 frames for the 5.0 s idle). "Mma Idle.fbx" in
# assets/characters/model0/source/ is that export.
"$OUT/jfa_convert" --bake "assets/characters/model0/source/Mma Idle.fbx" \
	--fps 30 -o "$OUT/model0_idle.baked.jfa"

echo
echo "== comparing against original =="
python3 tools/asset-pipeline/jfa_compare.py assets/characters/model0/animations/model0_idle.jfa "$OUT/model0_idle.baked.jfa" "$@"
