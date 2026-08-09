# Scorecards: Tier-1 physics invariants (the "expected tracking shape"),
# Tier-2 cross-modal consistency, Tier-3 truth metrics where truth.json
# exists. Every check is a named verdict with a value and a threshold so a
# model (or a human) can act on it without watching video.

import math

import numpy as np

from . import RunResult, Swing, git_sha


def _check(name, ok, value, threshold, severity="fail", **detail):
    c = {"name": name, "pass": bool(ok), "value": value,
         "threshold": threshold, "severity": severity}
    c.update(detail)   # additive per-check detail (e.g. truth.line_dist's distances_px)
    return c


def _perp_dist_to_line(pt, a, b):
    """Perpendicular distance from point `pt` to the infinite line through `a`
    and `b` (each an (x, y) pair, any array-like). Degenerate `a == b` (zero-
    length drawn line) falls back to point-to-point distance rather than
    dividing by zero."""
    a = np.asarray(a, dtype=float)
    b = np.asarray(b, dtype=float)
    pt = np.asarray(pt, dtype=float)
    d = b - a
    norm = float(np.hypot(d[0], d[1]))
    if norm < 1e-6:
        return float(np.hypot(*(pt - a)))
    return float(abs(d[0] * (pt[1] - a[1]) - d[1] * (pt[0] - a[0])) / norm)


# Layer A — line re-registration ("snap") — shaft_position_first design §6 /
# impl plan Phase A2. PROVISIONAL: no corpus gate has run yet; re-seat this
# once the A2 studio numbers land (gate: distance improves on every labelled
# swing when shaft.snap is enabled).
LINE_DIST_THRESHOLD_PX = 6.0


def invariants(run: RunResult, scope="full"):
    """Tier 1+2 — no labels needed. `scope` is the markup swing scope
    (full/pitch/chip/putt); full-swing-only checks are skipped for partial
    swings, whose arc/tempo legitimately fall outside the full-swing bounds."""
    full = (scope or "full") == "full"
    checks = []
    an = run.analysis
    club = run.club
    samples = run.club_samples()
    impact_us = None
    for p in an.get("phases", []):
        if p.get("phase") == 5:
            impact_us = p["t_us"]

    checks.append(_check("club.valid", club.get("valid", False), club.get("valid"), True))
    cov = club.get("coverage", 0.0)
    checks.append(_check("club.coverage", cov >= 0.6, round(cov, 3), ">=0.6"))

    if samples:
        t = np.array([s["t_us"] for s in samples], dtype=np.int64)
        th = np.array([s["theta"] for s in samples])
        flags = np.array([s.get("flags", 0) for s in samples], dtype=int)
        heads = np.array([s["head"] for s in samples])  # normalized 0..1
        lens = np.array([s.get("lenPx", 0.0) for s in samples])

        checks.append(_check("track.monotonic_t", bool(np.all(np.diff(t) > 0)),
                             int(np.sum(np.diff(t) <= 0)), "0 inversions"))

        # No teleporting theta between MEASURED neighbours (coasted spans may
        # legitimately bridge larger angles).
        meas = (flags & 0x01) != 0
        dth = np.abs(np.diff(th))
        both_meas = meas[1:] & meas[:-1]
        worst = float(np.max(dth[both_meas])) if np.any(both_meas) else 0.0
        checks.append(_check("track.theta_step", worst < math.radians(25),
                             round(math.degrees(worst), 1), "<25 deg/frame"))

        # Downswing sweep: total |theta| travel in the 400 ms before impact.
        # Full-swing-only: a pitch/chip/putt has far less arc, so the [86, 458]
        # band doesn't apply — skip it rather than fail it.
        if impact_us is not None:
            if full:
                win = (t >= impact_us - 400_000) & (t <= impact_us)
                sweep = float(np.sum(np.abs(np.diff(th[win])))) if np.sum(win) > 3 else 0.0
                checks.append(_check("track.downswing_sweep", 1.5 <= sweep <= 8.0,
                                     round(math.degrees(sweep), 0), "[86, 458] deg"))

            # theta_dot peak near impact.
            dt = np.diff(t) / 1e6
            rate = np.abs(np.diff(th)) / np.maximum(dt, 1e-6)
            if len(rate):
                pk = int(np.argmax(rate))
                pk_dt_ms = abs(int(t[pk] - impact_us)) / 1000.0
                checks.append(_check("track.peak_rate_near_impact", pk_dt_ms <= 120,
                                     round(pk_dt_ms, 0), "<=120 ms", severity="warn"))

        # Head-point continuity (normalized units; 0.25 ~ a quarter frame).
        dhead = np.linalg.norm(np.diff(heads, axis=0), axis=1)
        worst_head = float(np.max(dhead)) if len(dhead) else 0.0
        checks.append(_check("track.head_step", worst_head < 0.25,
                             round(worst_head, 3), "<0.25 frame/frame"))

        # L smoothness between measured neighbours.
        dlen = np.abs(np.diff(lens))
        worst_len = float(np.max(dlen[both_meas])) if np.any(both_meas) else 0.0
        checks.append(_check("track.len_step", worst_len < 80,
                             round(worst_len, 0), "<80 px/frame", severity="warn"))

    # Tier 2 — cross-modal.
    corr = club.get("imuVisionCorr", 0.0)
    has_bindings = bool(an.get("bindings"))
    checks.append(_check("xmodal.imu_vision_corr",
                         (corr >= 0.9) if has_bindings else True,
                         round(corr, 3), ">=0.9 when IMUs bound",
                         severity="warn"))

    # Segmentation sanity.
    phases = {p["phase"]: p for p in an.get("phases", [])}
    ts = [p["t_us"] for p in an.get("phases", [])]
    checks.append(_check("seg.monotone", ts == sorted(ts), 0, "ordered"))
    # Tempo ratio is a full-swing notion (backswing vs downswing); partial swings
    # don't have a comparable top, so skip it for pitch/chip/putt.
    #
    # Basis: ADDRESS(0)→Top(2) over Top(2)→Impact(5) — matching the shipped
    # tempoRatio metric (src/Analysis/tempo_metrics.h). This check used to run on
    # Takeaway(1)→Top, which is the definition the golf literature uses; the two
    # disagreed, so the harness was measuring something the product never showed.
    # The band stays [1.2, 6.0]: it is a plausibility range, not the coaching
    # corridor, and it is wide enough to absorb the (small) basis shift.
    if full and 0 in phases and 2 in phases and 5 in phases:
        back = (phases[2]["t_us"] - phases[0]["t_us"]) / 1e6
        down = (phases[5]["t_us"] - phases[2]["t_us"]) / 1e6
        ratio = back / down if down > 0 else 0
        checks.append(_check("seg.tempo_ratio", 1.2 <= ratio <= 6.0,
                             round(ratio, 2), "[1.2, 6.0]", severity="warn"))
    return checks


def truth_metrics(run: RunResult, swing: Swing):
    """Tier 3 — only when truth.json exists."""
    truth = swing.truth()
    if not truth or "shaft" not in truth:
        return []
    samples = run.club_samples()
    if not samples:
        return [_check("truth.theta_rms", False, None, "track empty")]
    club = run.club
    Wpx, Hpx = club.get("frameWidth", 1), club.get("frameHeight", 1)

    t = np.array([s["t_us"] for s in samples], dtype=np.int64)
    th = np.unwrap(np.array([s["theta"] for s in samples]))
    heads = np.array([s["head"] for s in samples]) * np.array([Wpx, Hpx])
    grips = np.array([s["grip"] for s in samples]) * np.array([Wpx, Hpx])

    errs_th, errs_head, errs_line = [], [], []
    for tf in truth["shaft"]:
        tt = tf["t_us"]
        if tt < t[0] or tt > t[-1]:
            continue
        i = int(np.searchsorted(t, tt))
        i = min(max(i, 1), len(t) - 1)
        f = (tt - t[i - 1]) / max(t[i] - t[i - 1], 1)
        thi = th[i - 1] + f * (th[i] - th[i - 1])
        # wrap-free comparison
        d = (thi - tf["theta"] + math.pi) % (2 * math.pi) - math.pi
        errs_th.append(d)
        hi = heads[i - 1] + f * (heads[i] - heads[i - 1])
        errs_head.append(float(np.linalg.norm(hi - np.array(tf["head"]))))
        # Layer A (design §6 / impl plan A2) — "does the drawn line lie on the
        # club": perpendicular distance from the labelled shaft segment's
        # midpoint to the infinite line through the run's (interpolated)
        # drawn grip->head at this frame. Instrumented-truth "ray" entries
        # (truth_json_schema.md Variant 2) omit `head` — skip those frames
        # rather than crash, same "absent ⇒ not counted" spirit as the rest
        # of this module.
        tgrip, thead = tf.get("grip"), tf.get("head")
        if tgrip is not None and thead is not None:
            gi = grips[i - 1] + f * (grips[i] - grips[i - 1])
            midpoint = (np.array(tgrip, dtype=float) + np.array(thead, dtype=float)) / 2.0
            errs_line.append(_perp_dist_to_line(midpoint, gi, hi))

    out = []
    if errs_th:
        rms = math.degrees(float(np.sqrt(np.mean(np.square(errs_th)))))
        out.append(_check("truth.theta_rms_deg", rms < 3.0, round(rms, 2), "<3 deg"))
        med_head = float(np.median(errs_head))
        out.append(_check("truth.head_median_px", med_head < 25, round(med_head, 1),
                          "<25 px"))
    if errs_line:
        med_line = float(np.median(errs_line))
        out.append(_check("truth.line_dist", med_line < LINE_DIST_THRESHOLD_PX,
                          round(med_line, 2),
                          f"<{LINE_DIST_THRESHOLD_PX} px (provisional)",
                          severity="warn",
                          distances_px=[round(float(x), 2) for x in errs_line]))
    ev = truth.get("events", {})
    if ev:
        t0 = ev.get("t0_us", 0)
        phases = {p["phase"]: p["t_us"] for p in run.analysis.get("phases", [])}

        # P-system event keys → analyzer Phase enum (swing_analysis.h):
        #   p1→Address(0) p3→MidBackswing(8) p4→Top(2) p5→ArmParallelDown(13)
        #   p6→Delivery(9) p7→Impact(5) p8→ShaftParallelThrough(14)
        #   p9→FollowThrough(11) p10→Finish(7).
        # p5/p8 got a real analyzer event once the segmenter started emitting
        # ArmParallelDown/ShaftParallelThrough directly (previously proxied via
        # Downswing(4)/Release(6), which were never truth-checked here). p2
        # (ShaftParallelBack) still has none — see the parallel check below,
        # which stays as the fallback for p2 and for any doc predating this
        # segmenter change (p5/p8 event checks simply won't fire on those).
        # (key, enum, label, tol_s, severity)
        P_CHECKS = [
            ("p1",  0,  "p1_address",       0.05, "warn"),
            ("p3",  8,  "p3_armpar_back",   0.06, "warn"),
            ("p4",  2,  "p4_top",           0.03, "fail"),
            ("p5",  13, "p5_armpar_down",   0.04, "warn"),
            ("p6",  9,  "p6_delivery",      0.04, "warn"),
            ("p7",  5,  "p7_impact",        0.03, "warn"),  # impact is often an input, not output
            ("p8",  14, "p8_shaftpar_fwd",  0.05, "warn"),
            ("p9",  11, "p9_armpar_fwd",    0.08, "warn"),
            ("p10", 7,  "p10_finish",       0.12, "warn"),
        ]
        # Legacy vocabulary (pre-P fixtures); applied only if the P-key for that
        # enum was not present, so we never double-check one phase.
        LEGACY = [("takeaway", 1, "takeaway", 0.08, "warn"),
                  ("top",      2, "top",      0.03, "fail"),
                  ("finish",   7, "finish",   0.12, "warn")]

        checked = set()
        for key, ph, name, tol, sev in P_CHECKS:
            want = ev.get(f"{key}_s")
            if want is None or ph not in phases:
                continue
            err = abs((phases[ph] - t0) / 1e6 - want)
            out.append(_check(f"truth.event_{name}_s", err <= tol, round(err, 3), f"<={tol}s", severity=sev))
            checked.add(ph)
        for key, ph, name, tol, sev in LEGACY:
            if ph in checked:
                continue
            want = ev.get(f"{key}_s")
            if want is None or ph not in phases:
                continue
            err = abs((phases[ph] - t0) / 1e6 - want)
            out.append(_check(f"truth.event_{name}_s", err <= tol, round(err, 3), f"<={tol}s", severity=sev))

        # Parallel-geometry consistency (label sanity): at P2/P6/P8 the shaft is
        # ~parallel to the ground, so the labelled club angle should be ~horizontal
        # in a face-on frame. theta = atan2(dy, dx); |sin(theta)| is the vertical
        # fraction → asin gives degrees off horizontal (0=flat, 90=vertical).
        shaft_by_tus = {f["t_us"]: f for f in truth.get("shaft", [])}
        for pkey in ("p2", "p6", "p8"):
            wsec = ev.get(f"{pkey}_s")
            if wsec is None or not shaft_by_tus:
                continue
            tus = t0 + int(round(wsec * 1e6))
            nearest = min(shaft_by_tus, key=lambda t: abs(t - tus))
            if abs(nearest - tus) > 30000:          # no labelled club within 30 ms of this P
                continue
            off_deg = math.degrees(math.asin(min(1.0, abs(math.sin(shaft_by_tus[nearest]["theta"])))))
            out.append(_check(f"truth.parallel_{pkey}_deg", off_deg <= 20.0, round(off_deg, 1),
                              "<=20 deg (shaft ~horizontal)", severity="warn"))
    return out


def diagnosis_metrics(run: RunResult, swing: Swing):
    """Tier-2-Ext — known-groups diagnosis. Only contributes checks when the offline
    analyzer emitted a Tier-2 assessment block (ShotAnalysisJob::runAssessment ⇒
    analysis.assessment.findings[]) AND the swing declares a known group in its truth.json
    meta (`knownGroup`: a scripted fault id like "cast"/"flip", or "clean"/"control").

    No label ⇒ no checks (returns []), so swings without a ground-truth group do NOT shift
    the 100-point normalisation — the additive-reader contract (validation doc §6.4/§8).
    A clean control must raise no confident fault (specificity); a scripted-fault swing must
    flag the matching fault (recall), matched by finding id or a name substring."""
    assessment = run.analysis.get("assessment")
    if not assessment:
        return []
    meta = swing.truth_meta()
    known = meta.get("knownGroup", meta.get("known_group"))
    if not known:
        return []

    findings = assessment.get("findings", [])
    faults = [f for f in findings if f.get("severity") == "fault"]
    label = str(known).lower()

    if label in ("clean", "control", "none", "neutral"):
        # Specificity: a clean control must raise no CONFIDENT false fault. A low-confidence
        # finding is demoted (not dropped) by the engine and is tolerable noise on a clean swing.
        confident = [f for f in faults if not f.get("lowConfidence", False)]
        return [_check("diag.clean_no_fault", len(confident) == 0, len(confident),
                       "0 confident faults")]
    # Recall: the scripted fault must be SURFACED (matched by id or name substring), even if the
    # engine demoted it to low confidence — "did the engine see it at all?".
    ids = {str(f.get("id", "")).lower() for f in faults}
    names = [str(f.get("name", "")).lower() for f in faults]
    hit = (label in ids) or any(label in n for n in names)
    return [_check("diag.recall", hit, label, "fault surfaced")]


def filter_metrics(run: RunResult):
    """Tier-2-Ext — orientation-filter quality. Only contributes when offline re-fusion ran
    (filter.refuse ⇒ analysis.filter). NOTE: after C3, filter.* is ALSO observable through the
    existing checks — `xmodal.imu_vision_corr` (filter → IMU shaft angle vs vision) on camera
    swings, and `diag.*` (filter → wrist angles → findings) on labelled swings — so a filter sweep
    is not blind even without this group.

    impact-continuity: the orientation discontinuity across the impact window beyond what the gyro
    predicts (validation §5.3.1). PROVISIONAL — emitted as a `warn` until calibrated/validated on a
    real swing with an actual impact shock (the synthetic corpus models no impact saturation, so it
    cannot exercise the blanking/saturation gates). Treat the value as advisory until then."""
    f = run.analysis.get("filter")
    if not f or "impactStepDeg" not in f:
        return []
    step = f["impactStepDeg"]
    # Provisional threshold (validation §5.3.1) — re-seat on the real corpus before promoting to fail.
    return [_check("filter.impact_continuity", step is not None and step < 12.0,
                   round(step, 2) if step is not None else None, "<12 deg (provisional)",
                   severity="warn")]


def score_metrics(run: RunResult, swing: Swing):
    """Tier-2-Ext — wrist resemblance score construction (design §B.0a/§B.7, A.5 #12/#13/#18).
    Reads analysis.score (the /3 ScoreBreakdown object; /2 docs carried a bare int and are skipped).
    These are INTERNAL-CONSISTENCY checks of the construction — robust to the (frozen) tuned values:
    each R_p bounded, the headline matches the penalty-based assessment score (`scoreV2`) when present,
    the surfaced label is the resemblance argmax, and — *when an interval is present* — it brackets the
    central score (`0<=lo<=overall<=hi`). The live wrist headline is the assessment score, which has no
    interval model yet, so its interval is absent (a `warn`, not a fail); the resemblance FE-budget
    interval is cleared once the assessment score becomes `overall` (see wrist_analyzer.cpp). When
    truth.json meta declares an `archetype`, also checks resemblance recall (the surfaced pattern
    matches the scripted archetype)."""
    score = run.analysis.get("score")
    if not isinstance(score, dict) or score.get("kind") != "resemblance":
        return []   # /2 int score, or an adherence (Swing/GRF) score — nothing to check here
    res = score.get("resemblance", {}) or {}
    if not res:
        return []   # empty resemblance map = no wrist score was produced (camera-only / IMU-less
                    # swing): there is no construction to validate, and — like diag/filter when their
                    # data is absent — it must not shift the 100-pt normalisation (additive-reader §8).
    checks = []
    vals = [int(v) for v in res.values()]
    overall = score.get("overall")

    checks.append(_check("score.resemblance_bounded", bool(vals) and all(0 <= v <= 100 for v in vals),
                         vals, "each R_p in [0,100]"))
    if vals:
        assessment = run.analysis.get("assessment", {})
        score_v2 = assessment.get("scoreV2")
        if score_v2 is not None:
            checks.append(_check("score.headline_matches_assessment", overall == score_v2, overall, f"== scoreV2 ({score_v2})"))
        argmax = max(res.items(), key=lambda kv: int(kv[1]))[0]
        checks.append(_check("score.pattern_is_argmax", score.get("pattern") == argmax,
                             score.get("pattern"), argmax))
        srt = sorted(vals, reverse=True)
        gap = (srt[0] - srt[1]) if len(srt) >= 2 else 999
        # Advisory: if flagged blended the top-two must be close (lenient — the exact delta is a
        # frozen, overridable engine constant, so this is a sanity check, not an equality).
        checks.append(_check("score.blended_sane", (not score.get("blended", False)) or gap <= 20,
                             {"blended": score.get("blended"), "gap": gap}, "blended ⇒ gap<=20",
                             severity="warn"))

    iv = score.get("interval")
    if isinstance(iv, dict):
        lo, hi, hw = iv.get("lo"), iv.get("hi"), iv.get("halfWidth")
        ok = (None not in (lo, hi, hw, overall)
              and 0 <= lo <= overall <= hi <= 100 and hw >= 0)
        checks.append(_check("score.interval_valid", ok, iv, "0<=lo<=overall<=hi<=100"))
    else:
        checks.append(_check("score.interval_present", False, None, "interval present", severity="warn"))

    arch = swing.truth_meta().get("archetype")
    if arch:
        checks.append(_check("score.resemblance_recall",
                             str(score.get("pattern", "")).lower() == str(arch).lower(),
                             score.get("pattern"), str(arch)))
    return checks


def scorecard(run_dir, swing_dir):
    run = RunResult(run_dir)
    swing = Swing(swing_dir)
    conditions = swing.truth_meta()
    checks = (invariants(run, conditions.get("scope"))
              + truth_metrics(run, swing)
              + diagnosis_metrics(run, swing)
              + filter_metrics(run)
              + score_metrics(run, swing))
    fails = [c for c in checks if not c["pass"] and c["severity"] == "fail"]
    warns = [c for c in checks if not c["pass"] and c["severity"] == "warn"]
    score = round(100.0 * sum(c["pass"] for c in checks) / max(len(checks), 1))
    return {
        "swing": swing.name,
        "score": score,
        "ok": run.meta.get("ok", False),
        "frames": run.meta.get("frames"),
        "analyzeMs": run.meta.get("analyzeMs"),
        "conditions": conditions,   # markup meta: scope/tempo/contact/club/shaft/lighting
        "sha": git_sha(),
        "failures": [c["name"] for c in fails],
        "warnings": [c["name"] for c in warns],
        "checks": checks,
    }
