#!/usr/bin/env bash
# experiment.sh [infile] [outcsv]
# Runs the transfer for p = 0.05, 0.10, ... 0.50 with T=5 and records the
# average number of transmissions per message.  Verifies file integrity each run.
cd /work
INF=${1:-data/exp_input.dat}
OUT=${2:-tests/results.csv}
# >100KB test file (kept fixed across all p so the comparison is fair).
[ -f "$INF" ] || head -c 112640 /dev/urandom > "$INF"   # 220 msgs (110 KB)

echo "p,avg_tx_per_msg,tx_total,unique,integrity" > "$OUT"
for p in 0.05 0.10 0.15 0.20 0.25 0.30 0.35 0.40 0.45 0.50; do
  of="data/exp_out_$p.dat"
  # T=5 (submitted default: no KTP_T override here); generous per-run timeout.
  bash tests/run_pair.sh "$p" "$INF" "$of" 0 0 0 900 > /tmp/exp_$p.log 2>&1
  stats=$(grep -o 'stats=[0-9 .]*' /tmp/exp_$p.log | head -1 | sed 's/stats=//')
  tx=$(echo "$stats" | awk '{print $1}')
  uniq=$(echo "$stats" | awk '{print $2}')
  avg=$(echo "$stats" | awk '{print $3}')
  if grep -q 'RESULT: IDENTICAL' /tmp/exp_$p.log; then integ=OK; else integ=FAIL; fi
  echo "$p,$avg,$tx,$uniq,$integ" | tee -a "$OUT"
  rm -f "$of"
done
echo "experiment complete -> $OUT"
