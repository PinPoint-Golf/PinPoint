#!/bin/zsh
# sweep_adapt.sh <out_root> <swinglab_run> <settings.jsonl>
#
# Phase 5 (motion-adaptive smoother window), contract C15 of
# docs/implementation/metric_presentation_honesty_phase5_contracts.md: re-analyse the 11-swing
# 2026-08-18 subset once per SETTING and write one series_noise CSV per setting, so
# sweep_summary.py --gate can score every setting against the control on the four numeric criteria
# (phase-sample movement, P1-P7 excursion, still-address jitter, sigma).
#
# The settings file is JSON Lines: one JSON object of dotted swinglab --params keys per line, e.g.
#   {"name":"accel_ms05_a20k","poseSmooth.adapt.mode":"accel","poseSmooth.adapt.minScale":0.05}
# The "name" key is NOT a tuning key - it names the output subdirectory and is stripped before the
# params file is written. {"name":"control"} therefore runs production defaults, which is exactly
# what the gate needs as its reference. Blank lines and lines starting with # are ignored, so the
# grid can carry comments about where its numbers came from.
#
# Resumable: a swing whose result.json already exists is skipped, so a run interrupted after four
# hours of the eleven-swing x N-setting matrix resumes instead of restarting. Delete the setting
# directory (or just its result.json files) to force a re-run.
#
# Nothing here judges anything. Read the verdict with:
#   sweep_summary.py --gate <out_root>/control <out_root>
set -u
OUT=${1:?out_root}
BIN=${2:?swinglab_run binary}
SETTINGS=${3:?settings jsonl}
HERE=$(cd "$(dirname "$0")" && pwd)

[[ -x $BIN ]]      || { echo "sweep_adapt: not executable: $BIN" >&2; exit 2 }
[[ -r $SETTINGS ]] || { echo "sweep_adapt: unreadable: $SETTINGS" >&2; exit 2 }

SESSIONS=(
  /mnt/swingdata/Mark-Liversedge/2026-08-18_Mark-Liversedge_Wrist_01
  /mnt/swingdata/Mark-Liversedge/2026-08-18_Mark-Liversedge_Wrist_02
)

mkdir -p $OUT

# Parse the settings file with python3 (zsh has no JSON reader): validate every line up front so a
# typo in setting 9 is reported before setting 1 spends 20 minutes, then materialise
# <out_root>/<name>/params.json (the "name" key stripped) and echo the names for the loop below.
NAMES=$(python3 - "$SETTINGS" "$OUT" <<'PY'
import json, os, re, sys
src, out = sys.argv[1], sys.argv[2]
names, errs = [], []
for ln, raw in enumerate(open(src, encoding="utf-8"), 1):
    s = raw.strip()
    if not s or s.startswith("#"):
        continue
    try:
        obj = json.loads(s)
    except Exception as exc:
        errs.append(f"line {ln}: not JSON: {exc}"); continue
    if not isinstance(obj, dict):
        errs.append(f"line {ln}: not a JSON object"); continue
    name = obj.pop("name", None)
    if not isinstance(name, str) or not name:
        errs.append(f'line {ln}: missing/blank "name"'); continue
    if not re.fullmatch(r"[A-Za-z0-9._-]+", name):
        errs.append(f"line {ln}: unusable directory name {name!r}"); continue
    if name in names:
        errs.append(f"line {ln}: duplicate name {name!r}"); continue
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "params.json"), "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=1, sort_keys=True)   # {} for the control: production defaults
        fh.write("\n")
    names.append(name)
if errs:
    for e in errs:
        print("sweep_adapt: " + e, file=sys.stderr)
    sys.exit(2)
if not names:
    print("sweep_adapt: settings file has no settings", file=sys.stderr)
    sys.exit(2)
print("\n".join(names))
PY
) || exit 2

echo "sweep_adapt: $(echo $NAMES | wc -l | tr -d ' ') settings -> $OUT"

for name in ${(f)NAMES}; do
  root=$OUT/$name
  echo "== $name ($(tr -d '\n ' < $root/params.json))"
  for sess in $SESSIONS; do
    for sw in $sess/swing_*; do
      o=$root/$(basename $sess)/$(basename $sw); mkdir -p $o
      if [[ -s $o/result.json ]]; then
        echo "   skip $(basename $sess)/$(basename $sw) (result.json present)"
        continue
      fi
      t0=$(date +%s)
      $BIN $sw --out $o --params $root/params.json > $o/run.log 2>&1; rc=$?
      if [[ $rc -ne 0 || ! -s $o/result.json ]]; then
        echo "   FAIL $name $(basename $sess)/$(basename $sw) rc=$rc (see $o/run.log)"
      else
        echo "   ok   $(basename $sess)/$(basename $sw) $(( $(date +%s) - t0 ))s"
      fi
    done
  done
  python3 $HERE/series_noise.py $root --file result.json --out $root/noise.csv \
      2> $root/noise_summary.txt \
    || echo "   series_noise FAILED for $name (see $root/noise_summary.txt)"
  rows=0; [[ -s $root/noise.csv ]] && rows=$(( $(grep -c . $root/noise.csv) - 1 ))
  echo "   $name: $rows noise rows, $(find $root -name result.json | wc -l | tr -d ' ') results"
done

echo "sweep_adapt: done. Score it with:"
echo "  python3 $HERE/sweep_summary.py --gate $OUT/control $OUT"
