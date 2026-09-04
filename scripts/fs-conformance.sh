#!/usr/bin/env bash
# Filesystem conformance check for a wsldrive mount.
#
# Exercises the operations real tools depend on, against a live mount, and fails
# if any regress. Unit tests cannot catch these: a missing FUSE callback only
# shows up when something actually calls it — an unimplemented chmod, say,
# passes the entire unit suite and then breaks any tool that probes file modes.
#
# Self-contained: starts an agent serving a temp directory and mounts it with the
# Linux client over loopback, so it runs anywhere libfuse3 is available (including
# CI) without WSL or a Windows peer.
#
#   scripts/fs-conformance.sh [build-dir]
#
# Exit status is the number of failed checks (0 = all good).

set -u
BUILD=${1:-build/linux-release}
AGENT="$BUILD/src/tools/wsldrived"
CLI="$BUILD/src/tools/wsldrive"
PORT=${PORT:-51999}

for exe in "$AGENT" "$CLI"; do
  [ -x "$exe" ] || { echo "not built: $exe"; exit 99; }
done

WORK=$(mktemp -d)
MNT=$(mktemp -d)
export WSLDRIVE_TOKEN="conformance-$$"
cleanup() {
  fusermount3 -u "$MNT" 2>/dev/null
  kill "${AGENT_PID:-0}" "${CLI_PID:-0}" 2>/dev/null
  rm -rf "$WORK" "$MNT" 2>/dev/null
}
trap cleanup EXIT

# Symlinks the AGENT serves (created on the backing store before the scan): a
# served home directory is full of these, and a mount that lists them but cannot
# read them shows every one as broken.
echo target-content > "$WORK/lt.txt"
ln -s lt.txt "$WORK/lnk"
ln -s /nowhere/at/all "$WORK/dangling"

echo "serving $WORK -> $MNT"
"$AGENT" --root "$WORK" --listen "tcp://127.0.0.1:$PORT" >/tmp/fsconf-agent.log 2>&1 &
AGENT_PID=$!
sleep 2
"$CLI" mount "$MNT" --connect "tcp://127.0.0.1:$PORT" >/tmp/fsconf-mount.log 2>&1 &
CLI_PID=$!
for _ in $(seq 1 60); do sleep 1; mountpoint -q "$MNT" && break; done
mountpoint -q "$MNT" || { echo "MOUNT FAILED"; tail -5 /tmp/fsconf-mount.log; exit 98; }

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); printf '  ok    %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$1"; }
# On failure, show what the command said and what the mount and the backing
# store hold - a bare FAIL is impossible to diagnose from a CI log.
check(){
  local out
  if out=$(eval "$2" 2>&1); then ok "$1"; else
    bad "$1"
    printf '%s\n' "$out" | sed 's/^/        | /' | head -20
    [ -d "$MNT/repo" ] && { echo "        mount .git : $(ls "$MNT/repo/.git" 2>&1 | tr '\n' ' ')"; echo "        backing .git: $(ls "$WORK/repo/.git" 2>&1 | tr '\n' ' ')"; }
  fi
}
# An operation we knowingly do not support: report it, do not fail the run.
known(){ if eval "$2" >/dev/null 2>&1; then printf '  note  %s (now works)\n' "$1"; else printf '  known %s (unsupported, documented)\n' "$1"; fi; }

echo "== basic file operations =="
check "create + read"          "echo hello > '$MNT/a.txt' && [ \"\$(cat '$MNT/a.txt')\" = hello ]"
check "append"                 "echo world >> '$MNT/a.txt' && [ \"\$(wc -l < '$MNT/a.txt')\" -eq 2 ]"
check "stat reports size"      "[ \$(stat -c%s '$MNT/a.txt') -gt 0 ]"
check "truncate"               "truncate -s 3 '$MNT/a.txt' && [ \$(stat -c%s '$MNT/a.txt') -eq 3 ]"
check "mkdir -p (nested)"      "mkdir -p '$MNT/d/sub/deeper'"
check "readdir sees entries"   "ls '$MNT/d/sub' >/dev/null && ls '$MNT' | grep -q a.txt"
check "rename file"            "mv '$MNT/a.txt' '$MNT/d/b.txt' && [ -f '$MNT/d/b.txt' ]"
check "rename directory keeps contents" "mv '$MNT/d/sub' '$MNT/d/sub2' && [ -d '$MNT/d/sub2/deeper' ]"
check "unlink"                 "cp '$MNT/d/b.txt' '$MNT/gone.txt' && rm '$MNT/gone.txt' && [ ! -e '$MNT/gone.txt' ]"
check "rmdir"                  "rmdir '$MNT/d/sub2/deeper' && [ ! -d '$MNT/d/sub2/deeper' ]"
check "missing file is ENOENT" "! cat '$MNT/nope.txt'"

echo "== metadata operations =="
check "chmod is accepted"      "chmod 755 '$MNT/d/b.txt'"
check "chmod on a directory"   "chmod 700 '$MNT/d'"
check "touch (utimens)"        "touch '$MNT/d/b.txt'"
check "touch creates a file"   "touch '$MNT/new.txt' && [ -f '$MNT/new.txt' ]"

echo "== changes made behind the mount's back (watcher) =="
ext_rename() {   # a directory renamed on the backing store must show its contents on the mount
  mkdir -p "$WORK/ext/inner" && echo x > "$WORK/ext/inner/f" || return 1
  sleep 0.5
  mv "$WORK/ext" "$WORK/ext2" || return 1
  for _ in $(seq 1 50); do [ -f "$MNT/ext2/inner/f" ] && return 0; sleep 0.1; done
  return 1
}
check "external dir rename shows contents" "ext_rename"

# The mount lets the kernel keep a file's pages across opens (auto_cache), so a
# file whose contents change on the backing store must still read back new on
# the next open. Without the revalidation these two checks read the old bytes.
ext_content() {
  echo old-content > "$WORK/ec.txt" || return 1
  for _ in $(seq 1 50); do [ -f "$MNT/ec.txt" ] && break; sleep 0.1; done
  [ "$(cat "$MNT/ec.txt")" = old-content ] || return 1   # warms the page cache
  echo new-content > "$WORK/ec.txt" || return 1
  for _ in $(seq 1 50); do
    [ "$(cat "$MNT/ec.txt")" = new-content ] && return 0
    sleep 0.1
  done
  echo "mount still reads: $(cat "$MNT/ec.txt")"
  return 1
}
check "external content change is seen on re-open" "ext_content"

ext_grow() {   # same, where the file also changes length
  printf 'aa\n' > "$WORK/eg.txt" || return 1
  for _ in $(seq 1 50); do [ -f "$MNT/eg.txt" ] && break; sleep 0.1; done
  [ "$(wc -c < "$MNT/eg.txt")" = 3 ] || return 1
  printf 'bbbbbbbbbb\n' > "$WORK/eg.txt" || return 1
  for _ in $(seq 1 50); do
    [ "$(wc -c < "$MNT/eg.txt")" = 11 ] && [ "$(cat "$MNT/eg.txt")" = bbbbbbbbbb ] && return 0
    sleep 0.1
  done
  echo "mount reads $(wc -c < "$MNT/eg.txt") bytes: $(cat "$MNT/eg.txt")"
  return 1
}
check "external size change is seen on re-open" "ext_grow"

echo "== symlinks served by the agent =="
check "symlink is a symlink"      "[ -L '$MNT/lnk' ]"
check "readlink gives the target" "[ \"\$(readlink '$MNT/lnk')\" = lt.txt ]"
check "following the link works"  "[ \"\$(cat '$MNT/lnk')\" = target-content ]"
check "dangling link keeps its target" "[ \"\$(readlink '$MNT/dangling')\" = /nowhere/at/all ] && ! cat '$MNT/dangling'"

echo "== data integrity =="
check "1 MiB round-trip"       "head -c 1048576 /dev/urandom > '$MNT/big.bin' && [ \$(stat -c%s '$MNT/big.bin') -eq 1048576 ]"
check "content matches"        "cp '$MNT/big.bin' '$WORK/../cmp.bin' 2>/dev/null; cmp -s '$MNT/big.bin' '$WORK/big.bin'"
many_files() {   # a loop is clearer as a function than as a quoted eval string
  mkdir -p "$MNT/many" || return 1
  for i in $(seq 1 200); do echo "$i" > "$MNT/many/f$i" || return 1; done
  [ "$(ls "$MNT/many" | wc -l)" -eq 200 ]
}
check "many small files"       "many_files"
check "seek + partial read"    "[ \"\$(dd if='$MNT/d/b.txt' bs=1 skip=1 count=1 2>/dev/null)\" = e ]"

echo "== a multi-step tool workload (git exercises many ops at once) =="
check "git init"               "git init -q '$MNT/repo' && [ -f '$MNT/repo/.git/HEAD' ]"
check "git add + commit"       "cd '$MNT/repo' && echo x > f.txt && git add . && git -c user.email=a@b -c user.name=c commit -qm first"
check "git status is clean"    "cd '$MNT/repo' && [ -z \"\$(git status --porcelain)\" ]"
check "git log reads back"     "cd '$MNT/repo' && git log --oneline | grep -q first"
check "git checkout -b"        "cd '$MNT/repo' && git checkout -qb feature && echo y >> f.txt && git add . && git -c user.email=a@b -c user.name=c commit -qm second"

echo "== knowingly unsupported (documented limitations) =="
known "creating a symlink"     "ln -s b.txt '$MNT/d/link'"
known "hard link"              "ln '$MNT/d/b.txt' '$MNT/d/hard'"

echo
echo "passed: $PASS   failed: $FAIL"
if [ "$FAIL" -ne 0 ]; then
  echo "--- agent log (tail) ---"; tail -20 /tmp/fsconf-agent.log 2>/dev/null
  echo "--- mount log (tail) ---"; tail -20 /tmp/fsconf-mount.log 2>/dev/null
fi
exit $FAIL
