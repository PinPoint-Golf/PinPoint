#!/bin/zsh
# sweep_legs_scale.sh <out_root> <swinglab_run> [scales...]
# Phase 4.2 of docs/implementation/metric_presentation_honesty_impl_plan.md: re-analyse the
# 11-swing 2026-08-18 subset once per poseSmooth.legsJerkScale value (dark key, default 1.0) and
# write one series_noise CSV per scale, so the residual jitter, the P1->P4 sway excursion and the
# P4/P7 phase samples can be compared across scales against the 1.0 control.
OUT=$1; BIN=${2:?swinglab_run binary}; shift 2
SCALES=(${@:-1.0 0.3 0.1 0.05 0.02})
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $OUT
for sc in $SCALES; do
  root=$OUT/scale_$sc; mkdir -p $root
  echo "{\"poseSmooth.legsJerkScale\": $sc}" > $root/params.json
  for sess in /mnt/swingdata/Mark-Liversedge/2026-08-18_Mark-Liversedge_Wrist_01 /mnt/swingdata/Mark-Liversedge/2026-08-18_Mark-Liversedge_Wrist_02; do
    for sw in $sess/swing_*; do
      o=$root/$(basename $sess)/$(basename $sw); mkdir -p $o
      $BIN $sw --out $o --params $root/params.json > $o/run.log 2>&1 || echo "FAIL $sc $(basename $sess)/$(basename $sw)"
    done
  done
  python3 $HERE/series_noise.py $root --file result.json --out $root/noise.csv 2> $root/noise_summary.txt
  echo "scale $sc done: $(grep -c . $root/noise.csv) rows"
done
