#!/usr/bin/env bash
# is-mounted.sh : "yes" if /tmp/benchB is a live mountpoint, else "no".
mountpoint -q /tmp/benchB && echo yes || echo no
