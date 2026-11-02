#!/usr/bin/env bash
echo "=== procs ==="
ps aux | grep initksocket | grep -v grep
ps aux | grep user1 | grep -v grep
ps aux | grep user2 | grep -v grep
echo "=== out_bin3 size ==="
ls -l /work/data/out_bin3.dat 2>/dev/null
echo "=== u1.log tail ==="
tail -3 /tmp/u1.log 2>/dev/null
echo "=== u2.log tail ==="
tail -3 /tmp/u2.log 2>/dev/null
echo "=== t_p3.out ==="
cat /tmp/t_p3.out 2>/dev/null
