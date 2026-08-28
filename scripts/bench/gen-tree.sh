#!/usr/bin/env bash
# gen-tree.sh <root> <files> : create <root> with ~<files> small files if it has fewer.
set -e
R="$1"; N="$2"
cur=$(find "$R" -type f 2>/dev/null | wc -l)
if [ "$cur" -ne "$N" ]; then
  rm -rf "$R"; mkdir -p "$R"
  per=$(( (N + 59) / 60 ))
  for d in $(seq 1 60); do
    mkdir -p "$R/dir$d"
    for f in $(seq 1 "$per"); do head -c 2048 /dev/urandom | base64 > "$R/dir$d/file$f.txt"; done
  done
fi
find "$R" -type f | wc -l
