#!/usr/bin/env python3
"""sweep_summary.py — tabulate a smoother sweep.

Two modes, both reading the per-setting series_noise.py CSVs (and, in gate mode, the swinglab
result.json documents themselves):

1. `sweep_summary.py SWEEP_ROOT [keys]` — the Phase 4.2 view of a sweep_legs_scale.sh run.
   Reads <root>/scale_<s>/noise.csv for every scale and prints, per key and scale: the median
   95th-percentile frame-to-frame jitter, the median P1–P7 excursion (domain_max − domain_min),
   and the median P4 and P7 phase samples — so the plan's 4.2 criterion can be read off: jitter
   should fall, the excursion must not shrink beyond the control's spread, and the phase samples
   must move by less than the series' σ.

2. `sweep_summary.py --gate CONTROL_DIR SWEEP_ROOT [keys] [--verbose]` — the Phase 5 gate over a
   sweep_adapt.sh run (contract C15 of
   docs/implementation/metric_presentation_honesty_phase5_contracts.md). Every setting directory
   under SWEEP_ROOT is scored PER SWING against the same swing in CONTROL_DIR, and the four
   numeric criteria are reduced to PASS/FAIL:

     C2 phase-sample stability   median |ΔP4|/σ < 1 AND max |ΔP4|/σ < 2, likewise ΔP7, on ALL
                                 five series; a phase the control resolved and the setting did
                                 not is a C2 failure and is listed by swing/series.
     C3 excursion preserved      median over swings of (domain_max−domain_min) setting/control
                                 within [0.97, 1.03] for every series (±3 %).
     C4 still-address jitter     median over swings of jitter_address_p95 setting/control ≤ 0.80
                                 (a ≥20 % reduction) on pelvisSway, hipLineTilt and
                                 plumbBobDistance.
     C5 σ not inflated           median over swings of sigma setting/control < 1 on all five.
     C6 no fragmentation         per swing and per series, n_valid(setting) >= n_valid(control)
                                 and n_samples identical. Without this the other criteria can be
                                 WON by losing samples: a keypoint whose smoother segment collapses
                                 at a low minScale emits a shorter, sparser curve, which is quieter
                                 (C4) and has a smaller σ (C5) for entirely the wrong reason. Any
                                 loss is a FAIL, listed by swing/series.

   Criterion 1 (byte parity with the policy off) is not scored here — it is parity_diff.py's job.
   The overall verdict is C2∧C3∧C4∧C5∧C6, and passers are ranked by the jitter GAIN, 1 − the mean
   of the three C4 ratios. Each setting also reports the total `pose2d.adaptFallbacks` over its
   swings (the count of keypoints where the adaptive second pass diverged and fell back), or
   "absent" when the documents do not carry it — it is a diagnostic, not a criterion.

Gate-mode exit status: 0 when at least one setting passes all four criteria, 1 when none does,
2 when the sweep could not be read at all — so a CI-style caller can tell "no winner yet" from
"you pointed me at the wrong directory".

stdlib only: this runs wherever the sweep ran (Mac, GOLFSIMPC, over the SMB share).
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import os
import statistics as st
import sys

# The five lower-body / body-line series the Phase 5 gate is defined over (C15).
GATE_KEYS = ["pelvisSway", "hipLineTilt", "plumbBobDistance", "leadKneeDrift", "pelvisLift"]

# C4 is claimed only for the three series the design says the adaptive window is FOR; leadKneeDrift
# and pelvisLift are along for C2/C3/C5 (they must not get worse) but are not required to improve.
JITTER_KEYS = ["pelvisSway", "hipLineTilt", "plumbBobDistance"]

# Phase enum ints as persisted (see series_noise.PHASE_TO_LADDER): Top is P4, Impact is P7.
PHASE_TOP = 2
PHASE_IMPACT = 5

# Thresholds, from the plan and C15. Named so a failing row can be read against the number.
C2_MED_MAX = 1.0     # median |Δphase| must be under 1 σ
C2_MAX_MAX = 2.0     # no single swing may move 2 σ
C3_LO, C3_HI = 0.97, 1.03   # ±3 % on the P1–P7 excursion
C4_RATIO_MAX = 0.80         # ≥20 % still-address jitter reduction
C5_RATIO_MAX = 1.0          # σ must not grow
# C6 has no tunable threshold: not one sample may be lost, on any swing, on any of the five series.


# ── shared CSV helpers ────────────────────────────────────────────────────────────────────

def load(path):
    """noise.csv -> {key: [row, ...]} (Phase 4 mode)."""
    rows = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.setdefault(r["key"], []).append(r)
    return rows


def load_by_swing(path):
    """noise.csv -> {key: {swing: row}} (gate mode: rows must pair across settings)."""
    rows = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.setdefault(r["key"], {})[r["swing"]] = r
    return rows


def phase(r, tag):
    """A phase sample out of the CSV's `phase_samples` cell, e.g. tag "p4"."""
    for part in (r.get("phase_samples") or "").split(";"):
        if part.startswith(tag + "="):
            try:
                return float(part.split("=", 1)[1])
            except ValueError:
                return None
    return None


def num(s):
    try:
        v = float(s)
    except (TypeError, ValueError):
        return None
    return v


def med(xs):
    xs = [x for x in xs if x is not None]
    return st.median(xs) if xs else float("nan")


def ratio(a, b):
    """a/b, or None when the denominator carries no information (missing or zero)."""
    if a is None or b is None or b == 0:
        return None
    return a / b


# ── gate mode: the result.json side ───────────────────────────────────────────────────────

def analysis_of(doc):
    """The analysis block, found by SHAPE (same rule as compare_runs.py / series_noise.py)."""
    def walk(o):
        if isinstance(o, dict):
            m = o.get("metrics")
            if isinstance(m, list) and m and isinstance(m[0], dict) and "key" in m[0]:
                return o
            for v in o.values():
                r = walk(v)
                if r:
                    return r
        if isinstance(o, list):
            for v in o:
                r = walk(v)
                if r:
                    return r
        return None
    return walk(doc)


def adapt_fallbacks(doc):
    """`pose2d.adaptFallbacks` if the document carries it, else None.

    W1's per-swing count of keypoints where the adaptive second pass diverged from the first and
    the smoother fell back. Found by SHAPE (the first `pose2d` dict, breadth-first) so it works on
    a device swing.json and a swinglab result.json alike. None ⇒ the field is absent, which is the
    normal reading for a control run or a build predating the counter - reported, never guessed.
    """
    queue = [doc]
    while queue:
        node = queue.pop(0)
        if isinstance(node, dict):
            for k, v in node.items():
                if k == "pose2d" and isinstance(v, dict):
                    fb = v.get("adaptFallbacks")
                    if isinstance(fb, bool) or not isinstance(fb, (int, float)):
                        return None
                    return int(fb)
                if isinstance(v, (dict, list)):
                    queue.append(v)
        elif isinstance(node, list):
            queue.extend(v for v in node if isinstance(v, (dict, list)))
    return None


def read_results(root, keys):
    """{swing: {key: {"sigma": float|None, "p4": float|None, "p7": float|None}}}, and
    {swing: adaptFallbacks|None}.

    σ and the phase samples come from the persisted document, not the CSV: σ is not a CSV column
    at all, and the phase values are wanted at full precision (the CSV rounds to 4 significant
    figures, which is coarse next to a 0.86° σ).
    """
    out, fallbacks = {}, {}
    want = set(keys)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        if "result.json" not in filenames:
            continue
        p = os.path.join(dirpath, "result.json")
        try:
            with open(p, encoding="utf-8") as fh:
                doc = json.load(fh)
            a = analysis_of(doc)
            if a is None:
                continue
        except Exception as exc:
            print(f"  ! unreadable {p}: {exc}", file=sys.stderr)
            continue
        swing = os.path.relpath(dirpath, root)
        fallbacks[swing] = adapt_fallbacks(doc)
        per = {}
        for m in a["metrics"]:
            k = m.get("key")
            if k not in want or k in per:
                continue
            ps = {}
            for s in (m.get("phaseSamples") or []):
                if isinstance(s.get("phase"), int) and isinstance(s.get("value"), (int, float)):
                    ps.setdefault(s["phase"], float(s["value"]))
            sig = m.get("sigma")
            per[k] = {
                "sigma": float(sig) if isinstance(sig, (int, float)) else None,
                "p4": ps.get(PHASE_TOP),
                "p7": ps.get(PHASE_IMPACT),
            }
        out[swing] = per
    return out, fallbacks


def load_setting(d, keys):
    """One setting directory: its noise.csv rows by (key, swing) plus its result.json values."""
    csv_path = os.path.join(d, "noise.csv")
    noise = load_by_swing(csv_path) if os.path.exists(csv_path) else {}
    res, fb = read_results(d, keys)
    return {"dir": d, "name": os.path.basename(os.path.normpath(d)),
            "noise": noise, "res": res, "fallbacks": fb}


def score(setting, control, keys, sigma_ref):
    """Per-series statistics for one setting against the control, plus the criterion verdicts."""
    series = {}
    missing = []          # (swing, key, "P4"/"P7") the control resolved and this setting did not
    frag = []             # (swing, key, why) C6: samples lost, or the curve length changed
    swings = sorted(set(setting["res"]) & set(control["res"]))

    for key in keys:
        dp4, dp7, exc, jit, sig, nv_delta = [], [], [], [], [], []
        for sw in swings:
            s_r = setting["res"].get(sw, {}).get(key)
            c_r = control["res"].get(sw, {}).get(key)
            if s_r is None or c_r is None:
                continue

            # σ denominator. Default: the CONTROL's σ for this swing/series — a fixed yardstick, so
            # the same physical move scores the same across settings. --sigma-ref setting uses the
            # setting's own σ (stricter when the policy shrinks σ), max uses the larger (lenient).
            if sigma_ref == "setting":
                sd = s_r["sigma"]
            elif sigma_ref == "max":
                sd = max([x for x in (s_r["sigma"], c_r["sigma"]) if x is not None] or [None])
            else:
                sd = c_r["sigma"]

            for tag, acc in (("p4", dp4), ("p7", dp7)):
                if c_r[tag] is None:
                    continue                      # control never resolved it: nothing to compare
                if s_r[tag] is None:
                    missing.append((sw, key, tag.upper()))
                    continue
                if sd:
                    acc.append(abs(s_r[tag] - c_r[tag]) / sd)

            r = ratio(s_r["sigma"], c_r["sigma"])
            if r is not None:
                sig.append(r)

            s_n = setting["noise"].get(key, {}).get(sw)
            c_n = control["noise"].get(key, {}).get(sw)
            if s_n and c_n:
                s_e = (num(s_n["domain_max"]), num(s_n["domain_min"]))
                c_e = (num(c_n["domain_max"]), num(c_n["domain_min"]))
                if None not in s_e and None not in c_e:
                    r = ratio(s_e[0] - s_e[1], c_e[0] - c_e[1])
                    if r is not None:
                        exc.append(r)
                r = ratio(num(s_n["jitter_address_p95"]), num(c_n["jitter_address_p95"]))
                if r is not None:
                    jit.append(r)

                # C6: sample-for-sample, this series must be no sparser and no shorter than the
                # control's. An older noise.csv has neither column - that is reported as a C6
                # violation rather than a pass, because the check simply cannot be made.
                s_v, c_v = num(s_n.get("n_valid")), num(c_n.get("n_valid"))
                s_s, c_s = num(s_n.get("n_samples")), num(c_n.get("n_samples"))
                if None in (s_v, c_v, s_s, c_s):
                    frag.append((sw, key, "no n_valid/n_samples columns - re-run series_noise.py"))
                else:
                    if s_v < c_v:
                        frag.append((sw, key, f"n_valid {c_v:.0f} -> {s_v:.0f}"))
                    if s_s != c_s:
                        frag.append((sw, key, f"n_samples {c_s:.0f} -> {s_s:.0f}"))
                    nv_delta.append(s_v - c_v)

        series[key] = {
            "n": len(swings),
            "dp4_med": med(dp4), "dp4_max": max(dp4) if dp4 else float("nan"), "n_dp4": len(dp4),
            "dp7_med": med(dp7), "dp7_max": max(dp7) if dp7 else float("nan"), "n_dp7": len(dp7),
            "exc": med(exc), "jit": med(jit), "sig": med(sig),
            "nv_min": min(nv_delta) if nv_delta else float("nan"),
        }

    def ok(v):                                    # a NaN (no data) can never pass a criterion
        return v == v

    c2 = not missing and all(
        ok(s["dp4_med"]) and ok(s["dp7_med"])
        and s["dp4_med"] < C2_MED_MAX and s["dp4_max"] < C2_MAX_MAX
        and s["dp7_med"] < C2_MED_MAX and s["dp7_max"] < C2_MAX_MAX
        for s in (series[k] for k in keys))
    c3 = all(ok(series[k]["exc"]) and C3_LO <= series[k]["exc"] <= C3_HI for k in keys)
    c4 = all(ok(series[k]["jit"]) and series[k]["jit"] <= C4_RATIO_MAX
             for k in JITTER_KEYS if k in series)
    c5 = all(ok(series[k]["sig"]) and series[k]["sig"] < C5_RATIO_MAX for k in keys)
    c6 = not frag

    jr = [series[k]["jit"] for k in JITTER_KEYS if k in series and ok(series[k]["jit"])]
    jit_mean = st.mean(jr) if len(jr) == len(JITTER_KEYS) else float("nan")

    # The worst series decides each row, because every criterion is an ALL-series claim. The
    # excursion column shows the ratio furthest from 1 (either direction is a distortion).
    def worst(get):
        vals = [get(series[k]) for k in keys]
        vals = [v for v in vals if v == v]          # drop NaNs so one empty series hides nothing
        return max(vals) if vals else float("nan")
    exc_worst = max((series[k]["exc"] for k in keys if ok(series[k]["exc"])),
                    key=lambda v: abs(v - 1.0), default=float("nan"))

    # Diagnostic only: the summed per-swing count of keypoints whose adaptive second pass diverged
    # and fell back. None on every swing ⇒ the field is absent from these documents.
    fbs = [setting["fallbacks"].get(sw) for sw in swings]
    fbs = [v for v in fbs if v is not None]

    return {
        "name": setting["name"], "series": series, "missing": missing, "frag": frag,
        "fallbacks": sum(fbs) if fbs else None, "fb_swings": len(fbs),
        "swings": len(swings),
        "dp4_med": worst(lambda s: s["dp4_med"]), "dp4_max": worst(lambda s: s["dp4_max"]),
        "dp7_med": worst(lambda s: s["dp7_med"]), "dp7_max": worst(lambda s: s["dp7_max"]),
        "exc": exc_worst, "jit": jit_mean, "sig": worst(lambda s: s["sig"]),
        "c2": c2, "c3": c3, "c4": c4, "c5": c5, "c6": c6,
        "pass": c2 and c3 and c4 and c5 and c6,
        "gain": (1.0 - jit_mean) if jit_mean == jit_mean else float("nan"),
    }


def f(v, nd=2):
    return "     -" if (v is None or v != v) else f"{v:{5 + nd}.{nd}f}"


def flag(b):
    return " ok " if b else "FAIL"


def run_gate(root, control_dir, keys, verbose, sigma_ref):
    control_dir = os.path.abspath(control_dir)
    control = load_setting(control_dir, keys)
    if not control["res"]:
        print(f"sweep_summary: no result.json under the control {control_dir}", file=sys.stderr)
        return 2

    dirs = sorted(d for d in glob.glob(os.path.join(os.path.abspath(root), "*"))
                  if os.path.isdir(d)
                  and (os.path.exists(os.path.join(d, "noise.csv"))
                       or glob.glob(os.path.join(d, "*", "*", "result.json"))))
    if not dirs:
        print(f"sweep_summary: no setting directories under {root}", file=sys.stderr)
        return 2

    rows = []
    for d in dirs:
        if os.path.abspath(d) == control_dir:
            continue
        rows.append(score(load_setting(d, keys), control, keys, sigma_ref))
    # Passers first, ranked by the jitter gain the gate is buying; then the failures, best first.
    rows.sort(key=lambda r: (not r["pass"], -(r["gain"] if r["gain"] == r["gain"] else -9)))

    print(f"gate: control {os.path.relpath(control_dir, os.path.abspath(root))} "
          f"({len(control['res'])} swings), {len(rows)} settings, "
          f"σ reference: {sigma_ref}, series: {','.join(keys)}")
    hdr = (f"{'setting':<20}{'n':>3} {'dP4med':>7}{'dP4max':>7}{'dP7med':>7}{'dP7max':>7}"
           f"{'exc':>7}{'jit':>7}{'sig':>7}{'miss':>5}{'frag':>5} {'fbk':>6}  "
           f"{'C2':>4} {'C3':>4} {'C4':>4} {'C5':>4} {'C6':>4}  {'GATE':>4} {'gain%':>6}")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['name']:<20}{r['swings']:>3} "
              f"{f(r['dp4_med'])}{f(r['dp4_max'])}{f(r['dp7_med'])}{f(r['dp7_max'])}"
              f"{f(r['exc'])}{f(r['jit'])}{f(r['sig'])}{len(r['missing']):>5}"
              f"{len(r['frag']):>5} "
              f"{(str(r['fallbacks']) if r['fallbacks'] is not None else 'absent'):>6}  "
              f"{flag(r['c2'])} {flag(r['c3'])} {flag(r['c4'])} {flag(r['c5'])} {flag(r['c6'])}  "
              f"{'PASS' if r['pass'] else 'fail':>4} "
              f"{f(100.0 * r['gain'], 1) if r['gain'] == r['gain'] else '     -':>6}")
    print(f"\ndP4/dP7 = |Δ phase sample| / σ vs the control, WORST series (med < {C2_MED_MAX:g}, "
          f"max < {C2_MAX_MAX:g}); exc = P1–P7 excursion ratio furthest from 1 "
          f"(in [{C3_LO}, {C3_HI}]); jit = mean still-address p95 jitter ratio over "
          f"{'/'.join(JITTER_KEYS)} (each ≤ {C4_RATIO_MAX}); sig = worst σ ratio "
          f"(< {C5_RATIO_MAX:g}); miss = phase samples the control has and this setting lost "
          f"(any ⇒ C2 FAIL); frag = series/swings that lost valid samples or changed length "
          f"(any ⇒ C6 FAIL); fbk = total pose2d.adaptFallbacks (diagnostic only); "
          f"gain = 1 − jit.")

    for r in rows:
        if r["frag"]:
            print(f"\n{r['name']}: {len(r['frag'])} fragmented series (C6 FAIL)")
            for sw, key, why in r["frag"]:
                print(f"  {sw:<40} {key:<18} {why}")

    for r in rows:
        if r["missing"]:
            print(f"\n{r['name']}: {len(r['missing'])} lost phase samples (C2 FAIL)")
            for sw, key, which in r["missing"]:
                print(f"  {sw:<40} {key:<18} {which}")

    if verbose:
        for r in rows:
            print(f"\n== {r['name']}  ({'PASS' if r['pass'] else 'fail'})")
            sub = (f"  {'series':<18}{'nP4':>4}{'dP4med':>8}{'dP4max':>8}"
                   f"{'nP7':>4}{'dP7med':>8}{'dP7max':>8}{'exc':>8}{'jit':>8}{'sig':>8}"
                   f"{'dnvalid':>9}")
            print(sub)
            print("  " + "-" * (len(sub) - 2))
            for key in keys:
                s = r["series"][key]
                print(f"  {key:<18}{s['n_dp4']:>4}{f(s['dp4_med'])}{f(s['dp4_max'])}"
                      f"{s['n_dp7']:>4}{f(s['dp7_med'])}{f(s['dp7_max'])}"
                      f"{f(s['exc'], 3)}{f(s['jit'], 3)}{f(s['sig'], 3)}"
                      f"{f(s['nv_min'], 0):>9}")   # worst per-swing n_valid change; < 0 is C6

    n_pass = sum(1 for r in rows if r["pass"])
    print(f"\n{n_pass}/{len(rows)} settings pass C2∧C3∧C4∧C5∧C6"
          + (f"; best: {rows[0]['name']} (gain {100.0 * rows[0]['gain']:.1f} %)"
             if n_pass and rows[0]["gain"] == rows[0]["gain"] else ""))
    return 0 if n_pass else 1


# ── Phase 4 mode (unchanged behaviour) ────────────────────────────────────────────────────

def run_scales(root, keys):
    scales = sorted(glob.glob(os.path.join(root, "scale_*")),
                    key=lambda p: -float(p.rsplit("_", 1)[1]))
    data = {os.path.basename(p).split("_", 1)[1]: load(os.path.join(p, "noise.csv"))
            for p in scales if os.path.exists(os.path.join(p, "noise.csv"))}
    for key in keys:
        print(f"\n== {key}")
        print(f"{'scale':>6s} {'n':>3s} {'p95 jitter':>11s} {'excursion':>10s} "
              f"{'P4':>8s} {'P7':>8s}")
        for sc, rows in data.items():
            rs = rows.get(key, [])
            jit = med([float(r["jitter_p95"]) for r in rs if r["jitter_p95"]])
            exc = med([float(r["domain_max"]) - float(r["domain_min"]) for r in rs
                       if r["domain_max"] and r["domain_min"]])
            p4 = med([phase(r, "p4") for r in rs])
            p7 = med([phase(r, "p7") for r in rs])
            print(f"{sc:>6s} {len(rs):3d} {jit:11.3f} {exc:10.3f} {p4:8.3f} {p7:8.3f}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="sweep root (scale_* dirs, or setting dirs in --gate mode)")
    ap.add_argument("keys", nargs="?", default=None,
                    help="comma-separated metric keys (default: the five gate series)")
    ap.add_argument("--gate", metavar="CONTROL_DIR", default=None,
                    help="score every setting under root against this control directory")
    ap.add_argument("--sigma-ref", choices=("control", "setting", "max"), default="control",
                    help="which σ normalises the phase-sample deltas (default: the control's)")
    ap.add_argument("--verbose", action="store_true", help="--gate: per-series detail per setting")
    args = ap.parse_args(argv)

    keys = [k.strip() for k in args.keys.split(",") if k.strip()] if args.keys else list(GATE_KEYS)
    if args.gate:
        return run_gate(args.root, args.gate, keys, args.verbose, args.sigma_ref)
    return run_scales(args.root, keys)


if __name__ == "__main__":
    sys.exit(main())
