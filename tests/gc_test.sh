#!/usr/bin/env bash
# Garbage-collector test: kill a socket-owning process without k_close and
# confirm the GC frees its SM entry.
cd "$(dirname "$0")/.." || exit 1
pkill -9 -x initksocket 2>/dev/null; pkill -9 -x holder 2>/dev/null; rm -f /dev/shm/ktp_sm; sleep 0.3
gcc -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE tests/holder.c -o tests/holder -L. -lksocket -pthread -lrt || exit 1
KTP_P=0 ./initksocket >/tmp/init.log 2>&1 &
INIT=$!
sleep 0.5
./tests/holder 7777 >/tmp/holder.log 2>&1 &
H=$!
sleep 0.7
echo "holder started: $(cat /tmp/holder.log)"
kill -USR1 $INIT; sleep 0.4
echo "allotted BEFORE kill: $(grep '\[dumpsummary\]' /tmp/init.log | tail -1)"
echo "killing holder (SIGKILL, no k_close) ..."
kill -9 $H 2>/dev/null; wait $H 2>/dev/null
freed=0
for i in $(seq 1 12); do
  kill -USR1 $INIT; sleep 0.5
  cnt=$(grep '\[dumpsummary\]' /tmp/init.log | tail -1 | sed 's/.*allotted=//')
  echo "  t=$i allotted=$cnt"
  if [ "$cnt" = "0" ]; then freed=1; break; fi
done
kill -TERM $INIT 2>/dev/null; sleep 0.5; pkill -9 -x initksocket 2>/dev/null; pkill -9 -x holder 2>/dev/null
rm -f /dev/shm/ktp_sm
if [ $freed -eq 1 ]; then echo "GC TEST: PASS (entry freed after owner killed without k_close)"; else echo "GC TEST: FAIL"; fi
