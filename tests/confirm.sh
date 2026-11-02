#!/usr/bin/env bash
cd /work
echo "=== required #defines in ksocket.h ==="
grep -nE "define[[:space:]]+(SOCK_KTP|T|P|N|ENOSPACE|ENOTBOUND|ENOMESSAGE|MSG_SIZE|RECV_BUF_SIZE|SEND_BUF_SIZE|SWND_MAX|SEND_RING|RECV_RING)[[:space:]]" ksocket.h
echo "=== dropMessage after recvfrom in R ==="
grep -nE "recvfrom|dropMessage" initksocket.c | head
echo "=== main() count in ksocket.c (must be 0) ==="
grep -c "int main" ksocket.c
echo "=== deliverable files present ==="
ls -1 ksocket.h ksocket.c ktp_internal.h initksocket.c user1.c user2.c Makefile Makefile.lib Makefile.init Makefile.user documentation.txt ktp_submit.tar.gz 2>&1
