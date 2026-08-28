#!/usr/bin/env bash
# walk.sh <path> : print milliseconds to enumerate all files under <path>.
p="$1"
S=$(date +%s%N)
find "$p" -type f | wc -l >/dev/null
E=$(date +%s%N)
echo $(( (E - S) / 1000000 ))
