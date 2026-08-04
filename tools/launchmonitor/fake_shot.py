#!/usr/bin/env python3
"""Write a plausible LastShot.CSV, as FSX2020 would, to exercise the launch monitor connector.

Testing the connector by hand is otherwise awkward: `touch` does nothing, because the
connector treats identical bytes as the same shot — correctly, since nothing about the
file has changed. What it needs is a genuinely different row, which is what this writes.

    # one shot into the folder the connector is watching
    python3 tools/launchmonitor/fake_shot.py ~/Documents/FSX2020

    # a session: six shots, four seconds apart
    python3 tools/launchmonitor/fake_shot.py ~/Documents/FSX2020 --shots 6 --interval 4

    # a specific club, and a shot that curves hard right
    python3 tools/launchmonitor/fake_shot.py ~/Documents/FSX2020 --club Drv --shape slice

The header is copied VERBATIM from the exemplar and only the data row is rewritten. That
matters: the parser is header-driven — it matches columns by name and converts on the unit
each header declares — so reusing the real header is what makes this a test of the
connector rather than a test of this script's idea of the format.

Values are jittered around the exemplar and kept INTERNALLY CONSISTENT, because the
connector derives from them: face-to-path is face minus path, total spin is the resultant
of back and side, ball speed follows club speed through a plausible smash factor. A row
whose numbers contradict each other would exercise arithmetic no real device produces.
"""

import argparse
import math
import pathlib
import random
import re
import sys
import time

DEFAULT_EXEMPLAR = "/mnt/swingdata/LastShot.CSV"

# Used when the exemplar is not reachable — the real captured row, byte for byte.
FALLBACK = (
    "Shot ID, Club, Club head Speed (m/s), Ball Speed (m/s), Launch Angle (deg), "
    "Azimuth (deg), Side Spin (rpm), Back Spin (rpm), Total Spin (rpm), "
    "Descent Angle (deg), Carry (m), Total Distance (m), Offline (m), "
    "Peak Height (m), Distance to Pin (m), Vert Path (deg), Horiz Path (deg), "
    "Face to Path (deg), Face to Target (deg), Lie (deg), Loft (deg), "
    "Closure Rate (deg/s), Horiz Impact (mm), Vert Impact (mm), \r\n"
    "283, Irn, 38.978607, 49.923466, 19.695501, 4.982033, 1635, 7437, 7614, "
    "47.953861, 137.551910, 145.092789, 29.383856, 28.229187, 37.012428, "
    "-6.255224, 1.319672, 5.622859, 6.942531, -1.328706, 29.739687, "
    "3232.705811, 2.136441, -17.925056, \r\n"
)

# Rough per-club centres, metric, as the file states them. Only what a club actually
# changes — everything else is jittered around the exemplar.
CLUBS = {
    "Drv": dict(club_ms=48.0, smash=1.48, launch=13.0, backspin=2600, loft=14.0, vert=2.0),
    "Wd3": dict(club_ms=44.0, smash=1.46, launch=12.0, backspin=3400, loft=16.0, vert=-1.0),
    "Hyb": dict(club_ms=41.0, smash=1.44, launch=14.0, backspin=4200, loft=19.0, vert=-2.5),
    "Irn": dict(club_ms=39.0, smash=1.38, launch=19.5, backspin=7400, loft=29.5, vert=-6.0),
    "Wdg": dict(club_ms=31.0, smash=1.20, launch=29.0, backspin=9500, loft=52.0, vert=-8.0),
}

# Shot shapes, as (start-direction bias, face-to-path bias) in degrees. Positive is right
# of the target line for a right-handed golfer — the convention Foresight publishes and
# the one the connector reads without negation.
SHAPES = {
    "straight": (0.0, 0.0),
    "draw":     (2.0, -3.0),
    "fade":     (-2.0, 3.0),
    "hook":     (3.0, -8.0),
    "slice":    (-3.0, 9.0),
    "pull":     (-5.0, 0.0),
    "push":     (5.0, 0.0),
}


def normalise(cell):
    """'Club head Speed (m/s)' -> 'clubheadspeed'. Same rule the C++ parser uses."""
    name = cell.split("(")[0]
    return "".join(c.lower() for c in name if c.isalnum())


def split_csv(line):
    return [c.strip() for c in line.split(",")]


def read_exemplar(path):
    try:
        return pathlib.Path(path).read_text(encoding="utf-8-sig")
    except OSError:
        return None


def jitter(value, frac, rng, lo=None, hi=None):
    out = value * (1.0 + rng.uniform(-frac, frac))
    if lo is not None:
        out = max(lo, out)
    if hi is not None:
        out = min(hi, out)
    return out


def build_row(header_cells, template_cells, shot_id, club, shape, rng):
    """One data row: the template, with a new id and coherent jittered numbers."""
    idx = {normalise(h): i for i, h in enumerate(header_cells) if normalise(h)}
    row = list(template_cells)

    def put(token, value, decimals=6):
        i = idx.get(token)
        if i is not None and i < len(row):
            row[i] = ("%d" % round(value)) if decimals == 0 else ("%.*f" % (decimals, value))

    spec = CLUBS[club]
    start_bias, ftp_bias = SHAPES[shape]

    # ── Club delivery ───────────────────────────────────────────────────────
    club_ms = jitter(spec["club_ms"], 0.04, rng, lo=5.0)
    smash = jitter(spec["smash"], 0.02, rng, lo=0.9, hi=1.52)
    ball_ms = club_ms * smash

    vert_path = spec["vert"] + rng.uniform(-1.5, 1.5)          # attack angle
    horiz_path = start_bias * 0.6 + rng.uniform(-1.5, 1.5)     # club path
    face_to_path = ftp_bias + rng.uniform(-1.0, 1.0)
    # THE ONE IDENTITY THE DEVICE'S OWN NUMBERS SATISFY: face to target is face to path
    # plus club path. The connector does not check it, but a row that broke it would be
    # unlike anything a real GC Quad writes.
    face_to_target = face_to_path + horiz_path

    loft = jitter(spec["loft"], 0.06, rng, lo=5.0)
    lie = rng.uniform(-3.0, 3.0)
    closure = jitter(3200.0, 0.25, rng, lo=200.0)

    # ── Ball ────────────────────────────────────────────────────────────────
    launch = jitter(spec["launch"], 0.10, rng, lo=1.0)
    azimuth = start_bias + rng.uniform(-1.2, 1.2)
    back_spin = jitter(spec["backspin"], 0.10, rng, lo=300.0)
    # Side spin follows the curvature the face-to-path implies, so the shape asked for is
    # the shape the numbers describe.
    side_spin = face_to_path * 180.0 + rng.uniform(-150.0, 150.0)
    total_spin = math.hypot(back_spin, side_spin)

    # A flight model this is not — it only has to be plausible and to move with the
    # inputs, so a reader comparing two generated shots sees a sensible relationship.
    carry_m = max(5.0, ball_ms * 3.05 * (1.0 - abs(launch - 16.0) / 90.0))
    total_m = carry_m * rng.uniform(1.03, 1.10)
    offline_m = carry_m * math.tan(math.radians(azimuth + face_to_path * 0.8)) * 0.55
    peak_m = max(1.0, carry_m * (0.10 + launch / 220.0))
    descent = min(80.0, 12.0 + launch * 1.6 + back_spin / 900.0)
    to_pin_m = abs(rng.uniform(120.0, 160.0) - carry_m) + abs(offline_m) * 0.5

    # ── Strike ──────────────────────────────────────────────────────────────
    horiz_impact = rng.uniform(-14.0, 14.0)
    vert_impact = rng.uniform(-20.0, 12.0)

    i = idx.get("shotid")
    if i is not None:
        row[i] = str(shot_id)
    i = idx.get("club")
    if i is not None:
        row[i] = club

    put("clubheadspeed", club_ms)
    put("ballspeed", ball_ms)
    put("launchangle", launch)
    put("azimuth", azimuth)
    put("sidespin", side_spin, 0)
    put("backspin", back_spin, 0)
    put("totalspin", total_spin, 0)
    put("descentangle", descent)
    put("carry", carry_m)
    put("totaldistance", total_m)
    put("offline", offline_m)
    put("peakheight", peak_m)
    put("distancetopin", to_pin_m)
    put("vertpath", vert_path)
    put("horizpath", horiz_path)
    put("facetopath", face_to_path)
    put("facetotarget", face_to_target)
    put("lie", lie)
    put("loft", loft)
    put("closurerate", closure)
    put("horizimpact", horiz_impact)
    put("vertimpact", vert_impact)
    return row


def next_shot_id(target, header_cells, fallback_id):
    """Continue the counter already in the target file, so ids climb across runs."""
    text = read_exemplar(target)
    if not text:
        return fallback_id + 1
    lines = [l for l in re.split(r"\r\n|\r|\n", text) if l.strip()]
    if len(lines) < 2:
        return fallback_id + 1
    idx = {normalise(h): i for i, h in enumerate(split_csv(lines[0]))}
    i = idx.get("shotid")
    cells = split_csv(lines[1])
    if i is None or i >= len(cells):
        return fallback_id + 1
    try:
        return int(cells[i]) + 1
    except ValueError:
        return fallback_id + 1


def main():
    ap = argparse.ArgumentParser(
        description="Write a plausible LastShot.CSV to exercise the launch monitor connector.")
    ap.add_argument("target", help="the folder the connector watches, or a file path")
    ap.add_argument("--shots", type=int, default=1, help="how many shots to write (default 1)")
    ap.add_argument("--interval", type=float, default=3.0,
                    help="seconds between shots (default 3)")
    ap.add_argument("--club", choices=sorted(CLUBS), default="Irn")
    ap.add_argument("--shape", choices=sorted(SHAPES), default="straight")
    ap.add_argument("--seed", type=int, default=None, help="repeatable output")
    ap.add_argument("--exemplar", default=DEFAULT_EXEMPLAR,
                    help="file to copy the header from (default %s)" % DEFAULT_EXEMPLAR)
    args = ap.parse_args()

    rng = random.Random(args.seed)

    text = read_exemplar(args.exemplar)
    if text is None:
        print("note: %s not readable — using the built-in copy of it" % args.exemplar,
              file=sys.stderr)
        text = FALLBACK
    lines = [l for l in re.split(r"\r\n|\r|\n", text) if l.strip()]
    if len(lines) < 2:
        sys.exit("exemplar %s has no header and row" % args.exemplar)

    header_line = lines[0]
    header_cells = split_csv(header_line)
    template_cells = split_csv(lines[1])
    if "shotid" not in {normalise(h) for h in header_cells}:
        sys.exit("exemplar %s has no 'Shot ID' column" % args.exemplar)

    target = pathlib.Path(args.target).expanduser()
    if target.is_dir() or not target.suffix:
        target.mkdir(parents=True, exist_ok=True)
        # The connector matches the name case-insensitively, so either spelling is found.
        existing = [p for p in target.iterdir()
                    if p.is_file() and p.name.lower() == "lastshot.csv"]
        target = existing[0] if existing else target / "LastShot.CSV"
    else:
        target.parent.mkdir(parents=True, exist_ok=True)

    try:
        seed_id = int(split_csv(lines[1])[
            {normalise(h): i for i, h in enumerate(header_cells)}["shotid"]])
    except (ValueError, KeyError, IndexError):
        seed_id = 0
    shot_id = next_shot_id(str(target), header_cells, seed_id)

    for n in range(args.shots):
        row = build_row(header_cells, template_cells, shot_id, args.club, args.shape, rng)
        # IN PLACE, as FSX2020 does — truncate and rewrite the same inode rather than
        # writing a temp file and renaming. A rename would look like delete-then-create,
        # which is the very thing the connector must not depend on noticing.
        body = header_line + "\r\n" + ", ".join(row) + "\r\n"
        with open(target, "w", encoding="utf-8", newline="") as f:
            f.write(body)

        speed_i = {normalise(h): i for i, h in enumerate(header_cells)}.get("clubheadspeed")
        speed = row[speed_i] if speed_i is not None else "?"
        print("shot %s  %s  %s  club %s m/s  ->  %s"
              % (shot_id, args.club, args.shape, speed, target))

        shot_id += 1
        if n + 1 < args.shots:
            time.sleep(args.interval)


if __name__ == "__main__":
    main()
