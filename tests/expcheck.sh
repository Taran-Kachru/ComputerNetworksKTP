#!/usr/bin/env bash
echo "=== running procs ==="
ps -eo pid,etime,cmd | grep experiment | grep -v grep
ps -eo pid,etime,cmd | grep initksocket | grep -v grep
ps -eo pid,etime,cmd | grep user1 | grep -v grep
ps -eo pid,etime,cmd | grep user2 | grep -v grep
echo "=== experiment.out tail ==="
tail -4 /tmp/experiment.out 2>/dev/null
echo "=== in-progress output files ==="
ls -l /work/data/exp_out_*.dat 2>/dev/null
echo "=== results so far ==="
cat /work/tests/results.csv 2>/dev/null
