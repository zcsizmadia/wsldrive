# Real-world benchmark results

Measured with `scripts/bench-real.ps1` on this dev machine: WSL2 (Ubuntu 24.04,
kernel 6.6) on Windows 11, `virtiofs` not enabled (default 9P for `/mnt/c`).
Trees are 3000 small files across 60 directories. Median of warm runs, in
milliseconds (lower is better). "native" is the same workload run locally on the
owning OS — the theoretical floor, shown for reference.

## Direction A — Windows accessing a WSL2 ext4 tree

| scenario                    | walk (enumerate) | read (all bytes) |
|-----------------------------|-----------------:|-----------------:|
| native (in WSL, reference)  |            4 ms  |          39 ms   |
| `\\wsl.localhost` (Plan 9)  |          104 ms  |        2856 ms   |
| **wsldrive drive**          |       **39 ms**  |  **cold 574 / warm 366 ms** |

**wsldrive is ~2.7× faster on metadata (walk) and ~5–8× faster on reads than the
`\\wsl.localhost` Plan 9 path** — the everyday path for Windows tools (Visual
Studio, Explorer, Git for Windows) reaching WSL files. Reads are served with
directory read-ahead (one bulk request per directory) and, once warm, from the
in-RAM cache.

## Direction B — WSL accessing a Windows NTFS tree

| scenario                     | walk (enumerate) | read (all bytes) |
|------------------------------|-----------------:|-----------------:|
| native (on Windows, ref)     |          10 ms   |         118 ms   |
| `/mnt/c` (9P)                |         119 ms   |        5853 ms   |
| `/mnt/c` (**virtiofs=true**) |         406 ms   |        7686 ms   |
| **wsldrive FUSE mount**      | metadata from RAM (see below) | cold: latency-bound; warm: from cache |

`/mnt/c` reads are ~50-65× slower than native — the well-known WSL pain.
Notably, on this machine **`virtiofs=true` did not beat 9P** for these
workloads (walk 406 ms, read 7686 ms; five warm runs stayed 5.9-9.8 s), and it
shows no persistent read cache. So the Direction B bar the plan gates against is
still multi-second — leaving clear room for wsldrive to win once cold reads are
addressed (metadata is already RAM-served; warm reads are cache hits). The
wsldrive FUSE mount is functional (mounts, reads, writes; verified end-to-end)
and serves **all metadata from the client's in-RAM mirror**, so `walk`/`stat`
workloads avoid the per-op boundary cost the same way Direction A does.

**Transport is the Direction B bottleneck.** Reads now use directory read-ahead
(one bulk `ReadMany` request per directory instead of one per file) — verified
to work on Direction A, where the same code path reads 3000 files cold in
574 ms. But over the **WSL→Windows** path, each request/response round-trip
costs on the order of *seconds* through the WSL2 localhost-forwarding relay
(this machine runs `networkingMode=mirrored`), so even a handful of bulk
round-trips is slow. Read-ahead cuts the round-trip *count*; it cannot fix a
multi-second-per-trip transport.

The fix is the **Hyper-V socket transport** (`AF_VSOCK`/`AF_HYPERV`, already
scaffolded in `net/socket`), which bypasses the TCP/relay path and talks
guest↔host directly. That is the next work for Direction B; until it lands, use
`virtiofs=true` for `/mnt/c`. Direction A is unaffected — its transport
(Windows→WSL loopback) is already sub-millisecond, which is why it is fast today.

## Performance work landed

- **Client-side read cache** (`RemoteRoot`): small files cached whole in RAM,
  validated by (mtime, size), LRU-evicted, invalidated on change. Repeat reads
  are served locally with no round-trip.
- **All metadata in RAM** on the accessing side (already in the design): `stat`,
  `readdir`, negative lookups never cross the boundary — the source of the
  Direction A walk win and the Direction B metadata win.
- **Directory read-ahead**: on a read miss, the file's directory is bulk-fetched
  in one `ReadMany` request (with an async prefetcher for large directories), so
  a sequential reader pays one round-trip per directory rather than per file.
