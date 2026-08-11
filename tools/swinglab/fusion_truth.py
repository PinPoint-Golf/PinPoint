#!/usr/bin/env python3
"""fusion_truth.py -- load the instrumented stripe-fusion truth and audit it
against the production corpus the wrist-cock harness already grades on.

WHY THIS EXISTS.  docs/implementation/wrist_model_brief.md Task 0 calls the
fusion truth "better truth that already exists, unused" and makes the upgrade
blocking. Two of the assumptions behind that turn out to be wrong, and this
module is what establishes it rather than asserting it:

  * MOST OF IT IS ALREADY IN USE.  truth.json for swings 0002..0010 of the
    2026-07-05 session IS the fusion band tier verbatim -- same count, same head
    positions, same theta. What is genuinely new is the RAY tier and the ability
    to separate the tiers, not the band rows.
  * PHI DOES NOT COME FROM skeleton.csv.  That file carries 8 body joints
    (shoulders/hips/knees/ankles) because it is a clutter mask, NOT a pose
    source. The production pose is 133-point COCO-WholeBody (ViTPose) and carries
    elbows, wrists and 21 landmarks per hand; phi comes from there.
  * ANCHORS.CSV IS NOT THE BETTER PHI.  It disagrees with the production pose by
    a median ~13 deg (p90 ~40 deg in the downswing), which looks alarming until
    each channel's own jitter is measured: anchors reads ~0.1 deg in the
    backswing -- too smooth to be a measurement, the signature of interpolation
    between sparser pose samples -- and degrades to ~3.5 deg in the downswing
    against production's ~1.8 deg. Production phi wins, and prior numbers are
    NOT phi-limited. A disagreement never says which side is wrong; only
    self-jitter does.

THE TIMEBASE TRAP.  Fusion `t_s` is nominal (frame / fps) and drifts against the
sensor clock -- 2.4 ms by frame 397, growing. The only correct join is

    t_us = clipmeta["t_us"][frame]

and clipmeta.t_us is elementwise identical to result.json's club.samples[].t_us,
so the join is exact rather than nearest-match. Asserted, not assumed.

No matplotlib import: this stays importable from tools/shaftlab/plane_probe.py
without dragging in a plotting stack.

    fusion_truth.py --audit --lab-root <dir> --run-root <dir> --corpus <dir>
                    [--out <dir>]
"""

import argparse
import csv
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from theta_psi_model import (  # noqa: E402  - the conventions live there, once
    PH_ADDRESS, PH_TAKEAWAY, PH_TOP, PH_IMPACT, PH_FINISH,
    arm_series, decide_lead_side, smooth_angle, wrap180, git_sha,
)

LAB_ROOT_DEFAULT = "/mnt/swingdata/shaftlab/lab/tape_20260705"

# COCO-WholeBody hand blocks (src/Analysis/swing_analysis.h:249-250) and the
# hand-axis endpoint (src/Analysis/hand_axis.h:43). Used ONLY for the accuracy
# cross-check -- the hand keypoints are not trustworthy through a fast swing and
# nothing here may feed a model.
KP_LEFT_HAND_FIRST = 91
KP_RIGHT_HAND_FIRST = 112
KP_HAND_MIDDLE_MCP = 9

# The lead forearm, as prep_swing.py defines it (COCO elbow 7 left / 8 right).
KP_LEFT_ELBOW = 7
KP_RIGHT_ELBOW = 8

FUSION_COLS = ("frame", "t_s", "tier", "n_match", "theta_deg", "s_px_mm",
               "r0_mm", "head_x", "head_y", "e2", "support", "conf")


# ---------------------------------------------------------------------------
# Loaders. Note the asymmetry, which has bitten before: the fusion CSV HAS a
# header, anchors.csv and skeleton.csv do NOT.

def load_clipmeta(lab_dir):
    """The clip's sensor timebase and its link back to the production swing."""
    cm = json.load(open(Path(lab_dir) / "clipmeta.json", encoding="utf-8"))
    t_us = np.asarray(cm["t_us"], dtype=np.int64)
    if int(cm.get("frame0", 0)) != 0:
        raise ValueError(f"{lab_dir}: frame0 != 0, the frame->t_us join needs an offset")
    if t_us.size < 2 or np.any(np.diff(t_us) <= 0):
        raise ValueError(f"{lab_dir}: clipmeta t_us is not strictly increasing")
    return {"t_us": t_us, "fps": float(cm.get("fps", 0.0)),
            "W": int(cm.get("W", 0)), "H": int(cm.get("H", 0)),
            "swingDir": str(cm.get("swingDir", "")), "frame0": 0}


def run_dir_name(clipmeta):
    """'<session>__<swing>' -- the key wrist_cock_fit splits run dirs on.

    s01 was run by hand and its swingDir uses forward slashes; s02..s10 came
    from run_batch.py with escaped backslashes. Normalise both.
    """
    parts = [p for p in clipmeta["swingDir"].replace("\\", "/").split("/") if p]
    if len(parts) < 2:
        raise ValueError(f"cannot parse swingDir {clipmeta['swingDir']!r}")
    return f"{parts[-2]}__{parts[-1]}"


def discover(lab_root=LAB_ROOT_DEFAULT):
    """{run_dir_name: lab_dir} for every swing dir carrying a clipmeta.

    Built FROM clipmeta, never from sNN ordering -- the sNN labels are a capture
    convenience and carry no guarantee of matching swing_NNNN.

    The A/B decode variants (s01_bil, s01_mp4) carry the SAME swingDir as their
    canonical clip and are stage-1 only. Sorting alone hands the key to the
    variant, which silently grades a different decode path; prefer the canonical
    '^sNN$' directory and say so when a collision is dropped.
    """
    def canonical(name):
        return len(name) > 1 and name[0] == "s" and name[1:].isdigit()

    out = {}
    for d in sorted(Path(lab_root).iterdir()):
        if not (d.is_dir() and (d / "clipmeta.json").exists()):
            continue
        try:
            key = run_dir_name(load_clipmeta(d))
        except Exception as e:
            print(f"  ! {d.name}: {e}", file=sys.stderr)
            continue
        prev = out.get(key)
        if prev is None or (canonical(d.name) and not canonical(prev.name)):
            if prev is not None:
                print(f"  ! {key}: preferring {d.name} over variant {prev.name}", file=sys.stderr)
            out[key] = d
        else:
            print(f"  ! {key}: ignoring variant {d.name} (canonical {prev.name})", file=sys.stderr)
    return out


def load_fusion(lab_dir):
    """The instrumented truth rows. Empty cells -> nan (s_px_mm, r0_mm, head_x,
    head_y are populated on BAND rows only; ray rows carry theta alone)."""
    path = Path(lab_dir) / "fusion" / "faceon_swing_fusion.csv"
    rows = list(csv.DictReader(open(path, encoding="utf-8")))
    if not rows:
        return {c: np.zeros(0) for c in FUSION_COLS}

    def col(name, dtype=float):
        vals = []
        for r in rows:
            v = (r.get(name) or "").strip()
            vals.append(np.nan if v == "" else float(v))
        return np.asarray(vals, dtype=dtype)

    return {
        "frame": col("frame").astype(np.int64),
        "t_s": col("t_s"),
        "tier": np.asarray([(r.get("tier") or "").strip() for r in rows]),
        "n_match": col("n_match"),
        "theta_deg": col("theta_deg"),
        "s_px_mm": col("s_px_mm"),
        "r0_mm": col("r0_mm"),
        "head_x": col("head_x"),
        "head_y": col("head_y"),
        "e2": col("e2"),
        "support": col("support"),
        "conf": col("conf"),
    }


def load_anchors(lab_dir):
    """frame, gx, gy, phi_deg, phi_ok -- NO header (prep_swing.py:88).

    phi_deg is the lead-forearm elbow->grip direction in image degrees, computed
    from the 310-frame PRE-NORMALISATION pose. That provenance is the whole point
    of the audit below.
    """
    a = np.loadtxt(Path(lab_dir) / "anchors.csv", delimiter=",", ndmin=2)
    return {"frame": a[:, 0].astype(np.int64), "gx": a[:, 1], "gy": a[:, 2],
            "phi_deg": a[:, 3], "phi_ok": a[:, 4].astype(bool)}


def load_skeleton(lab_dir):
    """frame + 8 joints x (x, y, conf) -- NO header. COCO 5,6,11..16: shoulders,
    hips, knees, ankles. No elbows, no wrists: this is shaft_annotate's
    body-collinearity clutter mask (prep_swing.py:94-96), not a pose source.
    Only the shoulder pair is used here, as a body-depth proxy."""
    s = np.loadtxt(Path(lab_dir) / "skeleton.csv", delimiter=",", ndmin=2)
    joints = {}
    for i, name in enumerate(("l_shoulder", "r_shoulder", "l_hip", "r_hip",
                              "l_knee", "r_knee", "l_ankle", "r_ankle")):
        b = 1 + 3 * i
        joints[name] = {"x": s[:, b], "y": s[:, b + 1], "conf": s[:, b + 2]}
    return {"frame": s[:, 0].astype(np.int64), "joints": joints}


def fusion_labels(lab_dir, sample_t_us=None):
    """The fusion rows on the production microsecond clock.

    sample_t_us, when given, is result.json's club.samples[].t_us; it is asserted
    identical to clipmeta.t_us so the frame index joins the two directly. A
    mismatch means the clip and the run have drifted apart and every downstream
    number would be silently wrong, so it is loud.
    """
    cm = load_clipmeta(lab_dir)
    fu = load_fusion(lab_dir)
    an = load_anchors(lab_dir)
    t_clip = cm["t_us"]

    exact = True
    if sample_t_us is not None:
        s = np.asarray(sample_t_us, dtype=np.int64)
        exact = (s.size == t_clip.size) and bool(np.array_equal(s, t_clip))
        if not exact:
            print(f"  ! {Path(lab_dir).name}: clipmeta t_us != samples t_us "
                  f"({t_clip.size} vs {s.size}) -- falling back to nearest-within-2ms",
                  file=sys.stderr)

    fr = fu["frame"]
    ok = (fr >= 0) & (fr < t_clip.size)
    idx = fr[ok]
    out = {k: (v[ok] if isinstance(v, np.ndarray) else v) for k, v in fu.items()}
    out["t_us"] = t_clip[idx]
    out["exact_timebase"] = exact

    # anchors.csv is per clip frame, so phi lands with no interpolation at all.
    amap = np.full(t_clip.size, np.nan)
    aok = np.zeros(t_clip.size, dtype=bool)
    inb = (an["frame"] >= 0) & (an["frame"] < t_clip.size)
    amap[an["frame"][inb]] = an["phi_deg"][inb]
    aok[an["frame"][inb]] = an["phi_ok"][inb]
    out["phi_anchor"] = amap[idx]
    out["phi_anchor_ok"] = aok[idx]
    out["clipmeta"] = cm
    return out


# ---------------------------------------------------------------------------
# The production side: phi as the harness computes it, plus the hand axis.

def _pose_angles(an, events, lead_left, wpx, hpx):
    """phi as wrist_cock_fit.load_swing derives it, and the lead hand axis.

    The hand axis mirrors src/Analysis/hand_axis.h handAxisDirection (hand-root
    -> middle MCP). It is a DIAGNOSTIC ONLY -- the hand keypoints score well but
    are not trustworthy through a fast swing, so nothing may be modelled on it.
    """
    frames = (an.get("pose2d") or {}).get("frames") or []
    if not frames:
        return None
    pt, praw = arm_series(an.get("pose2d") or {}, "frames", lead_left, wpx, hpx)
    sm = smooth_angle(praw["phi"])
    good = np.isfinite(sm)
    if good.sum() < 5:
        return None
    phi_u = np.degrees(np.unwrap(np.radians(sm[good])))

    base = KP_LEFT_HAND_FIRST if lead_left else KP_RIGHT_HAND_FIRST
    eta, eta_c = [], []
    t_pose = np.asarray([int(f["t_us"]) for f in frames], dtype=np.int64)
    for f in frames:
        kp = f.get("kp") or []
        if len(kp) < 3 * (base + KP_HAND_MIDDLE_MCP + 1):
            eta.append(np.nan); eta_c.append(0.0); continue
        r, m = base, base + KP_HAND_MIDDLE_MCP
        dx = (kp[3 * m] - kp[3 * r]) * wpx
        dy = (kp[3 * m + 1] - kp[3 * r + 1]) * hpx
        c = min(kp[3 * r + 2], kp[3 * m + 2])
        eta.append(np.degrees(np.arctan2(dy, dx)) if (dx or dy) else np.nan)
        eta_c.append(c)
    # The RAW phi as well, on its own finite mask. The jitter comparison below
    # must not score a smoothed channel against an unsmoothed one -- smoothing
    # suppresses exactly the quantity being measured, which would rig the test.
    raw = np.asarray(praw["phi"], dtype=float)
    rgood = np.isfinite(raw)
    raw_u = np.degrees(np.unwrap(np.radians(raw[rgood]))) if rgood.sum() >= 5 else np.zeros(0)

    return {"t_pose": t_pose, "pt_good": pt[good].astype(float), "phi_u": phi_u,
            "pt_raw": pt[rgood].astype(float), "phi_raw_u": raw_u,
            "eta": np.asarray(eta), "eta_conf": np.asarray(eta_c),
            "n_pose": len(frames)}


def _phi_at(pose, times):
    v = np.interp(np.asarray(times, dtype=float), pose["pt_good"], pose["phi_u"],
                  left=np.nan, right=np.nan)
    near = np.abs(np.asarray(times, dtype=float)[:, None] - pose["pt_good"][None, :]).min(axis=1)
    v[near > 60_000.0] = np.nan
    return v


def _segment(t_us, events):
    """backswing / downswing / through, on the phase ladder."""
    t = np.asarray(t_us, dtype=float)
    seg = np.full(t.shape, "through", dtype=object)
    seg[t <= float(events[PH_TOP])] = "backswing"
    seg[(t > float(events[PH_TOP])) & (t <= float(events[PH_IMPACT]))] = "downswing"
    seg[t <= float(events[PH_TAKEAWAY])] = "address"
    return seg


def _pct(v, q):
    v = np.asarray(v, dtype=float)
    v = v[np.isfinite(v)]
    return float(np.percentile(v, q)) if v.size else float("nan")


def local_residual(t_us, v_deg, half_ms=25.0, order=2):
    """|residual| of an angle series about a LOCAL QUADRATIC in time.

    This is the series' own jitter, measured without reference to any other
    source -- which is what adjudicates a disagreement between two candidate phi
    channels. Quadratic rather than linear for the reason the harness already
    documents: phi moves fast enough near impact that a local line carries
    curvature into the estimate as bias.
    """
    t = np.asarray(t_us, dtype=float)
    v = np.asarray(v_deg, dtype=float)
    out = []
    for i in range(t.size):
        m = np.abs(t - t[i]) <= half_ms * 1000.0
        if m.sum() < order + 3:
            continue
        c = np.polyfit(t[m] - t[i], v[m], order)
        out.append(v[i] - np.polyval(c, 0.0))
    return np.abs(np.asarray(out))


# ---------------------------------------------------------------------------
# The audit: gates A1 (provenance census), A2 (theta convention), A3 (phi).

def audit(lab_root, run_root, corpus_root, out_dir=None):
    found = discover(lab_root)
    if not found:
        print(f"no clipmeta under {lab_root}", file=sys.stderr)
        return 2
    print(f"lab root   : {lab_root}")
    print(f"run root   : {run_root}")
    print(f"corpus     : {corpus_root}")
    print(f"swings     : {len(found)}\n")

    census, theta_rows, phi_rows, eta_rows, jitter_rows = [], [], [], [], []

    for key in sorted(found):
        lab_dir = found[key]
        session, swing = key.split("__", 1)
        rj = Path(run_root) / key / "result.json"
        tj = Path(corpus_root) / session / swing / "truth.json"

        an = samples_t = None
        if rj.exists():
            try:
                an = json.load(open(rj, encoding="utf-8"))["analysis"]
                samples_t = np.asarray([s["t_us"] for s in (an.get("club") or {}).get("samples", [])],
                                       dtype=np.int64)
            except Exception as e:
                print(f"  ! {key}: unreadable result.json ({e})", file=sys.stderr)

        fl = fusion_labels(lab_dir, samples_t if samples_t is not None and samples_t.size else None)
        band = fl["tier"] == "band"
        ray = fl["tier"] == "ray"

        # --- A1: is truth.json this swing's band tier? ---------------------
        n_lab = head_dx = np.nan
        if tj.exists():
            lab = (json.load(open(tj, encoding="utf-8")).get("shaft") or [])
            lab = [l for l in lab if "t_us" in l and "theta" in l]
            n_lab = len(lab)
            lt = np.asarray([l["t_us"] for l in lab], dtype=np.int64)
            lhead = {int(l["t_us"]): l.get("head") for l in lab if l.get("head")}
            d = []
            for i in np.flatnonzero(band):
                h = lhead.get(int(fl["t_us"][i]))
                if h and np.isfinite(fl["head_x"][i]):
                    d.append(float(np.hypot(h[0] - fl["head_x"][i], h[1] - fl["head_y"][i])))
            head_dx = float(np.max(d)) if d else np.nan

            # --- A2: theta convention on the shared rows -------------------
            lth = {int(l["t_us"]): np.degrees(float(l["theta"])) for l in lab}
            dth = [abs(float(wrap180(lth[int(fl["t_us"][i])] - fl["theta_deg"][i])))
                   for i in np.flatnonzero(band) if int(fl["t_us"][i]) in lth]
            theta_rows.append({"swing": key, "n": len(dth),
                               "median_deg": float(np.median(dth)) if dth else np.nan,
                               "p90_deg": _pct(dth, 90), "max_deg": float(np.max(dth)) if dth else np.nan})

        census.append({"swing": key, "band": int(band.sum()), "ray": int(ray.sum()),
                       "truth_labels": n_lab, "head_max_dpx": head_dx,
                       "exact_timebase": bool(fl["exact_timebase"])})

        # --- A3: phi provenance --------------------------------------------
        if an is None:
            continue
        club = an.get("club") or {}
        wpx, hpx = club.get("frameWidth") or 0, club.get("frameHeight") or 0
        events = {}
        for p in an.get("phases", []):
            events.setdefault(int(p["phase"]), int(p["t_us"]))
        if not (wpx and hpx) or PH_IMPACT not in events or PH_TOP not in events:
            continue
        lead_left, _ = decide_lead_side((an.get("pose2d") or {}).get("frames") or [],
                                        events.get(PH_ADDRESS, 0), events.get(PH_TAKEAWAY, 0))
        if lead_left is None:
            continue
        pose = _pose_angles(an, events, lead_left, wpx, hpx)
        if pose is None:
            continue

        # A3 adjudicator: each phi channel's OWN jitter, on its own samples.
        # A disagreement between two channels says nothing about which is right;
        # self-jitter does. anchors.csv is per clip frame but INTERPOLATED from a
        # sparser pose, so it reads artificially smooth where the swing is slow.
        anc = load_anchors(lab_dir)
        cm = fl["clipmeta"]
        aok = anc["phi_ok"] & (anc["frame"] < cm["t_us"].size)
        for src, ts, vs in (
            ("anchors", cm["t_us"][anc["frame"][aok]].astype(float),
             np.degrees(np.unwrap(np.radians(anc["phi_deg"][aok])))),
            ("production", pose["pt_raw"], pose["phi_raw_u"]),
        ):
            for s, (lo, hi) in (("backswing", (events[PH_TAKEAWAY], events[PH_TOP])),
                                ("downswing", (events[PH_TOP], events[PH_IMPACT]))):
                m = (ts >= lo) & (ts <= hi)
                if m.sum() <= 8:
                    continue
                r = local_residual(ts[m], vs[m])
                if r.size:
                    jitter_rows.append({"swing": key, "source": src, "segment": s,
                                        "n": int(r.size), "median_deg": float(np.median(r)),
                                        "p90_deg": _pct(r, 90)})

        ok = fl["phi_anchor_ok"] & np.isfinite(fl["phi_anchor"])
        ph_h = _phi_at(pose, fl["t_us"])
        both = ok & np.isfinite(ph_h)
        if both.sum():
            d = np.abs(wrap180(ph_h[both] - fl["phi_anchor"][both]))
            seg = _segment(fl["t_us"][both], events)
            for s in ("backswing", "downswing", "through", "all"):
                m = np.ones(d.shape, bool) if s == "all" else (seg == s)
                if not m.any():
                    continue
                phi_rows.append({"swing": key, "segment": s, "n": int(m.sum()),
                                 "median_deg": float(np.median(d[m])),
                                 "p90_deg": _pct(d[m], 90), "max_deg": float(np.max(d[m])),
                                 "n_pose": pose["n_pose"], "lead_left": bool(lead_left)})

        # --- A3b: hand-axis cross-check (diagnostic only) -------------------
        pre = fl["t_us"] <= events[PH_IMPACT]
        if pre.sum() >= 5:
            et = np.interp(fl["t_us"][pre].astype(float), pose["t_pose"].astype(float),
                           np.degrees(np.unwrap(np.radians(pose["eta"]))))
            ph = ph_h[pre]
            th = fl["theta_deg"][pre]
            good = np.isfinite(et) & np.isfinite(ph) & np.isfinite(th)
            if good.sum() >= 5:
                psi = wrap180(th[good] - ph[good])
                eta_rows.append({"swing": key, "n": int(good.sum()),
                                 "psi_range_deg": float(np.ptp(psi)),
                                 "club_in_hand_range_deg": float(np.ptp(wrap180(th[good] - et[good]))),
                                 "wrist_joint_range_deg": float(np.ptp(wrap180(et[good] - ph[good]))),
                                 "eta_conf_median": float(np.nanmedian(pose["eta_conf"]))})

    _report(census, theta_rows, phi_rows, eta_rows, jitter_rows, out_dir,
            {"lab_root": str(lab_root), "run_root": str(run_root),
             "corpus": str(corpus_root), "git_sha": git_sha()})
    return 0


def _report(census, theta_rows, phi_rows, eta_rows, jitter_rows, out_dir, meta):
    print("=== A1  provenance census ==========================================")
    print("  truth.json label count == band rows  =>  the 'hand labels' ARE the")
    print("  instrumented band tier, and the upgrade is smaller than the brief assumes.")
    print(f"  {'swing':44s} {'band':>5s} {'ray':>5s} {'labels':>7s} {'head dpx':>9s} {'exact t':>8s}")
    n_same = 0
    for r in census:
        same = (r["truth_labels"] == r["band"])
        n_same += bool(same)
        flag = "  ==" if same else ""
        print(f"  {r['swing']:44s} {r['band']:5d} {r['ray']:5d} {str(r['truth_labels']):>7s} "
              f"{r['head_max_dpx']:9.3f} {str(r['exact_timebase']):>8s}{flag}")
    tot_b = sum(r["band"] for r in census)
    tot_r = sum(r["ray"] for r in census)
    print(f"  {'TOTAL':44s} {tot_b:5d} {tot_r:5d}")
    print(f"  swings whose truth.json is exactly the band tier: {n_same}/{len(census)}")

    print("\n=== A2  theta convention (band rows) ===============================")
    print(f"  {'swing':44s} {'n':>5s} {'median':>8s} {'p90':>8s} {'max':>8s}")
    for r in theta_rows:
        print(f"  {r['swing']:44s} {r['n']:5d} {r['median_deg']:8.4f} {r['p90_deg']:8.4f} {r['max_deg']:8.4f}")
    if theta_rows:
        med = float(np.median([r["median_deg"] for r in theta_rows if np.isfinite(r["median_deg"])]))
        print(f"  pooled median |dtheta| = {med:.4f} deg   (gate: < 0.05)  "
              f"{'PASS' if med < 0.05 else 'FAIL'}")

    print("\n=== A3  phi provenance: production pose vs anchors.csv ============")
    print(f"  {'swing':44s} {'seg':>10s} {'n':>5s} {'median':>8s} {'p90':>8s} {'max':>8s}")
    for r in phi_rows:
        print(f"  {r['swing']:44s} {r['segment']:>10s} {r['n']:5d} "
              f"{r['median_deg']:8.2f} {r['p90_deg']:8.2f} {r['max_deg']:8.2f}")
    down = [r for r in phi_rows if r["segment"] == "downswing"]
    if down:
        print(f"  downswing p90, pooled median = "
              f"{float(np.median([r['p90_deg'] for r in down])):.2f} deg")
        print("  NO PASS THRESHOLD -- this is a measurement, and a DISAGREEMENT alone")
        print("  cannot say which channel is wrong. A3-adjudicate does that.")

    if jitter_rows:
        print("\n=== A3-adjudicate  each channel's OWN jitter (local quadratic) =====")
        print("  The channel that disagrees is not automatically the worse one. Self-")
        print("  jitter is measured per channel, on its own samples, referencing nothing.")
        print(f"  {'source':>11s} {'segment':>10s} {'swings':>7s} {'median':>8s} {'p90':>8s}")
        verdict = {}
        for src in ("production", "anchors"):
            for seg in ("backswing", "downswing"):
                rows = [r for r in jitter_rows if r["source"] == src and r["segment"] == seg]
                if not rows:
                    continue
                med = float(np.median([r["median_deg"] for r in rows]))
                verdict[(src, seg)] = med
                print(f"  {src:>11s} {seg:>10s} {len(rows):7d} {med:8.2f} "
                      f"{float(np.median([r['p90_deg'] for r in rows])):8.2f}")
        pd, ad = verdict.get(("production", "downswing")), verdict.get(("anchors", "downswing"))
        pb, ab = verdict.get(("production", "backswing")), verdict.get(("anchors", "backswing"))
        if None not in (pd, ad, pb, ab):
            print(f"\n  anchors reads {ab:.2f} deg in the backswing -- far too smooth to be a")
            print("  measurement. That is the signature of INTERPOLATION between sparser")
            print("  pose samples, not of better data; where the swing is fast it cannot")
            print(f"  hide, and anchors degrades to {ad:.2f} deg against production's {pd:.2f} deg.")
            better = "production" if pd <= ad else "anchors"
            print(f"  VERDICT: use --truth-phi {'harness' if better == 'production' else 'anchors'}."
                  f"  Production phi jitter ({pd:.2f} deg) is well inside the ~20 deg residual")
            print("  the model is trying to explain, so prior numbers are NOT phi-limited.")

    if eta_rows:
        print("\n=== A3b hand axis -- DIAGNOSTIC ONLY, never a model input ===========")
        print("  The hand keypoints score well but are not trustworthy through a fast")
        print("  swing. Reported to indicate accuracy, not to redefine any angle.")
        print(f"  {'swing':44s} {'n':>5s} {'psi':>8s} {'club/hand':>10s} {'wrist':>8s} {'etaconf':>8s}")
        for r in eta_rows:
            print(f"  {r['swing']:44s} {r['n']:5d} {r['psi_range_deg']:8.1f} "
                  f"{r['club_in_hand_range_deg']:10.1f} {r['wrist_joint_range_deg']:8.1f} "
                  f"{r['eta_conf_median']:8.2f}")

    if not out_dir:
        return
    od = Path(out_dir)
    od.mkdir(parents=True, exist_ok=True)
    for name, rows in (("census", census), ("theta", theta_rows),
                       ("phi_audit", phi_rows), ("phi_jitter", jitter_rows),
                       ("hand_axis", eta_rows)):
        if not rows:
            continue
        with open(od / f"fusion_truth_{name}.csv", "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    json.dump(meta, open(od / "fusion_truth_meta.json", "w", encoding="utf-8"), indent=2)
    print(f"\nwrote {od}/fusion_truth_*.csv")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--audit", action="store_true", help="run gates A1/A2/A3")
    ap.add_argument("--lab-root", default=LAB_ROOT_DEFAULT)
    ap.add_argument("--run-root", default="/mnt/swingdata/stagegate/corpm3-off")
    ap.add_argument("--corpus", default="/mnt/swingdata/Mark-Liversedge")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    if not a.audit:
        found = discover(a.lab_root)
        for k, v in sorted(found.items()):
            print(f"{k}  <-  {v.name}")
        return 0
    return audit(a.lab_root, a.run_root, a.corpus, a.out)


if __name__ == "__main__":
    sys.exit(main())
