#!/usr/bin/env python3
"""Reference acceleration for the motion-adaptive smoother window (Phase 0).

Phase 0 of ~/.claude/plans/buzzing-frolicking-valley.md / C14 of
docs/implementation/metric_presentation_honesty_phase5_contracts.md. C14's `accel` policy
scales the per-step process noise by `clamp((|a| / aRefPxS2)^expo, minScale, 1)` where |a| is
the smoothed acceleration magnitude of the keypoint in px/s². `aRefPxS2` ships as a
placeholder (20000.0) until this probe measures it. This script measures nothing else: it
reports, per swing and per joint, how big |a| actually is when the joint is QUIET (still
address, backswing) and when it is MOVING (transition through impact), so aRef can be set to
the value that separates the two.

What it reads
-------------
`pose2d.smoothed` (the same track the producers read, and the same track C14's pass 1 would
produce) and falls back to `pose2d.frames` with `smoothed=0` in the CSV so a document without
a cached smoothed track is never silently mixed in with the rest.

Pixels, not normalised units
----------------------------
`kp` is 133 keypoints x 3 floats (x, y, conf), normalised 0..1 against the FULL camera frame.
src/Analysis/pose_smoother.cpp smoothPoseTrack does `zx = kp.x() * W` with W/H taken from the
face-on CameraFormat (wrist_analyzer.cpp PoseSmoothStage passes `cfmt->width/height`), so
aRefPxS2 lives in those pixels and this probe has to use the same ones. In swing.json that
format is `streams[<camera>].source.{width,height}` for the video streams in camera order
(`encoded` is the mp4, which happens to match today); `analysis.club.frameWidth/frameHeight`
is used as a cross-check and as the fallback when the stream block is absent. The `dims_src`
column says which was used for each swing. NB the corpus is NOT one format: the June/July
sessions are 720x1024 and the 08-18 sessions 1280x1024, so a px/s² threshold is per-format
and the wider frame reads larger for the same physical motion.

Acceleration
------------
Central differences on the NON-UNIFORM pose timestamps (the tracks jitter by several ms):

    a_k ~ 2 * [ (p_{k+1} - p_k)/dt_plus - (p_k - p_{k-1})/dt_minus ] / (dt_plus + dt_minus)

with dt in seconds and p in pixels, then |a| = hypot(a_x, a_y). This is the standard
non-uniform second difference; it degrades to the uniform 2nd difference when dt+ == dt-.

Windows (a swing missing Address, Top or Impact is skipped and counted)
    address   [Address - 300 ms, Address]   - the body is still: this is the noise floor
    backswing [Address, Top]                - slow, mostly quiet
    downswing [Top, Impact]                 - the fast stretch the corridors are seeded in
    post      [Impact, Impact + 250 ms]     - still fast, decelerating

Per ladder position: the median |a| within +/-25 ms of each of P1..P7 (Address 0,
ShaftParallelBack 12, MidBackswing 8, Top 2, ArmParallelDown 13, Delivery 9, Impact 5) is
reported as a_p1..a_p7 with its sample count. aRef is chosen from the IMPACT side - the
smallest value that keeps the scale saturated at 1.0 through P6-P7 on essentially every swing
- and NOT from the still address, because outside the 08-18 sessions the address window
carries real setup motion and reads as loud as the downswing.

Joints: hip centre (mean of COCO 11 left / 12 right), lead knee and lead ankle. Lead is the
target side: `athlete.handedness` "Right" => the LEFT leg leads => knee 13, ankle 15 (and
mirrored for "Left"). That matches wrist_analyzer.cpp's `leftLeads = (handedness != 2)`.

Usage:
    hip_accel_reference.py ROOT [--file swing.json|result.json] [--out CSV] [--only SUBSTR]
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Reused verbatim from the Phase 0 series probe - same JSON walk, same percentile convention.
from series_noise import (  # noqa: E402
    LADDER_TO_PHASE,
    PHASE_ADDRESS,
    PHASE_IMPACT,
    find_blocks,
    fmt,
    median,
    percentile,
    phase_times,
)

PHASE_TOP = LADDER_TO_PHASE[4]        # Top == P4 == phase enum 2

# COCO body keypoints inside the 133-point WholeBody layout (0-16 are unchanged COCO).
KP_L_HIP, KP_R_HIP = 11, 12
KP_L_KNEE, KP_R_KNEE = 13, 14
KP_L_ANKLE, KP_R_ANKLE = 15, 16

ADDRESS_WINDOW_US = 300_000     # still-address window, matching series_noise.py
POST_WINDOW_US = 250_000        # post-impact window
MIN_CONF = 0.0                  # smoothed conf carries render alpha; > 0 means "has a value"
MIN_WINDOW_SAMPLES = 3          # fewer than this in a window and the statistic is blank
PHASE_HALF_US = 25_000          # +/-25 ms around a ladder instant for the per-P median

# The P-position ladder as C14 needs it read: the reference has to be chosen from the IMPACT
# side (the smallest |a| that still keeps s = 1 through P6-P7 on essentially every swing),
# because the address of most corpus sessions is NOT quiet - it carries real setup motion.
LADDER = [1, 2, 3, 4, 5, 6, 7]

# Default frame size when neither the stream block nor the club block carries one. Only used
# with dims_src=default so such rows can be excluded from a threshold decision.
FALLBACK_W, FALLBACK_H = 1280, 1024

COLUMNS = [
    "swing",          # swing directory relative to ROOT
    "session",
    "joint",          # hip_centre | lead_knee | lead_ankle
    "kp",             # keypoint index/indices used
    "smoothed",       # 1 = pose2d.smoothed, 0 = fell back to pose2d.frames
    "hand",           # athlete.handedness as persisted
    "lead",           # left | right (the side resolved as leading)
    "W",
    "H",
    "dims_src",       # stream.source | stream.encoded | club | default
    "n_frames",
    "dt_med_ms",
    "addr_n",
    "addr_med",
    "addr_p95",       # THE quiet floor: the still body's apparent acceleration
    "back_n",
    "back_med",
    "back_p95",       # quiet-ish: most of the backswing
    "down_n",
    "down_med",       # THE moving level: transition -> impact
    "down_p95",
    "post_n",
    "post_med",
    "peak_max",       # max |a| over [Address, Impact + 250 ms]
    "ratio_down_addr",   # down_med / addr_p95: the separation this probe exists to measure
] + [f"a_p{l}" for l in LADDER] + [f"a_p{l}_n" for l in LADDER]
# a_p<L> = median |a| within +/-25 ms of ladder position L (blank when the phase is absent or
# fewer than MIN_WINDOW_SAMPLES accel samples fall in the window); a_p<L>_n = that count.
# These are the columns the aRef decision is actually made on: P6/P7 set the floor aRef must
# clear and P1 says what is left over at address.


# --------------------------------------------------------------------------------------
# Frame pixel dimensions. See the module docstring for why these and not the crop.
# --------------------------------------------------------------------------------------

def frame_dims(doc, pose):
    """(W, H, source_label). Video streams in camera order, indexed by pose2d.camera."""
    cam = pose.get("camera")
    streams = doc.get("streams")
    if isinstance(streams, list) and isinstance(cam, int) and cam >= 0:
        vids = [s for s in streams
                if isinstance(s, dict) and (s.get("kind") == "video"
                                            or isinstance(s.get("encoded"), dict))]
        if cam < len(vids):
            s = vids[cam]
            for key, label in (("source", "stream.source"), ("encoded", "stream.encoded")):
                blk = s.get(key)
                if isinstance(blk, dict):
                    w, h = blk.get("width"), blk.get("height")
                    if isinstance(w, int) and isinstance(h, int) and w > 0 and h > 0:
                        return w, h, label

    # Fallback: the club tracker persists the frame size it ran on. Same camera in every
    # corpus document checked, so it is the right size when the stream block is missing.
    club = (doc.get("analysis") or {}).get("club") if isinstance(doc.get("analysis"), dict) else None
    if isinstance(club, dict):
        w, h = club.get("frameWidth"), club.get("frameHeight")
        if isinstance(w, int) and isinstance(h, int) and w > 0 and h > 0:
            return w, h, "club"

    return FALLBACK_W, FALLBACK_H, "default"


def lead_side(doc):
    """(lead_label, knee_kp, ankle_kp). handedness "Right" => the left leg leads."""
    hand = ((doc.get("athlete") or {}).get("handedness")
            if isinstance(doc.get("athlete"), dict) else None)
    label = hand if isinstance(hand, str) else ""
    left_leads = not (isinstance(hand, str) and hand.strip().lower().startswith("left"))
    if left_leads:
        return label, "left", KP_L_KNEE, KP_L_ANKLE
    return label, "right", KP_R_KNEE, KP_R_ANKLE


# --------------------------------------------------------------------------------------
# Track extraction and the non-uniform second difference.
# --------------------------------------------------------------------------------------

def point_track(frames, kps, W, H):
    """[(t_us, x_px, y_px)] for the MEAN of the given keypoint indices, in pixels.

    A frame is dropped when any contributing keypoint has conf <= MIN_CONF (on the smoothed
    track that means the smoother produced no value there, which is exactly the case C14
    treats as |a| = 0; dropping is the honest choice for a reference measurement because a
    fabricated zero would drag the quiet percentiles down)."""
    need = 3 * (max(kps) + 1)
    out = []
    for fr in frames:
        kp = fr.get("kp") or []
        t = fr.get("t_us")
        if t is None or len(kp) < need:
            continue
        xs, ys, ok = 0.0, 0.0, True
        for i in kps:
            if kp[3 * i + 2] <= MIN_CONF:
                ok = False
                break
            xs += kp[3 * i]
            ys += kp[3 * i + 1]
        if not ok:
            continue
        n = float(len(kps))
        out.append((t, (xs / n) * W, (ys / n) * H))
    out.sort(key=lambda r: r[0])
    return out


def accel_track(track):
    """[(t_us, |a| px/s^2)] by central difference on non-uniform timestamps.

    a ~ 2 * [ (p+ - p)/dt+ - (p - p-)/dt- ] / (dt+ + dt-).
    Only interior samples with both neighbours present and both dt > 0 are produced; a gap in
    the track (a dropped frame above) simply widens dt and is not bridged with a fake sample.
    """
    out = []
    for k in range(1, len(track) - 1):
        t0, x0, y0 = track[k - 1]
        t1, x1, y1 = track[k]
        t2, x2, y2 = track[k + 1]
        dtm = (t1 - t0) * 1e-6
        dtp = (t2 - t1) * 1e-6
        if dtm <= 0.0 or dtp <= 0.0:
            continue
        ax = 2.0 * ((x2 - x1) / dtp - (x1 - x0) / dtm) / (dtp + dtm)
        ay = 2.0 * ((y2 - y1) / dtp - (y1 - y0) / dtm) / (dtp + dtm)
        out.append((t1, math.hypot(ax, ay)))
    return out


def window(acc, lo, hi):
    return [a for (t, a) in acc if lo <= t <= hi]


def win_stats(vals):
    """(n, median, p95) with median/p95 blank below MIN_WINDOW_SAMPLES."""
    if len(vals) < MIN_WINDOW_SAMPLES:
        return len(vals), None, None
    return len(vals), median(vals), percentile(vals, 0.95)


# --------------------------------------------------------------------------------------
# Per-swing work.
# --------------------------------------------------------------------------------------

def analyse_swing(path, root):
    """(rows, note). note non-empty => the swing contributed nothing and is counted."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except Exception as exc:
        return [], f"unreadable: {exc}"

    _metrics, phases, pose = find_blocks(doc)
    if not pose:
        return [], "no pose2d block"

    ptimes = phase_times(phases)
    addr_t, top_t, imp_t = (ptimes.get(PHASE_ADDRESS), ptimes.get(PHASE_TOP),
                            ptimes.get(PHASE_IMPACT))
    missing = [n for n, t in (("Address", addr_t), ("Top", top_t), ("Impact", imp_t))
               if t is None]
    if missing:
        return [], "no " + "/".join(missing)
    if not (addr_t < top_t < imp_t):
        return [], f"phases out of order (Address {addr_t} Top {top_t} Impact {imp_t})"

    frames = pose.get("smoothed")
    is_smoothed = 1
    if not frames:
        frames = pose.get("frames")
        is_smoothed = 0
    if not frames:
        return [], "no pose frames"

    W, H, dims_src = frame_dims(doc, pose)
    hand, lead, knee_kp, ankle_kp = lead_side(doc)

    swing_dir = os.path.relpath(os.path.dirname(path), root)
    session = os.path.basename(os.path.dirname(os.path.dirname(path)))

    joints = [
        ("hip_centre", [KP_L_HIP, KP_R_HIP]),
        ("lead_knee", [knee_kp]),
        ("lead_ankle", [ankle_kp]),
    ]

    rows = []
    for name, kps in joints:
        track = point_track(frames, kps, W, H)
        acc = accel_track(track)
        dts = [(track[i][0] - track[i - 1][0]) / 1000.0 for i in range(1, len(track))]

        a_n, a_med, a_p95 = win_stats(window(acc, addr_t - ADDRESS_WINDOW_US, addr_t))
        b_n, b_med, b_p95 = win_stats(window(acc, addr_t, top_t))
        d_n, d_med, d_p95 = win_stats(window(acc, top_t, imp_t))
        p_n, p_med, _p95 = win_stats(window(acc, imp_t, imp_t + POST_WINDOW_US))
        peak = window(acc, addr_t, imp_t + POST_WINDOW_US)

        # Per-ladder-position level, read off the same accel track. LADDER_TO_PHASE maps the
        # P slot to the persisted phase enum (P4 = Top = 2, P7 = Impact = 5, and so on).
        per_p = {}
        for lad in LADDER:
            tt = ptimes.get(LADDER_TO_PHASE[lad])
            if tt is None:
                per_p[lad] = (0, None)
                continue
            # No MIN_WINDOW_SAMPLES floor here, deliberately: the pose track is sparse away
            # from impact (~27 ms at address vs ~7 ms in the downswing), so +/-25 ms holds
            # only 1-2 samples at P1/P2 and blanking those would hide the address level
            # entirely. The a_p<L>_n column carries the count so a thin window is visible.
            w = window(acc, tt - PHASE_HALF_US, tt + PHASE_HALF_US)
            per_p[lad] = (len(w), median(w) if w else None)

        row = {c: "" for c in COLUMNS}
        row.update({
            "swing": swing_dir,
            "session": session,
            "joint": name,
            "kp": "+".join(str(i) for i in kps),
            "smoothed": is_smoothed,
            "hand": hand,
            "lead": lead,
            "W": W,
            "H": H,
            "dims_src": dims_src,
            "n_frames": len(track),
            "dt_med_ms": fmt(median(dts), 2),
            "addr_n": a_n, "addr_med": fmt(a_med, 1), "addr_p95": fmt(a_p95, 1),
            "back_n": b_n, "back_med": fmt(b_med, 1), "back_p95": fmt(b_p95, 1),
            "down_n": d_n, "down_med": fmt(d_med, 1), "down_p95": fmt(d_p95, 1),
            "post_n": p_n, "post_med": fmt(p_med, 1),
            "peak_max": fmt(max(peak) if peak else None, 1),
            "ratio_down_addr": fmt((d_med / a_p95) if (d_med is not None and a_p95) else None, 2),
        })
        for lad in LADDER:
            n, med = per_p[lad]
            row[f"a_p{lad}"] = fmt(med, 1)
            row[f"a_p{lad}_n"] = n
        rows.append(row)

    return rows, ""


# --------------------------------------------------------------------------------------
# Summary to stderr: medians over swings, per joint, per column.
# --------------------------------------------------------------------------------------

STAT_COLS = ["addr_med", "addr_p95", "back_med", "back_p95",
             "down_med", "down_p95", "post_med", "peak_max", "ratio_down_addr"]


def _num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def _cell(v, nd):
    """Fixed-width number for the stderr table. NOT series_noise.fmt: that one rstrips
    trailing zeros, which turns 12560 into 1256 at nd=0."""
    if v is None:
        return ""
    return f"{v:.{nd}f}"


def print_summary(rows, n_read, skipped, out=sys.stderr):
    joints = ["hip_centre", "lead_knee", "lead_ankle"]
    hdr = f"{'joint':<12} {'n':>4}" + "".join(f" {c:>10}" for c in STAT_COLS)
    print(f"\n{n_read} document(s) read, {len(skipped)} skipped "
          f"(no Address/Top/Impact or no pose)", file=out)
    for path, note in skipped:
        print(f"  skipped {path}: {note}", file=out)
    print(f"\nmedians over swings of each per-swing statistic, |a| in px/s^2", file=out)
    print(hdr, file=out)
    print("-" * len(hdr), file=out)
    for j in joints:
        jr = [r for r in rows if r["joint"] == j]
        cells = []
        for c in STAT_COLS:
            xs = [_num(r[c]) for r in jr]
            cells.append(_cell(median([x for x in xs if x is not None]),
                               2 if c == "ratio_down_addr" else 0))
        print(f"{j:<12} {len(jr):>4}" + "".join(f" {v:>10}" for v in cells), file=out)

    print(f"\nmedians over swings of |a| within +/-25 ms of each ladder position", file=out)
    phdr = f"{'joint':<12} {'n':>4}" + "".join(f" {'P'+str(l):>9}" for l in LADDER)
    print(phdr, file=out)
    print("-" * len(phdr), file=out)
    for j in joints:
        jr = [r for r in rows if r["joint"] == j]
        cells = []
        for l in LADDER:
            xs = [_num(r[f"a_p{l}"]) for r in jr]
            cells.append(_cell(median([x for x in xs if x is not None]), 0))
        print(f"{j:<12} {len(jr):>4}" + "".join(f" {v:>9}" for v in cells), file=out)

    fmts = sorted({(r["W"], r["H"]) for r in rows})
    print(f"\nframe formats present: "
          + ", ".join(f"{w}x{h} ({sum(1 for r in rows if (r['W'], r['H']) == (w, h)) // 3} swings)"
                      for w, h in fmts), file=out)
    raw = sum(1 for r in rows if str(r["smoothed"]) == "0") // 3
    if raw:
        print(f"WARNING: {raw} swing(s) had no pose2d.smoothed and used raw frames "
              f"(smoothed=0 in the CSV)", file=out)
    dflt = sum(1 for r in rows if r["dims_src"] == "default") // 3
    if dflt:
        print(f"WARNING: {dflt} swing(s) fell back to the default {FALLBACK_W}x{FALLBACK_H} "
              f"frame size (dims_src=default)", file=out)

    # Per-format breakdown, because px/s^2 is not comparable across frame widths.
    if len(fmts) > 1:
        for w, h in fmts:
            print(f"\n  {w}x{h}", file=out)
            print("  " + hdr, file=out)
            for j in joints:
                jr = [r for r in rows if r["joint"] == j and (r["W"], r["H"]) == (w, h)]
                cells = []
                for c in STAT_COLS:
                    xs = [_num(r[c]) for r in jr]
                    cells.append(_cell(median([x for x in xs if x is not None]),
                                       2 if c == "ratio_down_addr" else 0))
                print(f"  {j:<12} {len(jr):>4}" + "".join(f" {v:>10}" for v in cells), file=out)
            print("  " + phdr, file=out)
            for j in joints:
                jr = [r for r in rows if r["joint"] == j and (r["W"], r["H"]) == (w, h)]
                cells = []
                for l in LADDER:
                    xs = [_num(r[f"a_p{l}"]) for r in jr]
                    cells.append(_cell(median([x for x in xs if x is not None]), 0))
                print(f"  {j:<12} {len(jr):>4}" + "".join(f" {v:>9}" for v in cells), file=out)


# --------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", metavar="ROOT", help="directory walked recursively")
    ap.add_argument("--file", default="swing.json",
                    help="document file name (swinglab run roots hold result.json)")
    ap.add_argument("--out", default=None, help="CSV path (default: stdout)")
    ap.add_argument("--only", default=None, help="only swings whose path contains this substring")
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    paths = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        if args.file in filenames:
            p = os.path.join(dirpath, args.file)
            if args.only is None or args.only in p:
                paths.append(p)
    paths.sort()

    rows, skipped = [], []
    for i, p in enumerate(paths, 1):
        print(f"[{i}/{len(paths)}] {os.path.relpath(p, root)}", file=sys.stderr)
        r, note = analyse_swing(p, root)
        if note:
            print(f"    ! {note}", file=sys.stderr)
            skipped.append((os.path.relpath(p, root), note))
        rows.extend(r)

    fh = open(args.out, "w", newline="", encoding="utf-8") if args.out else sys.stdout
    try:
        w = csv.DictWriter(fh, fieldnames=COLUMNS, lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    finally:
        if args.out:
            fh.close()

    print_summary(rows, len(paths), skipped)
    return 0


if __name__ == "__main__":
    sys.exit(main())
