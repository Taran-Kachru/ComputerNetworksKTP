#!/usr/bin/env bash
cd /work
pkill -9 -x initksocket 2>/dev/null; pkill -9 -x user1 2>/dev/null; pkill -9 -x user2 2>/dev/null
rm -f /dev/shm/ktp_sm data/out_bin.dat; sleep 0.3
KTP_P=0 ./initksocket >/tmp/init.log 2>&1 &
INIT=$!
sleep 0.5
./user2 127.0.0.1 5002 127.0.0.1 5001 data/out_bin.dat >/tmp/u2.log 2>&1 &
U2=$!
sleep 0.3
./user1 127.0.0.1 5001 127.0.0.1 5002 data/input_bin.dat /tmp/stats.txt >/tmp/u1.log 2>&1 &
U1=$!

# watch progress; if output size stops growing, dump state
last=-1; stable=0
for i in $(seq 1 40); do
  sz=$(stat -c%s data/out_bin.dat 2>/dev/null || echo 0)
  echo "t=$i size=$sz u1=$(kill -0 $U1 2>/dev/null && echo up || echo done) u2=$(kill -0 $U2 2>/dev/null && echo up || echo done)"
  if ! kill -0 $U1 2>/dev/null && ! kill -0 $U2 2>/dev/null; then echo "both done"; break; fi
  if [ "$sz" = "$last" ]; then stable=$((stable+1)); else stable=0; fi
  last=$sz
  if [ $stable -ge 4 ]; then
    echo "STALL detected at size=$sz -> dumping init state"
    kill -USR1 $INIT; sleep 0.5
    break
  fi
  sleep 1
done
echo "=== init.log (incl dump) ==="; cat /tmp/init.log
echo "=== u1.log ==="; cat /tmp/u1.log
echo "=== u2.log ==="; cat /tmp/u2.log
pkill -9 -x user1 2>/dev/null; pkill -9 -x user2 2>/dev/null
kill -TERM $INIT 2>/dev/null; sleep 0.5; pkill -9 -x initksocket 2>/dev/null
rm -f /dev/shm/ktp_sm
echo DONE
