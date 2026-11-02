#!/usr/bin/env bash
# End-to-end run from the freshly extracted tarball + a quick valgrind check.
V=/tmp/ktp_verify/ktp
cd "$V"
export KTP_T=1
pkill -9 -x initksocket 2>/dev/null; pkill -9 -x user1 2>/dev/null; pkill -9 -x user2 2>/dev/null
rm -f /dev/shm/ktp_sm; sleep 0.3
head -c 204800 /dev/urandom > in.dat

echo "=== 1) end-to-end transfer from extracted tarball (p=0, drops off) ==="
KTP_P=0 KTP_T=1 ./initksocket >/tmp/fv_init.log 2>&1 &
I=$!
sleep 0.5
./user2 127.0.0.1 5002 127.0.0.1 5001 out.dat >/tmp/fv_u2.log 2>&1 &
U2=$!
sleep 0.3
timeout 60 ./user1 127.0.0.1 5001 127.0.0.1 5002 in.dat >/tmp/fv_u1.log 2>&1
for i in $(seq 1 120); do kill -0 $U2 2>/dev/null || break; sleep 0.5; done
pkill -9 -x user2 2>/dev/null || true
kill -TERM $I 2>/dev/null; sleep 0.5; pkill -9 -x initksocket 2>/dev/null || true
rm -f /dev/shm/ktp_sm
cat /tmp/fv_u1.log /tmp/fv_u2.log
cmp -s in.dat out.dat && echo "EXTRACTED-TARBALL RUN: IDENTICAL" || echo "EXTRACTED-TARBALL RUN: MISMATCH"

echo "=== 2) valgrind memcheck on init during a short transfer (KTP_T=1) ==="
export KTP_T=1
rm -f /dev/shm/ktp_sm out2.dat; sleep 0.3
head -c 20480 /dev/urandom > in2.dat
valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite \
         ./initksocket >/tmp/vg_init.log 2>&1 &
I=$!
sleep 2
./user2 127.0.0.1 5002 127.0.0.1 5001 out2.dat >/tmp/vg_u2.log 2>&1 &
U2=$!
sleep 0.3
timeout 90 ./user1 127.0.0.1 5001 127.0.0.1 5002 in2.dat >/tmp/vg_u1.log 2>&1
for i in $(seq 1 180); do kill -0 $U2 2>/dev/null || break; sleep 0.5; done
pkill -9 -x user2 2>/dev/null || true
kill -TERM $I 2>/dev/null; sleep 3; pkill -9 -x memcheck-amd64- 2>/dev/null || true
rm -f /dev/shm/ktp_sm
cmp -s in2.dat out2.dat && echo "valgrind-run transfer: IDENTICAL" || echo "valgrind-run transfer: MISMATCH"
echo "--- valgrind summary ---"
grep -E "ERROR SUMMARY|definitely lost|Invalid read|Invalid write" /tmp/vg_init.log | head
echo DONE
