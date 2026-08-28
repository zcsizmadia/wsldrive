#!/usr/bin/env bash
# read.sh <path> : print milliseconds to read every file's bytes under <path>.
p="$1"
S=$(date +%s%N)
find "$p" -type f -print0 | xargs -0 cat >/dev/null 2>&1
E=$(date +%s%N)
echo $(( (E - S) / 1000000 ))
