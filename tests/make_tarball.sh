#!/usr/bin/env bash
# Assemble a clean submission tarball and verify it builds from scratch with the
# Makefiles alone (build only - does not run transfers, safe any time).
set -e
cd /work
STAGE=/tmp/ktp_stage
rm -rf "$STAGE"; mkdir -p "$STAGE/ktp/tests"

# source deliverables
cp ksocket.h ksocket.cpp ktp_internal.h initksocket.cpp user1.cpp user2.cpp "$STAGE/ktp/"
cp Makefile Makefile.lib Makefile.init Makefile.user "$STAGE/ktp/"
cp documentation.txt DESIGN_NOTES.md "$STAGE/ktp/"

# test scripts + helpers (not required by the build, included for reproducibility)
cp tests/*.sh "$STAGE/ktp/tests/" 2>/dev/null || true
cp tests/errtest.cpp tests/holder.cpp "$STAGE/ktp/tests/" 2>/dev/null || true
cp tests/results.csv "$STAGE/ktp/tests/" 2>/dev/null || true

# tarball
cd "$STAGE"
tar czf /work/ktp_submit.tar.gz ktp
cd /work
echo "created ktp_submit.tar.gz:"; ls -l ktp_submit.tar.gz
echo "contents:"; tar tzf ktp_submit.tar.gz

# clean-build verification in a fresh directory
echo "=== clean-build verification ==="
V=/tmp/ktp_verify
rm -rf "$V"; mkdir -p "$V"
tar xzf /work/ktp_submit.tar.gz -C "$V"
cd "$V/ktp"
make clean >/dev/null 2>&1 || true
if make > /tmp/verify_build.log 2>&1; then
  echo "CLEAN BUILD: OK"
  ls -l libksocket.a initksocket user1 user2
else
  echo "CLEAN BUILD: FAILED"; cat /tmp/verify_build.log
fi
cd /work
