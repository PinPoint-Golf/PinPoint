#!/usr/bin/env python3
"""compare_runs.py BEFORE_ROOT AFTER_ROOT — Phase 1 gate over swinglab result.json pairs.

For every metric series in every paired swing: value[] and t_us[] must be identical
(Phase 1 changes no persisted value); report `valid` arrays that appeared (count of 0s,
which keys), phase samples removed/added (key, phase), sigma keys, and anything else that
differs at the metric level. Also diff the non-metric part of `analysis` textually.
"""
import json, sys
from pathlib import Path

def find_results(root):
    return {str(p.relative_to(root)): p for p in Path(root).rglob("result.json")}

def analysis_of(doc):
    def walk(o):
        if isinstance(o, dict):
            if "metrics" in o and isinstance(o["metrics"], list) and o["metrics"] and isinstance(o["metrics"][0], dict) and "key" in o["metrics"][0]:
                return o
            for v in o.values():
                r = walk(v)
                if r: return r
        if isinstance(o, list):
            for v in o:
                r = walk(v)
                if r: return r
    return walk(doc)

def main():
    a_root, b_root = sys.argv[1], sys.argv[2]
    A, B = find_results(a_root), find_results(b_root)
    keys = sorted(set(A) | set(B))
    value_diffs = 0; masks = {}; removed = []; added = []; other = []
    for k in keys:
        if k not in A or k not in B:
            other.append((k, "unpaired")); continue
        da, db = json.load(open(A[k])), json.load(open(B[k]))
        ma = {m["key"]: m for m in analysis_of(da)["metrics"]}
        mb = {m["key"]: m for m in analysis_of(db)["metrics"]}
        for mk in sorted(set(ma) | set(mb)):
            if mk not in ma: other.append((k, f"metric added: {mk}")); continue
            if mk not in mb: other.append((k, f"metric removed: {mk}")); continue
            x, y = ma[mk], mb[mk]
            if x.get("value") != y.get("value") or x.get("t_us") != y.get("t_us"):
                value_diffs += 1; other.append((k, f"VALUE/T_US DIFFER: {mk}"))
            if "valid" in y and "valid" not in x:
                v = y["valid"]; masks.setdefault(mk, []).append((k, sum(1 for e in v if e == 0), len(v)))
            elif "valid" in x and "valid" not in y:
                other.append((k, f"valid disappeared: {mk}"))
            elif x.get("valid") != y.get("valid"):
                other.append((k, f"valid changed: {mk}"))
            pa = {(p["phase"]) for p in x.get("phaseSamples", [])}
            pb = {(p["phase"]) for p in y.get("phaseSamples", [])}
            for ph in sorted(pa - pb): removed.append((k, mk, ph, next(p["value"] for p in x["phaseSamples"] if p["phase"] == ph)))
            for ph in sorted(pb - pa): added.append((k, mk, ph))
            for ph in pa & pb:
                va = next(p["value"] for p in x["phaseSamples"] if p["phase"] == ph)
                vb = next(p["value"] for p in y["phaseSamples"] if p["phase"] == ph)
                if va != vb: other.append((k, f"PHASE SAMPLE VALUE DIFFERS: {mk} phase {ph} {va} -> {vb}"))
            if x.get("sigma") != y.get("sigma"): other.append((k, f"sigma changed: {mk}"))
        # non-metric analysis diff
        aa = {kk: vv for kk, vv in analysis_of(da).items() if kk != "metrics"}
        bb = {kk: vv for kk, vv in analysis_of(db).items() if kk != "metrics"}
        for kk in sorted(set(aa) | set(bb)):
            if kk in ("timings",): continue
            if aa.get(kk) != bb.get(kk): other.append((k, f"analysis.{kk} differs"))
    print(f"swings paired: {len([k for k in keys if k in A and k in B])}")
    print(f"value/t_us differences: {value_diffs}")
    print("valid masks appeared:")
    for mk, rows in sorted(masks.items()):
        print(f"  {mk:22s} on {len(rows)} swings; zeros per swing: {[r[1] for r in rows]}")
    print(f"phase samples removed ({len(removed)}):")
    for r in removed: print(f"  {r[0]} {r[1]} phase {r[2]} (was {r[3]:.2f})")
    print(f"phase samples added ({len(added)}):")
    for r in added: print(f"  {r}")
    print(f"other differences ({len(other)}):")
    for r in other: print(f"  {r[0]}: {r[1]}")

if __name__ == "__main__":
    main()
