#!/usr/bin/env bash
# 3 concurrent user1/user2 pairs on different ports, one init, one shared memory.
cd /work
export KTP_T=1
P=${1:-0.1}
pkill -9 -x initksocket 2>/dev/null; pkill -9 -x user1 2>/dev/null; pkill -9 -x user2 2>/dev/null
rm -f /dev/shm/ktp_sm; sleep 0.3

# three distinct input files (>100KB each would be slow under drops; use ~60KB)
for i in 1 2 3; do head -c $((61440 + i*2048)) /dev/urandom > data/cc_in$i.dat; rm -f data/cc_out$i.dat; done

KTP_P=$P KTP_T=1 ./initksocket >/tmp/init.log 2>&1 &
INIT=$!
sleep 0.5

# pair i uses local ports (6000+2i-1)/(6000+2i)
declare -a U2 U1
for i in 1 2 3; do
  lp=$((6000 + 2*i - 1)); rp=$((6000 + 2*i))
  ./user2 127.0.0.1 $rp 127.0.0.1 $lp data/cc_out$i.dat >/tmp/cc_u2_$i.log 2>&1 &
  U2[$i]=$!
done
sleep 0.3
for i in 1 2 3; do
  lp=$((6000 + 2*i - 1)); rp=$((6000 + 2*i))
  ./user1 127.0.0.1 $lp 127.0.0.1 $rp data/cc_in$i.dat /tmp/cc_stats_$i.txt >/tmp/cc_u1_$i.log 2>&1 &
  U1[$i]=$!
done

# wait for all user procs (bounded)
for t in $(seq 1 240); do
  busy=0
  for i in 1 2 3; do kill -0 ${U1[$i]} 2>/dev/null && busy=1; kill -0 ${U2[$i]} 2>/dev/null && busy=1; done
  [ $busy -eq 0 ] && break
  sleep 0.5
done
pkill -9 -x user1 2>/dev/null; pkill -9 -x user2 2>/dev/null
kill -TERM $INIT 2>/dev/null; sleep 0.5; pkill -9 -x initksocket 2>/dev/null
rm -f /dev/shm/ktp_sm

ok=0
for i in 1 2 3; do
  if cmp -s data/cc_in$i.dat data/cc_out$i.dat; then
    echo "pair $i: IDENTICAL ($(stat -c%s data/cc_out$i.dat) bytes) stats=$(cat /tmp/cc_stats_$i.txt 2>/dev/null)"
    ok=$((ok+1))
  else
    echo "pair $i: MISMATCH in=$(stat -c%s data/cc_in$i.dat) out=$(stat -c%s data/cc_out$i.dat 2>/dev/null)"
  fi
done
echo "RESULT: $ok/3 pairs identical (p=$P)"
rm -f data/cc_in*.dat data/cc_out*.dat
