#!/usr/bin/env bash
cd /work
pkill -9 -x initksocket 2>/dev/null; pkill -9 -x user1 2>/dev/null; pkill -9 -x user2 2>/dev/null
rm -f /dev/shm/ktp_sm data/out_s.dat; sleep 0.3
head -c 20480 /dev/urandom > data/in_s.dat    # 40 messages
KTP_DEBUG=1 KTP_P=0 ./initksocket >/tmp/init.log 2>&1 &
INIT=$!
sleep 0.5
./user2 127.0.0.1 5002 127.0.0.1 5001 data/out_s.dat >/tmp/u2.log 2>&1 &
U2=$!
sleep 0.3
timeout 30 ./user1 127.0.0.1 5001 127.0.0.1 5002 data/in_s.dat /tmp/stats.txt >/tmp/u1.log 2>&1
echo "u1 rc=$?"
for i in $(seq 1 20); do kill -0 $U2 2>/dev/null || break; sleep 0.5; done
pkill -9 -x user2 2>/dev/null
kill -TERM $INIT 2>/dev/null; sleep 0.5; pkill -9 -x initksocket 2>/dev/null
rm -f /dev/shm/ktp_sm
if cmp -s data/in_s.dat data/out_s.dat; then echo "RESULT IDENTICAL"; else echo "RESULT MISMATCH in=$(stat -c%s data/in_s.dat) out=$(stat -c%s data/out_s.dat 2>/dev/null)"; fi
echo "===== init.log ====="; cat /tmp/init.log
echo "===== u1.log ====="; cat /tmp/u1.log
echo "===== u2.log ====="; cat /tmp/u2.log
