#!/usr/bin/env bash
cd "$(dirname "$0")/.." || exit 1
gcc -fsanitize=thread -g -std=c11 -Wall -Wextra -D_GNU_SOURCE ksocket.c initksocket.c -o initksocket.tsan -pthread -lrt 2>/dev/null
pkill -9 -x initksocket.tsan 2>/dev/null; pkill -9 -x user1 2>/dev/null; pkill -9 -x user2 2>/dev/null; rm -f /dev/shm/ktp_sm; sleep 0.3
head -c 20480 /dev/urandom > data/tsan_in.dat     # 40 msgs
TSAN_OPTIONS=halt_on_error=0 KTP_P=0 setarch "$(uname -m)" -R ./initksocket.tsan >/tmp/tsan_init.log 2>&1 &
I=$!
sleep 0.8
./user2 127.0.0.1 5002 127.0.0.1 5001 data/tsan_out.dat >/tmp/tsan_u2.log 2>&1 &
U2=$!
sleep 0.3
timeout 120 ./user1 127.0.0.1 5001 127.0.0.1 5002 data/tsan_in.dat >/tmp/tsan_u1.log 2>&1
for i in $(seq 1 240); do kill -0 $U2 2>/dev/null || break; sleep 0.5; done
pkill -9 -x user2 2>/dev/null; kill -TERM $I 2>/dev/null; sleep 1.5; pkill -9 -x initksocket.tsan 2>/dev/null; rm -f /dev/shm/ktp_sm
cmp -s data/tsan_in.dat data/tsan_out.dat && echo "TSAN transfer IDENTICAL" || echo "TSAN transfer MISMATCH"
echo "u2: $(cat /tmp/tsan_u2.log)"
echo "ThreadSanitizer report count: $(grep -c 'ThreadSanitizer' /tmp/tsan_init.log)"
echo "data race count: $(grep -c 'data race' /tmp/tsan_init.log)"
grep -E "WARNING: ThreadSanitizer|SUMMARY:|FATAL" /tmp/tsan_init.log | head -10
rm -f initksocket.tsan data/tsan_in.dat data/tsan_out.dat
echo DONE
