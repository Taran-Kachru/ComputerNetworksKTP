#!/usr/bin/env bash
# startup / shutdown probe
cd /work
pkill -9 -x initksocket 2>/dev/null; rm -f /dev/shm/ktp_sm; sleep 0.3
KTP_P=0 ./initksocket >/tmp/init.log 2>&1 &
INIT=$!
sleep 1
if kill -0 "$INIT" 2>/dev/null; then echo "init ALIVE"; else echo "init DEAD"; fi
echo "shm:"; ls -l /dev/shm/ 2>/dev/null
echo "initlog:"; cat /tmp/init.log
echo "--- SIGTERM ---"
kill -TERM "$INIT" 2>/dev/null
sleep 3
if kill -0 "$INIT" 2>/dev/null; then echo "STILL ALIVE after TERM"; else echo "exited cleanly after TERM"; fi
echo "initlog-final:"; cat /tmp/init.log
echo "shm-after:"; ls -l /dev/shm/ 2>/dev/null
pkill -9 -x initksocket 2>/dev/null; rm -f /dev/shm/ktp_sm
echo DONE
