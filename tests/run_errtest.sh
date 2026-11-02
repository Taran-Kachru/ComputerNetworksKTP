#!/usr/bin/env bash
cd /work
pkill -9 -x initksocket 2>/dev/null; rm -f /dev/shm/ktp_sm; sleep 0.3
gcc -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE tests/errtest.c -o tests/errtest -L. -lksocket -pthread -lrt || exit 1
KTP_P=0 KTP_T=1 ./initksocket >/tmp/init.log 2>&1 &
INIT=$!
sleep 0.5
./tests/errtest
RC=$?
kill -TERM $INIT 2>/dev/null; sleep 0.5; pkill -9 -x initksocket 2>/dev/null
rm -f /dev/shm/ktp_sm
exit $RC
