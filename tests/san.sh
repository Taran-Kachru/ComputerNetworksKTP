#!/usr/bin/env bash
# Sanitizer runs: ASan+UBSan across all components, and TSan on the threaded init.
cd /work
export KTP_T=1
head -c 51200 /dev/urandom > data/san_in.dat    # 100 msgs

run_one() {  # $1=tag  $2=initbin  $3=u1  $4=u2
  local tag=$1 ib=$2 u1=$3 u2=$4
  pkill -9 -x "$ib" 2>/dev/null; pkill -9 -x "$u1" 2>/dev/null; pkill -9 -x "$u2" 2>/dev/null
  rm -f /dev/shm/ktp_sm data/san_out.dat; sleep 0.3
  ASAN_OPTIONS=detect_leaks=0 TSAN_OPTIONS=halt_on_error=0 KTP_P=0.2 KTP_T=1 ./$ib >/tmp/san_init_$tag.log 2>&1 &
  local I=$!
  sleep 0.6
  ASAN_OPTIONS=detect_leaks=0 ./$u2 127.0.0.1 5002 127.0.0.1 5001 data/san_out.dat >/tmp/san_u2_$tag.log 2>&1 &
  local U2=$!
  sleep 0.3
  ASAN_OPTIONS=detect_leaks=0 timeout 120 ./$u1 127.0.0.1 5001 127.0.0.1 5002 data/san_in.dat >/tmp/san_u1_$tag.log 2>&1
  for i in $(seq 1 200); do kill -0 $U2 2>/dev/null || break; sleep 0.5; done
  pkill -9 -x "$u2" 2>/dev/null
  kill -TERM $I 2>/dev/null; sleep 0.8; pkill -9 -x "$ib" 2>/dev/null
  rm -f /dev/shm/ktp_sm
  if cmp -s data/san_in.dat data/san_out.dat; then echo "[$tag] transfer IDENTICAL"; else echo "[$tag] transfer MISMATCH"; fi
  local errs
  errs=$(grep -hE "runtime error|AddressSanitizer|ThreadSanitizer|data race|heap-buffer|use-after|SUMMARY:" /tmp/san_init_$tag.log /tmp/san_u1_$tag.log /tmp/san_u2_$tag.log 2>/dev/null)
  if [ -z "$errs" ]; then echo "[$tag] NO sanitizer errors"; else echo "[$tag] SANITIZER FINDINGS:"; echo "$errs" | head -20; fi
}

echo "=== building ASan+UBSan variants ==="
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -std=c11 -Wall -Wextra -D_GNU_SOURCE"
gcc $SAN ksocket.c initksocket.c -o initksocket.asan -pthread -lrt 2>/tmp/bld.log || { echo BUILD_FAIL_ASAN_INIT; cat /tmp/bld.log; }
gcc $SAN ksocket.c user1.c -o user1.asan -pthread -lrt 2>>/tmp/bld.log || echo BUILD_FAIL_ASAN_U1
gcc $SAN ksocket.c user2.c -o user2.asan -pthread -lrt 2>>/tmp/bld.log || echo BUILD_FAIL_ASAN_U2
echo "=== running ASan+UBSan ==="
run_one asan initksocket.asan user1.asan user2.asan

echo "=== building TSan init ==="
TSN="-fsanitize=thread -g -std=c11 -Wall -Wextra -D_GNU_SOURCE"
gcc $TSN ksocket.c initksocket.c -o initksocket.tsan -pthread -lrt 2>/tmp/bld2.log || { echo BUILD_FAIL_TSAN; cat /tmp/bld2.log; }
echo "=== running TSan init (normal users) ==="
run_one tsan initksocket.tsan user1 user2

rm -f initksocket.asan user1.asan user2.asan initksocket.tsan data/san_in.dat data/san_out.dat
echo "SANITIZER RUN COMPLETE"
