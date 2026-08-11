#!/usr/bin/env python3
"""theta_psi_model.py -- a corpus SHAPE MODEL for the shaft angle and the lead
arm that carries it, synthesised from every swing in a run with a confidence
band, plus the data series behind it.

The research report's psi figure plots one swing. This builds the same curves
from the WHOLE corpus: every swing is time-warped onto a common swing axis
anchored on its own event ladder (address / top / impact / finish), sign- and
datum-normalised, resampled onto a fixed grid, and reduced per grid point to
median, mean, sd and percentile band. What comes out is the corpus's typical
shape with an honest spread -- which is both a picture of the atypical swing
and a reference an individual swing can be scored against.

SEVEN SERIES, and the naming, since the report has been loose about it:

  theta      the shaft direction, from the grip -- the tracker's own theta
  phi        lead FOREARM: lead elbow -> grip. This is the production phi
             (shaft_tracker.cpp), and it is a forearm, NOT the whole arm --
             the report's "the direction of the lead arm" is imprecise
  beta       lead UPPER ARM: lead shoulder -> lead elbow
  alpha      WHOLE lead arm: lead shoulder -> grip
  psi        wrist angle, theta - phi  (the report's psi, unchanged)
  psi_alpha  the shaft against the whole-arm line, theta - alpha
  elbow      elbow opening, phi - beta (0 = a straight arm)

beta, alpha, psi_alpha and elbow are new here; the first two of the seven are
the pair the report already plots. They are carried together because the
question they answer is which arm signal predicts theta best: the residual
series (theta minus an arm direction) with the TIGHTER band is the one an
estimator has less left to explain, and the run prints that ranking.

Why the warp is anchored on events rather than on wall-clock: swings differ in
tempo, so a plain time normalisation smears the top of one swing across the
downswing of another and the band ends up measuring tempo variance rather than
shape variance. Anchoring on the ladder aligns the STRUCTURE; the anchors are
placed on the axis at the corpus-median relative time of each event, so the
picture still reads at a natural tempo.

Two normalisations are applied before averaging, both recorded in the JSON:
  * DATUM  -- each DIRECTION series is referenced to its own value at a chosen
              event (default impact), because an absolute image angle carries
              the setup, not the swing. The DIFFERENCE series are never
              re-referenced: they already mean something on their own. Mind the
              consequence -- a referenced theta minus a referenced phi is NOT
              psi, which is why the difference series are carried explicitly
              rather than left to be derived in the sheet.
  * SIGN   -- each swing is multiplied by the sign of its own downswing sweep,
              so left- and right-handed captures (and any mirrored setup)
              progress the same way on the axis. Applied to every series alike.

Handedness is never read from metadata: which side leads is decided per swing
from the pose, on the grip ordering over the address hold (the trail hand sits
below the lead hand on the grip), matching the production rule that chirality
is detected rather than configured.

Outputs, under --out:
  theta_psi_model.png        the figure: the shaft angle, the three arm
                             directions together, the theta-minus-arm
                             residuals, and the elbow -- each as median +
                             p25-p75 + p10-p90, with the event anchors marked
                             and a swing-count strip along the bottom.
  theta_psi_model.csv        the model itself -- one row per grid point, with
                             n / mean / sd / median / p10 / p25 / p75 / p90 for
                             all seven series. This is the Excel sheet.
  theta_psi_swings.csv       every swing resampled onto the same grid, wide
                             format (one column per swing per series) -- for
                             plotting individual swings against the band.
  theta_psi_deviation.csv    per swing and per series: rms / p95 / max
                             deviation from the model median, and the fraction
                             of grid points outside the p10-p90 band. The first
                             cut of the measurement-error score this is for.
  theta_psi_model.json       the model plus full provenance (run root, sha,
                             every option, the definitions above, the swing
                             list, the anchor placement) so a later estimator
                             can load it without re-deriving anything.

  theta_psi_model.py <run_root> --out <dir>
      [--swings a,b,...] [--exclude a,b,...]
      [--tier vision|measured|all] [--min-conf F]
      [--theta-ref impact|address|top|none] [--no-sign-normalise]
      [--bins N] [--max-gap F] [--min-swings N] [--warp-anchors core|ladder]
      [--pose frames|synth] [--lead-side auto|left|right]
      [--per-swing-lines] [--title T]

<run_root> is a directory of per-swing run dirs (each holding result.json), i.e.
whatever `lab.py run` or a gate run produced. Nothing here re-runs the analyzer;
it reads persisted results only, so it is safe to point at a frozen gate run.
"""

import argparse
import csv
import json
import math
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

REPO_ROOT = Path(__file__).resolve().parents[2]

# swing_analysis.h ShaftSampleFlags
F_MEASURED = 0x01
F_COASTED = 0x04
F_WEDGE = 0x08

# swing_analysis.h Phase
PH_ADDRESS, PH_TAKEAWAY, PH_TOP, PH_IMPACT, PH_FINISH = 0, 1, 2, 5, 7
PH_MIDBACK, PH_DELIVERY, PH_SHAFTPAR_BACK, PH_ARMPAR_DOWN, PH_SHAFTPAR_THRU = 8, 9, 12, 13, 14

PHASE_NAME = {
    PH_ADDRESS: "Address", PH_TAKEAWAY: "Takeaway", PH_TOP: "Top",
    PH_IMPACT: "Impact", PH_FINISH: "Finish", PH_MIDBACK: "P3 arm par (back)",
    PH_DELIVERY: "P6 delivery", PH_SHAFTPAR_BACK: "P2 shaft par (back)",
    PH_ARMPAR_DOWN: "P5 arm par (down)", PH_SHAFTPAR_THRU: "P8 shaft par (thru)",
}

# The warp skeleton. Address and Finish bound the axis; Top and Impact are the
# interior anchors every swing carries.
#
# By default nothing else is warped on, and that choice is worth understanding
# because it sets what the band MEANS. With four anchors, two swings that reach
# the top at the same fraction but get there with different takeaway tempo are
# NOT aligned in between, so the band through the backswing carries tempo
# variance as well as shape variance -- which is honest, and is why it is wide
# there. Adding the shaft- and arm-parallel ladder events as anchors
# (--warp-anchors ladder) tightens the band to pure shape, at the price of
# defining away exactly the timing differences one might want to measure. Use
# core to ask "how much do these swings differ?", ladder to ask "given the same
# ladder, how much does the SHAPE between the ladder points differ?".
CORE_WARP_PHASES = [PH_ADDRESS, PH_TOP, PH_IMPACT, PH_FINISH]
LADDER_PHASES = [PH_SHAFTPAR_BACK, PH_MIDBACK, PH_ARMPAR_DOWN, PH_DELIVERY, PH_SHAFTPAR_THRU]

# COCO body keypoints (the pose block's first 17 of 133).
KP_L_SHOULDER, KP_R_SHOULDER = 5, 6
KP_L_ELBOW, KP_R_ELBOW, KP_L_WRIST, KP_R_WRIST = 7, 8, 9, 10

# The series the model carries. "direction" series are absolute image angles:
# they are unwrapped, sign-normalised and referenced to their OWN value at the
# datum event, because an absolute image angle carries the setup rather than
# the swing. "difference" series are angles BETWEEN two measured directions:
# they are folded into (-180, 180], unwrapped along time and sign-normalised,
# but never re-referenced -- they already mean something on their own.
#
# NOTE the datum: because each direction is referenced to its own value at the
# datum event, a referenced theta MINUS a referenced phi is NOT psi. The
# difference series are computed from the raw angles before any referencing,
# which is why they are carried separately rather than derived in the sheet.
SERIES = [
    ("theta",     "shaft θ",                   "direction",  "#1f4e79", "#8fb8de"),
    ("phi",       "forearm φ  (elbow→grip)",   "direction",  "#1e7b45", "#9ed4b4"),
    ("beta",      "upper arm β  (shoulder→elbow)", "direction", "#b5651d", "#f0c08a"),
    ("alpha",     "whole arm α  (shoulder→grip)",  "direction", "#8a1f4a", "#e2a3bd"),
    ("psi",       "wrist ψ = θ − φ",           "difference", "#7d3c98", "#c9a0dc"),
    ("psi_alpha", "shaft-vs-arm ψα = θ − α",   "difference", "#2b6c8f", "#a6cfe3"),
    ("elbow",     "elbow ε = φ − β",           "difference", "#6b6b6b", "#c4c4c4"),
]
SERIES_BY_NAME = {k: (lab, kind, dark, light) for k, lab, kind, dark, light in SERIES}
DIRECTIONS = [k for k, _, kind, _, _ in SERIES if kind == "direction"]
DIFFERENCES = [k for k, _, kind, _, _ in SERIES if kind == "difference"]

# The residuals a theta-from-the-arm estimator would be built on: each is
# theta minus one arm direction, so the tighter its band, the better that arm
# signal predicts theta on its own.
PREDICTOR_RESIDUALS = {"psi": "phi", "psi_alpha": "alpha"}

PHI_CONF_MIN = 0.30      # shaft_tracker.cpp gates phi on the lead-elbow conf
PHI_MIN_LEN_PX = 8.0     # ... and on a forearm long enough to have a direction
PHI_HAMPEL_DEG = 20.0    # shaft_track_assembly.h armOutlierDeg


def git_sha():
    try:
        return subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip() or "unknown"
    except Exception:
        return "unknown"


def wrap180(d):
    return (np.asarray(d, dtype=float) + 180.0) % 360.0 - 180.0


# ---------------------------------------------------------------- phi from pose

def _kp(flat, i):
    """(x, y, conf) of keypoint i out of a flat [x,y,c]*n array."""
    return flat[3 * i], flat[3 * i + 1], flat[3 * i + 2]


def decide_lead_side(pose_frames, t_addr, t_takeaway):
    """Which physical side leads, decided from the pose alone -- no handedness
    is read from metadata, matching the production rule that chirality is
    detected per swing rather than configured.

    The cue is the grip itself: both hands hold the club, but the TRAIL hand
    sits below the lead hand on the grip, so at address (club pointing down)
    the trail wrist carries the larger image y. Voted only over the address
    hold, because once the swing starts the hands rotate about each other and
    the ordering stops meaning anything. The recorded lead-hand point breaks a
    tie -- on its own it is a coin toss, since it sits between two wrists a
    couple of centimetres apart.

    Returns (is_left, margin), the margin being the winning share, so a corpus
    that is not what it claims to be shows up instead of being averaged away."""
    votes_left = votes_right = 0
    for f in pose_frames:
        t = f.get("t_us")
        if t is None or not (t_addr - 500_000 <= t <= t_takeaway):
            continue
        kp = f.get("kp")
        if not kp:
            continue
        _, ly, lc = _kp(kp, KP_L_WRIST)
        _, ry, rc = _kp(kp, KP_R_WRIST)
        if min(lc, rc) < 0.5:
            continue
        if ry > ly:
            votes_left += 1      # right wrist lower => right trails => left leads
        elif ly > ry:
            votes_right += 1
    total = votes_left + votes_right
    if total and votes_left != votes_right:
        return votes_left > votes_right, max(votes_left, votes_right) / total

    # Tie (or nothing voted): fall back to the recorded lead-hand point.
    dl = dr = 0
    for f in pose_frames:
        lead, kp = f.get("lead"), f.get("kp")
        if not lead or not kp:
            continue
        lx, ly, lc = _kp(kp, KP_L_WRIST)
        rx, ry, rc = _kp(kp, KP_R_WRIST)
        if min(lc, rc) < 0.5:
            continue
        if math.hypot(lead[0] - lx, lead[1] - ly) < math.hypot(lead[0] - rx, lead[1] - ry):
            dl += 1
        else:
            dr += 1
    if dl + dr == 0:
        return None, 0.0
    return dl >= dr, max(dl, dr) / (dl + dr)


def arm_series(pose_block, which, lead_left, wpx, hpx):
    """Per-pose-frame directions of the lead arm's segments, on the same
    convention the tracker uses for phi: image atan2 in PIXEL space (so the
    frame's aspect ratio is respected), NaN wherever a joint is unconfident or
    a segment too short to have a direction.

      phi   -- forearm:   lead elbow -> grip     (this IS the production phi)
      beta  -- upper arm: lead shoulder -> lead elbow
      alpha -- whole arm: lead shoulder -> grip

    The grip is the recorded lead/trail hand midpoint when the block carries it
    (the raw `frames` do), else the wrist midpoint (the `synth` fallback).

    Returns (t_us, {name: deg}) with NaNs left in place."""
    frames = pose_block.get(which) or []
    if not frames:
        return np.array([]), {}
    elbow_i = KP_L_ELBOW if lead_left else KP_R_ELBOW
    sho_i = KP_L_SHOULDER if lead_left else KP_R_SHOULDER

    t = np.array([f["t_us"] for f in frames], dtype=np.int64)
    out = {k: np.full(len(frames), np.nan) for k in ("phi", "beta", "alpha")}

    def seg(ax, ay, ac, bx, by, bc):
        """Direction from a to b, or NaN when either end is untrustworthy."""
        if min(ac, bc) < PHI_CONF_MIN:
            return np.nan
        if math.hypot(bx - ax, by - ay) <= PHI_MIN_LEN_PX:
            return np.nan
        return math.degrees(math.atan2(by - ay, bx - ax))

    for i, f in enumerate(frames):
        kp = f.get("kp")
        if not kp:
            continue
        if f.get("lead") and f.get("trail"):
            gx = 0.5 * (f["lead"][0] + f["trail"][0]) * wpx
            gy = 0.5 * (f["lead"][1] + f["trail"][1]) * hpx
            gc = 1.0
        else:
            lx, ly, lc = _kp(kp, KP_L_WRIST)
            rx, ry, rc = _kp(kp, KP_R_WRIST)
            gx, gy = 0.5 * (lx + rx) * wpx, 0.5 * (ly + ry) * hpx
            gc = min(lc, rc)
        ex, ey, ec = _kp(kp, elbow_i)
        ex, ey = ex * wpx, ey * hpx
        sx, sy, sc = _kp(kp, sho_i)
        sx, sy = sx * wpx, sy * hpx
        out["phi"][i] = seg(ex, ey, ec, gx, gy, gc)
        out["beta"][i] = seg(sx, sy, sc, ex, ey, ec)
        out["alpha"][i] = seg(sx, sy, sc, gx, gy, gc)
    return t, out


def smooth_angle(phi_deg):
    """A robust smoother in the SPIRIT of the production smoothPhi (Hampel gate
    then a short circular mean), not a port of it -- this is a plotting and
    modelling aid, never fed back into the tracker. Operates on the unwrapped
    angle so the +-180 seam cannot manufacture a spike."""
    phi = np.asarray(phi_deg, dtype=float)
    ok = np.isfinite(phi)
    if ok.sum() < 3:
        return phi
    idx = np.flatnonzero(ok)
    u = np.degrees(np.unwrap(np.radians(phi[idx])))
    # Hampel: replace a point more than PHI_HAMPEL_DEG from the local median.
    k = 3
    med = np.copy(u)
    for i in range(len(u)):
        lo, hi = max(0, i - k), min(len(u), i + k + 1)
        med[i] = np.median(u[lo:hi])
    bad = np.abs(u - med) > PHI_HAMPEL_DEG
    u[bad] = med[bad]
    # Short moving average.
    win = 5
    pad = np.pad(u, (win // 2, win // 2), mode="edge")
    sm = np.convolve(pad, np.ones(win) / win, mode="valid")
    out = np.full_like(phi, np.nan)
    out[idx] = wrap180(sm)
    return out


# ------------------------------------------------------------------ one swing

def load_swing(run_dir, args):
    """Read one run dir into the per-swing series the model needs. Returns None
    (with a printed reason) when the swing cannot contribute."""
    rj = Path(run_dir) / "result.json"
    if not rj.exists():
        return None, "no result.json"
    try:
        an = json.load(open(rj, encoding="utf-8"))["analysis"]
    except Exception as e:
        return None, f"unreadable result.json ({e})"

    club = an.get("club", {})
    samples = club.get("samples", [])
    if not samples:
        return None, "no club samples"
    wpx = club.get("frameWidth") or 0
    hpx = club.get("frameHeight") or 0
    if not (wpx and hpx):
        return None, "no frame size"

    events = {}
    for p in an.get("phases", []):
        events.setdefault(int(p["phase"]), int(p["t_us"]))
    missing = [PHASE_NAME[p] for p in CORE_WARP_PHASES if p not in events]
    if missing:
        return None, f"ladder missing {', '.join(missing)}"
    if not (events[PH_ADDRESS] < events[PH_TOP] < events[PH_IMPACT] < events[PH_FINISH]):
        return None, "ladder not monotone"

    t = np.array([s["t_us"] for s in samples], dtype=np.int64)
    # result.json persists theta WRAPPED into [0, 2pi) — the struct comment on
    # ShaftSample2D describes the in-memory unwrapped value, not what lands on
    # disk. Unwrap here, over the whole sample series in time order (including
    # coasted samples, whose predicted theta is what makes the series
    # continuous); the tier filter is applied afterwards, to the values that
    # actually contribute.
    theta = np.degrees(np.unwrap(np.array([s["theta"] for s in samples], dtype=float)))
    flags = np.array([int(s.get("flags", 0)) for s in samples])
    conf = np.array([float(s.get("conf", 0.0)) for s in samples])

    if args.tier == "measured":
        keep = (flags & F_MEASURED) != 0
    elif args.tier == "vision":
        keep = (flags & (F_MEASURED | F_WEDGE)) != 0
    else:
        keep = np.ones(len(t), dtype=bool)
    keep &= conf >= args.min_conf
    if keep.sum() < 10:
        return None, f"only {int(keep.sum())} admissible samples"

    # phi onto the sample grid. Interpolating a slowly-moving arm angle across
    # the pose stride is fine; interpolating ACROSS a gap where the pose had no
    # confident elbow is not, so NaNs are carried, not filled.
    pose_block = an.get("pose2d", {})
    if args.lead_side == "auto":
        lead_left, lead_margin = decide_lead_side(pose_block.get("frames") or [],
                                                  events[PH_ADDRESS],
                                                  events.get(PH_TAKEAWAY, events[PH_TOP]))
        if lead_left is None:
            return None, "cannot decide which side leads from the pose"
    else:
        lead_left, lead_margin = (args.lead_side == "left"), 1.0

    pt, praw = arm_series(pose_block, args.pose, lead_left, wpx, hpx)
    if len(pt) < 3:
        return None, "no pose frames"

    # Each arm direction onto the sample grid. Interpolating a slowly-moving
    # arm angle across the pose stride is fine; interpolating ACROSS a gap
    # where the pose had no confident joint is not, so NaNs are carried rather
    # than filled.
    gap_us = float(args.phi_max_gap_ms) * 1000.0
    arms = {}
    for key, raw in praw.items():
        sm = smooth_angle(raw)
        good = np.isfinite(sm)
        if good.sum() < 3:
            arms[key] = np.full(len(t), np.nan)
            continue
        u = np.degrees(np.unwrap(np.radians(sm[good])))
        v = np.interp(t.astype(float), pt[good].astype(float), u, left=np.nan, right=np.nan)
        nearest = np.abs(t[:, None] - pt[good][None, :]).min(axis=1)
        v[nearest > gap_us] = np.nan
        arms[key] = v
    if not np.isfinite(arms.get("phi", np.array([np.nan]))).any():
        return None, "no usable forearm angle"

    def difference(a, b):
        """An angle BETWEEN two directions: folded back into (-180, 180] --
        the shaft angle is unwrapped and carries the swing's accumulated turns
        while the arm angles do not -- then unwrapped along time so the cock
        and release read as one continuous tent rather than sawing at the
        seam."""
        d = wrap180(a - b)
        fin = np.isfinite(d)
        if fin.sum() >= 2:
            d[fin] = np.degrees(np.unwrap(np.radians(d[fin])))
        return d

    series = {"theta": theta, "phi": arms["phi"], "beta": arms["beta"],
              "alpha": arms["alpha"]}
    series["psi"] = difference(theta, arms["phi"])
    series["psi_alpha"] = difference(theta, arms["alpha"])
    series["elbow"] = difference(arms["phi"], arms["beta"])

    # Sign and datum are bookkeeping, not measurement, so both are taken from
    # the continuous track rather than from the admissible subset -- at impact
    # the club is usually in blur and carries no vision sample at all, and a
    # swing should not drop out of the model over the choice of a datum.
    sign = 1.0
    if args.sign_normalise:
        dsw = (t >= events[PH_TOP]) & (t <= events[PH_IMPACT])
        if dsw.sum() >= 3:
            if theta[dsw][-1] - theta[dsw][0] < 0:
                sign = -1.0
        else:
            return None, "no downswing samples to fix the sign"

    ref_map = {"impact": PH_IMPACT, "address": PH_ADDRESS, "top": PH_TOP}
    refs = {}
    for key in series:
        if SERIES_BY_NAME[key][1] == "direction" and args.theta_ref != "none":
            rt = float(events[ref_map[args.theta_ref]])
            ok = np.isfinite(series[key])
            refs[key] = (float(np.interp(rt, t[ok].astype(float), series[key][ok]))
                         if ok.sum() >= 2 else 0.0)
        else:
            refs[key] = 0.0
        series[key] = sign * (series[key] - refs[key])

    return {
        "name": Path(run_dir).name,
        "t": t, "keep": keep,
        "series": series,
        "events": events,
        "sign": sign, "refsDeg": {k: round(v, 4) for k, v in refs.items()},
        "leadSide": "left" if lead_left else "right",
        "leadVoteMargin": round(float(lead_margin), 3),
        "nAdmissible": int(keep.sum()),
        "coverage": float(club.get("coverage", float("nan"))),
        "valid": bool(club.get("valid", False)),
    }, None


# --------------------------------------------------------------- the time warp

def anchor_fractions(swings, warp_phases):
    """Where each warp anchor sits, as the corpus MEDIAN of its relative time
    between address and finish. Address is 0 and finish is 1 by construction."""
    fr = {PH_ADDRESS: 0.0, PH_FINISH: 1.0}
    for ph in warp_phases:
        if ph in fr:
            continue
        vals = []
        for s in swings:
            e = s["events"]
            span = e[PH_FINISH] - e[PH_ADDRESS]
            if span > 0:
                vals.append((e[ph] - e[PH_ADDRESS]) / span)
        fr[ph] = float(np.median(vals)) if vals else float("nan")
    ordered = [(ph, fr[ph]) for ph in warp_phases]
    # Guard the monotonicity the interpolation depends on.
    for (a, fa), (b, fb) in zip(ordered, ordered[1:]):
        if not (fa < fb):
            raise SystemExit(f"[model] anchor fractions not increasing: "
                             f"{PHASE_NAME[a]}={fa:.3f} {PHASE_NAME[b]}={fb:.3f}")
    return fr


def warp(swing, frac, warp_phases):
    """Map this swing's sample times onto the common axis: piecewise linear
    through its own anchors, which land on the corpus-median fractions."""
    e = swing["events"]
    xs = np.array([e[ph] for ph in warp_phases], dtype=float)
    ys = np.array([frac[ph] for ph in warp_phases], dtype=float)
    return np.interp(swing["t"].astype(float), xs, ys)


def resample(tau, y, keep, grid, max_gap):
    """One swing's series onto the common grid. Only admissible samples are
    interpolated between, and only across gaps no wider than max_gap on the
    axis -- so an honest hole (the impact blackout, a coverage gap) stays a
    hole in the model instead of being quietly bridged."""
    ok = keep & np.isfinite(y) & np.isfinite(tau)
    if ok.sum() < 2:
        return np.full(len(grid), np.nan)
    ts, ys = tau[ok], y[ok]
    order = np.argsort(ts)
    ts, ys = ts[order], ys[order]
    out = np.interp(grid, ts, ys, left=np.nan, right=np.nan)
    # Punch out the wide gaps.
    idx = np.searchsorted(ts, grid)
    lo = np.clip(idx - 1, 0, len(ts) - 1)
    hi = np.clip(idx, 0, len(ts) - 1)
    span = ts[hi] - ts[lo]
    out[(span > max_gap) & (grid > ts[0]) & (grid < ts[-1])] = np.nan
    out[(grid < ts[0]) | (grid > ts[-1])] = np.nan
    return out


def reduce_band(mat, min_swings):
    """Per grid point across swings -> the model's summary statistics."""
    n = np.sum(np.isfinite(mat), axis=0)
    stats = {}
    with np.errstate(invalid="ignore"):
        stats["n"] = n
        stats["mean"] = np.nanmean(mat, axis=0)
        stats["sd"] = np.nanstd(mat, axis=0, ddof=1)
        for p in (10, 25, 50, 75, 90):
            stats[f"p{p}"] = np.nanpercentile(mat, p, axis=0)
    thin = n < min_swings
    for k, v in stats.items():
        if k == "n":
            continue
        v = np.asarray(v, dtype=float)
        v[thin] = np.nan
        stats[k] = v
    return stats


# ------------------------------------------------------------------- artefacts

def write_model_csv(path, grid, frac, stats):
    cols = ["tau", "phase_zone"]
    for key, *_ in SERIES:
        cols += [f"{key}_n", f"{key}_mean", f"{key}_sd", f"{key}_median",
                 f"{key}_p10", f"{key}_p25", f"{key}_p75", f"{key}_p90"]
    zones = []
    for x in grid:
        zones.append("backswing" if x <= frac[PH_TOP]
                     else ("downswing" if x <= frac[PH_IMPACT] else "through"))
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for i, x in enumerate(grid):
            row = [f"{x:.5f}", zones[i]]
            for key, *_ in SERIES:
                st = stats[key]
                row.append(int(st["n"][i]))
                row += [("" if not np.isfinite(st[k][i]) else f"{st[k][i]:.4f}")
                        for k in ("mean", "sd", "p50", "p10", "p25", "p75", "p90")]
            w.writerow(row)


def write_swings_csv(path, grid, names, mats):
    header = ["tau"]
    for key, *_ in SERIES:
        header += [f"{n}__{key}" for n in names]
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(header)
        for i, x in enumerate(grid):
            row = [f"{x:.5f}"]
            for key, *_ in SERIES:
                row += [("" if not np.isfinite(v) else f"{v:.4f}") for v in mats[key][:, i]]
            w.writerow(row)


def deviation_rows(mat, stats):
    """Each swing against the model: rms / p95 / max |deviation from median|,
    and how much of the swing sits outside the p10-p90 band."""
    med, p10, p90 = stats["p50"], stats["p10"], stats["p90"]
    rows = []
    for i in range(mat.shape[0]):
        d = mat[i] - med
        ok = np.isfinite(d)
        if ok.sum() == 0:
            rows.append([0, "", "", "", ""])
            continue
        dv = d[ok]
        outside = (np.isfinite(mat[i]) & np.isfinite(p10) & np.isfinite(p90)
                   & ((mat[i] < p10) | (mat[i] > p90)))
        denom = int((np.isfinite(mat[i]) & np.isfinite(p10)).sum())
        rows.append([int(ok.sum()),
                     f"{math.sqrt(float(np.mean(dv ** 2))):.4f}",
                     f"{float(np.percentile(np.abs(dv), 95)):.4f}",
                     f"{float(np.max(np.abs(dv))):.4f}",
                     ("" if denom == 0 else f"{int(outside.sum()) / denom:.4f}")])
    return rows


def write_deviation_csv(path, names, mats, stats):
    per = {key: deviation_rows(mats[key], stats[key]) for key, *_ in SERIES}
    header = ["swing"]
    for key, *_ in SERIES:
        header += [f"{key}_points", f"{key}_rms_dev_deg", f"{key}_p95_dev_deg",
                   f"{key}_max_dev_deg", f"{key}_frac_outside_p10_p90"]
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(header)
        for i, n in enumerate(names):
            row = [n]
            for key, *_ in SERIES:
                row += per[key][i]
            w.writerow(row)


def _band_limits(axes_stats):
    lo = min((np.nanmin(st["p10"]) for st in axes_stats
              if np.any(np.isfinite(st["p10"]))), default=0.0)
    hi = max((np.nanmax(st["p90"]) for st in axes_stats
              if np.any(np.isfinite(st["p90"]))), default=1.0)
    pad = 0.08 * max(hi - lo, 1.0)
    return lo - pad, hi + pad


def make_figure(path, grid, frac, mark_frac, warp_phases, stats, mats, title,
                per_swing_lines):
    """Four reading panels plus a coverage strip:
       1  the shaft angle
       2  the three lead-arm directions together, so their shapes can be compared
       3  the theta-minus-arm residuals -- the panel that says which arm signal
          would predict theta best, since the tighter band is the better predictor
       4  the elbow, which is the degree of freedom that separates them."""
    layout = [
        ("shaft", ["theta"], "shaft angle θ  [deg, referenced]"),
        ("arms", ["phi", "beta", "alpha"], "lead-arm directions  [deg, referenced]"),
        ("resid", ["psi", "psi_alpha"], "θ − arm  [deg]"),
        ("elbow", ["elbow"], "elbow ε = φ − β  [deg]"),
    ]
    fig, axes = plt.subplots(len(layout) + 1, 1, figsize=(13, 15), dpi=110, sharex=True,
                             gridspec_kw={"height_ratios": [1] * len(layout) + [0.22]})

    for ax, (_, keys, ylab) in zip(axes, layout):
        for key in keys:
            lab, _kind, dark, light = SERIES_BY_NAME[key]
            st = stats[key]
            if per_swing_lines and len(keys) == 1:
                for i in range(mats[key].shape[0]):
                    ax.plot(grid, mats[key][i], color=dark, lw=0.5, alpha=0.18, zorder=1)
            ax.fill_between(grid, st["p10"], st["p90"], color=light, alpha=0.30,
                            lw=0, zorder=2)
            ax.fill_between(grid, st["p25"], st["p75"], color=light, alpha=0.60,
                            lw=0, zorder=3)
            ax.plot(grid, st["p50"], color=dark, lw=2.0, zorder=4, label=lab)
        # Scale to the BANDS, not to the individual swings: a swing whose track
        # runs away in the follow-through would otherwise flatten the model it
        # is supposed to be read against. Runaways clip; the deviation CSV is
        # where they are meant to be found.
        ax.set_ylim(*_band_limits([stats[k] for k in keys]))
        ax.set_ylabel(ylab, fontsize=9)
        ax.grid(alpha=0.25, lw=0.6)
        for ph in warp_phases[1:-1]:
            ax.axvline(frac[ph], color="k", alpha=0.45, lw=1.0, zorder=5)
        for _ph, x in mark_frac.items():
            ax.axvline(x, color="k", alpha=0.18, lw=0.8, ls=":", zorder=5)
        ax.legend(fontsize=8, loc="best", framealpha=0.9,
                  ncol=1 if len(keys) == 1 else len(keys))

    top = axes[0]
    for ph in warp_phases:
        top.annotate(PHASE_NAME[ph].split(" ")[0], xy=(frac[ph], 1.005),
                     xycoords=("data", "axes fraction"), ha="center", va="bottom",
                     fontsize=9, fontweight="bold")
    for ph, x in mark_frac.items():
        top.annotate(PHASE_NAME[ph].split(" ")[0], xy=(x, 0.985),
                     xycoords=("data", "axes fraction"), ha="center", va="top",
                     fontsize=7, color="#555555", rotation=90)

    axn = axes[-1]
    for key in ("theta", "phi", "alpha"):
        lab, _kind, dark, _light = SERIES_BY_NAME[key]
        axn.plot(grid, stats[key]["n"], color=dark, lw=1.2, label=lab.split(" ")[0])
    axn.set_ylim(0, max(int(np.max(stats["theta"]["n"])), 1) * 1.2)
    axn.set_ylabel("swings", fontsize=8)
    axn.tick_params(axis="both", labelsize=7)
    axn.grid(alpha=0.25, lw=0.6)
    axn.legend(fontsize=7, loc="lower left", ncol=3, framealpha=0.9)
    for ph in warp_phases[1:-1]:
        axn.axvline(frac[ph], color="k", alpha=0.45, lw=1.0)
    axn.set_xlabel("swing progress τ  (event-warped: address 0 → finish 1, "
                   "anchors at the corpus-median tempo)", fontsize=10)

    axes[0].set_xlim(0, 1)
    fig.suptitle(title, fontsize=12)
    fig.tight_layout(rect=(0, 0, 1, 0.975))
    fig.savefig(path)
    plt.close(fig)


# ------------------------------------------------------------------------ main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_root", help="directory of per-swing run dirs (each with result.json)")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--swings", help="comma-separated substrings; keep only matching swings")
    ap.add_argument("--exclude", help="comma-separated substrings; drop matching swings")
    ap.add_argument("--tier", choices=("vision", "measured", "all"), default="vision",
                    help="which samples may contribute: vision = measured or blur-wedge "
                         "(default), measured = the measured bit only, all = including coasted")
    ap.add_argument("--min-conf", type=float, default=0.0, help="minimum sample confidence")
    ap.add_argument("--theta-ref", choices=("impact", "address", "top", "none"),
                    default="impact", help="event theta is referenced to (default impact)")
    ap.add_argument("--no-sign-normalise", dest="sign_normalise", action="store_false",
                    help="do NOT flip swings onto a common sweep direction")
    ap.add_argument("--bins", type=int, default=400, help="grid points across the swing")
    ap.add_argument("--max-gap", type=float, default=0.02,
                    help="widest hole in tau a swing may be interpolated across (default 0.02)")
    ap.add_argument("--warp-anchors", choices=("core", "ladder"), default="core",
                    help="core = address/top/impact/finish only (default; the band then "
                         "includes tempo variance between them). ladder = also warp on any "
                         "of P2/P3/P5/P6/P8 present on EVERY contributing swing, which "
                         "isolates shape at the cost of defining the timing away")
    ap.add_argument("--min-swings", type=int, default=5,
                    help="grid points with fewer contributing swings are blanked")
    ap.add_argument("--pose", choices=("frames", "synth"), default="frames",
                    help="pose series phi is read from (frames carries the recorded "
                         "lead/trail hands; synth is denser but wrist-midpoint only)")
    ap.add_argument("--phi-max-gap-ms", type=float, default=60.0,
                    help="blank phi further than this from a confident pose frame")
    ap.add_argument("--lead-side", choices=("auto", "left", "right"), default="auto")
    ap.add_argument("--per-swing-lines", action="store_true",
                    help="draw every swing faintly behind the band")
    ap.add_argument("--title", help="figure title override")
    args = ap.parse_args(argv)

    root = Path(args.run_root)
    if not root.is_dir():
        raise SystemExit(f"[model] not a directory: {root}")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    dirs = sorted(d for d in root.iterdir() if (d / "result.json").exists())
    if args.swings:
        pats = [p.strip() for p in args.swings.split(",") if p.strip()]
        dirs = [d for d in dirs if any(p in d.name for p in pats)]
    if args.exclude:
        pats = [p.strip() for p in args.exclude.split(",") if p.strip()]
        dirs = [d for d in dirs if not any(p in d.name for p in pats)]
    if not dirs:
        raise SystemExit(f"[model] no swing run dirs under {root}")

    swings, rejected = [], []
    for d in dirs:
        s, why = load_swing(d, args)
        if s is None:
            rejected.append({"swing": d.name, "reason": why})
            print(f"[model] skip {d.name}: {why}")
        else:
            swings.append(s)
    if len(swings) < 2:
        raise SystemExit(f"[model] only {len(swings)} usable swing(s) — nothing to model")

    warp_phases = list(CORE_WARP_PHASES)
    if args.warp_anchors == "ladder":
        extra = [ph for ph in LADDER_PHASES if all(ph in s["events"] for s in swings)]
        dropped = [PHASE_NAME[ph] for ph in LADDER_PHASES if ph not in extra]
        warp_phases = sorted(set(warp_phases) | set(extra),
                             key=lambda ph: np.median([s["events"][ph] - s["events"][PH_ADDRESS]
                                                       for s in swings]))
        print(f"[model] warping on {len(warp_phases)} anchors: "
              + ", ".join(PHASE_NAME[p] for p in warp_phases))
        if dropped:
            print(f"[model] not warped (absent on at least one swing): {', '.join(dropped)}")
    mark_phases = [ph for ph in LADDER_PHASES if ph not in warp_phases]

    frac = anchor_fractions(swings, warp_phases)
    mark_frac = {}
    for ph in mark_phases:
        vals = []
        for s in swings:
            if ph not in s["events"]:
                continue
            e = s["events"]
            if not (e[PH_ADDRESS] <= s["events"][ph] <= e[PH_FINISH]):
                continue
            xs = np.array([e[p] for p in warp_phases], dtype=float)
            ys = np.array([frac[p] for p in warp_phases], dtype=float)
            vals.append(float(np.interp(float(s["events"][ph]), xs, ys)))
        if len(vals) >= max(3, args.min_swings // 2):
            mark_frac[ph] = float(np.median(vals))

    grid = np.linspace(0.0, 1.0, args.bins)
    names = [s["name"] for s in swings]
    mats = {key: np.full((len(swings), len(grid)), np.nan) for key, *_ in SERIES}
    for i, sw in enumerate(swings):
        tau = warp(sw, frac, warp_phases)
        for key, *_ in SERIES:
            mats[key][i] = resample(tau, sw["series"][key], sw["keep"], grid, args.max_gap)

    stats = {key: reduce_band(mats[key], args.min_swings) for key, *_ in SERIES}

    title = args.title or (f"Corpus shaft- and arm-angle shape model — "
                           f"{len(swings)} swings, {root.name}")
    png = out / "theta_psi_model.png"
    make_figure(png, grid, frac, mark_frac, warp_phases, stats, mats, title,
                args.per_swing_lines)
    write_model_csv(out / "theta_psi_model.csv", grid, frac, stats)
    write_swings_csv(out / "theta_psi_swings.csv", grid, names, mats)
    write_deviation_csv(out / "theta_psi_deviation.csv", names, mats, stats)

    def band_width(key):
        w = stats[key]["p90"] - stats[key]["p10"]
        w = w[np.isfinite(w)]
        return (float(np.median(w)), float(np.min(w)), float(np.max(w))) if len(w) else None

    model = {
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "sha": git_sha(),
        "runRoot": str(root),
        "options": {k: v for k, v in vars(args).items() if k not in ("out",)},
        "definitions": {
            "theta": "shaft direction, image atan2, from the grip (the tracker's own theta)",
            "phi": "lead FOREARM direction: lead elbow -> grip. This is the production phi.",
            "beta": "lead UPPER ARM direction: lead shoulder -> lead elbow",
            "alpha": "WHOLE lead arm direction: lead shoulder -> grip",
            "psi": "wrist angle, theta - phi (folded, then unwrapped in time)",
            "psi_alpha": "shaft against the whole-arm line, theta - alpha",
            "elbow": "elbow opening, phi - beta (0 = straight arm)",
            "datum": ("direction series are referenced to their OWN value at the datum "
                      "event, so a referenced theta minus a referenced phi is NOT psi; "
                      "the difference series are computed from the raw angles"),
        },
        "anchors": {PHASE_NAME[p]: frac[p] for p in warp_phases},
        "markers": {PHASE_NAME[p]: v for p, v in mark_frac.items()},
        "bandWidthDeg": {key: band_width(key) for key, *_ in SERIES},
        "swings": [{"name": sw["name"], "sign": sw["sign"], "refsDeg": sw["refsDeg"],
                    "leadSide": sw["leadSide"], "leadVoteMargin": sw["leadVoteMargin"],
                    "nAdmissible": sw["nAdmissible"],
                    "coverage": sw["coverage"], "valid": sw["valid"],
                    "eventsUs": {PHASE_NAME.get(k, str(k)): v for k, v in sw["events"].items()}}
                   for sw in swings],
        "rejected": rejected,
        "grid": [round(float(x), 5) for x in grid],
        "series": {key: {k: [None if (k != "n" and not np.isfinite(v)) else
                             (int(v) if k == "n" else round(float(v), 4))
                             for v in stats[key][k]]
                         for k in ("n", "mean", "sd", "p10", "p25", "p50", "p75", "p90")}
                   for key, *_ in SERIES},
    }
    with open(out / "theta_psi_model.json", "w", encoding="utf-8") as f:
        json.dump(model, f, indent=1)

    cov = int(np.max(stats["theta"]["n"])) if len(grid) else 0
    print(f"[model] {len(swings)} swings ({len(rejected)} skipped), "
          f"{cov} contributing at the fullest grid point")
    print("[model] anchors: " + "  ".join(f"{PHASE_NAME[p]}={frac[p]:.3f}" for p in warp_phases))
    print("[model] p10–p90 band width (deg), median over the swing:")
    for key, lab, _kind, _d, _l in SERIES:
        bw = band_width(key)
        if bw:
            print(f"[model]   {lab:32s} median {bw[0]:6.1f}   min {bw[1]:5.1f}   max {bw[2]:6.1f}")
    # Which arm signal is the better single predictor of theta: the residual
    # with the tighter band is the one an estimator has less left to explain.
    ranked = sorted(((band_width(k)[0], k, src) for k, src in PREDICTOR_RESIDUALS.items()
                     if band_width(k)), key=lambda r: r[0])
    if ranked:
        print("[model] theta-from-arm residual spread (tighter = better predictor):")
        for width, key, src in ranked:
            print(f"[model]   theta − {src:5s} : {width:5.1f}°  ({SERIES_BY_NAME[key][0]})")
    print(f"[model] wrote {png} + 3 CSV + theta_psi_model.json under {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
