#!/usr/bin/env python3
"""sweep_summary.py SWEEP_ROOT — tabulate a sweep_legs_scale.sh run.

Reads <root>/scale_<s>/noise.csv (series_noise.py output) for every scale and prints, per key and
scale: the median 95th-percentile frame-to-frame jitter, the median P1–P7 excursion
(domain_max − domain_min), and the median P4 and P7 phase samples — so the plan's 4.2 criterion can
be read off: jitter should fall, the excursion must not shrink beyond the control's spread, and the
phase samples must move by less than the series' σ.
"""
import csv, glob, os, statistics as st, sys


def load(path):
    rows = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.setdefault(r["key"], []).append(r)
    return rows


def phase(r, tag):
    for part in (r.get("phase_samples") or "").split(";"):
        if part.startswith(tag + "="):
            try:
                return float(part.split("=", 1)[1])
            except ValueError:
                return None
    return None


def med(xs):
    xs = [x for x in xs if x is not None]
    return st.median(xs) if xs else float("nan")


def main():
    root = sys.argv[1]
    keys = sys.argv[2].split(",") if len(sys.argv) > 2 else [
        "pelvisSway", "hipLineTilt", "plumbBobDistance", "leadKneeDrift", "pelvisLift"]
    scales = sorted(glob.glob(os.path.join(root, "scale_*")),
                    key=lambda p: -float(p.rsplit("_", 1)[1]))
    data = {os.path.basename(p).split("_", 1)[1]: load(os.path.join(p, "noise.csv"))
            for p in scales if os.path.exists(os.path.join(p, "noise.csv"))}
    for key in keys:
        print(f"\n== {key}")
        print(f"{'scale':>6s} {'n':>3s} {'p95 jitter':>11s} {'excursion':>10s} {'P4':>8s} {'P7':>8s}")
        for sc, rows in data.items():
            rs = rows.get(key, [])
            jit = med([float(r["jitter_p95"]) for r in rs if r["jitter_p95"]])
            exc = med([float(r["domain_max"]) - float(r["domain_min"]) for r in rs
                       if r["domain_max"] and r["domain_min"]])
            p4 = med([phase(r, "p4") for r in rs])
            p7 = med([phase(r, "p7") for r in rs])
            print(f"{sc:>6s} {len(rs):3d} {jit:11.3f} {exc:10.3f} {p4:8.3f} {p7:8.3f}")


if __name__ == "__main__":
    main()
