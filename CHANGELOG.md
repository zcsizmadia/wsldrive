# Changelog

## v0.9.0 — first public release

Native-speed file access across the WSL2 boundary, in both directions, with an
installer that leaves you with a drive that comes back after every reboot.

### Highlights

- **Direction A — a WSL folder as a Windows drive letter** (WinFsp). Serve your
  home directory or the whole distro (`--wsl-root /`). ~2.7× faster metadata and
  ~5–8× faster reads than `\\wsl.localhost` on the reference workload; cold reads
  are close to warm thanks to prefetch-on-mount.
- **Direction B — a Windows folder mounted inside WSL** (libfuse3), over
  Hyper-V sockets: ~10× faster than `/mnt/c` on both 9P and virtiofs.
- **Installer.** A GUI wizard (`wsldrive-setup.exe`) and a PowerShell installer
  that it drives, so both routes do the same thing. It installs the binaries,
  checks for (or chain-installs) WinFsp, registers the Hyper-V socket services,
  and creates an at-logon task — the drive is back after a reboot with nothing
  to click. Uninstall removes all of it.
- **Multiple mounts.** `install.ps1 -Config wsldrive.json` sets up several
  drives and distros side by side, each with its own port and logon task.

### Performance

- Whole-tree metadata mirrored in RAM: `stat`/`readdir`/negative lookups never
  cross the boundary.
- Read cache with directory read-ahead and **prefetch-on-mount** (measured cold
  first-read 585 → 423 ms on the reference tree).
- O(1) LRU eviction, the cached-read copy taken outside the cache lock, and FUSE
  reads served straight into the kernel's buffer.

### Security

- **Peers authenticate** with a per-mount 128-bit shared secret from the platform
  CSPRNG, passed through the environment (never a command line) and compared in
  constant time. `wsldrived` refuses to run unauthenticated unless you pass
  `--insecure-no-auth`.
- Peer paths are confined to the served root: `..`, absolute and drive-letter
  paths are rejected and the resolved path is re-checked against the root.
- Wire-count-driven allocations are bounded, so a malformed frame cannot make
  either side commit gigabytes.
- Agent sessions are capped and reaped.
- Release binaries are **unsigned** (SmartScreen will warn); every release
  publishes `SHA256SUMS.txt`.

### Known limitations

- The mount follows a **symlink inside the tree** to its target, even outside the
  served root — so do not serve a tree containing links you would not expose.
- **Symlinks are listed but cannot be followed through the mount.** They are
  reported as links, but there is no `readlink` support yet, so opening one from
  the other side fails. A tree full of symlinks will show them as broken.
- **A watcher overflow leaves metadata stale until remount.** Under very heavy
  churn the agent's coalescer emits a single "rescan" signal instead of
  individual events; the client does not yet act on it, so the in-RAM mirror can
  keep serving stale metadata for the affected paths until the mount restarts.
- Hard links are reported as independent files (`st_nlink` is not preserved).
- Names the tree cannot represent — a backslash, which systemd uses in unit
  filenames — are dropped from the mount and reported as `N entries dropped`.
- Direction B needs a one-time elevated Hyper-V socket registration and a single
  `wsl --shutdown`; the installer does both.
- WSL2 only. WSL1 has no VM boundary and is unsupported by design.
