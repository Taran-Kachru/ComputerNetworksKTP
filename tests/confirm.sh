#!/usr/bin/env bash
cd /work
echo "=== required #defines in ksocket.h ==="
grep -nE "define[[:space:]]+(SOCK_KTP|T|P|N|ENOSPACE|ENOTBOUND|ENOMESSAGE|MSG_SIZE|RECV_BUF_SIZE|SEND_BUF_SIZE|SWND_MAX|SEND_RING|RECV_RING)[[:space:]]" ksocket.h
echo "=== dropMessage after recvfrom in R ==="
grep -nE "recvfrom|dropMessage" initksocket.cpp | head
echo "=== main() count in ksocket.cpp (must be 0) ==="
grep -c "int main" ksocket.cpp
echo "=== deliverable files present ==="
ls -1 ksocket.h ksocket.cpp ktp_internal.h initksocket.cpp user1.cpp user2.cpp Makefile Makefile.lib Makefile.init Makefile.user documentation.txt ktp_submit.tar.gz 2>&1
