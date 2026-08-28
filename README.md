# wsldrive

[![CI](https://github.com/zcsizmadia/wsldrive/actions/workflows/ci.yml/badge.svg)](https://github.com/zcsizmadia/wsldrive/actions/workflows/ci.yml)

Native-speed file access across the WSL2 boundary, in both directions:

- **Direction A (primary):** mount a WSL2 ext4 path as a real Windows drive letter so
  Windows-only tools (Visual Studio, GitHub Desktop, Explorer, Unity, ...) stop paying the
  `\\wsl.localhost` Plan 9 tax.
- **Direction B (secondary, benchmark-gated):** a cached view of Windows drives inside WSL2.

See [plan.md](plan.md) for the design, semantics table, benchmark targets and roadmap.

## Status

Early. The platform-independent core is implemented and tested:

| module | purpose |
|---|---|
| `core/metadata_tree` | in-RAM directory tree: O(1) child lookup, case-insensitive view, readdir, snapshots |
| `core/string_pool`, `core/flat_map` | interned names and an open-addressing (parent, name) → node index |
| `core/protocol` | 24-byte framed, little-endian wire format with zero-copy decoding |
| `core/coalescer` | collapses watcher event bursts into minimal ordered invalidation batches |
| `core/content_cache` | content-addressed (BLAKE3) on-disk cache with atomic writes and LRU eviction |
| `core/unicode`, `core/path` | locale-independent case folding, path normalisation |

Platform layers (WinFsp, fanotify, Hyper-V sockets) come next.

### Baseline micro-benchmarks

`core_bench`, MSVC 19.42 RelWithDebInfo, 20-thread laptop CPU @ 2.8 GHz, 2026-08-27. Synthetic tree =
100 modules × 10 components × 100 files (100k files, 1.1k directories).

| operation | time |
|---|---|
| path lookup, 3 components, exact hit | ~150–180 ns |
| path lookup, case-insensitive, wrong case | ~220 ns |
| negative lookup (name unknown anywhere) | ~140 ns |
| negative lookup (name exists in another dir) | ~100 ns |
| readdir, 100 entries (attrs + names) | ~150 ns |
| upsert existing path / insert+remove churn | ~230 ns / ~130 ns |
| (parent,name)→node probe, 1M entries, hit / miss | 15 ns / 14 ns (`std::unordered_map`: 44 ns) |
| snapshot encode / streaming decode, 100k entries | 2.5 ms / 3.2 ms (36 bytes per entry) |
| snapshot decode + build tree, 100k entries | ~27 ms |
| coalesce 30k watcher events (10k-file checkout) | ~7 ms |
| BLAKE3, 1 MiB / 16 MiB | 4.4 GiB/s |

Run them yourself with `.\scripts\build.ps1 -Bench`.

## Build

Windows (Visual Studio 2022, from any PowerShell):

```powershell
.\scripts\build.ps1            # configure + build + unit tests (RelWithDebInfo)
.\scripts\build.ps1 -Bench     # ... and run the micro-benchmarks
```

WSL2 (Ubuntu 22.04+ / Debian 12+):

```bash
sudo ./scripts/wsl-setup.sh
cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release
```

Dependencies (GoogleTest, Google Benchmark, BLAKE3) are fetched by CMake at configure time.
