#!/usr/bin/env bash
echo "=== fv_u1.log ==="; cat /tmp/fv_u1.log 2>/dev/null
echo "=== fv_u2.log ==="; cat /tmp/fv_u2.log 2>/dev/null
echo "=== fv_init.log ==="; cat /tmp/fv_init.log 2>/dev/null
echo "=== extracted result ==="
cd /tmp/ktp_verify/ktp 2>/dev/null || { echo "no extracted dir"; exit; }
ls -l in.dat out.dat 2>/dev/null
if cmp -s in.dat out.dat; then echo "EXTRACTED RUN IDENTICAL"; else echo "EXTRACTED RUN mismatch/incomplete"; fi
echo "=== valgrind logs present? ==="
ls -l /tmp/vg_init.log 2>/dev/null && echo "(valgrind section ran)" || echo "(valgrind section did NOT run - script exited early)"
