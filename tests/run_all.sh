#!/usr/bin/env bash
# Master functional test suite - runs every test case end to end.
# Portable: cd's to the project root wherever the repo was unzipped.
# Builds without make (uses build.sh).  Uses KTP_T=1 to shorten retransmit waits;
# correctness/counts are unaffected on localhost where RTT << 1ms.
cd "$(dirname "$0")/.." || exit 1
export KTP_T=1

bash build.sh >/dev/null 2>&1 || { echo "BUILD FAILED"; bash build.sh; exit 1; }
bash tests/gen_files.sh >/dev/null 2>&1 || { echo "could not generate test files"; exit 1; }

pass=0; fail=0
note() { if [ "$1" = 0 ]; then echo "PASS: $2"; pass=$((pass+1)); else echo "FAIL: $2"; fail=$((fail+1)); fi; }

echo "### 1. p=0 single pair (200KB, crosses wraparound) ###"
bash tests/run_pair.sh 0 data/input_bin.dat data/o1.dat 0 0 0 60 >/tmp/a1 2>&1
grep -q 'RESULT: IDENTICAL' /tmp/a1; note $? "p=0 identical"; grep 'stats=' /tmp/a1 | tail -1

echo "### 2. p=0.3 single pair (retransmits) ###"
bash tests/run_pair.sh 0.3 data/input_bin.dat data/o2.dat 0 0 0 120 >/tmp/a2 2>&1
grep -q 'RESULT: IDENTICAL' /tmp/a2; note $? "p=0.3 identical"; grep 'stats=' /tmp/a2 | tail -1

echo "### 3. wraparound (300 msgs, >256) ###"
bash tests/dbg_med.sh 300 >/tmp/a3 2>&1
grep -q 'RESULT IDENTICAL' /tmp/a3; note $? "wraparound identical"

echo "### 4. nospace deadlock (hang-without-fix / works-with-fix) ###"
bash tests/nospace_test.sh >/tmp/a4 2>&1
A=$(grep -c 'HUNG as expected' /tmp/a4); B=$(grep -c 'FILE IDENTICAL' /tmp/a4)
[ "$A" = 1 ] && [ "$B" = 1 ]; note $? "nospace hang-without / works-with fix"

echo "### 5. three concurrent pairs (p=0.1) ###"
bash tests/concurrent.sh 0.1 >/tmp/a5 2>&1
grep -q 'RESULT: 3/3' /tmp/a5; note $? "3 concurrent pairs identical"

echo "### 6. error paths ###"
bash tests/run_errtest.sh >/tmp/a6 2>&1
grep -q 'ERRTEST: ALL PASS' /tmp/a6; note $? "error paths (ENOSPACE/ENOTBOUND/ENOMESSAGE)"

echo "### 7. garbage collector ###"
bash tests/gc_test.sh >/tmp/a7 2>&1
grep -q 'GC TEST: PASS' /tmp/a7; note $? "GC frees entry of killed process"

echo "====================================================="
echo "SUITE RESULT: $pass passed, $fail failed"
rm -f data/o1.dat data/o2.dat
exit $fail
