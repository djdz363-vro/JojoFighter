#!/bin/sh
# validate_jfa.sh - build tools/jfa_convert and validate the re-bake of
# model0_idle.jfa against the original file.
#
# The re-baked file is written to build/jfa/ (never over the original).
#
# Usage: sh tools/validate_jfa.sh [--tol E]
#   --tol E is forwarded to tools/jfa_compare.py as an allowed maximum
#   float difference (default: require byte-identical output).

set -e
cd "$(dirname "$0")/.."

OUT=build/jfa
mkdir -p "$OUT"

echo "== building tools/jfa_convert =="
${CC:-cc} -std=c99 -O2 -I Android/app/jni/ufbx \
	tools/jfa_convert.c Android/app/jni/ufbx/ufbx.c -lm -o "$OUT/jfa_convert"

echo
echo "== --info on all existing JFA files =="
for f in model0_*.jfa; do
	"$OUT/jfa_convert" --info "$f" | tail -3
done

echo
echo "== re-baking idle animation =="
# The original model0_idle.jfa was baked from a Mixamo export of the same
# character at 30 fps (151 frames for the 5.0 s idle). "Mma Idle.fbx" in the
# Android assets is that export.
"$OUT/jfa_convert" --bake "Android/app/src/main/assets/Mma Idle.fbx" \
	--fps 30 -o "$OUT/model0_idle.baked.jfa"

echo
echo "== comparing against original =="
python3 tools/jfa_compare.py model0_idle.jfa "$OUT/model0_idle.baked.jfa" "$@"
