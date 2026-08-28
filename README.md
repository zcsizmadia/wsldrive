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

### Mounting (Direction A)

With [WinFsp](https://winfsp.dev) installed (runtime **and** the Developer/SDK feature), a WSL2
ext4 tree can be mounted as a Windows drive letter:

```powershell
# One command: wsldrive launches the agent inside the distro and mounts it.
wsldrive mount W: --distro Ubuntu --wsl-root ~/project

# Check the environment first (WinFsp + WSL):
wsldrive doctor

# Or attach to an already-running agent:
#   in WSL:      wsldrived --root ~/project --listen tcp://0.0.0.0:7788
#   on Windows:  wsldrive mount W: --connect tcp://127.0.0.1:7788
```

With `--distro`, `wsldrive` starts `wsldrived` in the distro over `wsl.exe`, waits for it to
listen, mounts, and tears the agent down on exit (the agent also self-terminates if the Windows
side goes away). Ctrl+C unmounts cleanly.

Add `--hvsocket` to use the **Hyper-V socket transport** instead of loopback TCP — the agent listens
on `vsock` and the client connects over `AF_HYPERV`, bypassing the localhost relay. This is what makes
the WSL→Windows direction fast (see `bench/RESULTS.md`). One-time setup: run
`scripts/register-hvsocket.ps1` elevated and `wsl --shutdown`. The WSL VM GUID is discovered via
`hcsdiag` (Hyper-V admin) or passed with `--vm-guid`.

Drop a `.wsldriveignore` at the served root (gitignore-style: `node_modules/`, `*.log`, `/build`, ...)
to exclude directories from the mounted view and from sync — handy for keeping build output and vendored
trees off the drive.

Metadata (`dir`, `stat`, directory listing) is served from the client's in-RAM mirror with no
round-trips; file contents are fetched over the socket on demand; live edits in WSL propagate via
pushed invalidations. **Read-write**: create, write, append, truncate, mkdir, rename, unlink and rmdir
on the drive letter are written through to ext4 (verified by mounting a WSL tree, writing from Windows,
and reading the bytes back inside WSL). ext4 names illegal on Windows (`: ? * < > | "`, control chars,
a trailing dot/space) are mapped to the Unicode private-use area (the WSL/Cygwin convention) so they
appear and round-trip on the drive. Pass `--writeback` to coalesce a file's writes and flush them on
`fsync`/close (fewer round-trips for write-heavy work such as build output; durability is at close
rather than per write). **Hardlinks** are currently reported as independent files (`st_nlink` is not
preserved) — a known limitation. Built via the FUSE3 API, so the same mount serves Linux
(Direction B) later. The mount target builds only when WinFsp is detected, so CI and non-Windows builds
are unaffected. Set `WSLDRIVE_FUSE_DEBUG=1` to log WinFsp FUSE operations.

### Direction B (Windows drive cached in WSL, experimental)

The mount is built on the FUSE3 API, so the same implementation runs in WSL via libfuse3:

```bash
# on Windows: serve a Windows path
wsldrived.exe --root C:\path\to\project --listen tcp://0.0.0.0:7788
# in WSL: mount it
wsldrive mount /mnt/win --connect tcp://<windows-host>:7788
```

It is functional (read/write across the transport, live invalidations via the Windows-side
`ReadDirectoryChangesW` watcher) and verified end-to-end. It is **gated and experimental**: reads
currently go over the socket rather than a local cache, so it does not yet beat `virtiofs=true` for
`/mnt/c`. For fast Linux access to Windows files today, enable virtiofs; Direction B becomes a
worthwhile replacement once the read-cache layer lands.

Platform layers (Hyper-V socket transport) continue in later phases.

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
