#!/usr/bin/env bash
# Generate the test input files (a >200KB random binary and a text file).
set -e
mkdir -p data
head -c 204800 /dev/urandom > data/input_bin.dat      # 200 KB random (~401 msgs)
# a text file > 100KB
: > data/input_txt.txt
i=0
while [ $(stat -c%s data/input_txt.txt) -lt 120000 ]; do
  echo "line $i: the quick brown fox jumps over the lazy dog 0123456789 ABCDEFGHIJKLMNOP" >> data/input_txt.txt
  i=$((i+1))
done
echo "generated:"
ls -l data/input_bin.dat data/input_txt.txt
