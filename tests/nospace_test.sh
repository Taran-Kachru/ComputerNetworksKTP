#!/usr/bin/env bash
# nospace (zero-window) deadlock demonstration.
#   - slow reader (user2) forces the receiver buffer to fill -> rwnd=0
#   - KTP_DROP_REOPEN=-1 drops ALL receiver window-reopen ACKs
# WITHOUT the fix (persist probe disabled) the transfer must HANG.
# WITH the fix (persist probe, default) it must COMPLETE with an identical file.
# Normal dropMessage(P) in R is untouched (p=0 here so only our deterministic
# reopen drops + flow control are exercised).
cd /work
rm -f data/ns_in.dat; head -c 40960 /dev/urandom > data/ns_in.dat   # 80 msgs
SLOW=25        # ms per read
export KTP_T=1 # speed up probe/timeout for the test (header still #define T 5)

echo "=========================================================="
echo "TEST A: WITHOUT fix (KTP_DISABLE_PERSIST=1) + all reopen ACKs dropped"
echo "expected: HANG (transfer does not complete)"
echo "----------------------------------------------------------"
bash tests/run_pair.sh 0 data/ns_in.dat data/ns_outA.dat $SLOW -1 1 18 > /tmp/nsA.log 2>&1
if grep -q 'RESULT: IDENTICAL' /tmp/nsA.log; then
  echo "TEST A: UNEXPECTED COMPLETION (fix-off still finished?) -> investigate"
else
  gotA=$(stat -c%s data/ns_outA.dat 2>/dev/null || echo 0)
  echo "TEST A: HUNG as expected (received only $gotA bytes of $(stat -c%s data/ns_in.dat), did not finish)"
fi

echo "=========================================================="
echo "TEST B: WITH fix (persist probe on) + all reopen ACKs dropped"
echo "expected: COMPLETES, file identical"
echo "----------------------------------------------------------"
bash tests/run_pair.sh 0 data/ns_in.dat data/ns_outB.dat $SLOW -1 0 90 > /tmp/nsB.log 2>&1
tail -2 /tmp/nsB.log
if cmp -s data/ns_in.dat data/ns_outB.dat; then
  echo "TEST B: COMPLETED, FILE IDENTICAL  (persist probe broke the deadlock)"
else
  echo "TEST B: FAILED (file mismatch) -> fix not working"
fi
rm -f data/ns_outA.dat data/ns_outB.dat
