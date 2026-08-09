#!/usr/bin/env python3
"""Re-analyse every swing under a corpus root, in place.

Walks CORPUS_ROOT for dirs containing swing.json and runs
`swinglab_run <dir> --write-back` on each — the headless twin of the in-app
ReanalysisController's "re-analyse all shown" (production defaults, analysis
block replaced, capture/streams/review preserved). Sequential on purpose:
each swing's ViTPose pass runs at physical-core thread count.

BACK UP the swing.json set before a sweep — write-back rewrites the source
documents (`tar czf backup.tgz <root>/*/swing_*/swing.json`).

Usage: reanalyze_corpus.py CORPUS_ROOT [--bin SWINGLAB_RUN] [--only SUBSTR]

Binary resolution matches swinglab.core: --bin, else $SWINGLAB_BIN, else the
default dev-tree build. A swing that fails (LM-only, no media, no impact) is
reported and skipped — the sweep continues.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--bin", default=None, help="swinglab_run binary")
    ap.add_argument("--only", default=None,
                    help="only swing dirs whose path contains this substring")
    a = ap.parse_args()

    exe = a.bin or os.environ.get("SWINGLAB_BIN")
    if not exe or not Path(exe).exists():
        print(f"reanalyze_corpus: no swinglab_run binary (--bin / $SWINGLAB_BIN)", file=sys.stderr)
        return 2

    root = Path(a.corpus)
    dirs = sorted(sj.parent for sj in root.rglob("swing.json"))
    if a.only:
        dirs = [d for d in dirs if a.only in str(d)]
    if not dirs:
        print(f"reanalyze_corpus: no swing.json under {root}", file=sys.stderr)
        return 2

    ok = 0
    failed = []
    t0 = time.time()
    for d in dirs:
        r = subprocess.run([exe, str(d), "--write-back"],
                           capture_output=True, text=True)
        tail = (r.stderr or r.stdout).strip().splitlines()
        tail = tail[-1] if tail else ""
        name = f"{d.parent.name}/{d.name}"
        if r.returncode == 0:
            ok += 1
            print(f"[ok  ] {name}  {tail}")
        else:
            failed.append(name)
            print(f"[FAIL] {name}  {tail}")
    print(f"done: {ok} re-analysed, {len(failed)} failed/skipped, "
          f"{int(time.time() - t0)}s")
    for n in failed:
        print(f"  failed: {n}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
