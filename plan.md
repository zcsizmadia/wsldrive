# wsldrive

**One line:** Native-speed file access across the WSL2 boundary in both directions: mount a WSL2 distro path as a real Windows drive letter (primary — Visual Studio, GitHub Desktop, Explorer/Search, Unity, Office on ext4 trees), and a cached view of Windows drives inside WSL (secondary — Linux builds/tools on NTFS trees, shipped only where it measurably beats virtiofs).

## Scope

**WSL2 only.** WSL1 has no VM boundary (DrvFs runs in the NT kernel and `\\wsl$` is served in-process), so neither direction has the problem this project solves; WSL1 is unsupported by design. Requires WSL 2.x on Windows 11.

## Problem

WSL2 crosses the VM boundary in two directions, and they are not in the same state:

| Direction | Today | Outlook |
|---|---|---|
| Linux → Windows (`/mnt/c`) | Plan 9 over hvsocket by default; **virtiofs available (`virtiofs=true` in `.wslconfig`)** and actively optimized by Microsoft (per-device DMA pools, May 2026). "Closed most of the gap." | Being solved upstream. **Secondary target**: cached FUSE view, kept only if benchmarks beat virtiofs; also covers hosts without virtiofs. |
| Windows → Linux (`\\wsl.localhost\<distro>`) | Plan 9 server in the distro + `p9rdr.sys` redirector. Every op is a serialized round trip. VS project open takes minutes; GitHub Desktop 10–20× slower; indexers can crash it. No virtiofs equivalent announced. | **Unserved gap. Primary target.** |

The standard advice ("keep files in ext4, use VS Code Remote-WSL") covers editors that can run their backend in Linux. It does nothing for tools that must run on Windows.

## Why not existing approaches

- **Ext4Windows / lwext4+WinFsp:** mounts the raw ext4 disk. Cannot be used safely while WSL has the VHDX mounted; no coherence with a running distro.
- **Mutagen / Unison / Syncthing / lsyncd:** generic bidirectional sync; duplicate trees, conflict semantics, no WSL-native transport, Go/OCaml runtimes. Mutagen has no first-class WSL transport.
- **`\\wsl.localhost` itself:** correct but slow; we keep it as the coherence source of truth, not as the data path.

## Architecture

Two user-space components, no kernel drivers of our own. Each binary contains both a **server** role (watch + serve the local filesystem) and a **client** role (present a cached view of the remote one); the direction is just which role is active. One protocol, one cache implementation, one ignore/prefetch engine.

### Direction A (primary): ext4 tree → Windows drive letter

### Windows: `wsldrive.exe` (WinFsp filesystem)
- Presents `<letter>:\` (or a mount-point folder) for a configured ext4 root, e.g. `~/src`.
- **Metadata entirely in RAM**: full tree (names, sizes, mtimes, mode→attributes) loaded on mount and kept coherent by invalidations. `stat`/`readdir`/`open`/negative lookups never touch the VM. Negative lookups are cached (Windows tools probe thousands of nonexistent paths).
- **Content cache on NTFS** (`%LOCALAPPDATA%\wsldrive\cache`), content-addressed by BLAKE3. Unchanged files never re-transfer across remounts.
- **Prefetch**: on first access to a directory, stream the whole subtree in bulk (one request, many files), honoring `.gitignore`/`.wsldriveignore`. Configurable depth/size caps.
- **Writes**: write-through by default (durability = ext4). Opt-in per-path **write-back** with a local journal for build outputs / caches.
- **Oplocks / change notification**: WinFsp change notifications so Explorer, VS, and indexers update without polling.

### WSL: `wsldrived` (Linux agent)
- Serves bulk tree snapshots, file content by hash, and applies writes.
- Watches with **fanotify `FAN_MARK_FILESYSTEM` + `FAN_REPORT_DFID_NAME`** (whole-filesystem, no per-directory watch limits; needs root — install as a systemd unit). inotify fallback for non-root.
- Coalesces events (checkout of 10k files → one batched invalidation), pushes over the transport.
- Periodic + on-reconnect full reconcile (scan → hash compare) to recover from missed events.

### Transport
- **Hyper-V sockets** (`AF_HYPERV` on Windows, `AF_VSOCK` in WSL) — this is the one place where latency matters: invalidations must land before the next Windows `stat`. Length-prefixed binary frames, zero-copy where possible, multiplexed streams.
- localhost TCP fallback (works in NAT and mirrored networking modes) for debugging and unusual setups.
- Discovery via `wsl.exe -d <distro> -- wsldrived --handshake` at mount time.

### Coherence model
- Source of truth is ext4. Windows sees ext4 within one invalidation round trip (target < 5 ms).
- Concurrent write to the same file from both sides in write-back mode: last writer wins, the loser is preserved as `name.conflict-<ts>`. Write-through mode has no conflicts (ext4 serializes).

### Semantics mapping (must be documented and tested)
| ext4 | Windows view |
|---|---|
| Case-sensitive names | Case-preserving; colliding names (`Foo`/`foo`) flagged, second entry hidden with a warning |
| Symlinks | Reparse points where target is inside the mount; otherwise plain file containing target (configurable) |
| Mode bits | Synthesized attributes (`READONLY` when no write bit); no ACL emulation in v1 |
| ns mtime | Truncated to 100 ns FILETIME |
| Hard links | Reported as separate files (same content hash) |
| Names invalid on Windows (`:`, `?`, trailing dots) | Escaped (`\u{f03a}`-style like Cygwin/WSL) |
| Unix sockets/FIFOs/devices | Hidden |

### Direction B (secondary): Windows drive → cached view inside WSL

- **WSL: FUSE mount** (e.g. `/w/c` beside `/mnt/c`; libfuse3, low-level API, `direct_io` off, kernel attr/entry caching on). Metadata + negative lookups in RAM; content cache on ext4 under `~/.cache/wsldrive`, BLAKE3-addressed. Reads served locally; writes pass through to NTFS via the Windows agent (write-through), with the same opt-in write-back journal for build outputs.
- **Windows: watcher** — `ReadDirectoryChangesW` on the shared roots, IOCP-driven (WIL/tokio), `FILE_NOTIFY_INFORMATION` coalesced into batched invalidations. Buffer overflow (`ERROR_NOTIFY_ENUM_DIR`) → subtree rescan. USN journal as an optional faster/more robust source on NTFS/ReFS volumes.
- **Same transport and coherence model** as Direction A; source of truth is NTFS.
- **Gate:** ships only if it beats `virtiofs=true` by ≥ 2× on `git status` and incremental build workloads in the benchmark suite. If virtiofs wins, Direction B degrades to "documentation + ignore/prefetch tooling on top of virtiofs" rather than a FUSE layer.
- Additional semantics: NTFS case-insensitivity → exposed case-sensitively as stored (`git config core.ignorecase` guidance); Windows attributes → mode bits (READONLY → 0444), executable bit synthesized from extension list / `.gitattributes`; junctions/reparse points → symlinks where target is inside the mount.

### Optional mirror mode (fallback)
For tools that refuse WinFsp volumes, mirror an ext4 tree one-directionally into a real NTFS folder using the same agent, watcher, and hash index. Same ignore rules. This is the only place the original "sync engine" lives, and it is not the core product.

## Explicit non-goals (v1)
- Replacing `/mnt/c` itself: Direction B mounts alongside it, never over it.
- Bidirectional general-purpose sync.
- io_uring, SQLite, rolling-chunk delta transfer. Source trees are small files; whole-file transfer by hash is enough. Add chunked delta only if large-file workloads demand it.
- Kernel drivers or code signing beyond WinFsp's.

## Technology
- **Language:** Modern C++23 (decided). One shared `core` library (metadata index, protocol, cache, coalescer, ignore rules) that is fully platform-independent and unit-tested; thin platform layers (WinFsp + `ReadDirectoryChangesW`/IOCP + `AF_HYPERV` on Windows; libfuse3 + fanotify + `AF_VSOCK` on Linux). Dependencies via CMake FetchContent with pinned SHA256: BLAKE3 1.5.5, GoogleTest 1.15.2, Google Benchmark 1.9.1 (WinFsp SDK and WIL added when the Windows platform layer lands). No exceptions on hot paths, `std::expected` for errors, `std::span`/`string_view` views over arena-owned data, no per-op heap allocation in lookup/readdir.
- **Build:** CMake ≥ 3.28 presets (`scripts/build.ps1` enters the VS dev shell); MSVC 2022 on Windows, Clang/GCC in WSL; `ctest` for unit tests, Google Benchmark micro-benchmarks + scripted end-to-end workload benchmarks.
- **Packaging:** winget/MSIX for `wsldrive.exe` (bundles WinFsp dependency), `.deb`/tarball + systemd unit for `wsldrived`; `wsldrive install` pushes the agent into the distro automatically.
- **Requirements:** Windows 11 (WinFsp 2.x), WSL 2.x with kernel ≥ 5.9 (fanotify DFID), libfuse3 in the distro for Direction B, Hyper-V socket support (present in all WSL2).

## Performance targets (published benchmarks, measured before/after)
Workloads, each run on `\\wsl.localhost`, `wsldrive`, and native NTFS:
1. Visual Studio: open a 2k-file C# solution, full IntelliSense ready.
2. Git for Windows / GitHub Desktop: `git status` on a 50k-file repo with warm cache.
3. Windows Search indexing of a 100k-file tree.
4. `dir /s` and Explorer folder open on `node_modules`.
5. Write burst: `npm ci` output written from Windows (write-through vs write-back).

Direction B, each run on `/mnt/c` (9P), `/mnt/c` (`virtiofs=true`), `wsldrive` FUSE, and native ext4:
6. `git status` on a 50k-file repo from WSL.
7. `npm ci` / `cargo build` incremental rebuild with sources on NTFS.
8. `find . -type f | wc -l` and `rg` over a 100k-file tree.

Targets: metadata ops within 1.5× the native filesystem on that side; cached reads within 1.2× native; invalidation latency p99 < 5 ms; RSS < 150 MB per side for a 500k-file tree (metadata is the dominant cost; ~8 MB claims are not credible and are dropped). Direction B additionally must beat virtiofs by ≥ 2× on workloads 6–7 to ship.

## Roadmap
1. **Spike (2–3 weeks):** WinFsp read-only mount fed by a full snapshot over TCP; measure VS open and `git status`. Go/no-go on the numbers.
2. **v0.1:** hvsocket transport, fanotify invalidations, content cache, write-through, ignore rules, semantics table implemented + tested.
3. **v0.2:** write-back journal, prefetch heuristics, change notifications, installer, multi-distro/multi-mount.
4. **v0.3 — Direction B spike:** FUSE read cache + `ReadDirectoryChangesW` watcher reusing the v0.1 protocol/cache; benchmark against `virtiofs=true`; ship or shelve on the ≥ 2× gate.
5. **v0.4:** mirror mode, telemetry-free diagnostics (`wsldrive doctor`), CI benchmark suite.

## Risks
- **Microsoft ships a fast Windows→Linux path** (virtiofs-like for `\\wsl.localhost`). Nothing announced; if it happens, mirror mode and the drive-letter/ignore/prefetch UX still have value, but the core advantage shrinks. Mitigate by shipping the spike fast.
- **WinFsp compatibility** with specific tools (installers, some antivirus). Mirror mode is the escape hatch.
- **fanotify requires root** in the distro. inotify fallback with watch-count guidance.
- **Direction B upside shrinks** as virtiofs improves; it is explicitly gated and shares ~80% of its code with Direction A, so the sunk cost if shelved is small.
- **FUSE overhead** in the WSL kernel (context switches per op) may eat the gain for tiny files; mitigate with aggressive kernel attr/entry caching and `FUSE_PASSTHROUGH` (kernel ≥ 6.9) for cached reads.
- **Case/symlink edge cases** breaking git on the Windows side. Covered by the semantics test suite from day one.

## Naming
`wsldrive` is descriptive and searchable ("mount WSL as a drive"); with Direction B the name still fits (a drive on either side). `WSLBridgeFS` collides in spirit with the established `wslbridge` (pty bridge) and a port-forwarding tool of the same name, and "bridge" reads as networking. Alternative brandable name: `Portside`. Availability checked 2026-08-27: no GitHub repository named or mentioning `wsldrive`, crate name free on crates.io; no winget package (checked). Spelling is all-lowercase everywhere: binaries `wsldrive.exe` / `wsldrived`, prose "wsldrive".
