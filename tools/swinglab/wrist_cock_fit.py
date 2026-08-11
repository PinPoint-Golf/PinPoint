#!/usr/bin/env python3
"""wrist_cock_fit.py -- fit and grade the wrist-cock table the shaft tracker
already uses to predict the club from the arm.

src/Analysis/shaft_kinematics.h predicts the club direction as

    theta_hat(f) = phi_arm(f) + chir * beta_hat(s(f))

from a HAND-AUTHORED 9-knot table beta_hat(s), with a companion spread
sigma_beta(s) that sets the blur-wedge search envelope and the kinCone penalty.
The table has never been fitted to data. This harness answers whether fitting it
helps, and refuses to recommend a change unless it does.

WHAT IT FITS.  beta = chir * wrap180(theta - phi) -- the signed wrist cock, in
exactly the convention the header uses, so a fitted table drops straight into
kWristCockKnots. Four candidate forms, each a superset of the last:

  F0  the shipped table, unchanged                          (the baseline)
  F1  same swing-progress axis, same knots, FITTED          (was the table ever right?)
  F2  re-axis to seconds-before-impact, fitted              (is the axis the problem?)
  F3  F2 plus a linear-in-phi slope table                   (does the arm angle add?)
  F4  F3 under the shape constraints the physics asserts    (can it be made honest?)

ANTI-CIRCULARITY.  A model fitted to the tracker's own theta and then used to
constrain the tracker would reinforce its errors. So:

  * fitting uses MEASURED-tier samples only (the tier graded at 0.6% bad against
    dense truth), never coasted or predicted ones;
  * grading uses the instrumented dense truth -- hand-placed shaft labels that
    owe nothing to the tracker;
  * every number is leave-one-swing-out: the held-out swing contributes nothing
    to the table it is scored against.

Both are reported, because the gap between them IS the circularity, and it
belongs in the write-up rather than in a footnote.

  wrist_cock_fit.py <run_root> --corpus <corpus_root> --out <dir>
      [--forms F0,F1,F2,F3,F4] [--knots N] [--min-conf F]
      [--domain full|to-impact] [--emit-header]

<run_root> is a directory of per-swing run dirs (result.json), e.g. a frozen gate
run. <corpus_root> is the swing library holding truth.json per swing. Nothing
here re-runs the analyzer.
"""

import argparse
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from theta_psi_model import (  # noqa: E402  - the conventions live there, once
    F_MEASURED, F_WEDGE, PH_ADDRESS, PH_TAKEAWAY, PH_TOP, PH_IMPACT, PH_FINISH,
    arm_series, decide_lead_side, smooth_angle, wrap180, git_sha,
)

# ---------------------------------------------------------------------------
# The shipped table (src/Analysis/shaft_kinematics.h kWristCockKnots), verbatim.
# Kept here as the baseline every candidate must beat; a divergence between this
# and the header is itself a bug, and --emit-header regenerates the header form.
SHIPPED_KNOTS = [
    (0.00, 8.0, 8.0), (0.15, 27.0, 15.0), (0.35, 70.0, 20.0), (0.50, 92.0, 22.0),
    (0.60, 100.0, 30.0), (0.80, 47.0, 25.0), (0.90, 7.0, 10.0), (0.95, -27.0, 20.0),
    (1.00, -95.0, 30.0),
]

# The shipped swing-progress anchors: bs0->0, top->0.5, impact->0.9, fin0->1.0.
PROGRESS_ANCHORS = [(PH_TAKEAWAY, 0.0), (PH_TOP, 0.5), (PH_IMPACT, 0.9), (PH_FINISH, 1.0)]


def piecewise(x, knots_x, knots_y):
    """Piecewise-linear interpolation with flat ends -- byte-for-byte the
    behaviour of betaHatDeg/sigmaBetaDeg in the header."""
    return np.interp(np.asarray(x, dtype=float), knots_x, knots_y)


def swing_progress(t_us, ev):
    """The header's swingProgress(), on timestamps rather than frame indices
    (the mapping is affine within each segment, so the two agree)."""
    xs = [float(ev[ph]) for ph, _ in PROGRESS_ANCHORS]
    ys = [v for _, v in PROGRESS_ANCHORS]
    t = np.asarray(t_us, dtype=float)
    s = np.interp(t, xs, ys)
    return np.clip(s, 0.0, 1.0)


def time_to_impact(t_us, ev):
    """Seconds before impact -- negative through the swing, 0 at impact."""
    return (np.asarray(t_us, dtype=float) - float(ev[PH_IMPACT])) / 1e6


# ---------------------------------------------------------------------------

def load_swing(run_dir, corpus_root, args):
    """One swing: the arm angle, the tracker's theta, the truth theta if it has
    any, and the phase ladder -- all on a common timebase."""
    rj = Path(run_dir) / "result.json"
    if not rj.exists():
        return None, "no result.json"
    try:
        an = json.load(open(rj, encoding="utf-8"))["analysis"]
    except Exception as e:
        return None, f"unreadable result.json ({e})"

    club = an.get("club", {})
    samples = club.get("samples", [])
    wpx, hpx = club.get("frameWidth") or 0, club.get("frameHeight") or 0
    if not samples or not (wpx and hpx):
        return None, "no samples or frame size"

    events = {}
    for p in an.get("phases", []):
        events.setdefault(int(p["phase"]), int(p["t_us"]))
    need = [PH_ADDRESS, PH_TAKEAWAY, PH_TOP, PH_IMPACT, PH_FINISH]
    missing = [str(p) for p in need if p not in events]
    if missing:
        return None, f"ladder missing phase {', '.join(missing)}"
    if not (events[PH_TAKEAWAY] < events[PH_TOP] < events[PH_IMPACT] < events[PH_FINISH]):
        return None, "ladder not monotone"

    t = np.array([s["t_us"] for s in samples], dtype=np.int64)
    theta = np.degrees(np.array([s["theta"] for s in samples], dtype=float))
    flags = np.array([int(s.get("flags", 0)) for s in samples])
    conf = np.array([float(s.get("conf", 0.0)) for s in samples])
    measured = ((flags & F_MEASURED) != 0) & (conf >= args.min_conf)
    vision = ((flags & (F_MEASURED | F_WEDGE)) != 0) & (conf >= args.min_conf)

    pose_block = an.get("pose2d", {})
    lead_left, _margin = decide_lead_side(pose_block.get("frames") or [],
                                          events[PH_ADDRESS], events[PH_TAKEAWAY])
    if lead_left is None:
        return None, "cannot decide the lead side"
    pt, praw = arm_series(pose_block, "frames", lead_left, wpx, hpx)
    if len(pt) < 3 or "phi" not in praw:
        return None, "no pose frames"
    sm = smooth_angle(praw["phi"])
    good = np.isfinite(sm)
    if good.sum() < 5:
        return None, "no usable forearm angle"
    phi_u = np.degrees(np.unwrap(np.radians(sm[good])))

    def phi_at(times):
        v = np.interp(np.asarray(times, dtype=float), pt[good].astype(float), phi_u,
                      left=np.nan, right=np.nan)
        near = np.abs(np.asarray(times, dtype=float)[:, None] - pt[good][None, :]).min(axis=1)
        v[near > 60_000.0] = np.nan
        return v

    phi = phi_at(t)

    # Chirality exactly as the header defines it: the sign of the unwrapped arm
    # angle's travel from the backswing start to the top.
    p0, p1 = np.interp([float(events[PH_TAKEAWAY]), float(events[PH_TOP])],
                       pt[good].astype(float), phi_u)
    chir = 1 if (p1 - p0) >= 0 else -1

    # beta = chir * (theta - phi): the signed wrist cock the header's table is in.
    beta_track = chir * wrap180(theta - phi)

    # Truth, when this swing has hand-placed shaft labels.
    truth_t = truth_beta = truth_lab = None
    name = Path(run_dir).name
    if "__" in name:
        session, swing = name.split("__", 1)
        tj = Path(corpus_root) / session / swing / "truth.json"
        if tj.exists():
            try:
                lab = (json.load(open(tj, encoding="utf-8")).get("shaft") or [])
            except Exception:
                lab = []
            lab = [l for l in lab if "t_us" in l and "theta" in l]
            if len(lab) >= args.min_truth:
                tt = np.array([l["t_us"] for l in lab], dtype=np.int64)
                th = np.degrees(np.array([l["theta"] for l in lab], dtype=float))
                ph = phi_at(tt)
                ok = np.isfinite(ph)
                if ok.sum() >= args.min_truth:
                    truth_t = tt[ok]
                    truth_beta = chir * wrap180(th[ok] - ph[ok])
                    truth_lab = [l for l, k in zip(lab, ok) if k]

    rec = {
        "name": name,
        "session": name.split("__")[0] if "__" in name else name,
        "events": events, "chir": chir,
        "t": t, "beta": beta_track, "phi": phi,
        "measured": measured, "vision": vision,
        "truth_t": truth_t, "truth_beta": truth_beta,
        "truth_phi": phi_at(truth_t) if truth_t is not None else None,
        "truth_src": None,
        "ftruth_t": None, "ftruth_beta": None, "ftruth_phi": None, "ftruth_tier": None,
    }

    # The instrumented fusion truth, when this swing has a lab clip. Additive:
    # without --lab-root nothing below runs and the record is unchanged.
    if getattr(args, "lab_root", None):
        _attach_fusion(rec, name, args, phi_at, chir, t)
    return rec, None


def _attach_fusion(rec, name, args, phi_at, chir, sample_t):
    """Join the instrumented stripe-fusion truth onto this swing.

    Two things come out of it, and the second matters as much as the first:

      * ftruth_* -- the fusion rows as a gradable channel, tiered band vs ray.
        The RAY tier is the genuinely new material; the band tier is largely
        already in truth.json.
      * truth_src -- provenance for each EXISTING corpus label. For the
        2026-07-05 session truth.json IS the band tier verbatim, so grading
        "hand labels" against "fusion band" without this would be comparing a
        set with itself and calling the agreement a result.
    """
    try:
        from fusion_truth import discover, fusion_labels
    except Exception as e:                                  # pragma: no cover
        print(f"[fit] --lab-root given but fusion_truth unavailable ({e})", file=sys.stderr)
        return
    lab_dirs = _lab_index(args)
    lab_dir = lab_dirs.get(name)
    if lab_dir is None:
        return
    try:
        fl = fusion_labels(lab_dir, sample_t)
    except Exception as e:
        print(f"[fit] {name}: fusion truth unreadable ({e})", file=sys.stderr)
        return

    want = set((args.fusion_tiers or "band,ray").split(","))
    keep = np.array([tr in want for tr in fl["tier"]], dtype=bool)
    keep &= np.isfinite(fl["theta_deg"])
    if args.truth_phi == "anchors":
        ph = np.where(fl["phi_anchor_ok"], fl["phi_anchor"], np.nan)
    else:
        ph = phi_at(fl["t_us"])
    keep &= np.isfinite(ph)
    if keep.sum():
        rec["ftruth_t"] = fl["t_us"][keep].astype(np.int64)
        rec["ftruth_phi"] = ph[keep]
        rec["ftruth_beta"] = chir * wrap180(fl["theta_deg"][keep] - ph[keep])
        rec["ftruth_tier"] = fl["tier"][keep]

    # Provenance of the corpus labels: a band row within 2 ms and 1 px is the
    # same observation, not an independent one.
    if rec["truth_t"] is not None:
        band = (fl["tier"] == "band") & np.isfinite(fl["head_x"])
        bt = fl["t_us"][band].astype(float)
        src = []
        for tt in rec["truth_t"].astype(float):
            src.append("instrumented-band"
                       if bt.size and np.min(np.abs(bt - tt)) <= 2000.0 else "hand")
        rec["truth_src"] = np.asarray(src)


_LAB_INDEX = {}


def _lab_index(args):
    key = str(args.lab_root)
    if key not in _LAB_INDEX:
        from fusion_truth import discover
        _LAB_INDEX[key] = discover(args.lab_root)
    return _LAB_INDEX[key]


# ---------------------------------------------------------------------------
# Model forms

class Form:
    """A fitted wrist-cock model: a knot table on some axis, optionally with a
    slope table in phi. Predict() returns (beta_hat, sigma)."""

    def __init__(self, key, label, axis, knots_x, use_phi=False, shape=False, quad=True):
        self.key, self.label, self.axis = key, label, axis
        self.knots_x = np.asarray(knots_x, dtype=float)
        self.use_phi = use_phi
        self.shape = shape
        # Local QUADRATIC by default: beta curves hard through the release, and
        # a local line inside any usable window carries that curvature straight
        # into the knot value as bias. The quadratic term absorbs it, which lets
        # the window stay wide enough for sigma to be estimated from data rather
        # than from a handful of samples.
        self.quad = quad
        self.centre = None
        self.sigma = None
        self.slope = None

    def clone(self):
        """A fresh, unfitted copy of this form. Leave-one-out fits one model per
        held-out swing, and the copy must be the same TYPE as the prototype --
        constructing a plain Form here would silently fit a parametric form as
        a two-knot lookup table, which is a straight line."""
        return Form(self.key, self.label, self.axis, self.knots_x,
                    use_phi=self.use_phi, shape=self.shape, quad=self.quad)

    # -- axis ------------------------------------------------------------
    def x_of(self, t_us, ev):
        return swing_progress(t_us, ev) if self.axis == "s" else time_to_impact(t_us, ev)

    # -- fit -------------------------------------------------------------
    def fit(self, X, B, PHI, halfwidth):
        """Local linear fit at each knot: the centre is the fitted value AT the
        knot, and sigma is the robust spread about the local line.

        The local line matters. beta moves through more than 100 deg in the
        last tenth of a second before impact, so inside any usable window the
        scatter is dominated by beta's own slope, not by how much swings differ
        -- a local median would report that slope as spread and inflate the
        envelope. Removing the trend first is what makes sigma mean what the
        wedge envelope needs it to mean."""
        n = len(self.knots_x)
        self.centre = np.full(n, np.nan)
        self.sigma = np.full(n, np.nan)
        self.slope = np.zeros(n)
        self.phi_ref = np.zeros(n)
        for i, kx in enumerate(self.knots_x):
            m = (np.abs(X - kx) <= halfwidth) & np.isfinite(B) & np.isfinite(X)
            if self.use_phi:
                m &= np.isfinite(PHI)
            if m.sum() < 12:
                continue
            b = B[m]
            dx = X[m] - kx
            cols = [dx]                              # the local trend in the axis
            if self.quad:
                cols.append(dx * dx)                 # ... and its curvature
            if self.use_phi:
                self.phi_ref[i] = np.median(PHI[m])
                cols.append(PHI[m] - self.phi_ref[i])
            cols.append(np.ones(m.sum()))
            A = np.vstack(cols).T
            c, *_ = np.linalg.lstsq(A, b, rcond=None)
            r = b - A @ c
            # one robust pass: drop the worst tenth and refit
            keep = np.abs(r) <= max(np.percentile(np.abs(r), 90), 1e-6)
            if keep.sum() >= 12:
                c, *_ = np.linalg.lstsq(A[keep], b[keep], rcond=None)
                r = b - A @ c
            self.centre[i] = c[-1]                   # the intercept IS the knot value
            if self.use_phi:
                self.slope[i] = c[-2]
            self.sigma[i] = 0.7413 * (np.percentile(r, 75) - np.percentile(r, 25))
        self._fill_gaps()
        if self.shape:
            self._apply_shape()
        return self

    def _fill_gaps(self):
        for arr in (self.centre, self.sigma):
            ok = np.isfinite(arr)
            if ok.sum() == 0:
                arr[:] = 0.0
            elif ok.sum() < len(arr):
                arr[~ok] = np.interp(self.knots_x[~ok], self.knots_x[ok], arr[ok])

    def _apply_shape(self):
        """The physics the programme already asserts, imposed on the table
        rather than checked afterwards: over address->impact the wrist cocks
        and then releases with a SINGLE reversal, and at impact the club has
        come back into line with the arm."""
        # Anchor: beta(impact) -> 0. On the time axis impact is x=0; on the
        # progress axis it is s=0.9.
        x_imp = 0.0 if self.axis == "t" else 0.9
        j = int(np.argmin(np.abs(self.knots_x - x_imp)))
        self.centre[j] = 0.0
        # Single reversal: rising to the peak, falling after it, over the
        # pre-impact domain. Enforced by isotonic projection on each side.
        pre = self.knots_x <= x_imp
        idx = np.flatnonzero(pre)
        if len(idx) >= 3:
            seg = self.centre[idx]
            k = int(np.argmax(seg))
            seg[:k + 1] = _isotonic(seg[:k + 1], increasing=True)
            seg[k:] = _isotonic(seg[k:], increasing=False)
            self.centre[idx] = seg

    # -- predict ---------------------------------------------------------
    def predict(self, X, PHI=None):
        b = piecewise(X, self.knots_x, self.centre)
        s = piecewise(X, self.knots_x, self.sigma)
        if self.use_phi and PHI is not None and np.any(np.isfinite(self.slope)):
            ref = piecewise(X, self.knots_x, getattr(self, "phi_ref",
                                                     np.zeros(len(self.knots_x))))
            b = b + piecewise(X, self.knots_x, self.slope) * (PHI - ref)
        return b, s


def _isotonic(y, increasing=True):
    """Pool-adjacent-violators, the same tool the tracker's psi reconciliation
    uses -- here it makes the fitted table monotone by construction."""
    y = np.asarray(y, dtype=float)
    if len(y) < 2:
        return y.copy()
    if not increasing:
        return -_isotonic(-y, increasing=True)
    vals, wts = list(y), [1.0] * len(y)
    i = 0
    while i < len(vals) - 1:
        if vals[i] <= vals[i + 1]:
            i += 1
            continue
        tot = wts[i] + wts[i + 1]
        vals[i] = (vals[i] * wts[i] + vals[i + 1] * wts[i + 1]) / tot
        wts[i] = tot
        del vals[i + 1]
        del wts[i + 1]
        if i > 0:
            i -= 1
    out = []
    for v, w in zip(vals, wts):
        out.extend([v] * int(round(w)))
    return np.asarray(out[:len(y)])


# ---------------------------------------------------------------------------
# A parametric form, and why one is worth having
#
# Everything above fits a LOOKUP TABLE: knots and linear interpolation. That is
# an empirical curve, not a model -- it has as many free numbers as it has
# knots, it can wiggle wherever the data is thin, and none of its numbers means
# anything on its own. The physics says the curve has a shape, so it should be
# possible to write that shape down.
#
#     beta(t) = b0 + A * Lc(t) * (1 - r * Lr(t))
#
#     Lc(t) = logistic((t - tc) / wc)     the wrist COCKING on the backswing
#     Lr(t) = logistic((t - tr) / wr)     the wrist RELEASING into impact
#
# Seven parameters, and every one of them is a quantity a coach already has a
# word for:
#
#     b0  the wrist offset at address (setup)
#     A   peak lag amplitude -- how much the wrist cocks
#     tc  when the cocking happens, seconds before impact
#     wc  how fast it cocks
#     tr  WHEN THE RELEASE HAPPENS -- casting vs holding lag, in seconds
#     wr  how fast the release is once it starts
#     r   release completeness: how much of the lag is spent by impact
#
# The form cannot express a second reversal, so the one-reversal law holds by
# construction rather than by assertion; and it cannot wiggle, so thin data
# produces a wrong curve rather than a jagged one -- which is the failure mode
# you want, because it is visible.

def _logistic(z):
    return 1.0 / (1.0 + np.exp(-np.clip(z, -60.0, 60.0)))


def beta_param(t, p):
    """The parametric wrist-cock curve. p = (b0, A, tc, wc, tr, wr, r)."""
    b0, A, tc, wc, tr, wr, r = p
    wc = max(abs(wc), 1e-3)
    wr = max(abs(wr), 1e-3)
    return b0 + A * _logistic((t - tc) / wc) * (1.0 - r * _logistic((t - tr) / wr))


def _nelder_mead(f, x0, step, iters=2000, tol=1e-7):
    """Nelder-Mead, so the harness keeps its numpy-only dependency set. Seven
    parameters with a sensible seed is well inside what a simplex handles."""
    n = len(x0)
    sim = [np.array(x0, dtype=float)]
    for i in range(n):
        y = np.array(x0, dtype=float)
        y[i] += step[i]
        sim.append(y)
    val = [f(s) for s in sim]
    for _ in range(iters):
        idx = np.argsort(val)
        sim = [sim[i] for i in idx]
        val = [val[i] for i in idx]
        if abs(val[-1] - val[0]) <= tol * (abs(val[0]) + tol):
            break
        cen = np.mean(sim[:-1], axis=0)
        xr = cen + (cen - sim[-1])
        fr = f(xr)
        if fr < val[0]:
            xe = cen + 2.0 * (cen - sim[-1])
            fe = f(xe)
            sim[-1], val[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < val[-2]:
            sim[-1], val[-1] = xr, fr
        else:
            xc = cen + 0.5 * (sim[-1] - cen)
            fc = f(xc)
            if fc < val[-1]:
                sim[-1], val[-1] = xc, fc
            else:
                for i in range(1, len(sim)):
                    sim[i] = sim[0] + 0.5 * (sim[i] - sim[0])
                    val[i] = f(sim[i])
    idx = int(np.argmin(val))
    return sim[idx], val[idx]


class ParamForm(Form):
    """The parametric curve, fitted by robust (Huber) loss on the same samples
    the tables see, and evaluated through the same predict() interface so the
    comparison is like for like."""

    def __init__(self, key, label):
        super().__init__(key, label, "t", [-1.1, 0.0])
        self.params = None

    def clone(self):
        return ParamForm(self.key, self.label)

    def fit(self, X, B, PHI, halfwidth):
        m = np.isfinite(X) & np.isfinite(B) & (X >= -1.3) & (X <= 0.05)
        x, b = X[m], B[m]
        if len(x) < 100:
            self.params = np.array([0.0, 90.0, -0.75, 0.12, -0.10, 0.03, 0.85])
            self.sigma_scalar = 20.0
            return self

        def loss(p):
            r = b - beta_param(x, p)
            a = np.abs(r)
            d = 15.0                                   # Huber knee, degrees
            return float(np.mean(np.where(a <= d, 0.5 * r * r, d * (a - 0.5 * d))))

        seed = np.array([0.0, 90.0, -0.75, 0.12, -0.10, 0.03, 0.85])
        step = np.array([5.0, 20.0, 0.1, 0.05, 0.05, 0.02, 0.1])
        best, _ = _nelder_mead(loss, seed, step)
        self.params = best
        # sigma: the robust spread about the fitted curve, in the same bins the
        # tables use, so the envelope is comparable.
        self.knots_x = np.linspace(-1.1, 0.0, 15)
        self.centre = beta_param(self.knots_x, best)
        self.sigma = np.full(len(self.knots_x), np.nan)
        for i, kx in enumerate(self.knots_x):
            w = np.abs(x - kx) <= 0.05
            if w.sum() >= 12:
                r = b[w] - beta_param(x[w], best)
                self.sigma[i] = 0.7413 * (np.percentile(r, 75) - np.percentile(r, 25))
        self._fill_gaps()
        return self

    def predict(self, X, PHI=None):
        if self.params is None:
            return np.full(len(np.atleast_1d(X)), np.nan), np.full(len(np.atleast_1d(X)), np.nan)
        b = beta_param(np.clip(np.asarray(X, dtype=float), -1.1, 0.0), self.params)
        return b, piecewise(X, self.knots_x, self.sigma)


def shipped_form():
    f = Form("F0", "shipped table (hand-authored)", "s",
             [k[0] for k in SHIPPED_KNOTS])
    f.centre = np.array([k[1] for k in SHIPPED_KNOTS], dtype=float)
    f.sigma = np.array([k[2] for k in SHIPPED_KNOTS], dtype=float)
    f.slope = np.zeros(len(SHIPPED_KNOTS))
    return f


def time_knots(n, layout):
    """Knot placement on the seconds-before-impact axis.

    Uniform spacing cannot represent this curve. The wrist holds its lag near
    90 deg through most of the downswing and then releases almost all of it in
    the last ~120 ms, so a 0.1 s knot spacing puts an 80 deg drop inside one
    linear segment. `dense-late` spaces knots uniformly in sqrt(-t), which puts
    roughly half of them inside the last quarter second where the curve
    actually moves."""
    if layout == "uniform":
        return list(np.round(np.linspace(-1.10, 0.05, n), 3))
    u = np.linspace(math.sqrt(1.10), 0.0, n)
    return list(np.round(-(u ** 2), 3))


def progress_knots(n, layout, hi=0.9):
    """Knot placement on the swing-progress axis.

    Matched to time_knots() on purpose. The shipped table puts two knots
    (s=0.8, 0.9) across the whole release, so refitting AT THOSE POSITIONS
    measures knot placement, not the axis -- an 85 deg drop inside one linear
    segment cannot be fitted by any choice of endpoint values."""
    if layout == "uniform":
        return list(np.round(np.linspace(0.0, hi, n), 4))
    u = np.linspace(math.sqrt(hi), 0.0, n)
    return list(np.round(hi - u ** 2, 4))


def build_forms(args):
    s_knots = ([k[0] for k in SHIPPED_KNOTS] if args.shipped_knots
               else progress_knots(args.knots, args.knot_layout))
    t_knots = time_knots(args.knots, args.knot_layout)
    forms = {
        "F0": shipped_form(),
        "F1": Form("F1", "refit on the shipped progress axis", "s", s_knots),
        "F2": Form("F2", "refit on seconds-before-impact", "t", t_knots),
        "F3": Form("F3", "+ linear in phi", "t", t_knots, use_phi=True),
        "F4": Form("F4", "+ shape constraints (one reversal, in line at impact)",
                   "t", t_knots, use_phi=True, shape=True),
        "F5": ParamForm("F5", "parametric: cock x release logistics (7 params)"),
    }
    want = [w.strip() for w in args.forms.split(",") if w.strip()]
    return [forms[w] for w in want if w in forms]


# ---------------------------------------------------------------------------
# Evaluation

def stats(resid, sigma=None):
    """The three numbers the decision turns on, plus the envelope check."""
    r = np.asarray(resid, dtype=float)
    r = r[np.isfinite(r)]
    if len(r) < 10:
        return None
    out = {
        "n": int(len(r)),
        "median": float(np.median(r)),
        "p10_p90": float(np.percentile(r, 90) - np.percentile(r, 10)),
        "bad30": float(np.mean(np.abs(r) > 30.0)),
    }
    if sigma is not None:
        s = np.asarray(sigma, dtype=float)
        m = np.isfinite(s) & np.isfinite(np.asarray(resid, dtype=float)) & (s > 0)
        if m.sum() >= 10:
            z = np.abs(np.asarray(resid, dtype=float)[m]) / s[m]
            out["within1sigma"] = float(np.mean(z <= 1.0))
    return out


def scoring_channels(te):
    """The label sources a held-out swing can be graded against.

    'track' and 'truth' are the originals and keep their exact meaning, so the
    decision gate is unchanged. The rest exist because the 2026-07-05 session's
    truth.json IS the instrumented band tier: without splitting hand from
    instrumented, and band from ray, a "new truth vs old truth" comparison is
    largely a set compared with itself.
    """
    out = [("track", te["t"][te["vision"]], te["beta"][te["vision"]], te["phi"][te["vision"]]),
           ("truth", te["truth_t"], te["truth_beta"], te["truth_phi"])]

    src = te.get("truth_src")
    if src is not None and te["truth_t"] is not None:
        for name, m in (("truth_hand", src == "hand"),
                        # The SAME observations as fuse_band, but routed through
                        # truth.json (radians, corpus loader) instead of the
                        # fusion CSV (degrees, frame index). The two must agree
                        # to rounding; a gap means the timebase join is wrong,
                        # and that is a far sharper check than comparing against
                        # a different swing set.
                        ("truth_band", src == "instrumented-band")):
            if m.any():
                out.append((name, te["truth_t"][m], te["truth_beta"][m], te["truth_phi"][m]))

    ft, fb, fp, tier = (te.get("ftruth_t"), te.get("ftruth_beta"),
                        te.get("ftruth_phi"), te.get("ftruth_tier"))
    if ft is not None and len(ft):
        out.append(("fuse_all", ft, fb, fp))
        for name in ("band", "ray"):
            m = tier == name
            if m.any():
                out.append((f"fuse_{name}", ft[m], fb[m], fp[m]))
    return out


def evaluate(forms, swings, args):
    """Leave-one-swing-out. Each held-out swing is scored against a table fitted
    without it -- on the tracker's measured tier (broad) and, where the swing has
    hand-placed labels, on truth (independent)."""
    results = {f.key: {"form": f, "track": [], "truth": [], "track_sig": [],
                       "truth_sig": [], "seg": {}, "sess": {}} for f in forms}
    fitted_tables = {f.key: [] for f in forms}
    channels_seen = []

    for held in range(len(swings)):
        te = swings[held]
        # Leave-one-SWING-out by default. With ten consecutive swings of one
        # session carrying the fusion truth, that leaks session structure into
        # every fold; --holdout session withholds the whole session and the gap
        # between the two IS the leak.
        if getattr(args, "holdout", "swing") == "session":
            train = [s for s in swings if s["session"] != te["session"]]
        else:
            train = [s for i, s in enumerate(swings) if i != held]
        if not train:
            continue
        for f in forms:
            if f.key == "F0":
                mdl = f                                    # nothing to fit
            else:
                X, B, P = [], [], []
                for s in train:
                    m = s["measured"] & np.isfinite(s["beta"]) & np.isfinite(s["phi"])
                    if args.domain == "to-impact":
                        m &= s["t"] <= s["events"][PH_IMPACT]
                    if m.sum() == 0:
                        continue
                    X.append(f.x_of(s["t"][m], s["events"]))
                    B.append(s["beta"][m])
                    P.append(s["phi"][m])
                if not X:
                    continue
                mdl = f.clone()
                # Constructing a base Form here instead of cloning by type fits a
                # parametric candidate as a two-knot straight line, which reads as
                # catastrophic model failure rather than a harness bug. It has
                # bitten once; pin it.
                assert type(mdl) is type(f), f"{f.key}: clone() changed type"
                mdl.fit(np.concatenate(X), np.concatenate(B), np.concatenate(P),
                        args.halfwidth if f.axis == "t" else args.halfwidth_s)
                fitted_tables[f.key].append((mdl.centre.copy(), mdl.sigma.copy()))
                # The fitted model owns its knots: a parametric form resamples
                # itself onto a grid, so the prototype's axis is not the one the
                # fitted curve lives on.
                results[f.key]["knots_x"] = np.asarray(mdl.knots_x, dtype=float)
                if getattr(mdl, "params", None) is not None:
                    results[f.key].setdefault("params", []).append(np.array(mdl.params))

            # score on the held-out swing
            for src, tsel, bsel, psel in scoring_channels(te):
                if src not in channels_seen:
                    channels_seen.append(src)
                if tsel is None or len(tsel) == 0:
                    continue
                results[f.key].setdefault(src, [])
                results[f.key].setdefault(src + "_sig", [])
                if args.domain == "to-impact":
                    keep = np.asarray(tsel) <= te["events"][PH_IMPACT]
                    tsel, bsel, psel = np.asarray(tsel)[keep], np.asarray(bsel)[keep], np.asarray(psel)[keep]
                    if len(tsel) == 0:
                        continue
                x = mdl.x_of(tsel, te["events"])
                bh, sg = mdl.predict(x, psel)
                r = wrap180(np.asarray(bsel) - bh)
                results[f.key][src].append(r)
                results[f.key][src + "_sig"].append(sg)
                if src != "track":
                    continue
                s_prog = swing_progress(tsel, te["events"])
                # Under the to-impact domain the model makes no claim past
                # impact, so a "through" bucket would hold only the handful of
                # samples that round onto the boundary -- a degenerate segment
                # that says nothing about either table.
                buckets = [(0, .5, "backswing"), (.5, .9, "downswing")]
                if args.domain == "full":
                    buckets.append((.9, 1.01, "through"))
                for lo, hi, lbl in buckets:
                    m = (s_prog >= lo) & (s_prog < hi)
                    if m.sum():
                        results[f.key]["seg"].setdefault(lbl, []).append(r[m])
                results[f.key]["sess"].setdefault(te["session"], []).append(r)

    for k, res in results.items():
        for src in channels_seen or ("track", "truth"):
            res[src + "_stats"] = stats(np.concatenate(res[src]),
                                        np.concatenate(res[src + "_sig"])) \
                if res.get(src) else None
        res["channels"] = list(channels_seen)
        res["seg_stats"] = {lbl: stats(np.concatenate(v)) for lbl, v in res["seg"].items()}
        res["sess_stats"] = {lbl: stats(np.concatenate(v)) for lbl, v in res["sess"].items()}
        if res.get("params"):
            res["params_median"] = list(np.median(np.array(res["params"]), axis=0))
            res["params_spread"] = list(np.percentile(np.array(res["params"]), 90, axis=0)
                                        - np.percentile(np.array(res["params"]), 10, axis=0))
        if fitted_tables[k]:
            c = np.median(np.array([t[0] for t in fitted_tables[k]]), axis=0)
            s = np.median(np.array([t[1] for t in fitted_tables[k]]), axis=0)
            res["table"] = (c, s)
    return results


def envelope_coverage(res, k_sigma=3.0, factors=(1.0, 1.25, 1.5, 2.0, 2.5, 3.0)):
    """What the wedge envelope actually catches. The tracker searches
    centre +- kSigma*sigma, so the number that matters is not the 1-sigma
    fraction but whether the TRUE wrist cock falls inside that arc. Reported
    against an inflation factor on sigma, because the corpus is one athlete and
    the envelope has to survive golfers it has never seen -- the factor is a
    deliberate choice, made visible here rather than buried in a constant."""
    if not res.get("truth"):
        return {}
    r = np.concatenate(res["truth"])
    sg = np.concatenate(res["truth_sig"])
    m = np.isfinite(r) & np.isfinite(sg) & (sg > 0)
    if m.sum() < 20:
        return {}
    out = {}
    for f in factors:
        out[f] = float(np.mean(np.abs(r[m]) <= k_sigma * f * sg[m]))
    out["median_half_deg"] = float(np.median(k_sigma * sg[m]))
    return out


def gate(res_base, res_cand):
    """The decision, as specified: a candidate replaces the shipped table only
    if it is better on independent truth, better in every phase segment, its
    envelope calibrates, and its gross-error rate falls."""
    b, c = res_base.get("truth_stats"), res_cand.get("truth_stats")
    reasons = []
    if not (b and c):
        return False, ["no truth samples to decide on"]
    if not (c["p10_p90"] <= 25.0):
        reasons.append(f"p10-p90 {c['p10_p90']:.1f} > 25 target")
    if not (c["p10_p90"] < b["p10_p90"]):
        reasons.append(f"p10-p90 {c['p10_p90']:.1f} not better than shipped {b['p10_p90']:.1f}")
    if not (c["bad30"] < b["bad30"]):
        reasons.append(f"|err|>30 rate {c['bad30']*100:.1f}% not better than {b['bad30']*100:.1f}%")
    w = c.get("within1sigma")
    if w is None or not (0.63 <= w <= 0.73):
        reasons.append(f"envelope {('%.0f%%' % (w*100)) if w else 'unknown'} outside 63-73%")
    for lbl, cs in res_cand["seg_stats"].items():
        bs = res_base["seg_stats"].get(lbl)
        if bs and cs and cs["p10_p90"] > bs["p10_p90"]:
            reasons.append(f"{lbl} worse ({cs['p10_p90']:.1f} vs {bs['p10_p90']:.1f})")
    return (len(reasons) == 0), reasons


# ---------------------------------------------------------------------------
# Artefacts

def _provenance_section(results, order, swings, args):
    """Grading by label provenance -- only when --lab-root supplied.

    THE FRAME-AVERAGED PARTNER IS NOT OPTIONAL. Where the labels sit changes the
    ranking: refitting the progress axis looked worthless scored against hand
    labels (75.6 vs 74.3) and was a 22 deg gain frame-wide, because the labels
    cluster where a human can see the shaft. The rule "always report
    frame-averaged beside truth" was written after that, and is enforced here in
    code rather than left to whoever next writes the table.
    """
    chans = [c for c in (results[order[0]].get("channels") or [])
             if c not in ("track", "truth")]
    if not chans:
        return []

    if "track" not in (results[order[0]].get("channels") or []):
        raise RuntimeError("refusing to emit a truth table with no frame-averaged "
                           "partner -- see docs/research/wrist_cock_model.md")

    n_hand = n_band = 0
    for s in swings:
        src = s.get("truth_src")
        if src is None:
            continue
        n_hand += int((src == "hand").sum())
        n_band += int((src == "instrumented-band").sum())

    lines = ["", "## Grading by label provenance", "",
             f"- fusion phi source: `{args.truth_phi}` · tiers `{args.fusion_tiers}` "
             f"· holdout `{args.holdout}`",
             f"- of the corpus labels on lab-covered swings, **{n_band} are the "
             f"instrumented band tier verbatim** and {n_hand} are genuinely hand-placed",
             "- `fuse_ray` is the material the corpus never had: the fast frames a "
             "human cannot label",
             "- every truth column is printed beside `track` (frame-averaged), never alone",
             "",
             "| form | " + " | ".join(["track (frame-avg)"] + chans) + " |",
             "|---|" + "---|" * (len(chans) + 1)]
    for k in order:
        cells = []
        for c in ["track"] + chans:
            st = results[k].get(c + "_stats")
            cells.append(f"{st['p10_p90']:.1f}° (n={st['n']})" if st else "—")
        lines.append(f"| {k} | " + " | ".join(cells) + " |")
    return lines


def write_report(path, results, order, swings, args):
    lines = ["# Wrist-cock model fit", "",
             f"- run root `{args.run_root}` · sha `{git_sha()}` · domain `{args.domain}`",
             f"- {len(swings)} swings, "
             f"{sum(1 for s in swings if s['truth_t'] is not None)} with hand-placed shaft labels "
             f"({sum(len(s['truth_t']) for s in swings if s['truth_t'] is not None)} labels)",
             "- every number leave-one-swing-out: the held-out swing never fits the "
             "table it is scored against", "",
             "## Against independent truth (the number the decision rests on)", "",
             "| form | n | median | p10–p90 | \\|err\\|>30° | within 1σ |",
             "|---|---|---|---|---|---|"]
    for k in order:
        st = results[k].get("truth_stats")
        f = results[k]["form"]
        if not st:
            lines.append(f"| {f.key} {f.label} | — | — | — | — | — |")
            continue
        w = st.get("within1sigma")
        lines.append(f"| {f.key} {f.label} | {st['n']} | {st['median']:+.1f}° | "
                     f"**{st['p10_p90']:.1f}°** | {st['bad30']*100:.1f}% | "
                     f"{('%.0f%%' % (w*100)) if w else '—'} |")
    lines += ["", "## Against the tracker's own vision tier (broader, but circular)", "",
              "| form | n | median | p10–p90 | \\|err\\|>30° | within 1σ |",
              "|---|---|---|---|---|---|"]
    for k in order:
        st = results[k].get("track_stats")
        f = results[k]["form"]
        if not st:
            continue
        w = st.get("within1sigma")
        lines.append(f"| {f.key} {f.label} | {st['n']} | {st['median']:+.1f}° | "
                     f"{st['p10_p90']:.1f}° | {st['bad30']*100:.1f}% | "
                     f"{('%.0f%%' % (w*100)) if w else '—'} |")
    lines += _provenance_section(results, order, swings, args)
    lines += ["", "## By phase segment (tracker tier, p10–p90)", "",
              "| form | backswing | downswing | through |", "|---|---|---|---|"]
    for k in order:
        seg = results[k]["seg_stats"]
        lines.append("| " + results[k]["form"].key + " | " + " | ".join(
            (f"{seg[l]['p10_p90']:.1f}°" if seg.get(l) else "—")
            for l in ("backswing", "downswing", "through")) + " |")
    lines += ["", "## By session (tracker tier, p10–p90)", "",
              "| form | " + " | ".join(sorted(results[order[0]]["sess_stats"])) + " |",
              "|---" * (1 + len(results[order[0]]["sess_stats"])) + "|"]
    for k in order:
        ss = results[k]["sess_stats"]
        lines.append("| " + results[k]["form"].key + " | " + " | ".join(
            (f"{ss[l]['p10_p90']:.1f}°" if ss.get(l) else "—")
            for l in sorted(results[order[0]]["sess_stats"])) + " |")
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_tables_csv(path, results, order):
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["form", "axis", "knot_x", "centre_deg", "sigma_deg"])
        for k in order:
            res = results[k]
            f = res["form"]
            c, s = res.get("table", (f.centre, f.sigma))
            for x, cc, ss in zip(res.get("knots_x", f.knots_x), c, s):
                w.writerow([f.key, f.axis, f"{x:.3f}", f"{cc:.2f}", f"{ss:.2f}"])


def make_figure(path, results, order, swings, args):
    fig, axes = plt.subplots(1, 2, figsize=(14, 6), dpi=120)
    # left: the tables, each on its own axis, drawn against the observed cloud
    ax = axes[0]
    xs_t = np.linspace(-1.1, 0.1, 300)
    for s in swings[:40]:
        m = s["measured"] & np.isfinite(s["beta"])
        if m.sum():
            ax.plot(time_to_impact(s["t"][m], s["events"]), s["beta"][m], ".",
                    ms=1.2, color="#999999", alpha=0.25, zorder=1)
    for k in order:
        res = results[k]
        f = res["form"]
        c, sg = res.get("table", (f.centre, f.sigma))
        kx = res.get("knots_x", f.knots_x)
        if f.axis == "t":
            x = xs_t
            y, se = piecewise(x, kx, c), piecewise(x, kx, sg)
        else:
            # map the progress axis onto seconds using the corpus-median tempo
            med = {ph: np.median([(s["events"][ph] - s["events"][PH_IMPACT]) / 1e6
                                  for s in swings]) for ph, _ in PROGRESS_ANCHORS}
            x = np.interp(kx, [v for _, v in PROGRESS_ANCHORS],
                          [med[ph] for ph, _ in PROGRESS_ANCHORS])
            y, se = c, sg
        lw = 2.5 if k == "F0" else 1.8
        ls = "--" if k == "F0" else "-"
        line, = ax.plot(x, y, ls, lw=lw, label=f"{k} {f.label}", zorder=3)
        ax.fill_between(x, y - se, y + se, alpha=0.12, color=line.get_color(), lw=0, zorder=2)
        # The knots themselves, so the reader can see where the curve is
        # pinned and where it is only interpolating.
        if f.axis == "t" and not isinstance(f, ParamForm):
            ax.plot(kx, c, "o", ms=4, color=line.get_color(),
                    markeredgecolor="white", markeredgewidth=0.6, zorder=4)
    ax.axvline(0, color="k", lw=1, alpha=0.5)
    ax.axhline(0, color="k", lw=0.6, alpha=0.3)
    ax.set_xlabel("seconds before impact")
    ax.set_ylabel("signed wrist cock β = chir·(θ − φ)  [deg]")
    ax.set_title("the tables, over the observed cloud (±1σ)")
    ax.legend(fontsize=7, loc="upper left")
    ax.grid(alpha=0.25, lw=0.6)
    ax.set_ylim(-140, 140)

    # right: residual distributions against truth
    ax = axes[1]
    labels, data = [], []
    for k in order:
        r = results[k].get("truth")
        if r:
            data.append(np.concatenate(r))
            labels.append(k)
    if data:
        ax.boxplot(data, tick_labels=labels, showfliers=False, whis=(10, 90))
        ax.axhline(0, color="k", lw=0.8, alpha=0.5)
        ax.set_ylabel("β_truth − β̂  [deg]")
        ax.set_title("residual against hand-placed truth (box = quartiles, whiskers = p10–p90)")
        ax.grid(alpha=0.25, lw=0.6, axis="y")
    fig.suptitle(f"Wrist-cock model: shipped table vs fitted candidates "
                 f"({len(swings)} swings, leave-one-out)", fontsize=12)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(path)
    plt.close(fig)


def emit_header(res, form, sigma_inflate=1.0):
    """The fitted table as the C++ literal it would replace."""
    c, s = res.get("table", (form.centre, form.sigma))
    kx = res.get("knots_x", form.knots_x)
    s = np.asarray(s) * sigma_inflate
    out = ["inline constexpr WristCockKnot kWristCockKnots[%d] = {" % len(kx)]
    for x, cc, ss in zip(kx, c, s):
        x = x + 0.0            # normalise negative zero: the last knot is -0.0
        out.append(f"    {{{x:6.3f}, {cc:7.1f}, {ss:6.1f}}},")
    out.append("};")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Per-swing parametric fits (--per-swing): can the seven numbers be read off ONE
# swing?
#
# Everything above fits a POPULATION curve. §7 of the model note then reads A,
# tr, wr and r as coaching numbers "comparable between swings", which is a claim
# about a per-swing estimate that no fit in this harness has ever produced. The
# +-1 ms fold stability of tr is a property of a 59-swing estimate and says
# nothing about one swing.
#
# So: fit the same parametric form, on one swing at a time, under three
# parametrisations of falling ambition (P7 all free, P4 release side plus
# amplitude, P3 release only), on two channels (the tracker's measured tier,
# which is what production would have, and the instrumented fusion truth, which
# grades it), and put the estimates through three gates -- identifiability,
# repeatability, truthfulness.
#
# Nothing here touches the fitting or grading paths above: --per-swing is a
# separate exit from main(), and without the flag not one line below runs.

PS_PARAM_NAMES = ("b0", "A", "tc", "wc", "tr", "wr", "r")
PS_PARAM_LABEL = {"b0": "b0 address offset (deg)", "A": "A peak lag (deg)",
                  "tc": "tc cock centre (s)", "wc": "wc cock width (s)",
                  "tr": "tr release centre (s)", "wr": "wr release width (s)",
                  "r": "r release completeness"}
# Physically generous bounds. They exist to catch a fit that has run away, not
# to shape one: a fit that lands ON a bound is reported as non-converged rather
# than quietly clamped into a plausible-looking number.
PS_BOUNDS = ((-90.0, 90.0),      # b0
             (0.0, 300.0),       # A
             (-1.50, -0.05),     # tc
             (0.002, 0.50),      # wc
             (-0.20, 0.02),      # tr
             (0.001, 0.10),      # wr
             (0.0, 1.5))         # r
PS_STEP = (5.0, 20.0, 0.10, 0.05, 0.02, 0.005, 0.10)
PS_FREE = {"P7": (0, 1, 2, 3, 4, 5, 6),      # everything
           "P4": (1, 4, 5, 6),               # cock side frozen: A, tr, wr, r
           "P3": (4, 5, 6)}                  # release only: tr, wr, r
PS_HUBER_D = 15.0                            # the same Huber knee ParamForm uses
# Two sessions are pathological AT THIS RUN ROOT (tracker-tier beta spreads of
# 247 deg and 139 deg, model note §12). They are fitted, but never pooled into a
# headline number -- a spread that large is a property of those runs.
PS_PATHOLOGICAL = ("2026-06-11", "2026-07-04")
PS_MIN_SAMPLES = 20


def _ps_project(p, free):
    """Clamp the free parameters into their bounds, returning the clamped point
    and a penalty proportional to how far outside it was -- so the simplex feels
    the wall rather than walking through it."""
    q = np.array(p, dtype=float)
    pen = 0.0
    for i in free:
        lo, hi = PS_BOUNDS[i]
        v = min(max(float(q[i]), lo), hi)
        if v != q[i]:
            pen += 1e3 * ((q[i] - v) / (hi - lo)) ** 2
        q[i] = v
    return q, pen


def _ps_on_bound(p, free, tol=1e-3):
    for i in free:
        lo, hi = PS_BOUNDS[i]
        span = hi - lo
        if (p[i] - lo) <= tol * span or (hi - p[i]) <= tol * span:
            return True
    return False


def _ps_loss_value(x, b, p):
    """The same robust loss ParamForm.fit minimises, Huber knee 15°."""
    r = b - beta_param(x, p)
    a = np.abs(r)
    d = PS_HUBER_D
    return float(np.mean(np.where(a <= d, 0.5 * r * r, d * (a - 0.5 * d))))


def _ps_fit(x, b, base, free, init=None, iters=2000, step_scale=1.0):
    """Fit the free subset of the parametric form to one swing's samples under
    the same robust loss ParamForm uses, with the rest held at `base`."""
    base = np.array(base, dtype=float)
    p_init = np.array(init if init is not None else base, dtype=float)
    free = list(free)

    def loss(v):
        p = np.array(base, dtype=float)
        p[free] = v
        p, pen = _ps_project(p, free)
        return _ps_loss_value(x, b, p) + pen

    st = np.array([PS_STEP[i] for i in free], dtype=float) * step_scale
    best, val = _nelder_mead(loss, p_init[free], st, iters=iters)
    p = np.array(base, dtype=float)
    p[free] = best
    p, _ = _ps_project(p, free)
    return p, float(val)


def _ps_bound_names(p, free, tol=1e-3):
    out = []
    for i in free:
        lo, hi = PS_BOUNDS[i]
        span = hi - lo
        if (p[i] - lo) <= tol * span or (hi - p[i]) <= tol * span:
            out.append(PS_PARAM_NAMES[i])
    return out


def _ps_flat(x, b, p, free):
    """The free parameters this swing's samples cannot see at all.

    beta(t) depends on tr, wr and r only through a logistic that is numerically
    zero well before the release, so a swing whose samples all sit earlier than
    that leaves the loss EXACTLY constant in those three. Nelder-Mead then stops
    wherever its own simplex happened to shrink -- at a number determined by the
    initialisation and nothing else, identical across swings, wearing the
    appearance of an estimate. It showed up immediately on the truth channel.
    Detect it and refuse to call the fit converged."""
    base = _ps_loss_value(x, b, p)
    out = []
    for i in free:
        d = 0.0
        for f in (0.1, 0.5, 1.0):
            for sgn in (+1.0, -1.0):
                q = np.array(p, dtype=float)
                q[i] += sgn * f * PS_STEP[i]
                q, _ = _ps_project(q, [i])
                d = max(d, abs(_ps_loss_value(x, b, q) - base))
        if d <= 1e-6 * max(abs(base), 1e-12):
            out.append(PS_PARAM_NAMES[i])
    return out


def _ps_resid_stats(x, b, p):
    r = b - beta_param(x, p)
    return {"resid_robust_deg": float(0.7413 * (np.percentile(r, 75) - np.percentile(r, 25))),
            "resid_med_abs_deg": float(np.median(np.abs(r)))}


def _ps_channels(s, args):
    """The two channels a per-swing fit can see, on the model's valid domain.

    tracker -- the measured tier, the samples the population fit uses and the
               only thing production would have.
    truth   -- the instrumented stripe-fusion rows (band + ray) joined by
               _attach_fusion, which exist for the ten swings of 2026-07-05.
    """
    out = []
    imp = s["events"][PH_IMPACT]
    m = s["measured"] & np.isfinite(s["beta"]) & (s["t"] <= imp)
    x = time_to_impact(s["t"][m], s["events"])
    keep = (x >= -1.3) & (x <= 0.05)
    out.append(("tracker", x[keep], s["beta"][m][keep]))

    ft, fb = s.get("ftruth_t"), s.get("ftruth_beta")
    if ft is not None and len(ft):
        k = np.asarray(ft) <= imp
        xt = time_to_impact(np.asarray(ft)[k], s["events"])
        bt = np.asarray(fb)[k]
        kk = (xt >= -1.3) & (xt <= 0.05) & np.isfinite(bt)
        if kk.sum():
            out.append(("truth", xt[kk], bt[kk]))
    return out


def _ps_estimate(x, b, pop, free):
    """The estimator, in one place: fit from the population value, restart from
    the answer, keep the better. The bootstrap re-runs THIS, initialisation
    included, so the interval measures the procedure and not a warm start."""
    p1, l1 = _ps_fit(x, b, pop, free)
    p2, l2 = _ps_fit(x, b, pop, free, init=p1, step_scale=0.15)
    p, val = (p2, l2) if l2 <= l1 else (p1, l1)
    # A restart from the answer that finds materially better ground means the
    # first simplex had not converged; 1% of the loss is the line.
    ok_simplex = ((l1 - l2) / max(abs(l1), 1e-12)) <= 0.01
    return p, val, ok_simplex


def _ps_one(x, b, pop, free, rng, n_boot):
    """One swing x channel x parametrisation: the point estimate, its bootstrap
    CI, and an honest convergence flag."""
    p, val, ok_simplex = _ps_estimate(x, b, pop, free)
    bound = _ps_bound_names(p, free)
    flat = _ps_flat(x, b, p, free)

    n = len(x)
    reps, boot_ok = [], 0
    for _ in range(n_boot):
        idx = rng.integers(0, n, n)
        pb, _lb, _ok = _ps_estimate(x[idx], b[idx], pop, free)
        reps.append(pb)
        if not _ps_on_bound(pb, free):
            boot_ok += 1
    ci = np.full(len(PS_PARAM_NAMES), np.nan)
    if reps:
        R = np.array(reps)
        ci = 0.5 * (np.percentile(R, 95, axis=0) - np.percentile(R, 5, axis=0))
    rec = {"params": p, "ci": ci, "loss": val,
           "converged": bool(ok_simplex and not bound and not flat),
           "on_bound": bool(bound), "bound_params": "|".join(bound),
           "flat": bool(flat), "flat_params": "|".join(flat),
           "simplex_ok": bool(ok_simplex),
           "boot_ok_frac": (boot_ok / n_boot) if n_boot else float("nan")}
    rec.update(_ps_resid_stats(x, b, p))
    return rec


# -- small robust statistics, numpy only ------------------------------------

def _ps_mad(v):
    v = np.asarray(v, dtype=float)
    v = v[np.isfinite(v)]
    if len(v) < 2:
        return float("nan")
    return float(np.median(np.abs(v - np.median(v))))


def _ps_sigma(v):
    """MAD rescaled to a Gaussian sigma, so a spread and a CI half-width can be
    put in the same units."""
    return 1.4826 * _ps_mad(v)


def _ps_rank(v):
    v = np.asarray(v, dtype=float)
    order = np.argsort(v, kind="mergesort")
    ranks = np.empty(len(v), dtype=float)
    ranks[order] = np.arange(1, len(v) + 1, dtype=float)
    # average ties
    sv = v[order]
    i = 0
    while i < len(sv):
        j = i
        while j + 1 < len(sv) and sv[j + 1] == sv[i]:
            j += 1
        if j > i:
            ranks[order[i:j + 1]] = np.mean(ranks[order[i:j + 1]])
        i = j + 1
    return ranks


def _ps_spearman(a, b):
    a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
    m = np.isfinite(a) & np.isfinite(b)
    if m.sum() < 4:
        return float("nan")
    ra, rb = _ps_rank(a[m]), _ps_rank(b[m])
    ra = ra - ra.mean()
    rb = rb - rb.mean()
    den = math.sqrt(float(ra @ ra) * float(rb @ rb))
    return float(ra @ rb / den) if den > 0 else float("nan")


def _ps_ok(row, nm):
    """Is THIS parameter usable on this swing?

    Whole-fit convergence is too blunt to read a gate with: a fit whose `r` is
    flat may still pin `t_r` perfectly well (conditional on `r` sitting at its
    population value, which is exactly what P3 does on purpose). So each gate
    asks per parameter -- the simplex converged, and this parameter is neither
    flat nor pressed against a bound."""
    return (row["simplex_ok"]
            and nm not in (row["flat_params"].split("|") if row["flat_params"] else [])
            and nm not in (row["bound_params"].split("|") if row["bound_params"] else []))


def _ps_sel(rows, **kw):
    return [r for r in rows if all(r.get(k) == v for k, v in kw.items())]


def _ps_vals(rows, key):
    return np.array([r[key] for r in rows], dtype=float)


def _ps_fmt(v, fmt="{:.4g}"):
    return fmt.format(v) if np.isfinite(v) else "—"


def _ps_verdict(ratio, good, marginal, invert=False):
    if not np.isfinite(ratio):
        return "degenerate"
    ok = (ratio >= good) if invert else (ratio <= good)
    mid = (ratio >= marginal) if invert else (ratio <= marginal)
    return "PASS" if ok else ("marginal" if mid else "FAIL")


def run_per_swing(swings, rejected, args):
    """The experiment of model note §13: fit the parametric form per swing and
    put the estimates through the three gates."""
    import time as _time
    t_start = _time.time()
    out = Path(args.out)

    # 1. The population fit, in-run: the freeze values and the initialisation.
    X, B, P = [], [], []
    for s in swings:
        m = s["measured"] & np.isfinite(s["beta"]) & np.isfinite(s["phi"])
        m &= s["t"] <= s["events"][PH_IMPACT]
        if m.sum() == 0:
            continue
        X.append(time_to_impact(s["t"][m], s["events"]))
        B.append(s["beta"][m])
        P.append(s["phi"][m])
    pop_form = ParamForm("F5", "population").fit(np.concatenate(X), np.concatenate(B),
                                                 np.concatenate(P), args.halfwidth)
    pop = np.asarray(pop_form.params, dtype=float)
    note = {"b0": -10.4, "A": 98.7, "tc": -0.702, "wc": 0.089,
            "tr": -0.032, "wr": 0.011, "r": 0.741}
    tol = {"b0": 1.0, "A": 3.0, "tc": 0.02, "wc": 0.005,
           "tr": 0.005, "wr": 0.003, "r": 0.05}
    pop_check = []
    for nm, v in zip(PS_PARAM_NAMES, pop):
        d = float(v) - note[nm]
        pop_check.append((nm, float(v), note[nm], d, abs(d) <= tol[nm]))
    print("[per-swing] population fit vs model note §7:")
    for nm, v, ref, d, ok in pop_check:
        print(f"[per-swing]   {nm:3s} {v:9.4f}  note {ref:8.3f}  delta {d:+8.4f}  "
              f"{'ok' if ok else 'MISMATCH'}")
    if not all(c[-1] for c in pop_check):
        raise SystemExit("[per-swing] population fit disagrees with the model note — "
                         "investigate before reading any per-swing number")

    # 2. Every swing x channel x parametrisation. Seeds are deterministic in the
    #    (sorted) iteration order, and recorded per row.
    rows, drops = [], []
    fit_i = 0
    for s in sorted(swings, key=lambda z: z["name"]):
        chans = _ps_channels(s, args)
        have = {c[0] for c in chans}
        if "truth" not in have and s.get("ftruth_t") is not None:
            drops.append((s["name"], "truth", "fusion rows present but none pre-impact"))
        for chan, x, b in chans:
            n = len(x)
            n250 = int((x >= -0.25).sum())
            n100 = int((x >= -0.10).sum())     # where the release parameters live
            if n < PS_MIN_SAMPLES:
                drops.append((s["name"], chan, f"only {n} samples on address→impact"))
                continue
            for par in ("P7", "P4", "P3"):
                fit_i += 1
                seed = int(args.per_swing_seed) + fit_i
                rng = np.random.default_rng(seed)
                nb = args.per_swing_boot_p7 if par == "P7" else args.per_swing_boot
                rec = _ps_one(x, b, pop, PS_FREE[par], rng, nb)
                row = {"swing": s["name"], "session": s["session"], "channel": chan,
                       "parametrisation": par, "n_samples": n,
                       "n_samples_last_250ms": n250,
                       "n_samples_last_100ms": n100,
                       "converged": int(rec["converged"]),
                       "on_bound": int(rec["on_bound"]),
                       "bound_params": rec["bound_params"],
                       "flat": int(rec["flat"]), "flat_params": rec["flat_params"],
                       "simplex_ok": int(rec["simplex_ok"]),
                       "boot_reps": nb, "boot_ok_frac": rec["boot_ok_frac"],
                       "seed": seed,
                       "pathological": int(s["session"].startswith(PS_PATHOLOGICAL)),
                       "loss": rec["loss"],
                       "resid_robust_deg": rec["resid_robust_deg"],
                       "resid_med_abs_deg": rec["resid_med_abs_deg"]}
                for j, nm in enumerate(PS_PARAM_NAMES):
                    row[nm] = float(rec["params"][j])
                    row["ci_" + nm] = (float(rec["ci"][j]) if j in PS_FREE[par]
                                       else float("nan"))
                rows.append(row)
        print(f"[per-swing] {s['name']}: " +
              ", ".join(f"{c[0]} n={len(c[1])}" for c in chans))

    # 3. The CSV.
    cols = (["swing", "session", "channel", "parametrisation", "n_samples",
             "n_samples_last_250ms", "n_samples_last_100ms", "converged",
             "on_bound", "bound_params", "flat", "flat_params", "simplex_ok",
             "boot_reps", "boot_ok_frac", "seed", "pathological"]
            + list(PS_PARAM_NAMES) + ["ci_" + n for n in PS_PARAM_NAMES]
            + ["resid_robust_deg", "resid_med_abs_deg", "loss"])
    with open(out / "per_swing_params.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({k: (f"{r[k]:.6g}" if isinstance(r[k], float) else r[k])
                        for k in cols})

    _ps_write_summary(out / "per_swing_summary.md", rows, pop, pop_check, swings,
                      rejected, drops, args, _time.time() - t_start)
    print(f"[per-swing] {len(rows)} fits in {(_time.time()-t_start)/60:.1f} min → "
          f"{out/'per_swing_params.csv'}, {out/'per_swing_summary.md'}")
    return 0


def _ps_write_summary(path, rows, pop, pop_check, swings, rejected, drops, args, secs):
    healthy = [r for r in rows if not r["pathological"]]
    V1, V2, V3 = {}, {}, {}          # gate verdicts, for the synthesis at the end
    L = ["# Per-swing wrist-cock parameters: identifiability, repeatability, truthfulness",
         "",
         f"*Generated by `wrist_cock_fit.py --per-swing`, sha `{git_sha()}`, "
         f"run root `{args.run_root}`, {secs/60:.1f} min.*", "",
         "The model note fits the seven parameters as a **population** curve over 59 "
         "swings and then reads `A`, `t_r`, `w_r`, `r` as coaching numbers "
         "\"comparable between swings\". That is a claim about a **per-swing** "
         "estimate. This is the test of it.", "",
         "## Method", "",
         "- same parametric form, same robust (Huber, knee 15°) loss, same "
         "Nelder–Mead, same address→impact domain on the seconds-before-impact axis;",
         "- three parametrisations of falling ambition: **P7** all seven free, "
         "**P4** `b₀,t_c,w_c` frozen at the population fit (`A,t_r,w_r,r` free), "
         "**P3** `A` frozen too (release only);",
         "- two channels: **tracker** = the measured tier, what production would "
         "have; **truth** = the instrumented stripe-fusion rows (band+ray) of "
         "2026-07-05, joined by the harness's existing `_attach_fusion`;",
         f"- confidence intervals by resampling frames within the swing with "
         f"replacement ({args.per_swing_boot} reps for P3/P4, "
         f"{args.per_swing_boot_p7} for P7), fixed seeds recorded per row; the "
         "half-width quoted is **half the central-90% interval**;",
         "- a fit is **converged** only if a restart from its own answer finds no "
         "more than 1% of the loss, no free parameter sits on a bound, and no "
         "free parameter is flat — see Convergence below;",
         "- seeds are fixed, so the command at the foot of this page reproduces "
         "every number in it.", "",
         "## The population fit (freeze and initialisation values)", "",
         "| parameter | this run | model note §7 | Δ |", "|---|---|---|---|"]
    for nm, v, ref, d, ok in pop_check:
        L.append(f"| `{nm}` | {v:.4f} | {ref:.3f} | {d:+.4f} {'' if ok else '**MISMATCH**'} |")
    L += ["", "It reproduces the note to within rounding on all seven, so the freeze "
          "values are the note's values.", ""]

    # inventory
    n_tr = len({r["swing"] for r in rows if r["channel"] == "tracker"})
    n_th = len({r["swing"] for r in rows if r["channel"] == "truth"})
    L += ["## Swings in and out", "",
          f"- {len(swings)} swings loaded from the run root; **{n_tr}** carry a "
          f"tracker channel and **{n_th}** carry the instrumented truth channel.",
          f"- dropped before loading ({len(rejected)}):"]
    for nm, why in rejected:
        L.append(f"  - `{nm}` — {why}")
    if drops:
        L.append(f"- dropped per channel ({len(drops)}):")
        for nm, ch, why in drops:
            L.append(f"  - `{nm}` / {ch} — {why}")
    else:
        L.append("- no swing×channel dropped after loading.")
    L += ["- sessions 2026-06-11 and 2026-07-04 are **pathological at this run root** "
          "(tracker-tier β spreads of 247°/139°, note §12). They are fitted and "
          "carried in the CSV, but every headline number below is healthy-sessions "
          "only; their table is separate.", ""]

    # Where the release parameters live, and whether the data is there at all.
    L += ["## Data coverage where the release lives", "",
          "`t_r ≈ −32 ms` with `w_r ≈ 11 ms`: the release transition occupies "
          "roughly the last 60 ms before impact. The population fit sees every "
          "swing's samples at once; a per-swing fit sees only its own.", "",
          "| channel | swings | median samples, last 250 ms | median samples, last "
          "100 ms | swings with **0** in the last 100 ms | pooled samples, last 100 ms |",
          "|---|---|---|---|---|---|"]
    for ch in ("tracker", "truth"):
        sel = _ps_sel(rows, parametrisation="P3", channel=ch)
        if not sel:
            continue
        v250 = _ps_vals(sel, "n_samples_last_250ms")
        v100 = _ps_vals(sel, "n_samples_last_100ms")
        L.append(f"| {ch} | {len(sel)} | {np.median(v250):.0f} | {np.median(v100):.0f} | "
                 f"{int((v100 == 0).sum())} | {int(v100.sum())} |")
    L += ["", "That is the mechanism behind everything below: the release fires in "
          "the frames the tracker is least able to measure and the tape is least "
          "legible, so a single swing often carries a handful of samples there — "
          "and sometimes none at all, in which case the loss is **exactly flat** in "
          "`t_r`, `w_r` and `r` and the fit returns its initialisation.", ""]

    # Convergence by session, tracker channel: where the usable fits actually are.
    L += ["### convergence by session (tracker channel)", "",
          "| session | swings | median n last 100 ms | P7 | P4 | P3 |",
          "|---|---|---|---|---|---|"]
    for ss in sorted({r["session"] for r in rows}):
        sel = _ps_sel(rows, session=ss, channel="tracker", parametrisation="P3")
        if not sel:
            continue
        cells = []
        for par in ("P7", "P4", "P3"):
            q = _ps_sel(rows, session=ss, channel="tracker", parametrisation=par)
            cells.append(f"{sum(r['converged'] for r in q)}/{len(q)}")
        flag = " *(pathological)*" if sel[0]["pathological"] else ""
        L.append(f"| `{ss}`{flag} | {len(sel)} | "
                 f"{np.median(_ps_vals(sel,'n_samples_last_100ms')):.0f} | "
                 + " | ".join(cells) + " |")
    L.append("")

    # convergence
    L += ["## Convergence", "",
          "A fit counts as converged only if the restart finds ≤1% more loss, no "
          "free parameter sits on a bound, and no free parameter is **flat** — "
          "invisible to that swing's samples, so that the number returned is a "
          "property of the initialisation rather than of the swing.", "",
          "| parametrisation | channel | swings | converged | on a bound | flat | "
          "restart moved | median robust residual |",
          "|---|---|---|---|---|---|---|---|"]
    for par in ("P7", "P4", "P3"):
        for ch in ("tracker", "truth"):
            sel = _ps_sel(rows, parametrisation=par, channel=ch)
            if not sel:
                continue
            n = len(sel)
            L.append(f"| {par} | {ch} | {n} | **{sum(r['converged'] for r in sel)}/{n}** | "
                     f"{sum(r['on_bound'] for r in sel)} | "
                     f"{sum(r['flat'] for r in sel)} | "
                     f"{sum(1 for r in sel if not r['simplex_ok'])} | "
                     f"{np.median(_ps_vals(sel,'resid_robust_deg')):.1f}° |")
    L.append("")
    tallies = []
    for par in ("P7", "P4", "P3"):
        for ch in ("tracker", "truth"):
            cnt = {}
            for r in _ps_sel(rows, parametrisation=par, channel=ch):
                for nm in (r["bound_params"].split("|") if r["bound_params"] else []):
                    cnt["`%s` on bound" % nm] = cnt.get("`%s` on bound" % nm, 0) + 1
                for nm in (r["flat_params"].split("|") if r["flat_params"] else []):
                    cnt["`%s` flat" % nm] = cnt.get("`%s` flat" % nm, 0) + 1
            if cnt:
                top = sorted(cnt.items(), key=lambda kv: -kv[1])
                tallies.append(f"- {par}/{ch}: " +
                               ", ".join(f"{k} ×{v}" for k, v in top))
    if tallies:
        L += ["Which parameters fail, and how:", ""] + tallies + [""]

    # ---- Gate 1
    L += ["## Gate 1 — identifiability", "",
          "*A parameter is identifiable per swing only if one swing's own "
          "confidence interval is clearly narrower than the spread between "
          "swings; otherwise the estimate carries no information about which "
          "swing it came from.* Between-swing spread is the MAD across converged "
          "healthy-session swings, rescaled to a Gaussian σ and then to a "
          "central-90% half-width (×1.645) so it is in the same units as the CI. "
          "**PASS** = ratio ≤ 0.5, **marginal** ≤ 1.0, **FAIL** above.", "",
          "Each row counts only the swings on which **that parameter** is "
          "estimable — the simplex converged and the parameter is neither flat "
          "nor on a bound — so the trailing fraction is itself a result: a "
          "parameter estimable on a third of swings is not a coaching readout "
          "whatever its ratio says.", ""]
    for ch in ("tracker", "truth"):
        L += [f"### channel: {ch}", "",
              "| parametrisation | parameter | median CI half-width | between-swing "
              "half-width | ratio | verdict |", "|---|---|---|---|---|---|"]
        # (`ratio` is the per-swing interval divided by the between-swing spread;
        #  a parameter only says something about a swing when it is well below 1)
        for par in ("P7", "P4", "P3"):
            pool = _ps_sel(healthy, parametrisation=par, channel=ch)
            for j, nm in enumerate(PS_PARAM_NAMES):
                if j not in PS_FREE[par]:
                    continue
                sel = [r for r in pool if _ps_ok(r, nm)]
                frac = f"{len(sel)}/{len(pool)}"
                if len(sel) < 4:
                    V1[(par, ch, nm)] = ("unusable", frac)
                    L.append(f"| {par} | `{nm}` | — | — | — | **unusable** "
                             f"({frac} swings estimable) |")
                    continue
                ci = float(np.median(_ps_vals(sel, "ci_" + nm)))
                hw = 1.645 * _ps_sigma(_ps_vals(sel, nm))
                ratio = ci / hw if hw > 0 else float("nan")
                v = _ps_verdict(ratio, 0.5, 1.0)
                V1[(par, ch, nm)] = (v, frac)
                L.append(f"| {par} | `{nm}` | {_ps_fmt(ci)} | {_ps_fmt(hw)} | "
                         f"{_ps_fmt(ratio, '{:.2f}')} | **{v}** ({frac} swings) |")
        L.append("")

    # ---- Gate 2
    L += ["## Gate 2 — repeatability", "",
          "*Within-session spread against between-session spread. If a golfer's "
          "ten swings in one session scatter as widely as his session medians "
          "differ, the number is repeatability noise wearing a coaching name.* "
          "Both are robust σ (1.4826·MAD); within-session is the median over "
          "healthy sessions with ≥4 converged swings, between-session is the σ of "
          "the session medians. **PASS** = between/within ≥ 1.5, **marginal** ≥ "
          "0.5.", ""]
    for ch in ("tracker", "truth"):
        sel_all = _ps_sel(healthy, channel=ch)
        sess = sorted({r["session"] for r in sel_all})
        if len(sess) < 2:
            L += [f"### channel: {ch}", "",
                  f"Only {len(sess)} healthy session(s) carry this channel — the "
                  "between-session term does not exist, so this gate cannot be "
                  "read here.", ""]
            continue
        L += [f"### channel: {ch}", "",
              "| parametrisation | parameter | within-session σ (median) | "
              "between-session σ of medians | ratio | verdict |",
              "|---|---|---|---|---|---|"]
        for par in ("P7", "P4", "P3"):
            for j, nm in enumerate(PS_PARAM_NAMES):
                if j not in PS_FREE[par]:
                    continue
                within, meds = [], []
                for ss in sess:
                    v = _ps_vals([r for r in sel_all
                                  if r["parametrisation"] == par and r["session"] == ss
                                  and _ps_ok(r, nm)], nm)
                    if len(v) >= 4:
                        within.append(_ps_sigma(v))
                        meds.append(float(np.median(v)))
                if len(meds) < 2:
                    continue
                w = float(np.median(within))
                bs = _ps_sigma(np.array(meds))
                ratio = bs / w if w > 0 else float("nan")
                v = _ps_verdict(ratio, 1.5, 0.5, invert=True)
                V2[(par, ch, nm)] = (v, f"{len(meds)} sess")
                L.append(f"| {par} | `{nm}` | {_ps_fmt(w)} | {_ps_fmt(bs)} | "
                         f"{_ps_fmt(ratio, '{:.2f}')} | **{v}** "
                         f"({len(meds)} sessions) |")
        L.append("")

    # The confound this gate cannot escape on its own: a between-session
    # difference that tracks how many samples the session HAS near impact is a
    # measurement property, not a property of the golfer's release.
    L += ["### session medians beside the coverage that produced them", "",
          "Read this table against the per-session `n last 100 ms` above. Where a "
          "session's release samples are absent its `w_r` collapses toward the "
          "lower bound and its `t_r` drifts to impact — so a between-session "
          "difference in the release parameters can be a difference in **coverage** "
          "rather than in the golfer.", "",
          "| session | n last 100 ms | " +
          " | ".join(f"P4 `{n}`" for n in ("A", "tr", "wr", "r")) + " |",
          "|---|---|---|---|---|---|"]
    for ss in sorted({r["session"] for r in rows}):
        pool = _ps_sel(rows, session=ss, channel="tracker", parametrisation="P4")
        if not pool:
            continue
        cells = []
        for nm in ("A", "tr", "wr", "r"):
            v = _ps_vals([r for r in pool if _ps_ok(r, nm)], nm)
            cells.append(f"{np.median(v):.4g} (n={len(v)})" if len(v) >= 2 else "—")
        flag = " *(path.)*" if pool[0]["pathological"] else ""
        L.append(f"| `{ss}`{flag} | "
                 f"{np.median(_ps_vals(pool,'n_samples_last_100ms')):.0f} | "
                 + " | ".join(cells) + " |")
    L.append("")

    # pathological sessions, separately
    L += ["### the two pathological sessions, separately", "",
          "| parametrisation | parameter | 2026-06-11 median (σ) | "
          "2026-07-04 median (σ) | healthy median (σ) |", "|---|---|---|---|---|"]
    for par in ("P7", "P4", "P3"):
        for j, nm in enumerate(PS_PARAM_NAMES):
            if j not in PS_FREE[par]:
                continue
            cells = []
            for pref in PS_PATHOLOGICAL:
                v = _ps_vals([r for r in rows if r["parametrisation"] == par
                              and r["channel"] == "tracker" and _ps_ok(r, nm)
                              and r["session"].startswith(pref)], nm)
                cells.append(f"{np.median(v):.4g} ({_ps_sigma(v):.3g})" if len(v) >= 3 else "—")
            v = _ps_vals([r for r in _ps_sel(healthy, parametrisation=par,
                                             channel="tracker") if _ps_ok(r, nm)], nm)
            cells.append(f"{np.median(v):.4g} ({_ps_sigma(v):.3g})" if len(v) >= 3 else "—")
            L.append(f"| {par} | `{nm}` | " + " | ".join(cells) + " |")
    L.append("")

    # ---- Gate 3
    L += ["## Gate 3 — truthfulness (the decisive one)", "",
          "*On the ten instrumented swings of 2026-07-05, does the estimate a "
          "production tracker would produce agree with the estimate the "
          "instrumented truth produces on the same swing? If tracker-based `t_r` "
          "does not track truth-based `t_r`, the parameter is real but "
          "unmeasurable in production.* Sign agreement is the fraction of swings "
          "whose deviation from the population value has the same sign in both "
          "channels (chance = 50%). **PASS** = ρ ≥ 0.6 and sign ≥ 0.8 and "
          "median|Δ| ≤ half the between-swing truth spread.", ""]
    L += ["| parametrisation | parameter | pairs used | median \\|Δ\\| | "
          "between-swing σ (truth) | \\|Δ\\|/σ | Spearman ρ | sign agreement | verdict |",
          "|---|---|---|---|---|---|---|---|---|"]
    for par in ("P7", "P4", "P3"):
        tr = {r["swing"]: r for r in _ps_sel(rows, parametrisation=par, channel="tracker")}
        th = {r["swing"]: r for r in _ps_sel(rows, parametrisation=par, channel="truth")}
        pairs = sorted(set(tr) & set(th))
        for j, nm in enumerate(PS_PARAM_NAMES):
            if j not in PS_FREE[par]:
                continue
            both = [s for s in pairs if _ps_ok(tr[s], nm) and _ps_ok(th[s], nm)]
            if len(both) < 4:
                V3[(par, nm)] = ("unusable", f"{len(both)}/{len(pairs)}")
                L.append(f"| {par} | `{nm}` | {len(both)}/{len(pairs)} | — | — | — | "
                         "— | — | **unusable** (too few estimable pairs) |")
                continue
            a = np.array([tr[s][nm] for s in both], dtype=float)
            b = np.array([th[s][nm] for s in both], dtype=float)
            md = float(np.median(np.abs(a - b)))
            sig = _ps_sigma(b)
            rho = _ps_spearman(a, b)
            sgn = float(np.mean(np.sign(a - pop[j]) == np.sign(b - pop[j])))
            ratio = md / sig if sig > 0 else float("nan")
            ok = (np.isfinite(rho) and rho >= 0.6 and sgn >= 0.8
                  and np.isfinite(ratio) and ratio <= 0.5)
            mid = (np.isfinite(rho) and rho >= 0.3 and sgn >= 0.6)
            v = "PASS" if ok else ("marginal" if mid else "FAIL")
            V3[(par, nm)] = (v, f"{len(both)}/{len(pairs)}")
            L.append(f"| {par} | `{nm}` | {len(both)}/{len(pairs)} | {_ps_fmt(md)} | "
                     f"{_ps_fmt(sig)} | {_ps_fmt(ratio, '{:.2f}')} | "
                     f"{_ps_fmt(rho, '{:+.2f}')} | {sgn*100:.0f}% | **{v}** |")

    # -- the three gates on one page, for the four parameters §7 sells ---------
    L += ["", "## The three gates together, for the four coaching parameters", "",
          "`A` how much lag, `t_r` when it is released, `w_r` how violently, `r` "
          "whether any is still held at impact. Gate 1 and Gate 2 are the tracker "
          "channel (healthy sessions); Gate 3 is tracker vs instrumented truth. "
          "The fraction after each verdict is how many swings (or pairs) the "
          "parameter was estimable on at all.", "",
          "| parametrisation | parameter | Gate 1 identifiable | Gate 2 repeatable "
          "| Gate 3 truthful |", "|---|---|---|---|---|"]
    for par in ("P7", "P4", "P3"):
        for j, nm in enumerate(PS_PARAM_NAMES):
            if j not in PS_FREE[par] or nm not in ("A", "tr", "wr", "r"):
                continue
            g1 = V1.get((par, "tracker", nm), ("no data", "—"))
            g2 = V2.get((par, "tracker", nm), ("no data", "—"))
            g3 = V3.get((par, nm), ("no data", "—"))
            L.append(f"| {par} | `{nm}` | {g1[0]} ({g1[1]}) | {g2[0]} ({g2[1]}) | "
                     f"**{g3[0]}** ({g3[1]}) |")
    passed = sorted({f"`{nm}` under {par}" for (par, nm), (v, _f) in V3.items()
                     if v == "PASS"})
    unusable = sorted({f"`{nm}` under {par}" for (par, nm), (v, _f) in V3.items()
                       if v == "unusable"})
    if passed:
        L += ["", "Gate 3 is the decisive one, and it is cleared by: "
              + ", ".join(passed) + ".", ""]
    else:
        L += ["", "Gate 3 is the decisive one and **no parameter clears it under "
              "any parametrisation**: where both channels can be estimated they "
              "disagree by more than the swings differ from one another.", ""]
    if unusable:
        L += ["Not even testable, for want of a swing on which both channels "
              "estimate the parameter at all: " + ", ".join(unusable) + ".", ""]

    L += ["## Reproduction", "",
          "```", args.per_swing_cmd or "(command not recorded)", "```", ""]
    Path(path).write_text("\n".join(L) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# P1 (--p1): one free parameter per swing, a release TIME-SHIFT.
#
# The P7/P4/P3 experiment above returned a negative result whose mechanism was
# coverage: the release occupies the last ~60 ms, a single swing carries a
# handful of measured samples there and the instrumented tape carries none on
# eight swings in ten, so t_r, w_r and r are unconstrained one swing at a time.
#
# P1 collapses the ambition to the smallest question that still means something
# to a coach -- DOES THIS SWING RELEASE EARLIER OR LATER THAN THE POPULATION? --
# by freezing the whole population curve except a scalar shift of the release
# event:
#
#     beta_d(t) = b0 + A*L((t-tc)/wc) * (1 - r*L((t-(tr+d))/wr))
#
# One parameter is constrained by the whole release SLOPE, not only by samples
# inside the transition, so the last 250 ms carries it -- where truth has a
# median of 15 rows rather than 0.
#
# Four channels, in rising order of how much they are inferred:
#   measured         the tracker's measured tier (what production has)
#   measured+anchor  the same, plus the single P7 impact anchor from
#                    club.positions as one weighted observation at t=0
#   synth            club.synth, the Layer-C 240 Hz Hermite series through the
#                    P-anchors. INFERRED, not measured (see the summary)
#   truth            the instrumented fusion band+ray rows (10 swings)
#
# The fit is a grid search rather than a simplex: with one bounded parameter a
# 0.5 ms sweep of the whole interval is cheap, deterministic, and cannot end in
# a local minimum -- which the seven-parameter fits above could and did.

P1_DELTA_LO = -0.10                 # s; earlier release than the population
P1_DELTA_HI = 0.06                  # s; later
P1_TR_CAP = 0.02                    # tr + delta may not pass 20 ms after impact
P1_COARSE = 0.0005                  # 0.5 ms sweep
P1_FINE = 0.00002                   # 0.02 ms refinement
P1_SIGMA_REF = 8.0                  # deg: the measured tier's own robust residual
P1_SIGMA_ANCHOR_DEFAULT = 5.0       # deg: used when sigmaThetaDeg is unset (-1)
P1_ANCHOR_WMAX = 25.0               # the anchor may never outweigh 25 samples
P1_NOTE = {"b0": -10.4, "A": 98.7, "tc": -0.702, "wc": 0.089,
           "tr": -0.032, "wr": 0.011, "r": 0.741}
P1_NOTE_TOL = {"b0": 1.0, "A": 3.0, "tc": 0.02, "wc": 0.005,
               "tr": 0.005, "wr": 0.003, "r": 0.05}
P1_CHANNELS = ("measured", "measured+anchor", "synth", "truth")


def beta_shift(t, pop, deltas):
    """The population curve with its release shifted by delta, vectorised over
    delta: returns an array of shape (len(deltas), len(t))."""
    b0, A, tc, wc, tr, wr, r = [float(v) for v in pop]
    t = np.asarray(t, dtype=float)[None, :]
    d = np.asarray(deltas, dtype=float)[:, None]
    Lc = _logistic((t - tc) / max(abs(wc), 1e-3))
    Lr = _logistic((t - (tr + d)) / max(abs(wr), 1e-3))
    return b0 + A * Lc * (1.0 - r * Lr)


def _p1_huber(res, w=None):
    """The harness's Huber loss, over the last axis, optionally weighted."""
    a = np.abs(res)
    d = PS_HUBER_D
    h = np.where(a <= d, 0.5 * res * res, d * (a - 0.5 * d))
    if w is None:
        return h.mean(axis=-1)
    return (h * w).sum(axis=-1) / float(np.sum(w))


def _p1_losses(x, b, w, pop, deltas):
    return _p1_huber(np.asarray(b, dtype=float)[None, :] - beta_shift(x, pop, deltas), w)


def _p1_bounds(pop):
    return P1_DELTA_LO, min(P1_DELTA_HI, P1_TR_CAP - float(pop[4]))


def _p1_fit(x, b, w, pop):
    """Grid-and-refine over the whole bounded interval."""
    lo, hi = _p1_bounds(pop)
    grid = np.arange(lo, hi + 1e-12, P1_COARSE)
    L = _p1_losses(x, b, w, pop, grid)
    i = int(np.argmin(L))
    fine = np.arange(max(lo, grid[i] - P1_COARSE), min(hi, grid[i] + P1_COARSE) + 1e-12,
                     P1_FINE)
    Lf = _p1_losses(x, b, w, pop, fine)
    j = int(np.argmin(Lf))
    return float(fine[j]), float(Lf[j]), grid, L


def _p1_diagnose(d_hat, val, grid, L, pop, x, b, w):
    """On a bound, or flat? With one parameter the flatness probe of the
    seven-parameter fits reduces to asking whether the loss moves over delta at
    all -- globally across the sweep, and locally about the optimum."""
    lo, hi = _p1_bounds(pop)
    span = hi - lo
    on_bound = (d_hat - lo) <= 1e-3 * span or (hi - d_hat) <= 1e-3 * span
    rng = float(L.max() - L.min())
    flat_global = rng <= 1e-6 * max(abs(float(L.min())), 1e-12)
    near = np.array([max(lo, d_hat - 0.005), min(hi, d_hat + 0.005)])
    Ln = _p1_losses(x, b, w, pop, near)
    flat_local = float(np.max(np.abs(Ln - val))) <= 1e-6 * max(abs(val), 1e-12)
    return bool(on_bound), bool(flat_global or flat_local)


def _p1_leverage(x, pop, frac=0.10):
    """How many of these samples can SEE delta at all.

    d(beta)/d(delta) = A*Lc*r*Lr*(1-Lr)/wr, which is appreciable only within a
    few w_r of the release and is exactly zero everywhere else. A swing whose
    samples all sit outside that window contains no information about when the
    release fired, however many samples it has -- which is why a raw sample
    count, even a count over the last 250 ms, overstates what a channel knows.
    Counted here against 10% of the derivative's theoretical maximum."""
    b0, A, tc, wc, tr, wr, r = [float(v) for v in pop]
    x = np.asarray(x, dtype=float)
    Lc = _logistic((x - tc) / max(abs(wc), 1e-3))
    Lr = _logistic((x - tr) / max(abs(wr), 1e-3))
    dbdd = np.abs(A * Lc * r * Lr * (1.0 - Lr) / max(abs(wr), 1e-3))
    dmax = abs(A) * abs(r) * 0.25 / max(abs(wr), 1e-3)
    return int((dbdd >= frac * dmax).sum()), float(dbdd.max() / 1e3)   # deg/ms


def _p1_phi_at(run_dir, rec):
    """The swing's arm angle as a function of time, rebuilt exactly as
    load_swing() builds it -- and PINNED to it: the assertion below fails if the
    two ever diverge, which is the only thing that could silently put the synth
    and anchor channels on a different phi from the measured one."""
    an = json.load(open(Path(run_dir) / "result.json", encoding="utf-8"))["analysis"]
    club = an.get("club", {})
    wpx, hpx = club.get("frameWidth") or 0, club.get("frameHeight") or 0
    ev = rec["events"]
    pose_block = an.get("pose2d", {})
    lead_left, _m = decide_lead_side(pose_block.get("frames") or [],
                                     ev[PH_ADDRESS], ev[PH_TAKEAWAY])
    pt, praw = arm_series(pose_block, "frames", lead_left, wpx, hpx)
    sm = smooth_angle(praw["phi"])
    good = np.isfinite(sm)
    phi_u = np.degrees(np.unwrap(np.radians(sm[good])))

    def phi_at(times):
        times = np.asarray(times, dtype=float)
        v = np.interp(times, pt[good].astype(float), phi_u, left=np.nan, right=np.nan)
        near = np.abs(times[:, None] - pt[good][None, :]).min(axis=1)
        v[near > 60_000.0] = np.nan
        return v

    chk = phi_at(rec["t"])
    m = np.isfinite(chk) & np.isfinite(rec["phi"])
    if m.any():
        assert float(np.max(np.abs(chk[m] - rec["phi"][m]))) < 1e-9, \
            f"{rec['name']}: rebuilt phi differs from load_swing's"
    return phi_at, an


def _p1_channels(rec, args):
    """The four channels, each as (name, x, beta, weights, meta)."""
    out = []
    imp = rec["events"][PH_IMPACT]
    chir = rec["chir"]
    run_dir = Path(args.run_root) / rec["name"]
    phi_at, an = _p1_phi_at(run_dir, rec)
    club = an.get("club", {})

    base = _ps_channels(rec, args)               # measured (+ truth, if present)
    meas = [c for c in base if c[0] == "tracker"][0]
    xm, bm = meas[1], meas[2]
    out.append(("measured", xm, bm, None, {}))

    # + the P7 impact anchor: one observation at t=0, weighted by its own stated
    # sigma against the measured tier's residual, and never allowed to outweigh
    # P1_ANCHOR_WMAX samples however confident it claims to be.
    p7 = [p for p in (club.get("positions") or []) if int(p.get("p", -1)) == 7]
    if p7:
        p7 = p7[0]
        gx, gy = p7["grip"]
        hx, hy = p7["head"]
        wpx, hpx = club.get("frameWidth") or 1, club.get("frameHeight") or 1
        th = math.degrees(math.atan2((hy - gy) * hpx, (hx - gx) * wpx))
        ta = float(p7["t_us"])
        ph = phi_at(np.array([ta]))[0]
        sg = float(p7.get("sigmaThetaDeg", -1.0))
        sg = sg if sg > 0 else P1_SIGMA_ANCHOR_DEFAULT
        wt = min((P1_SIGMA_REF / sg) ** 2, P1_ANCHOR_WMAX)
        if np.isfinite(ph):
            ba = chir * wrap180(np.array([th - ph]))[0]
            xa = float(time_to_impact(np.array([ta]), rec["events"])[0])
            out.append(("measured+anchor",
                        np.append(xm, xa), np.append(bm, ba),
                        np.append(np.ones(len(xm)), wt),
                        {"anchor_beta_deg": float(ba), "anchor_sigma_deg": sg,
                         "anchor_weight": float(wt),
                         "anchor_dt_ms": (ta - float(imp)) / 1e3}))

    # synth: the Layer-C 240 Hz Hermite series. Same phi, same chirality, same
    # domain window as every other channel.
    syn = club.get("synth") or []
    if syn:
        ts = np.array([s["t_us"] for s in syn], dtype=np.int64)
        th = np.degrees(np.array([s["theta"] for s in syn], dtype=float))
        ph = phi_at(ts.astype(float))
        xs = time_to_impact(ts, rec["events"])
        k = (np.asarray(ts) <= imp) & (xs >= -1.3) & (xs <= 0.05) & np.isfinite(ph) \
            & np.isfinite(th)
        if k.sum() >= PS_MIN_SAMPLES:
            out.append(("synth", xs[k], chir * wrap180(th[k] - ph[k]), None,
                        {"synth_rows_total": len(syn)}))

    tr = [c for c in base if c[0] == "truth"]
    if tr:
        out.append(("truth", tr[0][1], tr[0][2], None, {}))
    return out


def run_p1(swings, rejected, args):
    """P1: fit the release time-shift per swing, on four channels, and gate it."""
    import time as _time
    t_start = _time.time()
    out = Path(args.out)

    # population fit, same in-run derivation and the same §7 sanity gate
    X, B, P = [], [], []
    for s in swings:
        m = s["measured"] & np.isfinite(s["beta"]) & np.isfinite(s["phi"])
        m &= s["t"] <= s["events"][PH_IMPACT]
        if m.sum() == 0:
            continue
        X.append(time_to_impact(s["t"][m], s["events"]))
        B.append(s["beta"][m])
        P.append(s["phi"][m])
    pop = np.asarray(ParamForm("F5", "population").fit(
        np.concatenate(X), np.concatenate(B), np.concatenate(P), args.halfwidth).params)
    pop_check = []
    for nm, v in zip(PS_PARAM_NAMES, pop):
        d = float(v) - P1_NOTE[nm]
        pop_check.append((nm, float(v), P1_NOTE[nm], d, abs(d) <= P1_NOTE_TOL[nm]))
    print("[p1] population fit vs model note §7:")
    for nm, v, ref, d, ok in pop_check:
        print(f"[p1]   {nm:3s} {v:9.4f}  note {ref:8.3f}  delta {d:+8.4f}  "
              f"{'ok' if ok else 'MISMATCH'}")
    if not all(c[-1] for c in pop_check):
        raise SystemExit("[p1] population fit disagrees with the model note")
    lo, hi = _p1_bounds(pop)
    print(f"[p1] delta bounds [{lo:+.4f}, {hi:+.4f}] s  (tr+delta capped at "
          f"{P1_TR_CAP:+.3f} s)")

    rows, drops = [], []
    fit_i = 0
    for s in sorted(swings, key=lambda z: z["name"]):
        chans = _p1_channels(s, args)
        have = {c[0] for c in chans}
        for want in P1_CHANNELS:
            if want == "truth" and s.get("ftruth_t") is None:
                continue
            if want not in have:
                drops.append((s["name"], want, "channel absent or below "
                              f"{PS_MIN_SAMPLES} samples in the domain window"))
        for chan, x, b, w, meta in chans:
            if len(x) < PS_MIN_SAMPLES:
                drops.append((s["name"], chan, f"only {len(x)} samples"))
                continue
            fit_i += 1
            seed = int(args.p1_seed) + fit_i
            rng = np.random.default_rng(seed)
            d_hat, val, grid, L = _p1_fit(x, b, w, pop)
            on_bound, flat = _p1_diagnose(d_hat, val, grid, L, pop, x, b, w)
            reps, boot_ok = [], 0
            n = len(x)
            for _ in range(args.p1_boot):
                idx = rng.integers(0, n, n)
                wb = None if w is None else np.asarray(w)[idx]
                db, vb, gb, Lb = _p1_fit(x[idx], b[idx], wb, pop)
                reps.append(db)
                ob, fl = _p1_diagnose(db, vb, gb, Lb, pop, x[idx], b[idx], wb)
                if not (ob or fl):
                    boot_ok += 1
            ci = (0.5 * (np.percentile(reps, 95) - np.percentile(reps, 5))
                  if reps else float("nan"))
            res = np.asarray(b) - beta_shift(x, pop, [d_hat])[0]
            n_lev, lev_max = _p1_leverage(x, pop)
            row = {"swing": s["name"], "session": s["session"], "channel": chan,
                   "n_samples": n,
                   "n_samples_last_250ms": int((x >= -0.25).sum()),
                   "n_samples_last_100ms": int((x >= -0.10).sum()),
                   "n_leverage": n_lev, "leverage_max_deg_per_ms": lev_max,
                   "converged": int(not on_bound and not flat),
                   "on_bound": int(on_bound), "flat": int(flat),
                   "boot_reps": args.p1_boot,
                   "boot_ok_frac": boot_ok / max(args.p1_boot, 1),
                   "seed": seed,
                   "pathological": int(s["session"].startswith(PS_PATHOLOGICAL)),
                   "delta_s": d_hat, "delta_ms": d_hat * 1e3,
                   "ci_delta_s": float(ci), "ci_delta_ms": float(ci) * 1e3,
                   "tr_eff_s": float(pop[4]) + d_hat,
                   "resid_robust_deg": float(0.7413 * (np.percentile(res, 75)
                                                       - np.percentile(res, 25))),
                   "resid_med_abs_deg": float(np.median(np.abs(res))),
                   "loss": val}
            for k in ("anchor_beta_deg", "anchor_sigma_deg", "anchor_weight",
                      "anchor_dt_ms", "synth_rows_total"):
                row[k] = meta.get(k, float("nan"))
            rows.append(row)
        print(f"[p1] {s['name']}: " + ", ".join(
            f"{c[0]} n={len(c[1])}({int((c[1] >= -0.10).sum())} in last 100ms)"
            for c in chans))

    cols = ["swing", "session", "channel", "n_samples", "n_samples_last_250ms",
            "n_samples_last_100ms", "n_leverage", "leverage_max_deg_per_ms",
            "converged", "on_bound", "flat", "boot_reps",
            "boot_ok_frac", "seed", "pathological", "delta_s", "delta_ms",
            "ci_delta_s", "ci_delta_ms", "tr_eff_s", "resid_robust_deg",
            "resid_med_abs_deg", "loss", "anchor_beta_deg", "anchor_sigma_deg",
            "anchor_weight", "anchor_dt_ms", "synth_rows_total"]
    with open(out / "p1_params.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({k: (f"{r[k]:.6g}" if isinstance(r[k], float) else r[k])
                        for k in cols})

    _p1_write_summary(out / "p1_summary.md", rows, pop, pop_check, swings, rejected,
                      drops, args, _time.time() - t_start)
    print(f"[p1] {len(rows)} fits in {(_time.time()-t_start)/60:.1f} min → "
          f"{out/'p1_params.csv'}, {out/'p1_summary.md'}")
    return 0


def _p1_pairs(rows, ch_a, ch_b, converged_both=True):
    a = {r["swing"]: r for r in rows if r["channel"] == ch_a}
    b = {r["swing"]: r for r in rows if r["channel"] == ch_b}
    keys = sorted(set(a) & set(b))
    if converged_both:
        keys = [k for k in keys if a[k]["converged"] and b[k]["converged"]]
    return keys, a, b


def _p1_write_summary(path, rows, pop, pop_check, swings, rejected, drops, args, secs):
    healthy = [r for r in rows if not r["pathological"]]
    lo, hi = _p1_bounds(pop)
    L = ["# P1: one free parameter per swing — the release time-shift δ", "",
         f"*Generated by `wrist_cock_fit.py --p1`, sha `{git_sha()}`, run root "
         f"`{args.run_root}`, {secs/60:.1f} min.*", "",
         "The P7/P4/P3 experiment (`per_swing_summary.md`) failed on **coverage**: "
         "`t_r`, `w_r` and `r` live in the last 60 ms, where one swing carries a "
         "handful of measured samples and the instrumented tape carries none at "
         "all on eight swings in ten. P1 asks the smallest question that still "
         "means something to a coach — *does this swing release earlier or later "
         "than the population?* — by freezing the entire population curve except a "
         "scalar shift δ of the release event:", "",
         "```",
         "β_δ(t) = b₀ + A·L((t−t_c)/w_c) · (1 − r·L((t−(t_r+δ))/w_r))",
         "```", "",
         "One parameter is constrained by the whole release **slope**, not only by "
         "samples inside the transition, so the last 250 ms carries it — where "
         "truth has a median of 15 rows rather than 0.", "",
         "## Method", "",
         f"- δ ∈ [{lo:+.3f}, {hi:+.3f}] s (upper bound is `t_r+δ ≤ "
         f"{P1_TR_CAP:+.3f}` s); fitted by a {P1_COARSE*1e3:.1f} ms sweep of the "
         f"whole interval refined to {P1_FINE*1e3:.2f} ms — deterministic, and "
         "with one bounded parameter it cannot end in a local minimum the way the "
         "seven-parameter simplex could;",
         "- same robust (Huber, knee 15°) loss, same address→impact domain, same "
         "seconds-before-impact axis, same φ and chirality as every other channel "
         "(the rebuilt φ is asserted identical to the one `load_swing` produced);",
         f"- bootstrap CI: {args.p1_boot} reps, resampling frames within the swing "
         "with replacement, fixed seeds recorded per row; half the central-90% "
         "interval;",
         "- **converged** = δ̂ is not on a bound and the loss is not flat in δ "
         "(globally over the sweep or locally within ±5 ms of the optimum).", "",
         "### the four channels", "",
         "| channel | what it is | measured or inferred |", "|---|---|---|",
         "| `measured` | the tracker's measured tier | measured |",
         "| `measured+anchor` | + the single P7 impact anchor from "
         "`club.positions` as one weighted observation at t≈0 | measured |",
         "| `synth` | `club.synth`, the Layer-C 240 Hz monotone-safe Hermite series "
         "through the P-anchors (θ, θ̇) | **inferred** |",
         "| `truth` | instrumented stripe-fusion band+ray, 2026-07-05 | measured, "
         "independent |", "",
         "**The `synth` channel is a deliberate research exception.** That tier is "
         "excluded from estimands in production — it exists for visualisation. Its "
         "information in the release window is not an observation of the shaft: it "
         "is the P5/P6/P7 anchor geometry smoothly interpolated. A δ fitted to it "
         "measures where the *phase ladder* puts the release, not where the club "
         "was seen to go. It is carried here because it is dense exactly where the "
         "measured tier is empty, and it must never be read as a fifth opinion of "
         "equal standing.", "",
         f"**The anchor weight.** σ_θ comes from the P7 row's own `sigmaThetaDeg`; "
         f"it is unset (−1) on 56 of 59 swings, so the default "
         f"{P1_SIGMA_ANCHOR_DEFAULT:.1f}° is used there. Weight = "
         f"min((σ_ref/σ_θ)², {P1_ANCHOR_WMAX:.0f}) with σ_ref = "
         f"{P1_SIGMA_REF:.1f}° (the measured tier's own robust residual), so the "
         f"anchor counts as at most {P1_ANCHOR_WMAX:.0f} samples and typically as "
         f"{(P1_SIGMA_REF/P1_SIGMA_ANCHOR_DEFAULT)**2:.1f}.", "",
         "## The population fit (the curve δ shifts)", "",
         "| parameter | this run | model note §7 | Δ |", "|---|---|---|---|"]
    for nm, v, ref, d, ok in pop_check:
        L.append(f"| `{nm}` | {v:.4f} | {ref:.3f} | {d:+.4f} "
                 f"{'' if ok else '**MISMATCH**'} |")

    L += ["", "## Availability and convergence", "",
          "`n leverage` is the number of samples that can see δ at all — where "
          "|∂β/∂δ| exceeds 10% of its theoretical maximum, i.e. inside the release "
          "transition. A sample count, even over the last 250 ms, overstates what "
          "a channel knows; this does not. The CI column is over **converged** "
          "rows only (a fit pinned to a bound has a degenerate bootstrap).", "",
          "| channel | swings | median n | median n last 250 ms | median n last "
          "100 ms | **median n leverage** | converged | on bound | flat | median CI "
          "half-width | median robust residual |",
          "|---|---|---|---|---|---|---|---|---|---|---|"]
    for ch in P1_CHANNELS:
        sel = [r for r in rows if r["channel"] == ch]
        if not sel:
            L.append(f"| `{ch}` | 0 | — | — | — | — | — | — | — | — | — |")
            continue
        cv = [r for r in sel if r["converged"]]
        L.append(f"| `{ch}` | {len(sel)} | {np.median(_ps_vals(sel,'n_samples')):.0f} | "
                 f"{np.median(_ps_vals(sel,'n_samples_last_250ms')):.0f} | "
                 f"{np.median(_ps_vals(sel,'n_samples_last_100ms')):.0f} | "
                 f"**{np.median(_ps_vals(sel,'n_leverage')):.0f}** | "
                 f"**{len(cv)}/{len(sel)}** | "
                 f"{sum(r['on_bound'] for r in sel)} | {sum(r['flat'] for r in sel)} | "
                 + (f"{np.median(_ps_vals(cv,'ci_delta_ms')):.1f} ms" if cv else "—")
                 + f" | {np.median(_ps_vals(sel,'resid_robust_deg')):.1f}° |")
    # where the on-bound fits pile up, and what that means
    L += ["", "Where the non-converged fits go:", ""]
    for ch in P1_CHANNELS:
        sel = [r for r in rows if r["channel"] == ch and r["on_bound"]]
        if not sel:
            continue
        v = _ps_vals(sel, "delta_ms")
        L.append(f"- `{ch}`: {len(sel)} on a bound — {int((v > 0).sum())} at the "
                 f"upper cap (δ = {v.max():+.0f} ms, the release pushed past "
                 f"impact: *this channel never saw a release*) and "
                 f"{int((v < 0).sum())} at the lower bound (δ = {v.min():+.0f} ms).")
    L.append("")
    L += ["### by session", "",
          "| session | swings | median n leverage (measured / synth) | converged "
          "measured | +anchor | synth | median δ measured | median δ synth |",
          "|---|---|---|---|---|---|---|---|"]
    for ss in sorted({r["session"] for r in rows}):
        cells, meds = [], []
        for ch in ("measured", "measured+anchor", "synth"):
            q = [r for r in rows if r["session"] == ss and r["channel"] == ch]
            cells.append(f"{sum(r['converged'] for r in q)}/{len(q)}")
        for ch in ("measured", "synth"):
            v = _ps_vals([r for r in rows if r["session"] == ss
                          and r["channel"] == ch and r["converged"]], "delta_ms")
            meds.append(f"{np.median(v):+.1f} ms" if len(v) >= 2 else "—")
        lev = []
        for ch in ("measured", "synth"):
            q = [r for r in rows if r["session"] == ss and r["channel"] == ch]
            lev.append(f"{np.median(_ps_vals(q,'n_leverage')):.0f}" if q else "—")
        n = len([r for r in rows if r["session"] == ss and r["channel"] == "measured"])
        flag = " *(path.)*" if any(r["pathological"] for r in rows
                                   if r["session"] == ss) else ""
        L.append(f"| `{ss}`{flag} | {n} | {' / '.join(lev)} | "
                 + " | ".join(cells) + " | " + " | ".join(meds) + " |")

    L += ["", f"- {len(swings)} swings loaded; {len(rejected)} dropped before "
          "loading:"]
    for nm, why in rejected:
        L.append(f"  - `{nm}` — {why}")
    if drops:
        L.append(f"- dropped per channel ({len(drops)}):")
        for nm, ch, why in drops:
            L.append(f"  - `{nm}` / {ch} — {why}")
    else:
        L.append("- no swing×channel dropped after loading: every swing carries a "
                 "P7 anchor row and a synth series.")

    # sanity: population-centred, so the median should sit near zero
    L += ["", "### sanity: δ is population-centred, so its median should sit near 0",
          "", "| channel | median δ (ms) | p10–p90 (ms) | healthy median (ms) |",
          "|---|---|---|---|"]
    for ch in P1_CHANNELS:
        sel = [r for r in rows if r["channel"] == ch and r["converged"]]
        hs = [r for r in healthy if r["channel"] == ch and r["converged"]]
        if not sel:
            continue
        v = _ps_vals(sel, "delta_ms")
        L.append(f"| `{ch}` | {np.median(v):+.1f} | "
                 f"{np.percentile(v,10):+.1f} … {np.percentile(v,90):+.1f} | "
                 + (f"{np.median(_ps_vals(hs,'delta_ms')):+.1f}" if hs else "—") + " |")

    # ---- Gate 1
    L += ["", "## Gate 1 — identifiability", "",
          "Per-swing bootstrap CI half-width against the between-swing spread "
          "(1.645 × 1.4826 × MAD, i.e. the same central-90% half-width the CI is), "
          "healthy sessions only. **PASS** = ratio ≤ 0.5, **marginal** ≤ 1.0.", "",
          "| channel | swings used | median CI half-width | between-swing "
          "half-width | ratio | verdict |", "|---|---|---|---|---|---|"]
    for ch in P1_CHANNELS:
        sel = [r for r in healthy if r["channel"] == ch and r["converged"]]
        if len(sel) < 4:
            L.append(f"| `{ch}` | {len(sel)} | — | — | — | **unusable** |")
            continue
        ci = float(np.median(_ps_vals(sel, "ci_delta_ms")))
        hw = 1.645 * _ps_sigma(_ps_vals(sel, "delta_ms"))
        ratio = ci / hw if hw > 0 else float("nan")
        L.append(f"| `{ch}` | {len(sel)} | {ci:.1f} ms | {hw:.1f} ms | "
                 f"{_ps_fmt(ratio,'{:.2f}')} | **{_ps_verdict(ratio, 0.5, 1.0)}** |")
    L += ["", "### the two pathological sessions, separately", "",
          "| channel | 2026-06-11 median δ (σ) | 2026-07-04 median δ (σ) | healthy "
          "median δ (σ) |", "|---|---|---|---|"]
    for ch in P1_CHANNELS:
        cells = []
        for pref in PS_PATHOLOGICAL:
            v = _ps_vals([r for r in rows if r["channel"] == ch and r["converged"]
                          and r["session"].startswith(pref)], "delta_ms")
            cells.append(f"{np.median(v):+.1f} ({_ps_sigma(v):.1f}) ms" if len(v) >= 3 else "—")
        v = _ps_vals([r for r in healthy if r["channel"] == ch and r["converged"]],
                     "delta_ms")
        cells.append(f"{np.median(v):+.1f} ({_ps_sigma(v):.1f}) ms" if len(v) >= 3 else "—")
        L.append(f"| `{ch}` | " + " | ".join(cells) + " |")

    # ---- Gate 2
    L += ["", "## Gate 2 — repeatability", "",
          "Within-session spread against between-session spread of session medians, "
          "both robust σ, healthy sessions with ≥4 converged swings. **PASS** = "
          "between/within ≥ 1.5.", "",
          "| channel | sessions | within-session σ (median) | between-session σ | "
          "ratio | verdict |", "|---|---|---|---|---|---|"]
    for ch in P1_CHANNELS:
        sel = [r for r in healthy if r["channel"] == ch and r["converged"]]
        within, meds = [], []
        for ss in sorted({r["session"] for r in sel}):
            v = _ps_vals([r for r in sel if r["session"] == ss], "delta_ms")
            if len(v) >= 4:
                within.append(_ps_sigma(v))
                meds.append(float(np.median(v)))
        if len(meds) < 2:
            L.append(f"| `{ch}` | {len(meds)} | — | — | — | **unusable** (needs ≥2 "
                     "sessions with ≥4 converged swings) |")
            continue
        w = float(np.median(within))
        bs = _ps_sigma(np.array(meds))
        ratio = bs / w if w > 0 else float("nan")
        L.append(f"| `{ch}` | {len(meds)} | {w:.1f} ms | {bs:.1f} ms | "
                 f"{_ps_fmt(ratio,'{:.2f}')} | "
                 f"**{_ps_verdict(ratio, 1.5, 0.5, invert=True)}** |")

    L += ["", "### the coverage-artefact probe", "",
          "*The previous experiment's between-session differences tracked how many "
          "samples a session had near impact rather than anything about the "
          "golfer. This asks the question directly: does δ correlate with "
          "coverage?* Spearman ρ across converged healthy swings. **A |ρ| above "
          "≈0.5 means the number is reading the camera, not the swing.**", "",
          "| channel | n | ρ(δ, n last 100 ms) | ρ(δ, n last 250 ms) | reading |",
          "|---|---|---|---|---|"]
    for ch in P1_CHANNELS:
        sel = [r for r in healthy if r["channel"] == ch and r["converged"]]
        if len(sel) < 6:
            L.append(f"| `{ch}` | {len(sel)} | — | — | too few |")
            continue
        r100 = _ps_spearman(_ps_vals(sel, "delta_ms"), _ps_vals(sel, "n_samples_last_100ms"))
        r250 = _ps_spearman(_ps_vals(sel, "delta_ms"), _ps_vals(sel, "n_samples_last_250ms"))
        both = [v for v in (r100, r250) if np.isfinite(v)]
        if not both:
            note = ("probe undefined — this channel's coverage is **constant** "
                    "across swings, so there is nothing to correlate against")
        else:
            worst = max(abs(v) for v in both)
            note = ("**contaminated by coverage**" if worst >= 0.5 else
                    "borderline" if worst >= 0.3 else "clean")
        L.append(f"| `{ch}` | {len(sel)} | {_ps_fmt(r100,'{:+.2f}')} | "
                 f"{_ps_fmt(r250,'{:+.2f}')} | {note} |")

    # ---- Gate 3
    tsel = [r for r in rows if r["channel"] == "truth"]
    tcv = [r for r in tsel if r["converged"]]
    tconv = len(tcv)
    tci = float(np.median(_ps_vals(tcv, "ci_delta_ms"))) if tcv else float("nan")
    tspread = 1.645 * _ps_sigma(_ps_vals([r for r in tsel if r["converged"]], "delta_ms")) \
        if tconv >= 4 else float("nan")
    L += ["", "## Gate 3 — truthfulness (decisive)", "",
          "*Before comparing anything to truth, does truth itself pin δ?*", "",
          f"- truth channel: median **{np.median(_ps_vals(tsel,'n_leverage')):.0f} "
          f"samples with leverage on δ** out of "
          f"{np.median(_ps_vals(tsel,'n_samples_last_250ms')):.0f} in the last "
          f"250 ms — the premise that the release *slope* would carry δ where the "
          f"release *window* could not does not survive contact with these rows.",
          f"- truth channel: **{tconv}/{len(tsel)} converged**, median CI "
          f"half-width **{_ps_fmt(tci,'{:.1f}')} ms** against a between-swing "
          f"half-width of {_ps_fmt(tspread,'{:.1f}')} ms "
          f"(ratio {_ps_fmt(tci/tspread if np.isfinite(tspread) and tspread > 0 else float('nan'),'{:.2f}')}).",
          ""]
    if not (tconv >= 4 and np.isfinite(tspread) and tspread > 0 and tci <= tspread):
        L += ["**The truth channel does not identify δ well enough to grade "
              "against.** Every comparison below is reported, but a disagreement "
              "cannot be attributed to the tracker channel when the reference "
              "itself is this loose.", ""]
    L += ["| comparison | pairs | median \\|Δδ\\| | between-swing σ (truth) | "
          "\\|Δ\\|/σ | Spearman ρ | sign agreement | verdict |",
          "|---|---|---|---|---|---|---|---|"]
    for ch in ("measured", "measured+anchor", "synth"):
        keys, a, b = _p1_pairs(rows, ch, "truth")
        allk, _a, _b = _p1_pairs(rows, ch, "truth", converged_both=False)
        if len(keys) < 4:
            L.append(f"| `{ch}` vs `truth` | {len(keys)}/{len(allk)} | — | — | — | "
                     "— | — | **unusable** |")
            continue
        va = np.array([a[k]["delta_ms"] for k in keys])
        vb = np.array([b[k]["delta_ms"] for k in keys])
        md = float(np.median(np.abs(va - vb)))
        sig = _ps_sigma(vb)
        rho = _ps_spearman(va, vb)
        sgn = float(np.mean(np.sign(va) == np.sign(vb)))
        ratio = md / sig if sig > 0 else float("nan")
        ok = (np.isfinite(rho) and rho >= 0.6 and sgn >= 0.8
              and np.isfinite(ratio) and ratio <= 0.5)
        mid = (np.isfinite(rho) and rho >= 0.3 and sgn >= 0.6)
        L.append(f"| `{ch}` vs `truth` | {len(keys)}/{len(allk)} | {md:.1f} ms | "
                 f"{_ps_fmt(sig,'{:.1f}')} ms | {_ps_fmt(ratio,'{:.2f}')} | "
                 f"{_ps_fmt(rho,'{:+.2f}')} | {sgn*100:.0f}% | "
                 f"**{'PASS' if ok else ('marginal' if mid else 'FAIL')}** |")
    L += ["", "Sign agreement here is agreement on the *sign of δ itself* — "
          "earlier or later than the population — because δ is population-centred "
          "by construction.", ""]

    # ---- cross-channel consistency
    L += ["## Cross-channel consistency (informative, not truth)", "",
          "| comparison | pairs | median \\|Δδ\\| | Spearman ρ | sign agreement |",
          "|---|---|---|---|---|"]
    for a_ch, b_ch in (("synth", "measured"), ("measured+anchor", "measured"),
                       ("synth", "measured+anchor")):
        keys, a, b = _p1_pairs(healthy, a_ch, b_ch)
        if len(keys) < 4:
            L.append(f"| `{a_ch}` vs `{b_ch}` | {len(keys)} | — | — | — |")
            continue
        va = np.array([a[k]["delta_ms"] for k in keys])
        vb = np.array([b[k]["delta_ms"] for k in keys])
        L.append(f"| `{a_ch}` vs `{b_ch}` | {len(keys)} | "
                 f"{np.median(np.abs(va-vb)):.1f} ms | "
                 f"{_ps_fmt(_ps_spearman(va,vb),'{:+.2f}')} | "
                 f"{np.mean(np.sign(va)==np.sign(vb))*100:.0f}% |")

    L += ["", "## Reproduction", "", "```",
          args.per_swing_cmd or "(command not recorded)", "```", ""]
    Path(path).write_text("\n".join(L) + "\n", encoding="utf-8")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_root")
    ap.add_argument("--corpus", required=True, help="swing library holding truth.json")
    ap.add_argument("--out", required=True)
    ap.add_argument("--forms", default="F0,F1,F2,F3,F4,F5")
    ap.add_argument("--knots", type=int, default=13, help="knots on the time axis")
    ap.add_argument("--shipped-knots", action="store_true",
                    help="hold F1 at the shipped 9 knot POSITIONS (confounds axis with "
                         "knot placement; the default gives both axes the same budget)")
    ap.add_argument("--knot-layout", choices=("dense-late", "uniform"), default="dense-late",
                    help="dense-late spaces knots in sqrt(-t), resolving the release")
    ap.add_argument("--halfwidth", type=float, default=0.05,
                    help="fit window half-width on the time axis (s)")
    ap.add_argument("--halfwidth-s", type=float, default=0.05,
                    help="fit window half-width on the progress axis")
    ap.add_argument("--min-conf", type=float, default=0.0)
    ap.add_argument("--min-truth", type=int, default=8,
                    help="minimum hand-placed labels for a swing to count as truth")
    ap.add_argument("--domain", choices=("full", "to-impact"), default="to-impact",
                    help="the model's valid domain; to-impact matches the one-reversal law")
    ap.add_argument("--sigma-inflate", type=float, default=2.5,
                    help="factor applied to the fitted sigma before it is shipped. The "
                         "fit measures ONE athlete's spread; the envelope has to survive "
                         "golfers it has never seen. 2.5 is not a guess -- it is the "
                         "factor at which the 3-sigma envelope covers as much true beta "
                         "as the shipped table does, while still searching a narrower arc")
    ap.add_argument("--lab-root", default=None,
                    help="instrumented stripe-fusion lab root (tape_20260705). "
                         "Omit and the run is byte-identical to the pre-fusion harness.")
    ap.add_argument("--truth-phi", choices=("harness", "anchors"), default="harness",
                    help="phi source for the fusion channel. Default harness: "
                         "anchors.csv is interpolated and reads 0.1 deg jitter in the "
                         "backswing while degrading to 3.5 deg in the downswing, "
                         "against production's 1.8 deg (fusion_truth.py --audit).")
    ap.add_argument("--fusion-tiers", default="band,ray",
                    help="which instrumented tiers to grade (band ~0.3 deg, ray ~1.7 deg)")
    ap.add_argument("--holdout", choices=("swing", "session"), default="swing",
                    help="leave-one-swing-out (default) or leave-one-session-out")
    ap.add_argument("--exclude-swings", default="",
                    help="comma list of run-dir names to drop")
    ap.add_argument("--doc-figure", help="write a single-panel figure to this path")
    ap.add_argument("--emit-header", action="store_true",
                    help="print the winning table as the C++ literal")
    ap.add_argument("--per-swing", action="store_true",
                    help="fit the F5 parametric form PER SWING under P7/P4/P3 on the "
                         "tracker and instrumented-truth channels, and put the "
                         "estimates through the identifiability/repeatability/"
                         "truthfulness gates (model note §13). Additive: without "
                         "this flag no per-swing code runs and the harness behaves "
                         "exactly as before.")
    ap.add_argument("--per-swing-boot", type=int, default=100,
                    help="bootstrap reps per P3/P4 per-swing fit")
    ap.add_argument("--per-swing-boot-p7", type=int, default=100,
                    help="bootstrap reps per P7 per-swing fit (7 parameters is the "
                         "expensive one; cut this first if runtime bites)")
    ap.add_argument("--per-swing-seed", type=int, default=20260811,
                    help="base RNG seed; each fit's seed is base+index and is "
                         "recorded in the CSV")
    ap.add_argument("--p1", action="store_true",
                    help="fit ONE parameter per swing — a shift of the release "
                         "event in the population F5 curve — on the measured, "
                         "measured+anchor, synth and instrumented-truth channels, "
                         "and gate it. Additive, like --per-swing.")
    ap.add_argument("--p1-boot", type=int, default=100,
                    help="bootstrap reps per P1 per-swing fit")
    ap.add_argument("--p1-seed", type=int, default=20260901,
                    help="base RNG seed for --p1; per-fit seed is base+index")
    args = ap.parse_args(argv)
    args.per_swing_cmd = "python3 tools/swinglab/wrist_cock_fit.py " + " ".join(
        argv if argv is not None else sys.argv[1:])
    args.run_root = str(args.run_root)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    drop = {x.strip() for x in (args.exclude_swings or "").split(",") if x.strip()}
    swings, rejected = [], []
    for d in sorted(Path(args.run_root).iterdir()):
        if not (d / "result.json").exists():
            continue
        if d.name in drop:
            rejected.append((d.name, "excluded by --exclude-swings"))
            continue
        s, why = load_swing(d, args.corpus, args)
        if s is None:
            rejected.append((d.name, why))
            print(f"[fit] skip {d.name}: {why}")
        else:
            swings.append(s)
    if len(swings) < 5:
        raise SystemExit(f"[fit] only {len(swings)} usable swings")
    ntruth = sum(1 for s in swings if s["truth_t"] is not None)
    nlab = sum(len(s["truth_t"]) for s in swings if s["truth_t"] is not None)
    print(f"[fit] {len(swings)} swings, {ntruth} with truth ({nlab} labels), "
          f"domain={args.domain}")

    if getattr(args, "per_swing", False):
        return run_per_swing(swings, rejected, args)
    if getattr(args, "p1", False):
        return run_p1(swings, rejected, args)

    forms = build_forms(args)
    order = [f.key for f in forms]
    results = evaluate(forms, swings, args)

    print(f"\n[fit] leave-one-swing-out, against INDEPENDENT TRUTH:")
    for k in order:
        st = results[k].get("truth_stats")
        f = results[k]["form"]
        if not st:
            print(f"  {k} {f.label:52s}  (no truth samples)")
            continue
        w = st.get("within1sigma")
        print(f"  {k} {f.label:52s} n={st['n']:5d}  median {st['median']:+6.1f}  "
              f"p10-p90 {st['p10_p90']:6.1f}  >30deg {st['bad30']*100:5.1f}%  "
              f"within1sig {('%.0f%%' % (w*100)) if w else '  --'}")
    print(f"\n[fit] the same, against the tracker's vision tier (circular, for scale):")
    for k in order:
        st = results[k].get("track_stats")
        if st:
            print(f"  {k} n={st['n']:6d}  median {st['median']:+6.1f}  "
                  f"p10-p90 {st['p10_p90']:6.1f}  >30deg {st['bad30']*100:5.1f}%")

    print(f"\n[fit] wedge envelope (centre +- {3.0:g}*sigma) — fraction of TRUE beta inside:")
    for k in order:
        cov = envelope_coverage(results[k])
        if cov:
            print(f"  {k}  half-width {cov.pop('median_half_deg'):5.1f} deg (median)   " +
                  "  ".join(f"x{f:g}:{v*100:5.1f}%" for f, v in sorted(cov.items())))

    print(f"\n[fit] DECISION (replace the shipped table only if a form clears every clause):")
    winner = None
    for k in order:
        if k == "F0":
            continue
        ok, reasons = gate(results["F0"], results[k])
        print(f"  {k}: {'PASS' if ok else 'fail'}" +
              ("" if ok else "  — " + "; ".join(reasons)))
        if ok and winner is None:
            winner = k
    print(f"[fit] verdict: " + (f"{winner} clears the gate" if winner
                                else "no candidate clears the gate — keep the shipped table"))

    for k in order:
        pm = results[k].get("params_median")
        if pm:
            names = ("b0 address offset", "A peak lag", "tc cock centre", "wc cock width",
                     "tr release centre", "wr release width", "r release completeness")
            print(f"\n[fit] {k} parametric parameters (median over folds, p10-p90 spread):")
            for nm, v, sp in zip(names, pm, results[k]["params_spread"]):
                print(f"[fit]   {nm:24s} {v:8.3f}   +-{sp:6.3f}")

    write_report(out / "WRIST_COCK_FIT.md", results, order, swings, args)
    write_tables_csv(out / "wrist_cock_tables.csv", results, order)
    make_figure(out / "wrist_cock_fit.png", results, order, swings, args)
    json.dump({"sha": git_sha(), "runRoot": args.run_root, "domain": args.domain,
               "options": vars(args), "winner": winner,
               "swings": [s["name"] for s in swings],
               "truthSwings": [s["name"] for s in swings if s["truth_t"] is not None],
               "rejected": rejected,
               "results": {k: {"truth": results[k].get("truth_stats"),
                               "track": results[k].get("track_stats"),
                               "segments": results[k]["seg_stats"],
                               "sessions": results[k]["sess_stats"],
                               "table": [list(map(float, results[k]["table"][0])),
                                         list(map(float, results[k]["table"][1]))]
                               if "table" in results[k] else None}
                           for k in order}},
              open(out / "wrist_cock_fit.json", "w", encoding="utf-8"), indent=1)
    if args.emit_header and winner:
        print(f"\n// sigma inflated x{args.sigma_inflate:g} from the fit (see --sigma-inflate)")
        print(emit_header(results[winner], results[winner]["form"], args.sigma_inflate))
    print(f"[fit] wrote WRIST_COCK_FIT.md + png + csv + json under {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
