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
