#!/usr/bin/env bash
# Assemble a submission tarball and verify it builds from scratch (build only).
# Portable: works wherever the repo is unzipped.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
STAGE=/tmp/ktp_stage
rm -rf "$STAGE"; mkdir -p "$STAGE/ktp/tests"

# source deliverables
cp ksocket.h ksocket.c ktp_internal.h initksocket.c user1.c user2.c "$STAGE/ktp/"
cp Makefile Makefile.lib Makefile.init Makefile.user build.sh "$STAGE/ktp/"
cp documentation.txt DESIGN_NOTES.md README.md "$STAGE/ktp/"

# test scripts + helpers
cp tests/*.sh "$STAGE/ktp/tests/" 2>/dev/null || true
cp tests/errtest.c tests/holder.c "$STAGE/ktp/tests/" 2>/dev/null || true
cp tests/results.csv "$STAGE/ktp/tests/" 2>/dev/null || true

# tarball
cd "$STAGE"
tar czf "$ROOT/ktp_submit.tar.gz" ktp
cd "$ROOT"
echo "created ktp_submit.tar.gz:"; ls -l ktp_submit.tar.gz
echo "contents:"; tar tzf ktp_submit.tar.gz

# clean-build verification in a fresh directory
echo "=== clean-build verification ==="
V=/tmp/ktp_verify
rm -rf "$V"; mkdir -p "$V"
tar xzf "$ROOT/ktp_submit.tar.gz" -C "$V"
cd "$V/ktp"
if make > /tmp/verify_build.log 2>&1; then
  echo "CLEAN BUILD (make): OK"
  ls -l libksocket.a initksocket user1 user2
else
  echo "CLEAN BUILD (make): FAILED"; cat /tmp/verify_build.log
fi
cd "$ROOT"
