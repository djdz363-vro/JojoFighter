#!/usr/bin/env python3
"""jfa_compare.py - section-wise comparison of two JFA1 files.

Used to validate a converter re-bake against an original JFA file
(e.g. regenerated model0_idle.jfa vs the original model0_idle.jfa).

Reports, per section:
  - header field equality
  - bone table equality (parents and names)
  - vertex section: exact byte equality, or per-field (position, normal, uv,
    bone indices, bone weights) count of differing entries and maximum
    absolute float difference
  - index buffer equality
  - animation matrices: per-bone maximum absolute float difference

Exit code 0 only if the files are byte-identical, or if every difference is
within --tol (default: strict, byte-identical). The comparison is never
silently weakened: every difference is quantified and reported.
"""

import struct
import sys
import math

LIMITS = dict(nb=(1, 64), nv=(1, 12000), ni=(1, 36000), nf=(1, 240))


def parse(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 32 or data[:4] != b"JFA1":
        raise SystemExit(f"error: {path} is not a JFA1 file")
    version, nb, nv, ni, nf, fps, flags = struct.unpack_from("<7I", data, 4)
    hdr = dict(version=version, nb=nb, nv=nv, ni=ni, nf=nf, fps=fps, flags=flags)
    off = 32
    bones = []
    for _ in range(nb):
        parent, = struct.unpack_from("<i", data, off)
        ln, = struct.unpack_from("<I", data, off + 4)
        off += 8
        name = data[off:off + ln].decode("utf-8", "replace")
        off += ln
        bones.append((parent, name))
    verts = []
    for _ in range(nv):
        p = struct.unpack_from("<3f", data, off)
        n = struct.unpack_from("<3f", data, off + 12)
        uv = struct.unpack_from("<2f", data, off + 24)
        bi = struct.unpack_from("<4I", data, off + 32)
        bw = struct.unpack_from("<4f", data, off + 48)
        off += 64
        verts.append((p, n, uv, bi, bw))
    indices = struct.unpack_from(f"<{ni}I", data, off)
    off += ni * 4
    mats = []
    for _ in range(nf * nb):
        mats.append(struct.unpack_from("<16f", data, off))
        off += 64
    expected = off
    return hdr, bones, verts, indices, mats, expected, len(data)


def max_diff(a, b):
    return max((abs(x - y) for x, y in zip(a, b)), default=0.0)


def main():
    args = sys.argv[1:]
    tol = None
    files = []
    i = 0
    while i < len(args):
        if args[i] == "--tol":
            tol = float(args[i + 1])
            i += 2
        else:
            files.append(args[i])
            i += 1
    if len(files) != 2:
        raise SystemExit("usage: jfa_compare.py [--tol E] <original.jfa> <baked.jfa>")
    orig_path, baked_path = files

    ohdr, obones, overt, oidx, omats, oexp, olen = parse(orig_path)
    bhdr, bbones, bert, bidx, bmats, bexp, blen = parse(baked_path)

    print(f"original: {orig_path} ({olen} bytes)")
    print(f"baked:    {baked_path} ({blen} bytes)")

    hard_fail = False

    # ---- header ----
    for k in ohdr:
        same = ohdr[k] == bhdr[k]
        if not same:
            hard_fail = True
        print(f"header.{k}: {ohdr[k]} vs {bhdr[k]} -> {'OK' if same else 'DIFF'}")

    # ---- bone table ----
    if obones == bbones:
        print(f"bone table: identical ({len(obones)} bones)")
    else:
        hard_fail = True
        print(f"bone table: DIFF ({len(obones)} vs {len(bbones)} bones)")
        for i, (o, b) in enumerate(zip(obones, bbones)):
            if o != b:
                print(f"  bone {i}: {o} vs {b}")

    # ---- vertices ----
    byte_ident_v = overt == bert
    if byte_ident_v:
        print(f"vertices: byte-identical ({len(overt)} soup vertices)")
    else:
        groups = [("position", 0), ("normal", 1), ("uv", 2), ("weights", 4)]
        for gname, gi in groups:
            diffs = 0
            mx = 0.0
            for o, b in zip(overt, bert):
                a, c = o[gi], b[gi]
                d = max_diff(a, c)
                if d > 0:
                    diffs += 1
                    mx = max(mx, d)
            pct = 100.0 * diffs / max(1, len(overt))
            print(f"vertex {gname}: {diffs}/{len(overt)} entries differ "
                  f"({pct:.2f}%), max |delta| = {mx:.3g}")
        bi_diffs = sum(1 for o, b in zip(overt, bert) if o[3] != b[3])
        print(f"vertex bone indices: {bi_diffs}/{len(overt)} entries differ")

    # ---- indices ----
    if oidx == bidx:
        print(f"indices: identical ({len(oidx)} entries, identity={oidx == tuple(range(len(oidx)))})")
    else:
        hard_fail = True
        mism = sum(1 for a, b in zip(oidx, bidx) if a != b)
        print(f"indices: DIFF ({mism} mismatches)")

    # ---- matrices ----
    nb = ohdr["nb"]
    nf = ohdr["nf"]
    if len(omats) == len(bmats):
        byte_ident_m = all(a == b for a, b in zip(omats, bmats))
        per_bone = []
        gmax = 0.0
        for b in range(nb):
            md = 0.0
            for f in range(nf):
                md = max(md, max_diff(omats[f * nb + b], bmats[f * nb + b]))
            per_bone.append(md)
            gmax = max(gmax, md)
        nonident = sum(1 for a, b in zip(omats, bmats) if a != b)
        print(f"matrices: byte-identical={byte_ident_m} "
              f"({nonident}/{len(omats)} matrices differ, max |delta| = {gmax:.3g})")
        if not byte_ident_m:
            worst = sorted(range(nb), key=lambda b: -per_bone[b])[:5]
            for b in worst:
                if per_bone[b] > 0:
                    name = obones[b][1] if b < len(obones) else "?"
                    print(f"  worst bone {b} {name}: max |delta| = {per_bone[b]:.3g}")
    else:
        hard_fail = True
        print(f"matrices: DIFF (count {len(omats)} vs {len(bmats)})")

    # ---- structural sanity of the original ----
    for k, (lo, hi) in LIMITS.items():
        v = ohdr[k]
        if not (lo <= v <= hi):
            print(f"warning: original header.{k}={v} outside loader limits {lo}..{hi}")
    if oexp != olen:
        print(f"warning: original file size {olen} != expected {oexp}")

    # ---- posed-mesh equivalence (the number the game actually cares about) ----
    # Apply each file's own matrices to its own vertices with its own weights
    # (exactly like the runtime JFA skinning does) and compare the resulting
    # posed positions per frame. This measures how the byte differences
    # translate to visible geometry.
    if len(overt) == len(bert) and len(omats) == len(bmats) and obones == bbones:
        posed_max = 0.0
        posed_frame_max = 0.0
        for f in range(nf):
            fmax = 0.0
            for vi in range(len(overt)):
                op, _, _, obi, obw = overt[vi]
                bp, _, _, bbi, bbw = bert[vi]
                oax = oay = oaz = 0.0
                bax = bay = baz = 0.0
                for k in range(4):
                    ow, bw_ = obw[k], bbw[k]
                    if ow > 1e-9:
                        om = omats[f * nb + obi[k]]
                        oax += ow * (om[0] * op[0] + om[1] * op[1] + om[2] * op[2] + om[3])
                        oay += ow * (om[4] * op[0] + om[5] * op[1] + om[6] * op[2] + om[7])
                        oaz += ow * (om[8] * op[0] + om[9] * op[1] + om[10] * op[2] + om[11])
                    if bw_ > 1e-9:
                        bm = bmats[f * nb + bbi[k]]
                        bax += bw_ * (bm[0] * bp[0] + bm[1] * bp[1] + bm[2] * bp[2] + bm[3])
                        bay += bw_ * (bm[4] * bp[0] + bm[5] * bp[1] + bm[6] * bp[2] + bm[7])
                        baz += bw_ * (bm[8] * bp[0] + bm[9] * bp[1] + bm[10] * bp[2] + bm[11])
                d = math.sqrt((oax - bax) ** 2 + (oay - bay) ** 2 + (oaz - baz) ** 2)
                fmax = max(fmax, d)
            posed_max = max(posed_max, fmax)
            posed_frame_max = max(posed_frame_max, fmax)
        print(f"posed-mesh equivalence: max per-vertex distance over all "
              f"{nf} frames = {posed_max:.3g} (meters on a ~1.7 m character)")

    # ---- verdict ----
    if olen == blen and open(orig_path, "rb").read() == open(baked_path, "rb").read():
        print("VERDICT: BYTE-IDENTICAL")
        return 0
    if tol is not None:
        ok = not hard_fail
        # re-check float differences against tolerance
        if len(overt) == len(bert):
            for gi in (0, 1, 2, 4):
                for o, b in zip(overt, bert):
                    if max_diff(o[gi], b[gi]) > tol:
                        ok = False
        if len(omats) == len(bmats):
            for a, b in zip(omats, bmats):
                if max_diff(a, b) > tol:
                    ok = False
        print(f"VERDICT: {'PASS (all differences within tol=%g)' % tol if ok else 'FAIL (differences exceed tol=%g)' % tol}")
        return 0 if ok else 1
    print("VERDICT: NOT BYTE-IDENTICAL (see quantified differences above; "
          "use --tol to allow bounded float differences)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
