#!/usr/bin/env python3
# SwingLab CLI (docs/implementation/swinglab_impl.md).
#
#   lab.py synth   <out_dir> [--clutter] [--seed N]   ground-truthed synthetic swing
#   lab.py ingest  <corpus_root>                      build corpus.json
#   lab.py run     <corpus_root> <runs_root> [--params f] [--id NAME] [--no-trace]
#   lab.py one     <swing_dir>  <run_dir>   [--params f] [--no-trace]  (run+score+plot)
#   lab.py score   <run_dir>    <swing_dir>            scorecard for one run
#   lab.py plot    <run_dir>    <swing_dir>            contact sheet PNG
#   lab.py report  <run_root>                          regenerate REPORT.md
#   lab.py diff    <run_a> <run_b>                     regression diff
#   lab.py coverage <run_root> [--pack core.json] [--out dir]  diagnostics-pack coverage (COVERAGE.md)
#   lab.py sweep   <corpus_root> <runs_root> <space.json> [--trials N]
#   lab.py label   <swing_dir> [--every N]             hand-label truth.json
#
# Run with the SwingLab venv:  ~/.swinglab-venv/bin/python lab.py …
# Binary override: SWINGLAB_BIN=/path/to/swinglab_run

import argparse
import json
import sys
from pathlib import Path

# Windows consoles default to cp1252 — reconfigure so unicode in reports
# (em-dashes, arrows) never crashes the tooling.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

sys.path.insert(0, str(Path(__file__).parent))

from swinglab import save_json                              # noqa: E402
from swinglab.core import diff, ingest, report, run_corpus, run_one, sweep  # noqa: E402
from swinglab.score import scorecard                        # noqa: E402


def main():
    ap = argparse.ArgumentParser(prog="lab.py")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("synth")
    p.add_argument("out_dir")
    p.add_argument("--clutter", action="store_true")
    p.add_argument("--seed", type=int, default=7)
    p.add_argument("--impact-spike", action="store_true",
                   help="add a ±16g saturation+ringing burst at impact (exercises blanking/saturation, D4)")
    p.add_argument("--archetype", choices=["bowed", "neutral", "cupped"], default=None,
                   help="script a lead-wrist archetype (forearm FE bias) + stamp truth.meta.archetype")
    p.add_argument("--fault", default=None,
                   help="known-groups fault label stamped in truth.meta.knownGroup "
                        "(default: 'cast' for a plain synth, none for an --archetype variant)")
    p.add_argument("--no-imu", action="store_true",
                   help="camera-only fixture: omit IMU streams + bindings (the IMU-independent "
                        "shaft-track loop — every IMU/wrist check then skips)")

    sub.add_parser("doctor")

    p = sub.add_parser("ingest")
    p.add_argument("corpus_root")

    p = sub.add_parser("run")
    p.add_argument("corpus_root")
    p.add_argument("runs_root")
    p.add_argument("--params")
    p.add_argument("--id")
    p.add_argument("--no-trace", action="store_true")
    p.add_argument("--session-type", type=int, default=None,
                   help="force the analyzer session type (1=Wrist, the only analyzer with a shaft "
                        "tracker); omit to respect each swing's recorded type (runner defaults to 1)")

    p = sub.add_parser("one")
    p.add_argument("swing_dir")
    p.add_argument("run_dir")
    p.add_argument("--params")
    p.add_argument("--no-trace", action="store_true")
    p.add_argument("--session-type", type=int, default=None,
                   help="force the analyzer session type (1=Wrist); omit to respect the recording")

    p = sub.add_parser("score")
    p.add_argument("run_dir")
    p.add_argument("swing_dir")

    p = sub.add_parser("plot")
    p.add_argument("run_dir")
    p.add_argument("swing_dir")

    p = sub.add_parser("report")
    p.add_argument("run_root")

    p = sub.add_parser("diff")
    p.add_argument("run_a")
    p.add_argument("run_b")

    p = sub.add_parser("coverage")
    p.add_argument("run_root")
    p.add_argument("--pack", default=None,
                   help="diagnostics pack json (default: src/Resources/diagnostics/core.json)")
    p.add_argument("--out", default=None,
                   help="dir to write COVERAGE.md into (default: run_root; use when run_root is "
                        "read-only)")

    p = sub.add_parser("sweep")
    p.add_argument("corpus_root")
    p.add_argument("runs_root")
    p.add_argument("space")
    p.add_argument("--trials", type=int, default=20)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--method", choices=["random", "coordinate"], default="random",
                   help="search strategy (coordinate descent is the §7.1 default for separable knobs)")
    p.add_argument("--baseline", default=None,
                   help="a prior run dir; trials that regress any swing vs it (per-swing 5pt) are rejected")
    p.add_argument("--partition", default=None,
                   help="partitions.json {tune:[],validation:[],heldout:[]}: sweep Tune, select Validation")
    p.add_argument("--freeze", action="store_true",
                   help="permit running the held-out set (the one-time freeze evaluation)")
    p.add_argument("--allow-frozen", action="store_true",
                   help="permit sweeping score.*/rules.*/bands.* (frozen until labels — post-label pass only)")

    p = sub.add_parser("label")
    p.add_argument("swing_dir")
    p.add_argument("--every", type=int, default=20)

    a = ap.parse_args()

    if a.cmd == "doctor":
        from swinglab.core import doctor
        sys.exit(0 if doctor() else 1)
    elif a.cmd == "synth":
        from swinglab.synth import generate
        # Default fault label: 'cast' for a plain synth (the geometry casts), none for an archetype
        # variant (the FE bias changes the wrist read, so don't assert an unrelated fault).
        fault = a.fault if a.fault is not None else ("" if (a.archetype or a.no_imu) else "cast")
        generate(a.out_dir, seed=a.seed, clutter=a.clutter,
                 impact_spike=a.impact_spike, archetype=a.archetype, fault=fault,
                 no_imu=a.no_imu)
    elif a.cmd == "ingest":
        ingest(a.corpus_root)
    elif a.cmd == "run":
        run_corpus(a.corpus_root, a.runs_root, run_id=a.id, params=a.params,
                   trace=not a.no_trace, session_type=a.session_type)
    elif a.cmd == "one":
        ok, rd = run_one(a.swing_dir, a.run_dir, params=a.params,
                         trace=not a.no_trace, session_type=a.session_type)
        card = scorecard(rd, a.swing_dir)
        save_json(Path(rd) / "scorecard.json", card)
        print(json.dumps({k: card[k] for k in
                          ("swing", "score", "ok", "failures", "warnings")}, indent=1))
        from swinglab.plots import contact_sheet
        png = contact_sheet(rd, a.swing_dir, Path(rd) / "contact_sheet.png")
        print(f"[plot] {png}")
        sys.exit(0 if ok else 2)
    elif a.cmd == "score":
        card = scorecard(a.run_dir, a.swing_dir)
        save_json(Path(a.run_dir) / "scorecard.json", card)
        print(json.dumps(card, indent=1))
    elif a.cmd == "plot":
        from swinglab.plots import contact_sheet
        print(contact_sheet(a.run_dir, a.swing_dir,
                            Path(a.run_dir) / "contact_sheet.png"))
    elif a.cmd == "report":
        report(a.run_root)
    elif a.cmd == "diff":
        sys.exit(1 if diff(a.run_a, a.run_b) else 0)
    elif a.cmd == "coverage":
        from swinglab.coverage import coverage
        sys.exit(0 if coverage(a.run_root, pack=a.pack, out=a.out) else 1)
    elif a.cmd == "sweep":
        sweep(a.corpus_root, a.runs_root, a.space, trials=a.trials, seed=a.seed,
              baseline=a.baseline, partition=a.partition, method=a.method, freeze=a.freeze,
              allow_frozen=a.allow_frozen)
    elif a.cmd == "label":
        from swinglab.label import label
        label(a.swing_dir, every_n=a.every)


if __name__ == "__main__":
    main()
