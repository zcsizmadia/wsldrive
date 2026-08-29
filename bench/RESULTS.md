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
| wsldrive over TCP relay      |          12 ms   |  minutes (transport-bound) |
| **wsldrive over hvsocket**   |       **12 ms**  |  **cold 621 / warm 383 ms** |

With the **Hyper-V socket transport** (ping **0.114 ms**, vs seconds/round-trip
over the localhost relay), read-ahead and the cache make wsldrive **~10× faster
than both 9P and virtiofs** on reads and near-native on metadata — well past the
2× gate. This is the configuration to use for Direction B.

`/mnt/c` reads are ~50-65× slower than native — the well-known WSL pain.
Notably, on this machine **`virtiofs=true` did not beat 9P** for these
workloads (walk 406 ms, read 7686 ms; five warm runs stayed 5.9-9.8 s), and it
shows no persistent read cache. So the Direction B bar (virtiofs) is still
multi-second — leaving clear room for wsldrive to win once cold reads are
addressed (metadata is already RAM-served; warm reads are cache hits). The
wsldrive FUSE mount is functional (mounts, reads, writes; verified end-to-end)
and serves **all metadata from the client's in-RAM mirror**, so `walk`/`stat`
workloads avoid the per-op boundary cost the same way Direction A does.

**The Direction B bottleneck was the transport, and Hyper-V sockets fix it.**
Reads use directory read-ahead (one bulk `ReadMany` request per directory), but
over the localhost-forwarding relay each guest→host round-trip cost seconds
(this machine runs `networkingMode=mirrored`), so bulk reads were still slow.
Switching the transport to `AF_VSOCK`/`AF_HYPERV` drops the round-trip to
~0.1 ms and makes Direction B fast (table above).

### Enabling the Hyper-V socket transport

WSL2 routes hvsocket **host→guest**, so wsldrive uses: the **WSL side listens**
on `vsock://any:<port>` and the **Windows side connects** to
`hv://{<wsl-vm-guid>}:<port>` — for both directions (in Direction B the Windows
agent dials the WSL client; in Direction A the Windows client dials the WSL
agent). Setup:

1. Register the vsock service GUIDs once, elevated (see
   `scripts/register-hvsocket.ps1`) — WSL2 only routes registered services.
2. Restart WSL (`wsl --shutdown`) so the VM picks up the registered services.

The `--hvsocket` flag then wires everything: the VM GUID is auto-discovered via
`hcsdiag` (or passed with `--vm-guid`), a registered vsock port is chosen, the
agent is auto-launched, and the client connects over `AF_HYPERV`:

```powershell
# Direction A (on Windows): mount a WSL tree over hvsocket
wsldrive mount W: --distro Ubuntu --wsl-root ~/project --hvsocket
```
```bash
# Direction B (in WSL): mount a Windows tree over hvsocket
wsldrive mount /tmp/win --win-root 'C:/project' --win-agent /mnt/c/.../wsldrived.exe --hvsocket
```

Direction A is also fast over plain loopback TCP and does not require hvsocket.

## Real-world: cloning dotnet/runtime with git

The synthetic numbers above use a generated tree. This one uses a real repository
and the tool developers actually wait on: **dotnet/runtime, 58,082 files, 952 MB**
(shallow clone). Every arm clones from a *local source on ext4*, so the network is
out of the measurement and what is left is the target filesystem. "walk" is
`find <tree> -type f | wc -l`.

**WSL's git writing to a Windows folder** (Direction B, over Hyper-V sockets):

| target | `git clone` | walk |
|--------|------------:|-----:|
| native ext4 (in WSL, reference floor) | 6.2 s | 0.1 s |
| `/mnt/c` (virtiofs) | 272.6 s | 13.1 s |
| **wsldrive** | **91.5 s** (3.0× faster) | **1.4 s** (9.4× faster) |

Targeting the Windows filesystem from WSL costs **44×** on a clone with `/mnt/c`;
wsldrive brings that to ~15×. The walk gap is larger (9.4×) because metadata is
served from the client's RAM mirror, while a clone is dominated by writes, which
are write-through and must cross the boundary either way. Neither approaches
native ext4 — the goal is to beat the boundary path, not to remove the boundary.

**Windows git working on a repo that lives in WSL** (Direction A) — the same
58k-file repo, reached two ways. `git log --oneline -1` is there to show the
fixed per-command overhead:

| access path | walk | `git status` | `git log -1` |
|-------------|-----:|-------------:|-------------:|
| `\\wsl.localhost` (Plan 9) | 28.0 s | 2.2 s | 0.10 s |
| **wsldrive `W:`** | **5.7 s** (4.9×) | **1.1 s** (2.0×) | **0.06 s** (1.7×) |

Enumerating the tree is where the RAM mirror shows most (4.9×); `git status`
also reads content, so it gains less.

### What this test caught

Running a real tool found two defects that the entire unit-test suite missed,
both fixed:

- **`chmod` was not implemented**, so `git init` and `git clone` aborted on any
  mount with `could not set 'core.filemode'`. A git repository could not live on
  a Direction B mount at all.
- **Every file reported `uid 0`**, so git refused to operate on repos there with
  `detected dubious ownership`.

`scripts/fs-conformance.sh` now mounts a filesystem and exercises this class of
operation (create/rename/truncate/chmod/utimens, data integrity, and a real
git add/commit/log/branch cycle) so it cannot regress; CI runs it on every push.

## Cold reads — prefetch-on-mount

Cold reads (the first touch of a file, before it is cached) used to pay the full
boundary fetch. The client now **warms the cache in the background on mount** —
it queues every directory whose small files fit the cache budget, so a tool's
first reads are already hot. A/B on Direction A (loopback TCP, 3000 files,
**first full read pass right after a fresh mount**, median of 5):

| mount | cold first-read |
|-------|----------------:|
| `--no-prefetch` | 585 ms |
| **prefetch-on-mount** (default) | **423 ms** |

That is ~28 % faster, landing near the ~366 ms warm floor — i.e. warm-up removes
most of the cold penalty that sits on top of the fixed per-file mount-serving
cost. It is transport-agnostic (it warms the client cache regardless of
transport) and on by default; disable with `--no-prefetch`. New `Stats`
counters (`read_miss_fetch_ns`, `prefetch_files`, `prefetch_bytes`) expose the
boundary-fetch time and prefetch coverage. Huge trees (small-file bytes beyond
the cache cap) skip warm-up and fall back to lazy per-directory read-ahead;
pipelined `ReadMany` and a persistent on-disk cache are the follow-ups for that
case.

## Performance work landed

- **Prefetch-on-mount**: after the snapshot the client bulk-fetches the tree's
  small files in the background (bounded by the cache budget), so cold reads are
  avoided for typical trees (see above). `--no-prefetch` opts out.

- **Client-side read cache** (`RemoteRoot`): small files cached whole in RAM,
  validated by (mtime, size), LRU-evicted, invalidated on change. Repeat reads
  are served locally with no round-trip.
- **All metadata in RAM** on the accessing side (already in the design): `stat`,
  `readdir`, negative lookups never cross the boundary — the source of the
  Direction A walk win and the Direction B metadata win.
- **Directory read-ahead**: on a read miss, the file's directory is bulk-fetched
  in one `ReadMany` request (with an async prefetcher for large directories), so
  a sequential reader pays one round-trip per directory rather than per file.
