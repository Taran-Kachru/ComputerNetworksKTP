#!/usr/bin/env bash
# build.sh - build everything WITHOUT make, using plain gcc + ar.
# Produces: libksocket.a, initksocket, user1, user2
# Usage:   bash build.sh        (run from anywhere; it cd's to its own folder)
set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
CFLAGS="-std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE"
LDLIBS="-pthread -lrt"      # if your system complains "cannot find -lrt", remove -lrt

echo "[build] libksocket.a"
$CC $CFLAGS -c ksocket.c -o ksocket.o
ar rcs libksocket.a ksocket.o

echo "[build] initksocket"
$CC $CFLAGS initksocket.c -o initksocket -L. -lksocket $LDLIBS

echo "[build] user1 / user2"
$CC $CFLAGS user1.c -o user1 -L. -lksocket $LDLIBS
$CC $CFLAGS user2.c -o user2 -L. -lksocket $LDLIBS

echo "[build] done: libksocket.a initksocket user1 user2"
