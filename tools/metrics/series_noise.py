#!/usr/bin/env python3
"""Baseline probe for metric-series noise, degeneracy and foreshortening.

Phase 0 of docs/implementation/metric_presentation_honesty_impl_plan.md (tasks 0.1-0.3).
This is the one-off probe from 2026-09-04 made repeatable: it reports, per (swing, key),
how noisy the persisted curve is, what the CURRENT chart-style PEAK RATE tile would read,
what the curve does inside the proposed P1-P7 domain, and - for the two body-line angles -
how far the hip/shoulder line has foreshortened out of the image plane.

Nothing here derives a product number. It measures what we ship today so the design's
definition of done (design doc section 7) has a before to be judged against.

stdlib only, deliberately: it has to run on the Mac, on GOLFSIMPC and over the SMB share
with no environment to set up. Every swing.json is opened exactly once (the share is SMB
and the files run to tens of MB).

Usage:
    series_noise.py ROOT [--keys k1,k2,...] [--out CSV] [--only SUBSTR]
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys

# --------------------------------------------------------------------------------------
# Contracts read out of the data, not guessed. See docs/reference/swing_json_schema.md
# sections `metrics[]`, `phases[]` and `pose2d`.
# --------------------------------------------------------------------------------------

DEFAULT_KEYS = [
    "hipLineTilt",
    "shoulderPlaneAngle",
    "elbowAlignment",
    "spineSideBend",
    "pelvisSway",
    "pelvisLift",
    "leadKneeDrift",
    "plumbBobDistance",
    "secondaryAxisTilt",
    "thoraxLateralDrift",
]

# Phase enum ints as persisted in analysis.phases[].phase and metrics[].phaseSamples[].phase.
# The enum order is append-only history; it is NOT the order the positions occur in.
PHASE_NAME = {
    0: "Address",
    1: "Takeaway",
    2: "Top",
    3: "Transition",
    4: "Downswing",
    5: "Impact",
    6: "Release",
    7: "Finish",
    8: "MidBackswing",
    9: "Delivery",
    10: "MaxSpeed",
    11: "FollowThrough",
    12: "ShaftParallelBack",
    13: "ArmParallelDown",
    14: "ShaftParallelThrough",
}

# The P-position ladder: where each phase sits in swing time. Phases with no ladder slot
# (Takeaway, Transition, Downswing, Release, MaxSpeed) are reported by name instead.
PHASE_TO_LADDER = {
    0: 1,    # Address            P1
    12: 2,   # ShaftParallelBack  P2
    8: 3,    # MidBackswing       P3
    2: 4,    # Top                P4
    13: 5,   # ArmParallelDown    P5
    9: 6,    # Delivery           P6
    5: 7,    # Impact             P7
    14: 8,   # ShaftParallelThrough P8
    11: 9,   # FollowThrough      P9
    7: 10,   # Finish             P10
}
LADDER_TO_PHASE = {v: k for k, v in PHASE_TO_LADDER.items()}

PHASE_ADDRESS = 0
PHASE_IMPACT = 5

# COCO body keypoint indices (pose2d kp indices 0-16 are the unchanged COCO body joints).
KP_L_SHOULDER, KP_R_SHOULDER = 5, 6
KP_L_HIP, KP_R_HIP = 11, 12

# The gate the design proposes (design doc 5.1: lowerBody.minHipSpanRatio /
# upperBody.minShoulderSpanRatio, both 0.40). Phase 0 only MEASURES against it.
SPAN_GATE = 0.40

ADDRESS_WINDOW_US = 300_000       # still-address PK RATE window: [Address - 300ms, Address]
ADDRESS_SPAN_HALF_US = 250_000    # +/-250ms of Address for the address span median
MIN_KP_CONF = 0.3
MIN_SAMPLES = 10                  # fewer than this and the row is not worth a statistic

COLUMNS = [
    "swing",             # path of the swing directory relative to ROOT
    "session",           # its parent directory name
    "key",
    "unit",
    "n",
    "dt_med_ms",
    "dt_min_ms",
    "dt_max_ms",
    "jitter_med",        # median |dv| frame to frame, in the metric's own unit
    "jitter_p95",        # 95th percentile of the same
    "pk_rate_all",       # today's chart PEAK RATE: max adjacent |dv| / (dt/100ms), whole swing
    "pk_rate_address",   # the same over [Address-300ms, Address] - the body is still here
    "pk_rate_domain",    # the same restricted to the P1-P7 domain
    "raw_min",
    "raw_max",
    "domain_min",        # min over samples in [Address, Impact]
    "domain_max",
    "phase_samples",     # "p4=11.28;p7=-9.02;Takeaway=..." - every phaseSample present
    "span_frac_p1p7_below_gate",   # body-line rows only: fraction of P1-P7 frames < 0.40
    "span_min_at_phase",           # min span ratio at any P1-P7 phase instant (nearest frame)
    "span_min_at_phase_which",     # which ladder position that was
    "span_min_any",                # min span ratio over every pose frame
    "span_at_phase",               # "p1=1.00;p2=0.83;..." ratio at each P1-P7 instant
]

SPAN_COLUMNS = COLUMNS[COLUMNS.index("span_frac_p1p7_below_gate"):]

# Which body line each key's span columns describe. Only these two rows carry them.
SPAN_KEY_FOR_ROW = {"hipLineTilt": "hips", "shoulderPlaneAngle": "shoulders"}


# --------------------------------------------------------------------------------------
# Small numeric helpers (no numpy - stdlib only).
# --------------------------------------------------------------------------------------

def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return None
    m = n // 2
    return s[m] if n % 2 else 0.5 * (s[m - 1] + s[m])


def percentile(xs, q):
    """Linear-interpolated percentile, q in 0..1. None on an empty list."""
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return None
    if n == 1:
        return s[0]
    pos = q * (n - 1)
    lo = int(math.floor(pos))
    hi = min(lo + 1, n - 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


def fmt(v, nd=4):
    if v is None:
        return ""
    if isinstance(v, float) and (math.isnan(v) or math.isinf(v)):
        return ""
    return f"{v:.{nd}f}".rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def peak_rate(t_us, value, lo=None, hi=None):
    """The CURRENT ChartMetrics::summary PK RATE: the largest adjacent-sample
    |dv| / (dt / 100ms). One noisy sample divided by one frame interval, which is exactly
    why the design calls it a reducer that rewards noise. Restricted to [lo, hi] when given
    (both endpoints of a pair must lie inside). None when no pair qualifies."""
    best = None
    for i in range(1, len(t_us)):
        t0, t1 = t_us[i - 1], t_us[i]
        if lo is not None and (t0 < lo or t1 < lo):
            continue
        if hi is not None and (t0 > hi or t1 > hi):
            continue
        dt_ms = (t1 - t0) / 1000.0
        if dt_ms <= 0:
            continue
        r = abs(value[i] - value[i - 1]) / (dt_ms / 100.0)
        if best is None or r > best:
            best = r
    return best


def nearest_index(sorted_t, target):
    """Index of the entry in sorted_t closest to target. Linear scan is fine: a pose track
    is a couple of hundred frames and we do this a handful of times per swing."""
    if not sorted_t:
        return None
    best_i, best_d = 0, abs(sorted_t[0] - target)
    for i in range(1, len(sorted_t)):
        d = abs(sorted_t[i] - target)
        if d < best_d:
            best_i, best_d = i, d
    return best_i


# --------------------------------------------------------------------------------------
# Locating the blocks. The plan says walk the JSON rather than hard-coding a path, because
# a device-only swing, a re-analysis fixture and a live shot do not all nest the same way.
# --------------------------------------------------------------------------------------

def _looks_like_metric(obj):
    return (isinstance(obj, dict) and isinstance(obj.get("key"), str)
            and ("value" in obj or "phaseSamples" in obj))


def _looks_like_phase_event(obj):
    return (isinstance(obj, dict) and isinstance(obj.get("phase"), int)
            and isinstance(obj.get("t_us"), int) and "value" not in obj)


def find_blocks(doc):
    """Return (metrics_list, phases_list, pose2d_dict), any of which may be None.

    Breadth-first so the shallowest match wins - `analysis.metrics` beats anything nested
    deeper in a stream. Recognised by SHAPE, not by path.
    """
    metrics = phases = pose = None
    queue = [doc]
    while queue and (metrics is None or phases is None or pose is None):
        node = queue.pop(0)
        if isinstance(node, dict):
            for k, v in node.items():
                if pose is None and k == "pose2d" and isinstance(v, dict):
                    pose = v
                if isinstance(v, list) and v:
                    if metrics is None and k == "metrics" and _looks_like_metric(v[0]):
                        metrics = v
                    elif phases is None and k == "phases" and _looks_like_phase_event(v[0]):
                        phases = v
                if isinstance(v, (dict, list)):
                    queue.append(v)
        elif isinstance(node, list):
            for v in node:
                if isinstance(v, (dict, list)):
                    queue.append(v)
    return metrics, phases, pose


def phase_times(phases):
    """phase int -> t_us. A phase the segmenter never resolved is simply ABSENT; there is
    no sentinel here and callers must handle the missing key."""
    out = {}
    for ev in phases or []:
        p, t = ev.get("phase"), ev.get("t_us")
        if isinstance(p, int) and isinstance(t, int) and p not in out:
            out[p] = t
    return out


def phase_label(p):
    lad = PHASE_TO_LADDER.get(p)
    return f"p{lad}" if lad else PHASE_NAME.get(p, f"phase{p}")


# --------------------------------------------------------------------------------------
# Foreshortening: how much of the address body-line span survives in the image plane.
# --------------------------------------------------------------------------------------

def span_track(pose, ia, ib):
    """[(t_us, |dx|, ok)] for the keypoint pair (ia, ib) over the pose track.

    Prefers `smoothed` (what the producers themselves read) and falls back to `frames`.
    kp are normalized 0..1, flat [x, y, conf] x 133; only the x separation matters here, and
    the ratio against the address span cancels the x/y anisotropy of the normalisation.
    `ok` is both keypoints at conf >= 0.3 - on the smoothed track conf carries the overlay
    render-alpha contract rather than a detector score, which is close enough for a floor.
    """
    frames = pose.get("smoothed") or pose.get("frames") or []
    out = []
    for fr in frames:
        kp = fr.get("kp") or []
        t = fr.get("t_us")
        if t is None or len(kp) < 3 * (max(ia, ib) + 1):
            continue
        xa, ca = kp[3 * ia], kp[3 * ia + 2]
        xb, cb = kp[3 * ib], kp[3 * ib + 2]
        out.append((t, abs(xa - xb), ca >= MIN_KP_CONF and cb >= MIN_KP_CONF))
    out.sort(key=lambda r: r[0])
    return out


def address_span(track, addr_t):
    """Median |dx| over the frames within +/-250ms of Address; failing that, the first five
    confident frames. None when neither exists - the ratio is then simply not computable and
    every span column stays blank rather than taking a fabricated denominator."""
    if addr_t is not None:
        win = [d for (t, d, ok) in track
               if ok and abs(t - addr_t) <= ADDRESS_SPAN_HALF_US]
        if win:
            return median(win)
    first = [d for (t, d, ok) in track if ok][:5]
    if first:
        return median(first)
    return None


def span_stats(pose, ia, ib, ptimes):
    """Foreshortening summary for one body line, or None when it cannot be computed."""
    track = span_track(pose, ia, ib)
    if not track:
        return None
    addr_t = ptimes.get(PHASE_ADDRESS)
    denom = address_span(track, addr_t)
    if not denom or denom <= 0:
        return None

    ts = [t for (t, _, _) in track]
    ratios = [d / denom for (_, d, _) in track]

    imp_t = ptimes.get(PHASE_IMPACT)
    frac_below = None
    if addr_t is not None and imp_t is not None and imp_t > addr_t:
        dom = [r for t, r in zip(ts, ratios) if addr_t <= t <= imp_t]
        if dom:
            frac_below = sum(1 for r in dom if r < SPAN_GATE) / len(dom)

    # Ratio at each P1-P7 instant, read off the nearest pose frame.
    at_phase = []
    min_at_phase, min_which = None, ""
    for lad in range(1, 8):
        p = LADDER_TO_PHASE[lad]
        t = ptimes.get(p)
        if t is None:
            continue
        i = nearest_index(ts, t)
        r = ratios[i]
        at_phase.append((lad, r))
        if min_at_phase is None or r < min_at_phase:
            min_at_phase, min_which = r, f"p{lad}"

    return {
        "frac_below": frac_below,
        "min_at_phase": min_at_phase,
        "min_at_phase_which": min_which,
        "min_any": min(ratios),
        "at_phase": ";".join(f"p{lad}={r:.3f}" for lad, r in at_phase),
    }


# --------------------------------------------------------------------------------------
# Per-swing work.
# --------------------------------------------------------------------------------------

def analyse_swing(path, root, keys):
    """Return (rows, absent_keys, note). rows are dicts keyed by COLUMNS."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except Exception as exc:                      # a truncated or half-written swing
        return [], list(keys), f"unreadable: {exc}"

    metrics, phases, pose = find_blocks(doc)
    if metrics is None:
        return [], list(keys), "no metrics[] block"

    ptimes = phase_times(phases)
    addr_t = ptimes.get(PHASE_ADDRESS)
    imp_t = ptimes.get(PHASE_IMPACT)

    swing_dir = os.path.relpath(os.path.dirname(path), root)
    session = os.path.basename(os.path.dirname(os.path.dirname(path)))

    spans = {}
    if pose:
        spans["hips"] = span_stats(pose, KP_L_HIP, KP_R_HIP, ptimes)
        spans["shoulders"] = span_stats(pose, KP_L_SHOULDER, KP_R_SHOULDER, ptimes)

    by_key = {}
    for m in metrics:
        by_key.setdefault(m.get("key"), m)

    rows, absent = [], []
    for key in keys:
        m = by_key.get(key)
        t_us = list(m.get("t_us") or []) if m else []
        value = list(m.get("value") or []) if m else []
        n = min(len(t_us), len(value))
        if n < MIN_SAMPLES:
            absent.append(key)
            continue
        t_us, value = t_us[:n], value[:n]

        dts = [(t_us[i] - t_us[i - 1]) / 1000.0 for i in range(1, n)]
        jit = [abs(value[i] - value[i - 1]) for i in range(1, n)]

        dom_lo = addr_t if (addr_t is not None and imp_t is not None) else None
        dom_hi = imp_t if dom_lo is not None else None
        dom_vals = ([v for t, v in zip(t_us, value) if dom_lo <= t <= dom_hi]
                    if dom_lo is not None else [])

        ps = []
        for s in (m.get("phaseSamples") or []):
            p, v = s.get("phase"), s.get("value")
            if isinstance(p, int) and isinstance(v, (int, float)):
                ps.append(f"{phase_label(p)}={v:.4g}")

        row = {c: "" for c in COLUMNS}
        row.update({
            "swing": swing_dir,
            "session": session,
            "key": key,
            "unit": m.get("unit", ""),
            "n": n,
            "dt_med_ms": fmt(median(dts), 2),
            "dt_min_ms": fmt(min(dts) if dts else None, 2),
            "dt_max_ms": fmt(max(dts) if dts else None, 2),
            "jitter_med": fmt(median(jit), 4),
            "jitter_p95": fmt(percentile(jit, 0.95), 4),
            "pk_rate_all": fmt(peak_rate(t_us, value), 2),
            "pk_rate_address": fmt(
                peak_rate(t_us, value, addr_t - ADDRESS_WINDOW_US, addr_t)
                if addr_t is not None else None, 2),
            "pk_rate_domain": fmt(
                peak_rate(t_us, value, dom_lo, dom_hi) if dom_lo is not None else None, 2),
            "raw_min": fmt(min(value), 4),
            "raw_max": fmt(max(value), 4),
            "domain_min": fmt(min(dom_vals) if dom_vals else None, 4),
            "domain_max": fmt(max(dom_vals) if dom_vals else None, 4),
            "phase_samples": ";".join(ps),
        })

        which = SPAN_KEY_FOR_ROW.get(key)
        st = spans.get(which) if which else None
        if st:
            row.update({
                "span_frac_p1p7_below_gate": fmt(st["frac_below"], 4),
                "span_min_at_phase": fmt(st["min_at_phase"], 4),
                "span_min_at_phase_which": st["min_at_phase_which"],
                "span_min_any": fmt(st["min_any"], 4),
                "span_at_phase": st["at_phase"],
            })
        rows.append(row)

    return rows, absent, ""


# --------------------------------------------------------------------------------------
# Summary to stderr.
# --------------------------------------------------------------------------------------

def _num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def print_summary(rows, absent_counts, keys, swings, out=sys.stderr):
    def med_of(key, col):
        xs = [_num(r[col]) for r in rows if r["key"] == key]
        return median([x for x in xs if x is not None])

    hdr = (f"{'key':<20} {'n':>4} {'absent':>6} {'p95 jit':>9} {'PK all':>9} "
           f"{'PK addr':>9} {'PK dom':>9} {'<gate@P':>8}")
    print(f"\n{len(swings)} swing.json read", file=out)
    print(hdr, file=out)
    print("-" * len(hdr), file=out)
    for key in keys:
        krows = [r for r in rows if r["key"] == key]
        below = sum(1 for r in krows
                    if (_num(r["span_min_at_phase"]) is not None
                        and _num(r["span_min_at_phase"]) < SPAN_GATE))
        has_span = any(r["span_min_at_phase"] for r in krows)
        print(f"{key:<20} {len(krows):>4} {absent_counts.get(key, 0):>6} "
              f"{fmt(med_of(key, 'jitter_p95'), 3):>9} "
              f"{fmt(med_of(key, 'pk_rate_all'), 1):>9} "
              f"{fmt(med_of(key, 'pk_rate_address'), 1):>9} "
              f"{fmt(med_of(key, 'pk_rate_domain'), 1):>9} "
              f"{(str(below) if has_span else '-'):>8}", file=out)
    print(f"\nmedians over swings; '<gate@P' = swings whose min P1-P7 phase-instant span "
          f"ratio < {SPAN_GATE:.2f}", file=out)


# --------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", metavar="ROOT", help="directory walked recursively for swing.json")
    ap.add_argument("--keys", default=",".join(DEFAULT_KEYS),
                    help="comma-separated metric keys (default: the lower-body and body-line set)")
    ap.add_argument("--out", default=None, help="CSV path (default: stdout)")
    ap.add_argument("--only", default=None, help="only swings whose path contains this substring")
    args = ap.parse_args(argv)

    keys = [k.strip() for k in args.keys.split(",") if k.strip()]
    root = os.path.abspath(args.root)

    paths = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        if "swing.json" in filenames:
            p = os.path.join(dirpath, "swing.json")
            if args.only is None or args.only in p:
                paths.append(p)
    paths.sort()

    rows, absent_counts, swings = [], {}, []
    for i, p in enumerate(paths, 1):
        print(f"[{i}/{len(paths)}] {os.path.relpath(p, root)}", file=sys.stderr)
        r, absent, note = analyse_swing(p, root, keys)
        if note:
            print(f"    ! {note}", file=sys.stderr)
        for k in absent:
            absent_counts[k] = absent_counts.get(k, 0) + 1
        rows.extend(r)
        swings.append(p)

    fh = open(args.out, "w", newline="", encoding="utf-8") if args.out else sys.stdout
    try:
        w = csv.DictWriter(fh, fieldnames=COLUMNS, lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    finally:
        if args.out:
            fh.close()

    print_summary(rows, absent_counts, keys, swings)
    return 0


if __name__ == "__main__":
    sys.exit(main())
