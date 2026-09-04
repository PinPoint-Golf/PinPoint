#!/bin/zsh
# run_pass.sh <out_root> <swinglab_run> — re-analyse the 11-swing 2026-08-18 subset into <out_root>/<session>/<swing>/
BIN=${2:?swinglab_run binary}
OUT=$1; mkdir -p $OUT
for sess in /mnt/swingdata/Mark-Liversedge/2026-08-18_Mark-Liversedge_Wrist_01 /mnt/swingdata/Mark-Liversedge/2026-08-18_Mark-Liversedge_Wrist_02; do
  for sw in $sess/swing_*; do
    o=$OUT/$(basename $sess)/$(basename $sw); mkdir -p $o
    t0=$(date +%s); $BIN $sw --out $o > $o/run.log 2>&1; rc=$?
    echo "$(basename $sess)/$(basename $sw) rc=$rc $(( $(date +%s) - t0 ))s $(ls $o | tr '\n' ' ')"
  done
done
