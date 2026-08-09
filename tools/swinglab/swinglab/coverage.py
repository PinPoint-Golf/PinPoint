# Diagnostics pack coverage: for a run root of per-swing result.json, how much
# of core.json's measures[] actually resolve — and when a measure can't, is
# that because the segmenter didn't emit the phase it needs, or because the
# analyzer never produced the metric at all? Silence is the failure mode this
# guards against: score.py's checks skip an absent phase without a trace, so a
# segmenter regression (or an un-ladder-mapped truth event, see score.py's
# P_CHECKS) can sit invisible in every scorecard. This module makes the gap a
# number.
#
# Reads ONLY analysis.phases[] / analysis.metrics[] from each swing's
# result.json plus the pack's measures[] (metricKey + reducer) — no
# swinglab_run invocation, stdlib-only.

from collections import Counter
from pathlib import Path

from . import REPO_ROOT, load_json

# Phase enum ints (src/Analysis/swing_analysis.h Phase), spelled out because
# swinglab has no compiled bridge to the C++ enum.
PHASE_LABEL = {
    0: "Address(P1)", 1: "Takeaway", 2: "Top(P4)", 3: "Transition",
    4: "Downswing", 5: "Impact(P7)", 6: "Release", 7: "Finish(P10)",
    8: "MidBackswing(P3)", 9: "Delivery(P6)", 10: "MaxSpeed",
    11: "FollowThrough(P9)", 12: "ShaftParallelBack(P2)",
    13: "ArmParallelDown(P5)", 14: "ShaftParallelThrough(P8)",
}

# Pack reducer phase TOKEN -> Phase enum int. The token vocabulary is
# phaseToken() (src/Diagnostics/measure_facets.cpp:74-101): phaseLabel() then
# `.remove(' ')` then lower-case the FIRST character only — it does not
# camel-case a multi-word label, so Phase::MaxSpeed ("max speed") tokenises to
# "maxspeed", not "maxSpeed". Lookups below lower() the incoming token so a
# pack spelled either way still resolves.
TOKEN_TO_PHASE = {
    "p1": 0, "p2": 12, "p3": 8, "p4": 2, "p5": 13, "p6": 9, "p7": 5, "p8": 14,
    "p9": 11, "takeaway": 1, "transition": 3, "downswing": 4, "release": 6,
    "finish": 7, "maxspeed": 10,
}

# The emission-matrix columns (design ask, in swing order). ShaftParallelBack
# (12), Downswing (4) and Release (6) are deliberately not columns: P2 has no
# analyzer event (score.py's parallel_p2 check is the only truth for it), and
# Downswing/Release were the pre-this-session proxies for p5/p8 that the
# segmenter no longer emits directly — a starred column (P5*/P8*) marks the
# two phases the segmenter change added.
MATRIX_COLUMNS = [
    (0, "P1"), (1, "TKW"), (8, "P3"), (3, "TRN"), (2, "P4"), (13, "P5*"),
    (9, "P6"), (10, "MAX"), (5, "P7"), (14, "P8*"), (11, "P9"), (7, "FIN"),
]

DEFAULT_PACK = REPO_ROOT / "src" / "Resources" / "diagnostics" / "core.json"


def _phase_int(token):
    return TOKEN_TO_PHASE.get(str(token).lower())


def _needed_tokens(reducer):
    """Phase tokens a reducer needs PRESENT to resolve — mirrors
    reduceOverGrid's phase requirements (src/Diagnostics/measure_sample.cpp:219-289):
    'at' reads the anchor; 'delta'/'rate' run anchor -> window's end (window[1]);
    'extremum' searches window[0]..window[1], plus the anchor when set (for the
    signed-deviation form)."""
    kind = reducer.get("kind")
    anchor = reducer.get("anchor")
    window = reducer.get("window") or []
    if kind == "at":
        return [anchor] if anchor else []
    if kind in ("delta", "rate"):
        need = [anchor] if anchor else []
        if len(window) >= 2:
            need.append(window[1])
        return need
    if kind == "extremum":
        need = list(window[:2])
        if anchor:
            need.append(anchor)
        return need
    return []


def _metric_by_key(analysis):
    return {m.get("key"): m for m in analysis.get("metrics", []) if m.get("key")}


def _valued_at(metric, phase_int, phases_present):
    """Presence-only surrogate for the C++ windowed-median + phaseSample-fallback
    read (measure_sample.cpp:155-175): a metric counts as valued at a phase if
    the phase event exists AND the metric carries curve samples (t_us[]/value[])
    OR a phaseSample labelled with that phase int. This says nothing about
    whether the *value* would be produced (the window-half/min-samples config
    could still starve it) — coverage only needs to know a producer had a shot."""
    if phase_int not in phases_present:
        return False
    has_curve = bool(metric.get("t_us")) and bool(metric.get("value"))
    has_sample = any(ps.get("phase") == phase_int for ps in metric.get("phaseSamples", []))
    return has_curve or has_sample


def load_pack(pack_path):
    return load_json(pack_path).get("measures", [])


def classify_swing(analysis, measures):
    """Per-measure classification for one swing's `analysis` block. Returns a
    list of {id, metricKey, label, status, missingPhases?} — status in
    RESOLVED / BLOCKED_PHASE / BLOCKED_METRIC / NO_LM."""
    phases_present = {p["phase"] for p in analysis.get("phases", []) if "phase" in p}
    metrics = _metric_by_key(analysis)
    has_lm = any(k.startswith("lm.") for k in metrics)

    out = []
    for m in measures:
        key = m.get("metricKey", "")
        rec = {"id": m.get("id"), "metricKey": key, "label": m.get("label") or m.get("id")}

        if key.startswith("lm.") and not has_lm:
            rec["status"] = "NO_LM"
            out.append(rec)
            continue

        metric = metrics.get(key)
        if metric is None:
            rec["status"] = "BLOCKED_METRIC"
            out.append(rec)
            continue

        missing = []
        for tok in _needed_tokens(m.get("reducer", {})):
            ph = _phase_int(tok)
            if ph is None or not _valued_at(metric, ph, phases_present):
                missing.append(tok)
        if missing:
            rec["status"] = "BLOCKED_PHASE"
            rec["missingPhases"] = missing
            out.append(rec)
            continue

        rec["status"] = "RESOLVED"
        out.append(rec)
    return out


def _find_swing_dirs(run_root):
    """Every dir under run_root carrying a result.json, sorted for a stable
    report. rglob rather than a flat listdir so a run_root that nests one
    level (a run-id folder) still works, not just the flat
    <run_root>/<swing>/result.json shape the real corpora use."""
    return sorted((p.parent for p in Path(run_root).rglob("result.json")))


def coverage(run_root, pack=None, out=None):
    run_root = Path(run_root)
    pack_path = Path(pack) if pack else DEFAULT_PACK
    measures = load_pack(pack_path)
    n_measures = len(measures)

    swing_dirs = _find_swing_dirs(run_root)
    if not swing_dirs:
        print(f"[coverage] no result.json found under {run_root}")
        return None

    matrix_rows = []           # [(display_name, {phase_int: conf})]
    per_swing = []              # [(display_name, [classification, ...])]
    phase_totals = Counter()
    status_totals = Counter()
    blocking_phase_counts = Counter()
    blocking_metric_counts = Counter()

    for sd in swing_dirs:
        try:
            result = load_json(sd / "result.json")
        except Exception as e:
            print(f"[coverage] SKIP {sd}: {e}")
            continue
        name = str(sd.relative_to(run_root)) if sd != run_root else sd.name
        analysis = result.get("analysis", {})

        phases = {p["phase"]: p.get("conf") for p in analysis.get("phases", []) if "phase" in p}
        matrix_rows.append((name, phases))
        for ph in phases:
            phase_totals[ph] += 1

        classes = classify_swing(analysis, measures)
        per_swing.append((name, classes))
        status_totals.update(c["status"] for c in classes)
        for c in classes:
            if c["status"] == "BLOCKED_PHASE":
                for tok in c["missingPhases"]:
                    blocking_phase_counts[tok] += 1
            elif c["status"] == "BLOCKED_METRIC":
                blocking_metric_counts[c["metricKey"]] += 1

    n_swings = len(matrix_rows)

    lines = [f"# Coverage — `{run_root}`", "",
             f"pack: `{pack_path}` ({n_measures} measures) · {n_swings} swings",
             ""]

    # a. Phase-emission matrix.
    lines.append("## Phase emission")
    lines.append("")
    lines.append("| swing | " + " | ".join(lbl for _, lbl in MATRIX_COLUMNS) + " |")
    lines.append("|---|" + "---|" * len(MATRIX_COLUMNS))
    for name, phases in matrix_rows:
        cells = []
        for ph, _ in MATRIX_COLUMNS:
            if ph in phases:
                conf = phases[ph]
                cells.append(f"{conf:.2f}" if isinstance(conf, (int, float)) else "yes")
            else:
                cells.append("-")
        lines.append(f"| {name} | " + " | ".join(cells) + " |")
    totals = [f"{phase_totals.get(ph, 0)}/{n_swings}" for ph, _ in MATRIX_COLUMNS]
    lines.append("| **totals** | " + " | ".join(totals) + " |")
    lines.append("")

    # b/c. Measure resolution + per-swing roll-up.
    lines.append("## Measure resolution")
    lines.append("")
    lines.append("| swing | RESOLVED | BLOCKED_PHASE | BLOCKED_METRIC | NO_LM |")
    lines.append("|---|---|---|---|---|")
    for name, classes in per_swing:
        c = Counter(x["status"] for x in classes)
        lines.append(f"| {name} | {c['RESOLVED']} | {c['BLOCKED_PHASE']} | "
                     f"{c['BLOCKED_METRIC']} | {c['NO_LM']} |")
    lines.append(f"| **corpus** ({n_swings}×{n_measures}={n_swings * n_measures}) | "
                 f"{status_totals['RESOLVED']} | {status_totals['BLOCKED_PHASE']} | "
                 f"{status_totals['BLOCKED_METRIC']} | {status_totals['NO_LM']} |")
    lines.append("")

    # Corpus-wide roll-up: top blockers.
    lines.append("## Top blocking phases (measures blocked)")
    lines.append("")
    lines.append("| phase token | phase | measures blocked |")
    lines.append("|---|---|---|")
    for tok, cnt in blocking_phase_counts.most_common(15):
        lines.append(f"| {tok} | {PHASE_LABEL.get(_phase_int(tok), '?')} | {cnt} |")
    if not blocking_phase_counts:
        lines.append("| — | — | 0 |")
    lines.append("")

    lines.append("## Top blocking metric keys (measures blocked)")
    lines.append("")
    lines.append("| metric key | measures blocked |")
    lines.append("|---|---|")
    for key, cnt in blocking_metric_counts.most_common(15):
        lines.append(f"| {key} | {cnt} |")
    if not blocking_metric_counts:
        lines.append("| — | 0 |")
    lines.append("")

    out_dir = Path(out) if out else run_root
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "COVERAGE.md"
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"[coverage] {n_swings} swings x {n_measures} measures -> {out_path}")
    print(f"[coverage] RESOLVED={status_totals['RESOLVED']} "
          f"BLOCKED_PHASE={status_totals['BLOCKED_PHASE']} "
          f"BLOCKED_METRIC={status_totals['BLOCKED_METRIC']} "
          f"NO_LM={status_totals['NO_LM']}")
    if blocking_phase_counts:
        top_p = ", ".join(f"{t}({c})" for t, c in blocking_phase_counts.most_common(5))
        print(f"[coverage] top blocking phases: {top_p}")
    if blocking_metric_counts:
        top_m = ", ".join(f"{k}({c})" for k, c in blocking_metric_counts.most_common(5))
        print(f"[coverage] top blocking metrics: {top_m}")
    return out_path
