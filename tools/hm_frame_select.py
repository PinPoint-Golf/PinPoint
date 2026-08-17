#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Mark Liversedge
"""hm_frame_select.py — pick the HackMotion frame candidate from a directed capture.

    ./tools/hm_frame_select.py ~/Desktop/singleaxis.hmwire
    ./tools/hm_frame_select.py capture.hmwire --expect bow,ulnar

Phase D reduces the HackMotion→PinPoint anatomical map to one of four candidates
(src/IMU/hm_frame.h explains why there are exactly four).  This tool reads a
capture, finds the motions in it, and reports what each candidate would make of
them, so the choice is read off a measurement rather than argued from geometry.

⚠ CROSS-TALK CANNOT MAKE THIS CHOICE, AND THAT IS THE WHOLE REASON THIS TOOL
  TAKES LABELS.  All four candidates score identical — and identically excellent
  — cross-talk on a pure single-axis motion; they differ only in SIGN.  So a
  capture is only decisive if you recorded WHICH WAY each motion went.  Pass
  those directions with --expect and the tool names the survivor; omit them and
  it can only show you the evidence and leave the choice to you.

  Cross-talk is still reported, and it still matters — as FALSIFICATION.  If
  every candidate shows large cross-talk, the two frames are not axis-aligned to
  each other and the four-candidate reduction is wrong.

⚠ AND DO NOT CHECK THE ANSWER AGAINST THE VENDOR'S APP.  It reports the inverse
  of us on bow/cup, so a CORRECT answer looks wrong there.  See
  docs/design/pinpoint_sign_conventions.md Rule 0.

⚠ RECORD THE MOUNTING THAT PRODUCED THE CAPTURE.  The constant describes where
  the boards sit on the strap, not the device, so it is meaningless without it.

Labels understood by --expect, in the order the motions were performed:
    bow     ulnar       pronate         (positive in our convention)
    cup     radial      supinate        (negative)

exit: 0 a single candidate survives, 1 none or several do, 2 usage or I/O error
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

# The library owns every byte of wire knowledge; this tool owns only the frame
# maths.  Nothing here parses a frame, a scale or a counter.
LIBHM = Path(__file__).resolve().parent.parent.parent / "libhackmotion"
sys.path.insert(0, str(LIBHM / "python"))

try:
    import hackmotion as hm
    from hackmotion import _types as T
except ImportError as exc:  # pragma: no cover - environment problem, not logic
    sys.exit(f"⛔ cannot import the hackmotion binding from {LIBHM}/python: {exc}")


# --- the candidate table, mirroring src/IMU/hm_frame.h -----------------------
#
# ⚠ ORDER IS THE CONTRACT.  These indices are what get written to
# pinpoint::tuned::hmframe::kCandidate, so entries may be corrected but never
# reordered.  R is the map HM-anatomical → PPS-anatomical, as (w, x, y, z).
S = math.sqrt(0.5)
CANDIDATES = [
    ("C1", (S, 0.0, -S, 0.0), "Ry(-90): x->+z, y->+y, z->-x"),
    ("C2", (S, 0.0, +S, 0.0), "Ry(+90): x->-z, y->+y, z->+x"),
    ("C3", (0.0, S, 0.0, +S), "180 about (1,0,1): x->+z, y->-y, z->+x"),
    ("C4", (0.0, S, 0.0, -S), "180 about (1,0,-1): x->-z, y->-y, z->-x"),
]

# Which DOF each label exercises, and the sign our convention gives it.
LABELS = {
    "bow": ("flexion", +1),
    "cup": ("flexion", -1),
    "ulnar": ("deviation", +1),
    "radial": ("deviation", -1),
    "pronate": ("pronation", +1),
    "supinate": ("pronation", -1),
}


# --- quaternion helpers, (w, x, y, z) throughout ------------------------------

def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw)


def qconj(q):
    return (q[0], -q[1], -q[2], -q[3])


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q))
    return tuple(c / n for c in q) if n else (1.0, 0.0, 0.0, 0.0)


def qmatrix(q):
    w, x, y, z = qnorm(q)
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]


def qrotv(q, v):
    r = qmul(qmul(q, (0.0,) + tuple(v)), qconj(q))
    return (r[1], r[2], r[3])


# --- PinPoint's own decomposition, transcribed from wrist_angles.h ------------
#
# ⚠ Transcribed, not reinvented: hm_frame_test.cpp holds the C++ side of the same
# identity, and the two must agree.  ZXY Tait-Bryan — flexion about Z, deviation
# about X, and the axial term drops out (wrist_angles.h:127-128).
def pps_wrist_deg(q_rel):
    R = qmatrix(q_rel)
    fe = math.atan2(-R[0][1], R[1][1])
    rud = math.asin(max(-1.0, min(1.0, R[2][1])))
    return math.degrees(fe), math.degrees(rud)


def decompose(q_arm, q_palm, R):
    """Both units' streamed quaternions -> our (flexion, deviation) in degrees."""
    # ⚠ q_arm ⊗ q_palm*, which is the CONJUGATE of the library's hm_quat_relative.
    # Both are correct in their own convention; only this one feeds a
    # decomposition.  Getting it backwards leaves the ANGLE right and every sign
    # inverted, and nothing downstream would notice.
    q_rel_hm = qmul(qnorm(q_arm), qconj(qnorm(q_palm)))
    R_ph = qconj(R)
    q_rel_pps = qmul(qmul(qconj(R_ph), q_rel_hm), R_ph)
    return pps_wrist_deg(q_rel_pps)


def pronation_dps(gyro_arm, R):
    """Rate about the forearm long axis, from the LOWER-ARM unit alone."""
    return qrotv(R, gyro_arm)[1]


# --- reading the capture ------------------------------------------------------

def load_samples(path: Path, relax_us: int = 15_000_000):
    """Replay a .hmwire through a session and return its CALIBRATED live samples.

    ⚠ Only calibrated samples are of any use here.  Before the device applies its
    own routine the streamed quaternions carry board placement rather than
    anatomy, so a frame solved on them would be solving for the strap.

    ⚠ THE PER-SAMPLE CALIBRATION FLAG IS NOT USABLE ON A REPLAYED CAPTURE, AND
    THE REASON IS WORTH KNOWING.  That flag is stamped from the session's own
    calibration state machine, which advances because the LIBRARY issued the
    pose markers and saw the replies.  `hm_capture.py` is a standalone recorder
    — it writes `a2 00` / `a2 01` itself and drives no session — so a session
    replaying those bytes never issued them, never leaves its idle phase, and
    stamps every sample UNCALIBRATED no matter how good the capture is.  Reading
    that as "the routine never completed" is wrong and it is what this tool did
    at first: it rejected a capture whose `0x94` is plainly on the wire.

    So the boundary is taken from the WIRE, where it is unambiguous: the device
    applies its own transform at the instant it emits the `0x94`, so samples
    after that chunk are in its anatomical frame.  The flag is still preferred
    wherever it IS populated (a live session), and the collapse of the relative
    angle across the boundary is the independent evidence that the transform was
    APPLIED rather than merely emitted — see `relative_angle_deg`.
    """
    session = hm.Session("frame-select", policy={"stream_start_timeout_us": relax_us})
    out = []
    before = []
    seen_94 = False
    flagged = 0

    def drain():
        nonlocal flagged
        while session.poll_writes(8):
            pass
        while session.poll_events(16):
            pass
        while True:
            live = session.poll_live(64)
            if not len(live):
                break
            for s in live:
                rec = {
                    "t": s.host_time_us,
                    "arm": tuple(s.lower_arm.q_world_to_body),
                    "palm": tuple(s.palm.q_world_to_body),
                    "gyro_arm": tuple(s.lower_arm.gyro_dps),
                    "gyro_palm": tuple(s.palm.gyro_dps),
                    # ⚠ THE LIBRARY'S OWN, not a local reimplementation. Its
                    # docstring calls this the blessed path, and the quantity is
                    # the library's presence discriminator rather than anything
                    # this tool is entitled to define. Taken here because it
                    # needs the hm_sample, which does not outlive the drain.
                    "rel_deg": hm.relative_angle_deg(s),
                }
                if s.calibration == T.CalibrationState.CALIBRATED:
                    flagged += 1
                (out if seen_94 else before).append(rec)

    # ⚠ THE META CHUNKS ARE NOT DECORATION. A session that was never told the
    # link came up, and never told to start its stream, silently yields nothing
    # at all — which reads exactly like a capture with no samples in it. This is
    # tools/hm_replay_py.py's driving pattern, and it is the same one for the
    # same reason.
    with hm.Replay(path) as replay:
        for chunk in replay:
            payload = bytes(chunk.data[:chunk.length])
            if chunk.direction == T.WireDirection.META:
                if payload.startswith(b"link_up"):
                    session.on_link_up(247, chunk.host_time_us)
                elif payload.startswith(b"stream_start"):
                    session.start_stream()
            elif chunk.direction == T.WireDirection.HOST_TO_DEVICE:
                session.tick(chunk.host_time_us)
            else:
                # `data` is a fixed-size array; only `length` bytes are the frame.
                session.on_bytes(payload, chunk.host_time_us)
                drain()
                # Drain FIRST, so samples in the same notification as the 0x94
                # are attributed to the side of the boundary they arrived on.
                if payload and payload[0] == 0x94:
                    seen_94 = True
                continue
            drain()
    drain()
    return out, before, seen_94, flagged


def still_median_angle(samples, window=200):
    """Median relative angle over the STILLEST stretch of a run.

    ⚠ The angle is only interpretable at rest with a straight wrist. Taken over
    a moving stretch it reads the wrist actually bending, which is not what the
    presence question is asking.

    ⚠ Convention-blind BY CONSTRUCTION, which is what makes it fit for this one
    job and unfit for every other job here: it cannot tell a reversed
    composition order from a correct one, so it never touches the candidate
    selection. It answers only "was a calibration applied at all", where the two
    populations sit an order of magnitude apart.
    """
    if not samples:
        return None
    scored = []
    for s in samples:
        rate = math.sqrt(sum((p - a) ** 2
                             for p, a in zip(s["gyro_palm"], s["gyro_arm"])))
        scored.append((rate, s["rel_deg"]))
    scored.sort(key=lambda x: x[0])
    take = [ang for _, ang in scored[:min(window, len(scored))]]
    take.sort()
    return take[len(take) // 2]
    drain()
    return out, skipped


# --- finding the motions ------------------------------------------------------

def segment(samples, still_dps=25.0, min_peak_dps=60.0, min_len=8):
    """Split into motion bursts on the JOINT rate, not either unit's own.

    ⚠ The joint rate (omega_palm - omega_arm) is what isolates a wrist
    articulation.  Each unit's gyro is absolute, so a flexion that swings the
    whole arm through space reads on both units and a per-unit threshold would
    call the arm swing a wrist motion.
    """
    bursts, cur = [], []
    for s in samples:
        joint = math.sqrt(sum((p - a) ** 2 for p, a in zip(s["gyro_palm"], s["gyro_arm"])))
        s["joint_dps"] = joint
        if joint > still_dps:
            cur.append(s)
        else:
            if len(cur) >= min_len:
                bursts.append(cur)
            cur = []
    if len(cur) >= min_len:
        bursts.append(cur)
    return [b for b in bursts if max(x["joint_dps"] for x in b) >= min_peak_dps]


def characterise(burst, R):
    """Peak signed excursion FROM THE CALIBRATED NEUTRAL, under one candidate.

    ⚠ FROM NEUTRAL, NOT FROM THE START OF THE BURST, and the difference decides
    whether this tool works at all.  A directed motion is performed and then
    returned: measured as a delta from where each burst began, the outward half
    reads positive and the return reads negative, so one bow produces both signs
    — and the sign is the only thing that selects a candidate.  Measured from
    neutral, the bow and its return both sit on the bowed side and agree.

    Neutral is zero by construction: the device zeroed the pair at its own
    reference pose, which is what the collapse check confirms before we get here.

    Cross-talk is read AT THE INSTANT OF PEAK PRIMARY, where it means something,
    rather than as a separate maximum somewhere else in the burst.
    """
    best_f = best_d = 0.0
    cross_at_f = cross_at_d = 0.0
    peak_p = 0.0
    for s in burst:
        f, d = decompose(s["arm"], s["palm"], R)
        if abs(f) > abs(best_f):
            best_f, cross_at_f = f, d
        if abs(d) > abs(best_d):
            best_d, cross_at_d = d, f
        p = pronation_dps(s["gyro_arm"], R)
        if abs(p) > abs(peak_p):
            peak_p = p
    return {"flexion": best_f, "deviation": best_d, "pronation": peak_p,
            "_cross_f": abs(cross_at_f), "_cross_d": abs(cross_at_d)}


def principal_axis(vecs):
    """Principal direction of a cloud of rate vectors, and how single-axis it is.

    Power iteration on the 3x3 scatter matrix. Sign-free: an axis, not a
    direction. Returns (unit_axis, single_axis_fraction) or None.
    """
    m = [[0.0] * 3 for _ in range(3)]
    for g in vecs:
        for i in range(3):
            for j in range(3):
                m[i][j] += g[i] * g[j]
    v = [1.0, 1.0, 1.0]
    for _ in range(80):
        w = [sum(m[i][j] * v[j] for j in range(3)) for i in range(3)]
        n = math.sqrt(sum(c * c for c in w))
        if n == 0:
            return None
        v = [c / n for c in w]
    trace = m[0][0] + m[1][1] + m[2][2]
    lead = sum(v[i] * sum(m[i][j] * v[j] for j in range(3)) for i in range(3))
    return v, (lead / trace if trace else 0.0)


def joint_axis_evidence(burst):
    """Where the JOINT rate points, in the device's own axes.

    ⚠ THIS IS THE REAL FALSIFICATION TEST, and it is not the same thing as the
    decomposition's cross-talk. The four-candidate reduction assumes the device
    carries flexion on its X and deviation on its Z; this measures that directly
    from the rate, with no candidate and no decomposition in the way. Decomposed
    cross-talk cannot do the job because a human "pure" single-axis motion
    contributes 15-20° of genuine off-axis movement, so a perfectly good capture
    shows cross-talk that looks alarming and means nothing.

    ⚠ Joint rate, not either unit's own: each block's gyro is absolute, so during
    a flexion the whole arm also swings through space and the per-unit axis says
    nothing about the articulation.
    """
    joint = [tuple(p - a for p, a in zip(s["gyro_palm"], s["gyro_arm"]))
             for s in burst]
    r = principal_axis(joint)
    if r is None:
        return None
    v, frac = r
    ang = [math.degrees(math.acos(min(1.0, abs(v[k])))) for k in range(3)]
    return {"axis": v, "frac": frac,
            "from_x": ang[0], "from_y": ang[1], "from_z": ang[2]}


def limb_axis_evidence(burst):
    """Where a burst puts the LOWER-ARM unit's angular velocity, in device axes.

    The four-candidate reduction assumes the device puts its limb axis on Y. This
    is the check on that assumption, and it needs a deliberate forearm ROTATION:
    during pronation the whole forearm turns about its own long axis, so the
    lower-arm unit's own angular velocity should lie along that axis and nowhere
    else. Returns (degrees_from_Y, single_axis_fraction).

    ⚠ Use the unit's OWN rate here, not the joint rate. The joint rate is right
    for isolating a wrist articulation and wrong for this: pronation is barely an
    articulation at all — both units turn together — so differencing them removes
    the very motion being measured.
    """
    # Principal direction of the angular-velocity cloud, by power iteration on
    # the 3x3 scatter matrix. Sign-free: an axis, not a direction.
    m = [[0.0] * 3 for _ in range(3)]
    for s in burst:
        g = s["gyro_arm"]
        for i in range(3):
            for j in range(3):
                m[i][j] += g[i] * g[j]
    v = [1.0, 1.0, 1.0]
    for _ in range(64):
        w = [sum(m[i][j] * v[j] for j in range(3)) for i in range(3)]
        n = math.sqrt(sum(c * c for c in w))
        if n == 0:
            return None
        v = [c / n for c in w]
    trace = m[0][0] + m[1][1] + m[2][2]
    lead = sum(v[i] * sum(m[i][j] * v[j] for j in range(3)) for i in range(3))
    frac = lead / trace if trace else 0.0
    from_y = math.degrees(math.acos(min(1.0, abs(v[1]))))
    return from_y, frac


def dominant(exc, burst=None):
    """Which DOF this burst was, and how much leaked into the others.

    Pronation is compared on its own terms — it is a rate in °/s against two
    angles in degrees — so it only wins when the two angles barely moved.
    """
    # ⚠ A FOREARM ROTATION IS RECOGNISED BY THE FOREARM TURNING, not by the
    # wrist staying still.  The first version of this asked for small flexion
    # AND small deviation AND a large rate, and a real supination fails that: the
    # wrist is not a rigid coupling, so it picks up 10-20° of genuine flex/dev
    # while the forearm rotates.  Every rotation in three captures was therefore
    # filed as flexion or deviation, where it then failed the axis test for the
    # entirely correct reason that a rotation does not sit on X or Z — an
    # artefact that reads exactly like the reduction being falsified.
    #
    # The direct test is the LOWER-ARM unit's own rate: during a rotation the
    # whole forearm turns about its long axis, hard and cleanly.
    if burst is not None:
        peak = max(math.sqrt(sum(c * c for c in s["gyro_arm"])) for s in burst)
        ev = limb_axis_evidence(burst)
        if ev is not None and peak > 60.0 and ev[0] < 25.0 and ev[1] > 0.9:
            return "pronation", 0.0
    if abs(exc["flexion"]) >= abs(exc["deviation"]):
        return "flexion", exc["_cross_f"]
    return "deviation", exc["_cross_d"]


PROTOCOL = """
================================================================================
PHASE D CAPTURE PROTOCOL — what the recording has to contain
================================================================================

⚠ IGNORE THE PROMPT hm_capture.py PRINTS WHILE RECORDING. That list — hold
  still, a fast flick, a few swings — is libhackmotion's own agenda for
  reconciling its protocol spec against a sensor. It is a DIFFERENT JOB and it
  produces a capture that cannot select a frame candidate. This is the list that
  matters for Phase D.

RUN, with PinPoint disconnected (the device allows one connection):

    ~/Projects/.venv-hackmotion/bin/python \\
        ../libhackmotion/tools/hm_capture.py --calibrate --duration 180 \\
        --out ~/Desktop/singleaxis.hmwire --device-id wg3-mount1

--calibrate runs the two-marker routine first, with prompts. The routine is:
    pose 0 = forearm ACROSS THE CHEST, palm down
    pose 1 = forearm ELEVATED 30°, elbow stationary

⚠ THE CALIBRATION MUST BE IN THE SAME CONNECTION AS THE MOTIONS. It does not
  survive a disconnect or a power cycle, so it cannot be done beforehand.

THEN, after the routine completes, and this is the part that selects:

  1. Settle at neutral — wrist straight — for a few seconds.
  2. BOW the wrist, hold ~2 s, return to neutral.        (repeat 3 times)
  3. Settle at neutral again.
  4. ULNAR deviate — the "hinge" — hold ~2 s, return.    (repeat 3 times)
  5. Settle at neutral again.
  6. Rotate the forearm, hold ~2 s, return.              (repeat 3 times)

⚠ STEP 6 STARTS FROM PALM DOWN, because that is the calibration pose — pose 0
  is the forearm across the chest, PALM DOWN. The forearm is therefore already
  near full pronation at neutral, so the motion available from there is
  SUPINATION (palm turning up), not more pronation. Label it for what you
  actually did: `--expect ...,supinate`. Getting this backwards inverts the one
  sign it contributes.

⚠ ONE DIRECTION FROM NEUTRAL, THEN BACK. Do NOT sweep bow-through-to-cup. The
  SIGN is the only thing that selects a candidate, and a motion that passes
  through neutral to the other side produces both signs and settles nothing.

⚠ SLOWLY. Slowness matters more than range — a fast motion smears the axis being
  isolated, and cross-talk is how this capture gets rejected.

⚠ SUPPORT THE ELBOW, seated, and do not rest the forearm flat on a desk, which
  pins pronation. Step 6 is the only test of the limb-axis assumption; without
  it that assumption is reported as NO EVIDENCE rather than confirmed.

⚠ RECORD THE MOUNTING — strap position, board orientation, which way the palm
  unit faces — and pass it as --mounting. The constant describes the mounting,
  not the device, and is meaningless without it.

WORTH CAPTURING IN THE SAME SESSION, while the rig is on:
  · a SECOND calibration routine on the same mounting, so the selection can be
    run twice and repeatability measured. If two runs disagree, the constant is
    not constant and baking it in is wrong.
  · ONE hard swing, for the palm-vs-arm acceleration confirmation, which needs
    peaks over a genuine swing and says nothing at rest.

THEN:

    ~/Projects/.venv-hackmotion/bin/python tools/hm_frame_select.py \\
        ~/Desktop/singleaxis.hmwire --expect bow,ulnar,pronate \\
        --mounting "<strap position, board orientation>"

⚠ AND DO NOT CHECK THE ANSWER AGAINST THE VENDOR'S APP — it reports the inverse
  of us on bow/cup, so a CORRECT answer looks wrong there.
================================================================================
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", type=Path, nargs="?")
    ap.add_argument("--protocol", action="store_true",
                    help="print what the capture must contain, and exit")
    ap.add_argument("--expect", default="",
                    help="comma-separated directions in performance order, e.g. bow,ulnar")
    ap.add_argument("--mounting", default="",
                    help="free text describing the strap position this capture used")
    args = ap.parse_args()

    if args.protocol:
        print(PROTOCOL)
        return 0
    if args.capture is None:
        ap.error("a capture is required (or --protocol)")

    if not args.capture.is_file():
        print(f"⛔ no such capture: {args.capture}", file=sys.stderr)
        return 2

    expect = [x.strip().lower() for x in args.expect.split(",") if x.strip()]
    for label in expect:
        if label not in LABELS:
            print(f"⛔ unknown direction {label!r}; known: {', '.join(sorted(LABELS))}",
                  file=sys.stderr)
            return 2

    samples, before, seen_94, flagged = load_samples(args.capture)
    print(f"\ncapture   {args.capture}")
    if args.mounting:
        print(f"mounting  {args.mounting}")
    else:
        print("mounting  ⚠ NOT RECORDED — the constant is meaningless without it")
    print(f"samples   {len(before)} before the calibration result, "
          f"{len(samples)} after")
    if flagged:
        print(f"          {flagged} carry the library's own CALIBRATED flag")

    if not seen_94:
        print("\n⛔ no calibration result (0x94) anywhere in this capture, so nothing\n"
              "   in it is in the device's anatomical frame. The routine has to run\n"
              "   INSIDE the capture — a calibration does not survive a disconnect,\n"
              "   so it cannot be done in an earlier connection. Use --calibrate.")
        return 1

    if not samples:
        print("\n⛔ the calibration result is the last thing in this capture — there\n"
              "   are no samples after it. Keep recording after the routine, and\n"
              "   perform the directed motions then.")
        return 1

    # --- was the transform APPLIED, or merely emitted? -----------------------
    #
    # ⚠ 0x94 IS NOT AN ACCEPTANCE VERDICT. The device emits it for attempts the
    # app rejects too, so its presence says the marker was answered and nothing
    # about whether the result is any good. The relative angle settles it: an
    # uncalibrated straight wrist sits at 11-19° of board placement and an
    # applied calibration collapses it to under about 4°, an order of magnitude
    # apart. ⚠ This tests the ZEROING only — it is structurally blind to whether
    # the axes point anywhere sensible, which is the half the directed motions
    # are for. Never read it as a quality score.
    a_before = still_median_angle(before)
    a_after = still_median_angle(samples)
    print("\ncalibration applied?  (relative angle at the stillest stretch)")
    if a_before is None:
        print("  ---- nothing before the result to compare against")
    else:
        print(f"  before {a_before:6.2f}°")
    print(f"  after  {a_after:6.2f}°")
    if a_after > 10.0:
        print("  ⛔ that has NOT collapsed. The device answered the marker but the\n"
              "     attempt does not look applied — 0x94 arrives for rejected\n"
              "     attempts too. Re-run the routine; do not select on this capture.")
        return 1
    if a_after > 6.0:
        print("  ⚠ between the two populations, which is evidence of NEITHER.\n"
              "    Re-run the routine rather than guessing from here.")
        return 1
    print("  ok — collapsed into the applied population (⚠ this tests the zeroing\n"
          "       only; the axes are what the directed motions below settle)")

    bursts = segment(samples)
    print(f"motions   {len(bursts)} detected\n")
    if not bursts:
        print("⛔ no motion bursts found. The capture may be all hold and no motion.")
        return 1

    # --- the evidence, POOLED PER DOF ----------------------------------------
    #
    # ⚠ POOLED, NOT PER BURST. A capture of three intended motions segments into
    # a dozen or more bursts — out, back, and every pause between — so asking for
    # one label per burst asks the athlete to label something they did not
    # perform. What they DID perform is a DOF and a direction, so that is the
    # unit of evidence here: every burst dominated by one DOF, pooled, with the
    # largest excursion carrying the answer and the rest showing whether it is
    # consistent.
    print("=" * 78)
    print("EVIDENCE — pooled per degree of freedom")
    print("=" * 78)

    pooled = {"flexion": [], "deviation": [], "pronation": []}
    for i, burst in enumerate(bursts):
        exc0 = characterise(burst, CANDIDATES[0][1])
        prim, cross = dominant(exc0, burst)
        pooled[prim].append((i, burst, cross))

    for dof in ("flexion", "deviation", "pronation"):
        entries = pooled[dof]
        unit = "°/s" if dof == "pronation" else "°"
        if not entries:
            print(f"\n{dof}: no motion read as this DOF")
            continue
        # The largest excursion is the least ambiguous; report the spread too.
        vals0 = [(characterise(b, CANDIDATES[0][1])[dof], i) for i, b, _ in entries]
        biggest = max(vals0, key=lambda x: abs(x[0]))
        agree = sum(1 for v, _ in vals0 if (v > 0) == (biggest[0] > 0))
        print(f"\n{dof}: {len(entries)} burst(s), largest is motion {biggest[1] + 1}; "
              f"{agree}/{len(vals0)} share its sign")
        for name, R, basis in CANDIDATES:
            vals = [characterise(b, R)[dof] for _, b, _ in entries]
            peak = max(vals, key=abs)
            print(f"   {name}  peak {peak:+8.2f}{unit:<4s}"
                  f"  range [{min(vals):+7.2f}, {max(vals):+7.2f}]")
        if agree < len(vals0) * 0.7:
            print("   ⚠ THE SIGNS DISAGREE ACROSS BURSTS. That means this DOF was moved\n"
                  "     in BOTH directions, so it cannot select — a directed motion has\n"
                  "     to go one way from neutral and return, not sweep through it.")

    # --- FALSIFICATION: do the device's axes carry the DOFs we assume? -------
    #
    # ⚠ Measured from the JOINT RATE, not from decomposed cross-talk. Cross-talk
    # is identical under all four candidates AND is dominated by how cleanly a
    # human performed the motion, so a good capture routinely shows 15-20° of it
    # and an alarm there means nothing. The rate axis is the direct measurement.
    print("\n" + "=" * 78)
    print("FALSIFICATION — are the device's axes where the reduction assumes?")
    print("=" * 78)
    print("  flexion should sit on device X, deviation on device Z\n")
    axis_ok = True
    tested = 0
    for dof, want, other in (("flexion", "from_x", ("from_y", "from_z")),
                             ("deviation", "from_z", ("from_x", "from_y"))):
        for i, burst, _ in pooled[dof]:
            ev = joint_axis_evidence(burst)
            if ev is None or ev["frac"] < 0.7:
                continue
            tested += 1
            near = ev[want]
            far = min(ev[o] for o in other)
            good = near < 35.0 and far > 55.0
            axis_ok = axis_ok and good
            print(f"  [{'ok  ' if good else 'FAIL'}] motion {i + 1:>2} ({dof:<9}) "
                  f"joint axis {near:5.1f}° from its own axis, "
                  f"{far:5.1f}° from the nearest other, {ev['frac'] * 100:3.0f}% single-axis")
    if not tested:
        print("  ---- NO EVIDENCE: no motion carried a clean enough joint rate to test.")
        axis_ok = False
    elif axis_ok:
        print("\n  ok — the axis roles are MEASURED, not assumed. The residuals are what\n"
              "       a human performing a 'pure' single-axis motion actually produces.")
    else:
        print("\n  ⚠ AT LEAST ONE MOTION DOES NOT SIT ON THE AXIS THE REDUCTION NEEDS.\n"
              "    Either it was not the motion it was labelled, or the two frames are\n"
              "    not axis-aligned and the four-candidate reduction does not hold.")

    # ⚠ The other assumption: is the device's LIMB axis its Y?
    #
    # Any burst whose lower-arm unit turned enough tests this — the forearm's own
    # rotation lands on its long axis whatever the wrist was doing, so this does
    # NOT require a burst classified as pronation. Requiring one was too strict
    # and reported NO EVIDENCE on captures that plainly contained the answer.
    print("\n  limb axis — is the device's Y the forearm's long axis?")
    best = None
    for i, burst in enumerate(bursts):
        peak = max(math.sqrt(sum(c * c for c in s["gyro_arm"])) for s in burst)
        if peak < 30.0:
            continue          # too little forearm motion to have an axis at all
        ev = limb_axis_evidence(burst)
        if ev is None:
            continue
        from_y, frac = ev
        if frac < 0.9:
            continue
        if best is None or frac > best[2]:
            best = (i, from_y, frac, peak)
    if best is None:
        print("  ---- NO EVIDENCE. No motion turned the forearm enough, cleanly enough,\n"
              "       to have a measurable axis. Add a deliberate forearm rotation.")
    else:
        i, from_y, frac, peak = best
        good = from_y < 30.0
        print(f"  [{'ok  ' if good else 'FAIL'}] motion {i + 1} — forearm rate axis "
              f"{from_y:.1f}° from Y, {frac * 100:.0f}% single-axis, peak {peak:.0f} °/s")
        if not good:
            print("       ⚠ that is not a limb axis on Y, and the reduction rests on it.")
        axis_ok = axis_ok and good

    # --- the selection, which needs the labels -------------------------------
    print()
    print("=" * 78)
    print("SELECTION")
    print("=" * 78)
    if not expect:
        print("\nNo --expect directions given, so nothing can be selected: all four\n"
              "candidates fit this capture equally well and differ only in sign.\n"
              "Re-run naming the direction you moved each DOF, e.g.\n"
              "    --expect bow,ulnar\n"
              "Run --protocol to see exactly what the capture needs to contain.")
        return 1

    # ⚠ ONE LABEL PER DOF, NOT PER BURST. The athlete performs a DOF and a
    # direction; the segmenter sees that as many bursts (out, back, and the
    # pauses). Labelling bursts would be labelling something nobody performed.
    want_by_dof = {}
    for label in expect:
        dof, want = LABELS[label]
        if dof in want_by_dof and want_by_dof[dof][1] != want:
            print(f"\n⛔ contradictory directions given for {dof}. Name one direction "
                  "per DOF.")
            return 2
        want_by_dof[dof] = (label, want)

    missing = [d for d in want_by_dof if not pooled[d]]
    if missing:
        print(f"\n⛔ no motion in this capture read as {', '.join(missing)}, so the\n"
              "   direction(s) given for it cannot be checked. Either that motion was\n"
              "   not performed, or it was too small to separate from the others.")
        return 1

    survivors = []
    for name, R, basis in CANDIDATES:
        ok = True
        for dof, (label, want) in want_by_dof.items():
            vals = [characterise(b, R)[dof] for _, b, _ in pooled[dof]]
            peak = max(vals, key=abs)
            if peak == 0.0 or (peak > 0) != (want > 0):
                ok = False
        if ok:
            survivors.append((name, basis))

    for name, R, basis in CANDIDATES:
        mark = "  <== SURVIVES" if any(n == name for n, _ in survivors) else ""
        print(f"  {name}  {basis}{mark}")

    print()
    if len(survivors) == 1:
        name, basis = survivors[0]
        idx = [n for n, _, _ in CANDIDATES].index(name)
        print(f"✅ {name} is the only candidate consistent with the directions performed.")
        print(f"   {basis}")
        print(f"\n   Set in src/Core/pp_tuned_constants.h:")
        print(f"       inline constexpr int kCandidate = {idx};   // {name}")
        if args.mounting:
            print(f"   Record the mounting alongside it: {args.mounting}")
        else:
            print("   ⚠ AND RECORD THE MOUNTING — this capture did not state one.")
        return 0

    if not survivors:
        print("⛔ NO candidate is consistent with the directions given. Either a label\n"
              "   is wrong, a motion was mis-detected, or the reduction does not hold.\n"
              "   Check the evidence table above before changing anything.")
        return 1

    print(f"⛔ {len(survivors)} candidates survive. The motions performed do not\n"
          "   separate them — a bow and an ulnar deviation between them do. Add\n"
          "   whichever of the two is missing and re-run.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
