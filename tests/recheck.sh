#!/usr/bin/env bash
cd /work
echo "=== build (warnings/errors only) ==="
make 2>&1 | grep -iE "error|warning" || echo "(clean build, no warnings)"
echo "=== quick p=0 transfer ==="
KTP_T=1 bash tests/run_pair.sh 0 data/input_bin.dat data/o.dat 0 0 0 60 2>&1 | grep RESULT
echo "=== TSan ==="
bash tests/tsan.sh 2>&1 | grep -E "TSAN transfer|data race count|ThreadSanitizer report|FATAL|SUMMARY"
echo FINI
