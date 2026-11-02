#!/usr/bin/env bash
# run_pair.sh <p> <infile> <outfile> [slow_read_ms] [drop_reopen] [disable_persist] [timeout_s]
# Starts init with drop prob <p>, runs one user1/user2 pair, compares files.
# Exits 0 if identical, 1 otherwise. Prints stats line.
cd "$(dirname "$0")/.." || exit 1
P=${1:-0}
INF=${2:-data/input_bin.dat}
OUTF=${3:-data/out_bin.dat}
SLOW=${4:-0}
DROPRE=${5:-0}
NOPERSIST=${6:-0}
TMO=${7:-120}

pkill -9 -x initksocket 2>/dev/null; pkill -9 -x user1 2>/dev/null; pkill -9 -x user2 2>/dev/null
rm -f /dev/shm/ktp_sm "$OUTF"; sleep 0.3

ENV_INIT="KTP_P=$P KTP_DROP_REOPEN=$DROPRE"
[ -n "$KTP_T" ] && ENV_INIT="$ENV_INIT KTP_T=$KTP_T"
[ "$NOPERSIST" = "1" ] && ENV_INIT="$ENV_INIT KTP_DISABLE_PERSIST=1"
env $ENV_INIT ./initksocket >/tmp/init.log 2>&1 &
INIT=$!
sleep 0.5

KTP_SLOW_READ_MS=$SLOW ./user2 127.0.0.1 5002 127.0.0.1 5001 "$OUTF" >/tmp/u2.log 2>&1 &
U2=$!
sleep 0.3
timeout "$TMO" ./user1 127.0.0.1 5001 127.0.0.1 5002 "$INF" /tmp/stats.txt >/tmp/u1.log 2>&1
U1RC=$?

# wait for receiver to finish (bounded)
for i in $(seq 1 $((TMO*2))); do
  if ! kill -0 "$U2" 2>/dev/null; then break; fi
  sleep 0.5
done
kill -0 "$U2" 2>/dev/null && { echo "user2 did not finish (timeout)"; kill -9 "$U2" 2>/dev/null; }

kill -TERM "$INIT" 2>/dev/null; sleep 1; pkill -9 -x initksocket 2>/dev/null
rm -f /dev/shm/ktp_sm

echo "user1 rc=$U1RC"
cat /tmp/u1.log; cat /tmp/u2.log
if cmp -s "$INF" "$OUTF"; then
  echo "RESULT: IDENTICAL ($(stat -c%s "$OUTF") bytes)  stats=$(cat /tmp/stats.txt 2>/dev/null)"
  exit 0
else
  echo "RESULT: MISMATCH  insize=$(stat -c%s "$INF" 2>/dev/null) outsize=$(stat -c%s "$OUTF" 2>/dev/null)"
  exit 1
fi
