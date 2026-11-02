#!/usr/bin/env bash
# Full remaining verification for the C port: ASan+UBSan, TSan, tarball clean
# build, extracted-dir run, valgrind.
cd /work
echo "########## ASan + UBSan ##########"
bash tests/san.sh 2>&1 | grep -E "transfer|sanitizer|SANITIZER|BUILD_FAIL" | grep -v tsan

echo "########## ThreadSanitizer (setarch -R) ##########"
bash tests/tsan.sh 2>&1 | grep -E "TSAN transfer|data race count|race report|FATAL|SUMMARY|ThreadSanitizer report"

echo "########## tarball + clean build ##########"
bash tests/make_tarball.sh 2>&1 | grep -E "CLEAN BUILD|created|FAILED"

echo "########## extracted-dir run + valgrind ##########"
bash tests/final_verify.sh 2>&1 | grep -E "IDENTICAL|MISMATCH|ERROR SUMMARY|definitely lost"

echo "########## DONE ##########"
