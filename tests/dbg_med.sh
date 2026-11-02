#!/usr/bin/env bash
# arg1 = number of messages (file = arg1*512 bytes)
cd /work
MSGS=${1:-300}
BYTES=$((MSGS*512))
pkill -9 -x initksocket 2>/dev/null; pkill -9 -x user1 2>/dev/null; pkill -9 -x user2 2>/dev/null
rm -f /dev/shm/ktp_sm data/out_m.dat; sleep 0.3
head -c $BYTES /dev/urandom > data/in_m.dat
KTP_DEBUG=1 KTP_P=0 ./initksocket >/tmp/init.log 2>&1 &
INIT=$!
sleep 0.5
./user2 127.0.0.1 5002 127.0.0.1 5001 data/out_m.dat >/tmp/u2.log 2>&1 &
U2=$!
sleep 0.3
timeout 40 ./user1 127.0.0.1 5001 127.0.0.1 5002 data/in_m.dat /tmp/stats.txt >/tmp/u1.log 2>&1
echo "u1 rc=$? ($MSGS msgs)"
for i in $(seq 1 40); do kill -0 $U2 2>/dev/null || break; sleep 0.5; done
pkill -9 -x user2 2>/dev/null
kill -TERM $INIT 2>/dev/null; sleep 0.5; pkill -9 -x initksocket 2>/dev/null
rm -f /dev/shm/ktp_sm
if cmp -s data/in_m.dat data/out_m.dat; then echo "RESULT IDENTICAL ($BYTES bytes)"; else echo "RESULT MISMATCH in=$(stat -c%s data/in_m.dat) out=$(stat -c%s data/out_m.dat 2>/dev/null)"; fi
echo "NOSPACE DROP count: $(grep -c 'NOSPACE DROP' /tmp/init.log)"
echo "first few NOSPACE DROP lines:"; grep -n 'NOSPACE DROP' /tmp/init.log | head
echo "u1: $(cat /tmp/u1.log)"
echo "u2: $(cat /tmp/u2.log)"
