#!/usr/bin/env bash
# Installs the toolchain needed to build the Linux agent inside a WSL2 distro.
# Run once: sudo ./scripts/wsl-setup.sh
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "run as root: sudo $0" >&2
  exit 1
fi

apt-get update
apt-get install -y --no-install-recommends \
  build-essential \
  clang \
  cmake \
  ninja-build \
  pkg-config \
  libfuse3-dev \
  fuse3

echo
echo "toolchain ready. build with:"
echo "  cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release"
