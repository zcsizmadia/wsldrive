# Real-world cross-boundary benchmark

Measures the two things wsldrive exists to speed up, against the OS's own
cross-boundary path and the native baseline:

- **Direction A** — Windows accessing a **WSL2 ext4** tree:
  `\\wsl.localhost` (Plan 9)  vs  `wsldrive` drive  vs  native ext4 (in WSL).
- **Direction B** — WSL accessing a **Windows NTFS** tree:
  `/mnt/c` (9P/virtiofs)  vs  `wsldrive` FUSE mount  vs  native NTFS (on Windows).

Workloads (metadata-heavy is where the in-RAM mirror wins; read-heavy is where
the read cache wins):

1. **walk**  — `find <tree> -type f | wc -l` (stat storm / directory traversal)
2. **read**  — read every file's bytes (throughput + per-file open overhead)
3. **grep**  — search all files for a token (read + metadata combined)

Numbers are steady-state (warm), median of several runs — that is what a
developer actually experiences. See `scripts/bench-real.ps1`.
