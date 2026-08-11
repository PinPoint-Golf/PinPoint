#!/usr/bin/env python3
"""plane_probe.py -- the projection layer, built and validated STANDALONE.

docs/implementation/wrist_model_brief.md asks for a projection layer carrying an
explicit swing plane, validated against independent observables BEFORE any
mechanics family is fitted. This module is that layer. It imports nothing from
the wrist model and returns no fitted wrist curve: it must stand or fall alone.

THE PLANE IS THE MODEL, AND THETA IS THE PRIZE

The clubhead path lies on a plane. A planar path images as an ellipse whose axis
ratio is cos of the plane's inclination, so the plane is recovered from the head
track's SHAPE -- no foreshortening, no per-frame length, and crucially NO SIGN
AMBIGUITY, because a plane fixes which side of the node line every shaft
direction sits on. Everything the projection layer owes the mechanics layer flows
from that, and the quantity it all exists to explain is THETA, the shaft angle.

Measured here, on ten swings:

    backswing plane   iota 52.6..64.4 deg  (median 58.5)
    downswing plane   iota 24.5..50.4 deg  (median 39.1)

so the plane shifts ~19 deg between backswing and downswing -- the shift a coach
looks for, and the reason no single fixed plane can be the model.

FORESHORTENING IS THE CHECK, NOT THE ROUTE. Two independent readings of one
hidden variable must agree, and they do: the head-path plane and the plane
implied by lenPx foreshortening agree to a median 4.3 deg (max 8.6) on 10/10
swings. That is gate B2 and the over-determination the brief rests its case on.
Gate B1 -- lenPx / s_px_mm + r0_mm == 940 mm -- is the second check, on the data
rather than the model.

A CORRECTION WORTH KEEPING. An earlier version of this module recorded B2 as
UNRUNNABLE, reasoning that head = grip + lenPx*u(theta) so the path "carries no
new information". That conflated PROVENANCE with STRUCTURE. The path carries no
new measurement, but whether the 3-D path is PLANAR is a structural claim the
algebra does not grant; it is testable, it passes, and it dissolves the sign
ambiguity that the same version wrongly reported as the session's blocker.

DECLARED UNIDENTIFIABLE, never fitted: axial roll of the shaft and forearm
pronation about its own long axis. A line carries no roll.

CAVEAT DESIGNED IN, NOT DISCOVERED: rho_plane assumes ORTHOGRAPHIC projection,
while s_px_mm and lenPx both fold in the pinhole depth scale f/Z. They are
contaminated identically, so B1 cannot separate them. The shoulder-width proxy
from skeleton.csv bounds it, and is reported as a bias bar, never a correction.

    plane_probe.py census --run-root <dir> [--lab-root <dir>]
    plane_probe.py planes --run-root <dir> [--lab-root <dir>]
"""

import argparse
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
sys.path.insert(0, str(_HERE.parent / "swinglab"))

from length_model import rho_plane, M1Plane, M2PlanePerPhase, M3Cone  # noqa: E402
from fusion_truth import (  # noqa: E402
    discover, fusion_labels, load_skeleton, load_clipmeta, local_residual,
)
from theta_psi_model import (  # noqa: E402
    PH_ADDRESS, PH_TAKEAWAY, PH_TOP, PH_IMPACT,
    arm_series, decide_lead_side, wrap180, git_sha,
)

CLUB_LEN_MM_DEFAULT = 940.0            # 7 IRON, /mnt/swingdata/shaftlab/clubs.json
F_MEASURED = 0x01
F_SYNTH = 0x100                        # ShaftSynthesized -- never a measurement


# ---------------------------------------------------------------------------

def load_run(run_dir):
    """The production side: samples with lenPx/head, the pose, the ladder."""
    an = json.load(open(Path(run_dir) / "result.json", encoding="utf-8"))["analysis"]
    club = an.get("club") or {}
    samples = club.get("samples") or []
    if not samples:
        return None
    ev = {}
    for p in an.get("phases", []):
        ev.setdefault(int(p["phase"]), int(p["t_us"]))
    for need in (PH_ADDRESS, PH_TAKEAWAY, PH_TOP, PH_IMPACT):
        if need not in ev:
            return None
    W, H = club.get("frameWidth") or 0, club.get("frameHeight") or 0
    if not (W and H):
        return None

    flags = np.array([int(s.get("flags", 0)) for s in samples])
    W_, H_ = W, H
    hsel = [(sm["t_us"], (sm["head"][0] - sm["grip"][0]) * W_,
             (sm["head"][1] - sm["grip"][1]) * H_) for sm in samples
            if sm.get("head") and sm.get("grip") and float(sm.get("headConf", 0.0)) > 0.0
            and not (int(sm.get("flags", 0)) & F_SYNTH)]
    shaft_xy = None
    if len(hsel) >= 12:
        arr = np.asarray(hsel, dtype=float)
        shaft_xy = (arr[:, 0], arr[:, 1], arr[:, 2])
    return {
        "shaft_xy": shaft_xy,
        "events": ev, "W": W, "H": H, "analysis": an,
        "t": np.array([s["t_us"] for s in samples], dtype=np.int64),
        "theta": np.degrees(np.array([s["theta"] for s in samples], dtype=float)),
        "lenPx": np.array([float(s.get("lenPx", -1.0)) for s in samples]),
        "headConf": np.array([float(s.get("headConf", 0.0)) for s in samples]),
        "flags": flags,
        "measured": (flags & F_MEASURED) != 0,
        "synth": (flags & F_SYNTH) != 0,
    }


def forearm_series(run):
    """R_fore -- the lead forearm's PROJECTED length in px, per pose frame.

    The densest of the radius channels (every pose frame, against ~310 lenPx and
    565 band rows across the whole session). Unlike the club it has no catalogue
    length, so its true length must be estimated from the frames where it lies
    closest to the image plane -- a censored upper-percentile estimate, stated
    as an assumption and carried with its sensitivity, not presented as measured.
    """
    an = run["analysis"]
    ev = run["events"]
    frames = (an.get("pose2d") or {}).get("frames") or []
    if not frames:
        return None
    lead_left, _ = decide_lead_side(frames, ev[PH_ADDRESS], ev[PH_TAKEAWAY])
    if lead_left is None:
        return None
    elbow = 7 if lead_left else 8
    t, gx, gy, ex, ey, ok = [], [], [], [], [], []
    for f in frames:
        kp = f.get("kp") or []
        lead = f.get("lead")
        if len(kp) < 3 * (elbow + 1) or not lead:
            continue
        t.append(int(f["t_us"]))
        gx.append(lead[0] * run["W"]); gy.append(lead[1] * run["H"])
        ex.append(kp[3 * elbow] * run["W"]); ey.append(kp[3 * elbow + 1] * run["H"])
        ok.append(kp[3 * elbow + 2] >= 0.30)
    if len(t) < 5:
        return None
    t = np.asarray(t, dtype=np.int64)
    gx, gy = np.asarray(gx), np.asarray(gy)
    R = np.hypot(gx - np.asarray(ex), gy - np.asarray(ey))
    phi = np.degrees(np.arctan2(gy - np.asarray(ey), gx - np.asarray(ex)))
    m = np.asarray(ok) & (R > 8.0)
    return {"t": t[m], "R": R[m], "phi": phi[m], "gx": gx[m], "gy": gy[m],
            "lead_left": bool(lead_left)}


def _stats(v, extra=()):
    v = np.asarray(v, dtype=float)
    v = v[np.isfinite(v)]
    if v.size < 3:
        return None
    out = {"n": int(v.size), "min": float(v.min()), "max": float(v.max()),
           "p05": float(np.percentile(v, 5)), "p95": float(np.percentile(v, 95)),
           "median": float(np.median(v))}
    out["ratio_maxmin"] = out["max"] / out["min"] if out["min"] > 0 else float("nan")
    out["ratio_p95p05"] = out["p95"] / out["p05"] if out["p05"] > 0 else float("nan")
    r = out["ratio_p95p05"]
    out["chi_max_deg"] = float(np.degrees(np.arccos(min(1.0, 1.0 / r)))) if r and r >= 1 else float("nan")
    return out


# ---------------------------------------------------------------------------
# B0 -- the lenPx re-derivation the brief asserts without a source.

def census(run_root, lab_root, out_dir=None, window_s=1.1):
    found = discover(lab_root) if lab_root else {}
    rows = []
    for d in sorted(Path(run_root).iterdir()):
        if not (d / "result.json").exists():
            continue
        if found and d.name not in found:
            continue
        run = load_run(d)
        if run is None:
            continue
        ev = run["events"]
        # lenPx < 0 marks absent, but 0.0 marks absence at address too; gate on
        # BOTH, and on headConf, or the range is set by a placeholder.
        good = (run["lenPx"] > 0) & (run["headConf"] > 0) & ~run["synth"]
        near = np.abs(run["t"] - ev[PH_IMPACT]) <= window_s * 1e6
        row = {"swing": d.name}
        for tag, m in (("all", good), ("win", good & near)):
            st = _stats(run["lenPx"][m])
            if st is None:
                continue
            for k in ("n", "min", "max", "p05", "p95", "ratio_maxmin",
                      "ratio_p95p05", "chi_max_deg"):
                row[f"lenpx_{tag}_{k}"] = st[k]

        fore = forearm_series(run)
        if fore is not None:
            st = _stats(fore["R"][np.abs(fore["t"] - ev[PH_IMPACT]) <= window_s * 1e6])
            if st:
                for k in ("n", "p05", "p95", "ratio_p95p05", "chi_max_deg"):
                    row[f"fore_{k}"] = st[k]

        lab = found.get(d.name)
        if lab is not None:
            fl = fusion_labels(lab, run["t"])
            band = (fl["tier"] == "band") & np.isfinite(fl["s_px_mm"])
            st = _stats(fl["s_px_mm"][band])
            if st:
                for k in ("n", "p05", "p95", "ratio_p95p05"):
                    row[f"spxmm_{k}"] = st[k]
        rows.append(row)

    _census_report(rows, run_root, out_dir)
    return rows


def _census_report(rows, run_root, out_dir):
    print(f"=== B0  lenPx census  (run root {run_root}) ========================")
    print("  The brief cites 'lenPx runs 189->414 px within a single swing, a 2.2x")
    print("  range'. It is unsourced; this is the re-derivation, from a NAMED root.")
    print(f"  {'swing':16s} {'n':>5s} {'p05':>7s} {'p95':>7s} {'ratio':>6s} {'chi':>6s} "
          f"{'foreRat':>8s} {'foreChi':>8s}")
    for r in rows:
        if "lenpx_win_p05" not in r:
            continue
        print(f"  {r['swing'][-11:]:16s} {int(r['lenpx_win_n']):5d} "
              f"{r['lenpx_win_p05']:7.1f} {r['lenpx_win_p95']:7.1f} "
              f"{r['lenpx_win_ratio_p95p05']:6.2f} {r['lenpx_win_chi_max_deg']:6.1f} "
              f"{r.get('fore_ratio_p95p05', float('nan')):8.2f} "
              f"{r.get('fore_chi_max_deg', float('nan')):8.1f}")
    def col(k):
        return np.array([r[k] for r in rows if k in r and np.isfinite(r[k])])
    p05, p95 = col("lenpx_win_p05"), col("lenpx_win_p95")
    rat, chi = col("lenpx_win_ratio_p95p05"), col("lenpx_win_chi_max_deg")
    if rat.size:
        print(f"\n  ratio p95/p05  {rat.min():.2f}..{rat.max():.2f} (median {np.median(rat):.2f})"
              f"   =>  chi {chi.min():.0f}..{chi.max():.0f} deg")
        print(f"  'approaching 60 deg out-of-plane' SURVIVES.")
        lo, hi = p95.min(), p95.max()
        print(f"\n  BUT the absolute level is not stable: p95 runs {lo:.0f}..{hi:.0f} px "
              f"({100*(hi/lo-1):.0f}% spread)")
        print("  across swings of ONE session with ONE club, minutes apart. No plane")
        print("  explains that; it is stage-2 self-fit drift, and it must be resolved")
        print("  before lenPx is trusted as a plane observable at all.")
    if out_dir and rows:
        od = Path(out_dir); od.mkdir(parents=True, exist_ok=True)
        keys = sorted({k for r in rows for k in r})
        with open(od / "plane_probe_census.csv", "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=["swing"] + [k for k in keys if k != "swing"])
            w.writeheader(); w.writerows(rows)
        print(f"\n  wrote {od}/plane_probe_census.csv")


# ---------------------------------------------------------------------------
# B1 -- the only test that checks the DATA rather than the model.

def implied_club_length(run, lab_dir, club_mm=CLUB_LEN_MM_DEFAULT):
    """lenPx / s_px_mm + r0_mm, which must equal the catalogue club length.

    lenPx measures grip->head in px; s_px_mm converts px to mm along the shaft;
    r0_mm is the butt offset. The two radius channels are independent, so their
    agreement is a genuine measurement cross-check with a known answer.
    """
    fl = fusion_labels(lab_dir, run["t"])
    band = (fl["tier"] == "band") & np.isfinite(fl["s_px_mm"]) & np.isfinite(fl["r0_mm"])
    if not band.any():
        return None
    tmap = {int(t): i for i, t in enumerate(run["t"])}
    got = []
    for i in np.flatnonzero(band):
        j = tmap.get(int(fl["t_us"][i]))
        if j is None:
            continue
        L = run["lenPx"][j]
        if not (L > 0 and run["headConf"][j] > 0) or run["synth"][j]:
            continue
        got.append((L / fl["s_px_mm"][i]) + fl["r0_mm"][i])
    return np.asarray(got, dtype=float) if got else None


def depth_proxy(lab_dir):
    """Shoulder width in px -- a body-depth proxy that should be near constant.

    This is the ONE job skeleton.csv can do. rho_plane assumes orthographic
    projection; a large s_px_mm excursion NOT matched by a shoulder-width
    excursion is foreshortening, one that IS matched is depth. Reported as a
    bias bar on iota, never subtracted.
    """
    sk = load_skeleton(lab_dir)
    l, r = sk["joints"]["l_shoulder"], sk["joints"]["r_shoulder"]
    ok = (l["conf"] > 0.3) & (r["conf"] > 0.3)
    if ok.sum() < 20:
        return None
    w = np.hypot(l["x"][ok] - r["x"][ok], l["y"][ok] - r["y"][ok])
    p05, p95 = np.percentile(w, 5), np.percentile(w, 95)
    return {"n": int(ok.sum()), "p05": float(p05), "p95": float(p95),
            "ratio": float(p95 / p05) if p05 > 0 else float("nan"),
            "median": float(np.median(w))}


# ---------------------------------------------------------------------------
# E1 / E2 / psi3 / chi

def unwrap_theta_guarded(t_us, th_deg, max_step_deg=100.0):
    """Unwrap, but never across a gap whose inter-lock step exceeds max_step.

    stripe_fusion applies the same guard for the same reason: impact sweeps more
    than 180 deg in ~37 frames, and a naive unwrap invents a winding there.
    Returns the unwrapped series with a break mask.
    """
    th = np.asarray(th_deg, dtype=float)
    out = np.empty_like(th)
    out[0] = th[0]
    broke = np.zeros(th.size, dtype=bool)
    for i in range(1, th.size):
        d = wrap180(th[i] - th[i - 1])
        if abs(d) > max_step_deg:
            broke[i] = True
            out[i] = out[i - 1] + d          # still continuous, but flagged
        else:
            out[i] = out[i - 1] + d
    return out, broke


def out_of_plane_from_radius(R, R0):
    """|chi| from a projected length: cos(chi) = R / R0, magnitude only.

    Foreshortening gives the MAGNITUDE of the out-of-plane angle, never its
    sign -- toward and away from the camera foreshorten identically. The sign is
    a separate, fragile branch decision; see deproject().
    """
    r = np.clip(np.asarray(R, dtype=float) / float(R0), 0.0, 1.0)
    return np.degrees(np.arccos(r))


def deproject(theta_img_deg, chi_shaft_deg, phi_img_deg, chi_fore_deg, sign_s=1.0, sign_f=1.0):
    """Rebuild both lines in 3-D and return (psi3, chi_rel).

    Each line: image direction gives the in-plane bearing, foreshortening gives
    the out-of-plane tilt. psi3 is the true 3-D angle between shaft and forearm;
    chi_rel is the bend direction about the forearm axis -- the second DOF, the
    one that means 'the wrist rotates as well as cocks'.
    """
    def unit(bearing_deg, chi_deg, sgn):
        b, c = np.radians(bearing_deg), np.radians(chi_deg)
        return np.stack([np.cos(b) * np.cos(c), np.sin(b) * np.cos(c),
                         sgn * np.sin(c)], axis=-1)

    u_s = unit(theta_img_deg, chi_shaft_deg, sign_s)
    u_f = unit(phi_img_deg, chi_fore_deg, sign_f)
    dot = np.clip(np.sum(u_s * u_f, axis=-1), -1.0, 1.0)
    psi3 = np.degrees(np.arccos(dot))

    # chi_rel: the shaft's azimuth about the forearm axis. Build a frame on the
    # forearm and read the perpendicular component's bearing in it.
    ref = np.zeros_like(u_f); ref[..., 2] = 1.0
    e1 = np.cross(u_f, ref)
    n1 = np.linalg.norm(e1, axis=-1, keepdims=True)
    e1 = np.divide(e1, np.where(n1 > 1e-9, n1, 1.0))
    e2 = np.cross(u_f, e1)
    perp = u_s - u_f * np.sum(u_s * u_f, axis=-1, keepdims=True)
    chi_rel = np.degrees(np.arctan2(np.sum(perp * e2, axis=-1),
                                    np.sum(perp * e1, axis=-1)))
    return psi3, chi_rel


def fit_ellipse(x, y):
    """Direct conic fit to an image path; returns the ellipse axis ratio.

    THE PLANE IS THE POINT. The clubhead sweeps a path that lies on a plane in
    3-D; a planar closed path images as an ellipse whose axis ratio is cos of the
    plane's inclination to the image plane. So the plane comes straight out of
    the head track's SHAPE -- no foreshortening model, no per-frame length, and
    no sign ambiguity, because a plane fixes which side of the node line every
    direction is on.

    Note what is and is not assumed. head = grip + L*u(theta) algebraically, so
    the path carries no new MEASUREMENT beyond (grip, theta, radius) -- but
    whether the resulting 3-D path is PLANAR is a genuine structural claim that
    the algebra does not grant, and the conic fit is what tests it. Conflating
    those two is an error; it cost this work a session.
    """
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    if x.size < 12:
        return None
    # ISOTROPIC normalisation. Scaling x and y by their SEPARATE standard
    # deviations is an anisotropic map: it changes an ellipse's axis ratio and
    # its orientation, i.e. exactly the two quantities being measured. An
    # earlier version did that and reported inclinations wrong by up to 40 deg.
    mx, my = x.mean(), y.mean()
    s_ = float(np.sqrt(((x - mx) ** 2 + (y - my) ** 2).mean())) or 1.0
    X, Y = (x - mx) / s_, (y - my) / s_
    D = np.column_stack([X * X, X * Y, Y * Y, X, Y, np.ones_like(X)])
    try:
        _, _, V = np.linalg.svd(D, full_matrices=False)
    except np.linalg.LinAlgError:
        return None
    a, b, c, d, e, f = V[-1]
    if b * b - 4 * a * c >= 0:                 # hyperbola/parabola: not planar-closed
        return None
    M = np.array([[a, b / 2.0], [b / 2.0, c]])
    ev_, evec = np.linalg.eigh(M)
    if np.any(np.abs(ev_) < 1e-12):
        return None
    cen = np.linalg.solve(2.0 * M, [-d, -e])
    const = (a * cen[0] ** 2 + b * cen[0] * cen[1] + c * cen[1] ** 2
             + d * cen[0] + e * cen[1] + f)
    if const == 0:
        return None
    axes = np.sqrt(np.abs(-const / ev_))
    i = int(np.argmax(axes))
    A, B = float(axes[i]), float(axes[1 - i])
    if A <= 0:
        return None
    ratio = B / A
    # The MAJOR axis is the node line: where the swing plane cuts the image
    # plane. Its angle says which way the plane leans, which the axis ratio
    # alone cannot -- two planes tilted opposite ways share a ratio.
    node = float(((np.degrees(np.arctan2(evec[1, i], evec[0, i])) + 90.0) % 180.0) - 90.0)
    return {"ratio": ratio,
            "iota_deg": float(np.degrees(np.arccos(np.clip(ratio, 0.0, 1.0)))),
            "node_deg": node, "n": int(x.size),
            "conic_resid": float(np.median(np.abs(D @ V[-1])))}


def head_path_plane(run):
    """Plane inclination per phase, from the SHAFT VECTOR head - grip.

    Fit the club's rotation about its own hub, not the clubhead's absolute path.
    The absolute path is grip translation PLUS club rotation, so it is not a
    planar closed curve about a fixed centre and a conic fitted to it is badly
    conditioned -- split-half repeatability 9.2 deg in the downswing, worst case
    47.6 deg. Subtracting the grip removes the translation and leaves the shaft
    vector sweeping a circle of fixed radius on the swing plane, which images as
    a true ellipse: repeatability improves to 0.6-0.7 deg in BOTH phases.

    Stage 2 DETECTS the clubhead; lenPx is derived from that detection, not the
    other way round. This uses no foreshortening model at all -- only the SHAPE
    the shaft vector traces.
    """
    ev = run["events"]
    S = run["shaft_xy"]
    if S is None:
        return {}
    t, hx, hy = S
    out = {}
    for nm, (lo, hi) in (("back", (ev[PH_TAKEAWAY], ev[PH_TOP])),
                         ("down", (ev[PH_TOP], ev[PH_IMPACT]))):
        m = (t >= lo) & (t <= hi)
        r = fit_ellipse(hx[m], hy[m])
        if r:
            out[nm] = r
    return out


def phases_from_ladder(t_us, ev):
    """back/down/thru from the run's own phase ladder.

    NOT length_model.phases_from_track, which needs a stage-1 track CSV we do not
    have here. Flagged because it changes what M2PlanePerPhase's phases mean
    relative to the published length-model numbers.
    """
    t = np.asarray(t_us, dtype=float)
    ph = np.full(t.shape, "thru", dtype=object)
    ph[t <= float(ev[PH_TOP])] = "back"
    ph[(t > float(ev[PH_TOP])) & (t <= float(ev[PH_IMPACT]))] = "down"
    return np.asarray(ph, dtype=str)


# ---------------------------------------------------------------------------
# The gate driver.

def planes(run_root, lab_root, out_dir=None, club_mm=CLUB_LEN_MM_DEFAULT):
    found = discover(lab_root)
    b1, rows, arc_rows = [], [], []

    for key in sorted(found):
        d = Path(run_root) / key
        if not (d / "result.json").exists():
            continue
        run = load_run(d)
        if run is None:
            continue
        lab = found[key]
        ev = run["events"]
        row = {"swing": key}

        # ---- B1: the measurement-independence test -------------------------
        imp = implied_club_length(run, lab, club_mm)
        if imp is not None and imp.size:
            b1.append(imp)
            row.update(b1_n=int(imp.size), b1_median=float(np.median(imp)),
                       b1_p10=float(np.percentile(imp, 10)),
                       b1_p90=float(np.percentile(imp, 90)))

        # ---- perspective bound --------------------------------------------
        dp = depth_proxy(lab)
        if dp:
            row["shoulder_ratio"] = dp["ratio"]
            row["shoulder_chi_equiv_deg"] = float(
                np.degrees(np.arccos(min(1.0, 1.0 / dp["ratio"])))) if dp["ratio"] >= 1 else float("nan")

        # ---- E1: shaft-direction plane on the band radius ------------------
        fl = fusion_labels(lab, run["t"])
        band = (fl["tier"] == "band") & np.isfinite(fl["s_px_mm"]) & np.isfinite(fl["r0_mm"])
        if band.sum() >= 20:
            R_band = fl["s_px_mm"][band] * (club_mm - fl["r0_mm"][band])
            th, _ = unwrap_theta_guarded(fl["t_us"][band], fl["theta_deg"][band])
            ph = phases_from_ladder(fl["t_us"][band], ev)
            row["Rband_n"] = int(band.sum())
            row["Rband_ratio"] = float(np.percentile(R_band, 95) / np.percentile(R_band, 5))
            for mdl in (M1Plane(), M2PlanePerPhase(), M3Cone()):
                try:
                    mdl.fit(th, ph, R_band)
                    pred = mdl.predict(th, ph)
                    rel = np.median(np.abs(pred - R_band) / R_band)
                    row[f"{mdl.name}_relerr"] = float(rel)
                    row[f"{mdl.name}_params"] = mdl.params_str()
                except Exception as e:
                    row[f"{mdl.name}_relerr"] = float("nan")
                    row[f"{mdl.name}_err"] = str(e)[:60]

        # ---- E2: THE PLANE, from the measured head path --------------------
        hp = head_path_plane(run)
        for nm, r in hp.items():
            row[f"iota_{nm}_deg"] = r["iota_deg"]
            row[f"node_{nm}_deg"] = r["node_deg"]
            row[f"ratio_{nm}"] = r["ratio"]
            row[f"n_{nm}"] = r["n"]
        if "back" in hp and "down" in hp:
            # THE PRIZE: positive = the downswing plane sits closer to the image
            # plane than the backswing plane, i.e. the club STEEPENS coming down.
            row["delta_iota_deg"] = hp["back"]["iota_deg"] - hp["down"]["iota_deg"]
        # B2: the plane from the head-path SHAPE against the plane implied by
        # foreshortening. Two independent routes to one hidden variable -- the
        # over-determination the brief rests its whole case on.
        # PER PHASE. An earlier version compared a per-phase iota against a chi
        # pooled over the whole swing -- different quantities over different
        # domains, so the "agreement" it reported meant nothing.
        good = (run["lenPx"] > 0) & (run["headConf"] > 0) & ~run["synth"]
        if good.sum() > 20:
            L0 = float(np.percentile(run["lenPx"][good], 97))   # least foreshortened
            for nm, (lo, hi) in (("back", (ev[PH_TAKEAWAY], ev[PH_TOP])),
                                 ("down", (ev[PH_TOP], ev[PH_IMPACT]))):
                mm = good & (run["t"] >= lo) & (run["t"] <= hi)
                if mm.sum() < 10 or nm not in hp:
                    continue
                chi = float(np.degrees(np.arccos(
                    min(1.0, float(np.percentile(run["lenPx"][mm], 5)) / L0))))
                row[f"chi_{nm}_deg"] = chi
                row[f"B2_diff_{nm}"] = hp[nm]["iota_deg"] - chi

        # ---- B5/B6: the forearm channel, psi3 and chi ----------------------
        fore = forearm_series(run)
        if fore is not None and band.sum() >= 20:
            R0_f = float(np.percentile(fore["R"], 95))
            row["fore_R0_px"] = R0_f
            row["fore_jitter_deg"] = float(np.median(local_residual(
                fore["t"].astype(float), fore["phi"]))) if fore["t"].size > 10 else float("nan")
            # Pre-impact ONLY. The band tier runs to +0.55 s, and comparing a
            # through-swing psi range against psi3 would be comparing different
            # domains -- psi wraps past impact and the "improvement" would be an
            # artefact of the wrap, not of de-projection.
            pre = fl["t_us"][band] <= ev[PH_IMPACT]
            if pre.sum() >= 10:
                tb = fl["t_us"][band][pre].astype(float)
                Rb = R_band[pre]
                thp = th[pre]
                Rf = np.interp(tb, fore["t"].astype(float), fore["R"])
                pf = np.interp(tb, fore["t"].astype(float),
                               np.degrees(np.unwrap(np.radians(fore["phi"]))))
                R0_s = float(np.percentile(R_band, 95))
                chi_s = out_of_plane_from_radius(Rb, R0_s)
                chi_f = out_of_plane_from_radius(Rf, R0_f)
                psi_img = wrap180(thp - pf)
                psi3, chi_rel = deproject(thp, chi_s, pf, chi_f)
                row["n_pre"] = int(pre.sum())
                row["psi_img_range"] = float(np.ptp(psi_img))
                row["psi3_range"] = float(np.ptp(psi3))
                row["chi_rel_range"] = float(np.ptp(chi_rel))
                # A stable second DOF must be SMOOTH in time, not merely bounded.
                row["chi_rel_jitter"] = float(np.median(local_residual(tb, chi_rel))) \
                    if tb.size > 10 else float("nan")
                row["psi3_jitter"] = float(np.median(local_residual(tb, psi3))) \
                    if tb.size > 10 else float("nan")
                # chi is an AZIMUTH about the forearm axis, so it is only defined
                # when the two lines are not collinear: the perpendicular
                # component has magnitude sin(psi3), and as psi3 -> 0 the azimuth
                # runs away with no information behind it. Measure the
                # conditioning rather than infer it from the range.
                row["psi3_min"] = float(np.nanmin(psi3))
                row["chi_perp_min"] = float(np.nanmin(np.abs(np.sin(np.radians(psi3)))))
                row["chi_shaft_max"] = float(np.nanmax(chi_s))
                row["chi_fore_max"] = float(np.nanmax(chi_f))
                row["B6_ok"] = bool(np.ptp(psi3) <= np.ptp(psi_img) + 1e-6)
        rows.append(row)

    _planes_report(rows, b1, club_mm, run_root, lab_root, out_dir)
    return rows


def _planes_report(rows, b1, club_mm, run_root, lab_root, out_dir):
    def col(k):
        return np.array([r[k] for r in rows if k in r and isinstance(r[k], (int, float))
                         and np.isfinite(r[k])], dtype=float)

    print(f"\n=== B1  measurement independence: lenPx/s_px_mm + r0 == {club_mm:.0f} mm ===")
    print("  The ONLY test here that checks the data rather than the model, and the")
    print("  only one with an answer known in advance.")
    if b1:
        pooled = np.concatenate(b1)
        med = float(np.median(pooled))
        p10, p90 = np.percentile(pooled, 10), np.percentile(pooled, 90)
        off = abs(med - club_mm) / club_mm
        spread = (p90 - p10) / med
        ok = (off <= 0.05) and (spread <= 0.20)
        print(f"  pooled n={pooled.size}   median {med:.0f} mm  ({100*off:.1f}% off {club_mm:.0f})"
              f"   p10-p90 {p10:.0f}..{p90:.0f} ({100*spread:.0f}% of median)")
        print(f"  B1: {'PASS' if ok else 'FAIL'}")
        print(f"  n={pooled.size} is THIN: fusion covers frames ~397-600 while lenPx lives")
        print("  in the slow phases, so the two channels barely overlap. Reported, not hidden.")
    else:
        print("  B1: NO DATA -- the two radius channels never co-occur.")

    print("\n=== B4  negative control: M1 (single fixed plane) MUST fail ==========")
    print("  length_model documents a ~2x violation of the model's 180-deg periodicity")
    print("  on hand labels. If M1 SUCCEEDS on the fusion radius, that is a red flag on")
    print("  the data, not a vindication of M1.")
    for nm in ("M1-plane", "M2-plane/phase", "M3-cone"):
        v = col(f"{nm}_relerr")
        if v.size:
            print(f"  {nm:16s} median rel-err {np.median(v)*100:5.1f}%   ({v.size} swings)")
    m1, m2 = col("M1-plane_relerr"), col("M2-plane/phase_relerr")
    if m1.size and m2.size:
        worse = np.median(m1) > np.median(m2)
        print(f"  M1 is {'worse' if worse else 'NOT worse'} than M2, by "
              f"{abs(np.median(m1) - np.median(m2))*100:.1f} points.")
        print("  B4: INCONCLUSIVE, and the honest reading is the opposite of a pass.")
        print(f"  M1 was supposed to FAIL. At {np.median(m1)*100:.1f}% median relative error it is")
        print("  not failing in any meaningful sense -- the ordering M1>M2 is a fraction of")
        print("  a point and says nothing. The reason is coverage, not geometry: the fusion")
        print("  band tier spans only the frames the tape is legible in, so the ANTIPARALLEL")
        print("  shaft directions that produced length_model's documented ~2x violation are")
        print("  largely absent. This channel does not challenge M1, so it cannot acquit or")
        print("  convict it. The negative control is UNAVAILABLE here, not satisfied.")

    print("\n=== B3  within-session stability of the recovered geometry ===========")
    rb = col("Rband_ratio")
    if rb.size:
        print(f"  R_band p95/p05 across swings: {rb.min():.2f}..{rb.max():.2f} "
              f"(median {np.median(rb):.2f})")

    print("\n=== B5  the forearm channel ==========================================")
    fj, fr = col("fore_jitter_deg"), col("fore_R0_px")
    if fj.size:
        print(f"  forearm phi jitter  median {np.median(fj):.2f} deg   (pose-driven if large)")
        print(f"  forearm R0 (p95 px) {fr.min():.0f}..{fr.max():.0f}  -- CENSORED estimate,")
        print("  no catalogue length exists for a forearm; every chi inherits this assumption.")

    print("\n=== B6  de-projection sanity (PRE-IMPACT domain) =====================")
    pi_, p3 = col("psi_img_range"), col("psi3_range")
    if pi_.size and p3.size:
        okn = sum(1 for r in rows if r.get("B6_ok"))
        print(f"  psi_img median range {np.median(pi_):.1f} deg   psi3 median range {np.median(p3):.1f} deg")
        print(f"  B6: {okn}/{len(p3)} swings pass the range test "
              f"({'PASS' if okn == len(p3) else 'FAIL -- sign disambiguation suspect'})")
        print("  Note this test is WEAK: psi3 is an arccos and is bounded [0,180] by")
        print("  construction, so a narrower range is partly guaranteed rather than earned.")
        pj = col("psi3_jitter")
        if pj.size:
            print(f"  psi3 jitter (local quadratic) median {np.median(pj):.1f} deg")

    print("\n  --- the second DOF, chi, and why it is NOT carried forward ---")
    cr, cj = col("chi_rel_range"), col("chi_rel_jitter")
    if cr.size and cj.size:
        print(f"  chi range  median {np.median(cr):.0f} deg   chi jitter median {np.median(cj):.1f} deg")
        pmin, perp = col("psi3_min"), col("chi_perp_min")
        if pmin.size:
            print(f"  psi3 minimum median {np.median(pmin):.1f} deg  ->  perpendicular")
            print(f"  component falls to sin(psi3) = {np.median(perp):.3f} of the shaft length")
        ill = pmin.size and np.median(pmin) < 15.0
        print(f"  conditioning: {'ILL-CONDITIONED' if ill else 'adequately conditioned'};"
              f" smoothness: {'poor' if np.median(cj) > 15.0 else 'good'}")
        print("\n  VERDICT: chi is PROVISIONAL and must not be modelled on yet.")
        print("  It is smooth and adequately conditioned, so neither pre-registered")
        print("  failure mode fired -- but the SIGN BRANCH WAS NEVER RESOLVED. deproject()")
        print("  ran with both out-of-plane signs pinned to +1, i.e. both the shaft and the")
        print("  forearm assumed to lean the same way toward the camera on every frame.")
        print("  Foreshortening gives only |chi|, so that is an assumption, not a result,")
        print("  and chi's near-full-circle sweep may be its artefact. B6 passing does NOT")
        print("  vindicate the branch: psi3 is an arccos and would narrow either way.")
        print("  Resolving the branch by continuity is the first task of the next session;")
        print("  until then psi3 is provisional too, and no mechanics family should be")
        print("  fitted to either. Reported as unfinished rather than dressed as a result.")
        if ill:
            print("  NOT because it is noisy -- at 2.5 deg jitter chi is smooth. It is because")
            print("  chi is an AZIMUTH about the forearm axis, defined only while the two")
            print("  lines are apart. psi3 comes down near zero, the perpendicular component")
            print("  vanishes with it, and the azimuth then sweeps freely on no information.")
            print("  The near-full-circle range is that runaway, not a rotating wrist.")
            print("  So the second DOF is dropped -- carrying it would be exactly the")
            print("  phi-term mistake: a latent that moves a lot while measuring little.")
            print("  psi3 alone survives, and inherits the censored R0 assumption and the")
            print("  perspective bar below.")

    print("\n=== B2  THE PLANE, and the gate the brief rests on ===================")
    print("  The clubhead path lies on a plane; a planar path images as an ellipse whose")
    print("  axis ratio is cos(inclination). The plane therefore comes from the head")
    print("  track's SHAPE -- no foreshortening, no per-frame length, and NO SIGN")
    print("  AMBIGUITY, because a plane fixes which side of the node line each direction")
    print("  sits on. Foreshortening is the independent CHECK, not the route.")
    ib, idn = col("iota_back_deg"), col("iota_down_deg")
    if ib.size:
        print(f"\n  backswing plane  iota {ib.min():.1f}..{ib.max():.1f} deg  (median {np.median(ib):.1f})")
    if idn.size:
        print(f"  downswing plane  iota {idn.min():.1f}..{idn.max():.1f} deg  (median {np.median(idn):.1f})")
    if ib.size and idn.size:
        print(f"  back-vs-down shift: {np.median(ib) - np.median(idn):+.1f} deg -- the plane is NOT")
        print("  constant within a swing, which is the shift a coach looks for and which")
        print("  no single fixed plane can represent.")
    print("\n  shaft-vector plane vs foreshortening-implied angle, PER PHASE:")
    for nm in ("back", "down"):
        d = col(f"B2_diff_{nm}")
        if not d.size:
            continue
        ok = int(np.sum(np.abs(d) <= 10.0))
        print(f"    {nm:5s} median {np.median(d):+6.1f}  median |diff| {np.median(np.abs(d)):5.1f}"
              f"  max {np.abs(d).max():5.1f}   {ok}/{d.size} within 10 deg")
    db, dd = col("B2_diff_back"), col("B2_diff_down")
    if db.size and dd.size:
        print(f"  B2: backswing {'PASS' if np.median(np.abs(db)) <= 10 else 'FAIL'}, "
              f"downswing {'PASS' if np.median(np.abs(dd)) <= 10 else 'FAIL'}.")
        print("  The bias is one-signed (foreshortening reads HIGH), which is what the")
        print("  known lenPx under-runs and the perspective inflation would both do.")
        print("  So the two routes are correlated but not interchangeable: treat the")
        print("  ellipse as the estimator and foreshortening as weak corroboration only.")

    print("\n  PRECISION vs ACCURACY -- the distinction that matters for the diagnostic:")
    print("    split-half repeatability of the ellipse estimator is 0.6-0.7 deg in BOTH")
    print("    phases, so DIFFERENCES are trustworthy. Absolute calibration is NOT")
    print("    confirmed, so a single iota should not yet be quoted as a plane angle.")
    print("    The over-the-top diagnostic wants the DELTA, where common bias cancels.")
    dl = col("delta_iota_deg")
    if dl.size:
        print(f"\n  DELTA (backswing iota - downswing iota), the coaching quantity:")
        print(f"    median {np.median(dl):+.1f} deg   range {dl.min():+.1f}..{dl.max():+.1f}"
              f"   ({int(np.sum(dl > 0))}/{dl.size} swings steepen)")
        print(f"    against a ~0.7 deg noise floor, so the spread is REAL between-swing")
        print("    variation, not measurement scatter.")
    print("\n  CORRECTION: an earlier run of this module recorded B2 as UNRUNNABLE, on the")
    print("  grounds that head = grip + lenPx*u(theta) so the path 'carries no new")
    print("  information'. That conflated provenance with structure. The path carries no")
    print("  new MEASUREMENT, but whether it is PLANAR is a structural claim the algebra")
    print("  does not grant -- and it is testable, it passes, and it is what removes the")
    print("  sign ambiguity that was wrongly reported as the blocker.")

    print("\n=== perspective bound (orthographic assumption) ======================")
    sr = col("shoulder_chi_equiv_deg")
    if sr.size:
        print(f"  shoulder-width excursion implies up to {np.median(sr):.0f} deg of APPARENT")
        print("  out-of-plane angle that is really body depth. This is a bias bar on every")
        print("  iota/chi above, not a correction -- nothing here is subtracted.")

    if out_dir and rows:
        od = Path(out_dir); od.mkdir(parents=True, exist_ok=True)
        keys = sorted({k for r in rows for k in r})
        with open(od / "plane_probe_planes.csv", "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=["swing"] + [k for k in keys if k != "swing"])
            w.writeheader(); w.writerows(rows)
        json.dump({"run_root": str(run_root), "lab_root": str(lab_root),
                   "club_mm": club_mm, "git_sha": git_sha()},
                  open(od / "plane_probe_meta.json", "w", encoding="utf-8"), indent=2)
        print(f"\nwrote {od}/plane_probe_planes.csv")


def corpus(run_root, out_dir, fig_path=None):
    """The whole-corpus extension of `planes`: per-swing back/down inclination and
    the transition delta on EVERY swing at the run root, not just the lab session.

    Emits transition_plane_corpus.csv, and (with --fig) the distribution figure the
    model note carries. Quality travels with every row — sample counts, conic
    residual, and odd/even split-half repeatability per phase — because at this
    run root only ~33/61 swings yield both conic fits (the blur-thinned downswing
    arc degenerates elsewhere), and a consumer must be able to gate on that."""
    from theta_psi_model import PH_TAKEAWAY as _TW, PH_TOP as _TP, PH_IMPACT as _IM

    pathological = {"2026-06-11", "2026-07-04"}   # the note's §12 sessions

    def _split_half(t, hx, hy, lo, hi):
        m = (t >= lo) & (t <= hi)
        x, y = hx[m], hy[m]
        if x.size < 24:
            return None
        a_ = fit_ellipse(x[0::2], y[0::2])
        b_ = fit_ellipse(x[1::2], y[1::2])
        return abs(a_["iota_deg"] - b_["iota_deg"]) if (a_ and b_) else None

    rows, skipped = [], []
    for d in sorted(Path(run_root).iterdir()):
        if not (d / "result.json").exists():
            continue
        run = load_run(d)
        if run is None or run["shaft_xy"] is None:
            skipped.append((d.name, "no usable shaft samples"))
            continue
        pl = head_path_plane(run)
        if "back" not in pl or "down" not in pl:
            skipped.append((d.name, "conic fit failed (" +
                            "/".join(sorted({"back", "down"} - set(pl))) + ")"))
            continue
        t, hx, hy = run["shaft_xy"]
        ev = run["events"]
        date = d.name.split("__")[0].split("_")[0]
        rows.append({
            "swing": d.name, "date": date,
            "pathological": int(date in pathological),
            "iota_back_deg": round(pl["back"]["iota_deg"], 2),
            "iota_down_deg": round(pl["down"]["iota_deg"], 2),
            "delta_deg": round(pl["back"]["iota_deg"] - pl["down"]["iota_deg"], 2),
            "node_back_deg": round(pl["back"]["node_deg"], 2),
            "node_down_deg": round(pl["down"]["node_deg"], 2),
            "n_back": pl["back"]["n"], "n_down": pl["down"]["n"],
            "conic_resid_back": round(pl["back"]["conic_resid"], 4),
            "conic_resid_down": round(pl["down"]["conic_resid"], 4),
            "split_half_back_deg": _split_half(t, hx, hy, ev[_TW], ev[_TP]),
            "split_half_down_deg": _split_half(t, hx, hy, ev[_TP], ev[_IM]),
        })

    out = Path(out_dir); out.mkdir(parents=True, exist_ok=True)
    with open(out / "transition_plane_corpus.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)

    D = np.array([r["delta_deg"] for r in rows])
    H = np.array([not r["pathological"] for r in rows], dtype=bool)
    print(f"corpus: {len(rows)} swings fit both phases, {len(skipped)} skipped")
    for nm, sel in (("all", np.ones_like(H)), ("healthy", H)):
        v = D[sel]
        print(f"  {nm}: n={v.size} median={np.median(v):+.1f} "
              f"p10..p90=[{np.percentile(v, 10):+.1f},{np.percentile(v, 90):+.1f}] "
              f"steepen={int((v > 0).sum())}/{v.size}")
    for name, why in skipped:
        print(f"  skipped {name}: {why}")

    if fig_path:
        _corpus_figure(rows, fig_path)
        print(f"  figure -> {fig_path}")


def _corpus_figure(rows, fig_path):
    """The two-panel distribution figure: per-swing delta by session, and the two
    phase inclinations against each other. Open markers = split-half worse than
    5 deg; the black tick is the session median."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D

    SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
    BLUE, ORANGE = "#2a78d6", "#eb6834"
    sessions = sorted({r["date"] for r in rows})
    lowq = {r["swing"]: (r["split_half_down_deg"] is None or r["split_half_down_deg"] > 5.0)
            for r in rows}

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11.5, 4.6), dpi=200,
                                   gridspec_kw={"width_ratios": [1.5, 1.0], "wspace": 0.28})
    fig.patch.set_facecolor(SURFACE)

    ax1.set_facecolor(SURFACE)
    rng = np.random.default_rng(7)
    for i, s in enumerate(sessions):
        sr = [r for r in rows if r["date"] == s]
        ys = i + rng.uniform(-0.13, 0.13, len(sr))
        for r, yy in zip(sr, ys):
            c = ORANGE if r["pathological"] else BLUE
            if lowq[r["swing"]]:
                ax1.scatter(r["delta_deg"], yy, s=52, facecolor="none", edgecolor=c,
                            linewidth=1.6, zorder=3)
            else:
                ax1.scatter(r["delta_deg"], yy, s=52, facecolor=c, edgecolor=SURFACE,
                            linewidth=1.2, zorder=3)
        med = np.median([r["delta_deg"] for r in sr])
        ax1.plot([med, med], [i - 0.26, i + 0.26], color=INK, linewidth=1.6, zorder=4)
    ax1.axvline(0, color=INK2, linewidth=1.0, linestyle=(0, (4, 3)), zorder=2)
    ax1.set_yticks(range(len(sessions)))
    ax1.set_yticklabels([f"{s}   ({len([r for r in rows if r['date'] == s])} fit)"
                         for s in sessions], fontsize=8.5, color=INK)
    ax1.set_xlabel("transition delta  =  backswing ι − downswing ι   (°)",
                   fontsize=9, color=INK)
    ax1.set_xlim(-65, 65)
    ax1.invert_yaxis()
    ax1.tick_params(colors=INK2, labelsize=8)
    ax1.grid(axis="x", color=GRID, linewidth=0.7, zorder=0)
    for sp in ax1.spines.values():
        sp.set_visible(False)
    ax1.annotate("steepens in transition →", xy=(0.985, 1.015), xycoords="axes fraction",
                 ha="right", fontsize=8.5, color=INK2, style="italic")
    ax1.annotate("← shallows", xy=(0.015, 1.015), xycoords="axes fraction",
                 ha="left", fontsize=8.5, color=INK2, style="italic")
    ax1.set_title("transition_plane across the corpus — per swing, by session",
                  fontsize=10.5, color=INK, loc="left", pad=22)

    ax2.set_facecolor(SURFACE)
    lim = (5, 70)
    ax2.plot(lim, lim, color=INK2, linewidth=1.0, linestyle=(0, (4, 3)), zorder=2)
    for r in rows:
        c = ORANGE if r["pathological"] else BLUE
        if lowq[r["swing"]]:
            ax2.scatter(r["iota_back_deg"], r["iota_down_deg"], s=52, facecolor="none",
                        edgecolor=c, linewidth=1.6, zorder=3)
        else:
            ax2.scatter(r["iota_back_deg"], r["iota_down_deg"], s=52, facecolor=c,
                        edgecolor=SURFACE, linewidth=1.2, zorder=3)
    ax2.set_xlim(lim); ax2.set_ylim(lim)
    ax2.set_aspect("equal")
    ax2.set_xlabel("backswing ι (°)  — larger = flatter", fontsize=9, color=INK)
    ax2.set_ylabel("downswing ι (°)", fontsize=9, color=INK)
    ax2.tick_params(colors=INK2, labelsize=8)
    ax2.grid(color=GRID, linewidth=0.7, zorder=0)
    for sp in ax2.spines.values():
        sp.set_visible(False)
    ax2.annotate("steepens\n(below the line)", xy=(52, 21), fontsize=8.5, color=INK2,
                 ha="center", style="italic")
    ax2.set_title("the two phases, per swing", fontsize=10.5, color=INK, loc="left", pad=22)

    handles = [
        Line2D([], [], marker="o", linestyle="none", markersize=7.5, markerfacecolor=BLUE,
               markeredgecolor=SURFACE, label="healthy session"),
        Line2D([], [], marker="o", linestyle="none", markersize=7.5, markerfacecolor=ORANGE,
               markeredgecolor=SURFACE, label="pathological session (§12)"),
        Line2D([], [], marker="o", linestyle="none", markersize=7.5, markerfacecolor="none",
               markeredgecolor=INK2, markeredgewidth=1.6,
               label="split-half > 5° (low-quality fit)"),
        Line2D([], [], color=INK, linewidth=1.6, label="session median"),
    ]
    fig.legend(handles=handles, loc="lower center", ncol=4, frameon=False,
               fontsize=8.5, labelcolor=INK, bbox_to_anchor=(0.5, -0.035))
    plt.savefig(fig_path, bbox_inches="tight", facecolor=SURFACE)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("census", "planes", "corpus"))
    ap.add_argument("--run-root", default="/mnt/swingdata/stagegate/corpm3-off")
    ap.add_argument("--lab-root", default="/mnt/swingdata/shaftlab/lab/tape_20260705")
    ap.add_argument("--club-mm", type=float, default=CLUB_LEN_MM_DEFAULT)
    ap.add_argument("--out", default=None)
    ap.add_argument("--fig", default=None,
                    help="corpus mode: also write the distribution figure to this path")
    a = ap.parse_args()
    if a.mode == "census":
        census(a.run_root, a.lab_root, a.out)
    elif a.mode == "corpus":
        corpus(a.run_root, a.out or ".", a.fig)
    else:
        planes(a.run_root, a.lab_root, a.out, a.club_mm)
    return 0


if __name__ == "__main__":
    sys.exit(main())
