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
    truth_t = truth_beta = None
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

    return {
        "name": name,
        "session": name.split("__")[0] if "__" in name else name,
        "events": events, "chir": chir,
        "t": t, "beta": beta_track, "phi": phi,
        "measured": measured, "vision": vision,
        "truth_t": truth_t, "truth_beta": truth_beta,
        "truth_phi": phi_at(truth_t) if truth_t is not None else None,
    }, None


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


def evaluate(forms, swings, args):
    """Leave-one-swing-out. Each held-out swing is scored against a table fitted
    without it -- on the tracker's measured tier (broad) and, where the swing has
    hand-placed labels, on truth (independent)."""
    results = {f.key: {"form": f, "track": [], "truth": [], "track_sig": [],
                       "truth_sig": [], "seg": {}, "sess": {}} for f in forms}
    fitted_tables = {f.key: [] for f in forms}

    for held in range(len(swings)):
        train = [s for i, s in enumerate(swings) if i != held]
        te = swings[held]
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
            for src, tsel, bsel, psel in (
                ("track", te["t"][te["vision"]], te["beta"][te["vision"]],
                 te["phi"][te["vision"]]),
                ("truth", te["truth_t"], te["truth_beta"], te["truth_phi"]),
            ):
                if tsel is None or len(tsel) == 0:
                    continue
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
                if src == "truth":
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
        for src in ("track", "truth"):
            res[src + "_stats"] = stats(np.concatenate(res[src]),
                                        np.concatenate(res[src + "_sig"])) \
                if res[src] else None
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
    ap.add_argument("--doc-figure", help="write a single-panel figure to this path")
    ap.add_argument("--emit-header", action="store_true",
                    help="print the winning table as the C++ literal")
    args = ap.parse_args(argv)
    args.run_root = str(args.run_root)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    swings, rejected = [], []
    for d in sorted(Path(args.run_root).iterdir()):
        if not (d / "result.json").exists():
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
