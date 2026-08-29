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
  checks for WinFsp (and can chain-install a downloaded MSI), registers the Hyper-V socket services,
  and creates an at-logon task — the drive is back after a reboot with nothing
  to click. Uninstall removes all of it.
- **Multiple mounts.** `install.ps1 -Config wsldrive.json` sets up several
  drives and distros side by side, each with its own port and logon task.
- **Tested end to end on both platforms.** CI mounts a real filesystem on Linux
  (libfuse3) and a real drive letter on Windows (WinFsp) and runs a conformance
  battery against each — file operations, metadata, names, data integrity, the
  write-back path, changes made behind the mount's back, and a git workload.

### Performance

- Whole-tree metadata mirrored in RAM: `stat`/`readdir`/negative lookups never
  cross the boundary. Kept current by pushed invalidations; when the agent's
  watcher overflows under heavy churn, the client re-fetches the snapshot in the
  background (replaying anything that changed meanwhile) instead of serving
  stale metadata until remount.
- Read cache with directory read-ahead and **prefetch-on-mount** (measured cold
  first-read 585 → 423 ms on the reference tree).
- O(1) LRU eviction, the cached-read copy taken outside the cache lock, and FUSE
  reads served straight into the kernel's buffer.

### Fixed before release (found by the mounted conformance batteries)

- **Renaming a directory lost its contents** in the mirror until remount: the
  client moved only the node, and a directory renamed behind the mount's back
  arrived as a single bare `Upsert`. The client now moves the whole subtree, and
  the agent enumerates a directory that has just appeared so peers see what it
  brought along.
- **`install.ps1 -Uninstall` removed nothing** (a helper was called before it was
  defined, so the script failed at once); the GUI uninstaller shares that path.
  Every `install.ps1` code path now runs in CI as a dry run, and the GUI installer
  is compiled there, so a regression of this kind cannot ship silently again.
- **`git init` on a Windows-served tree aborted about one run in three.** A
  just-written backing file is briefly held open by the search indexer or an
  antivirus scanner, so the rename right behind the write (git's lock-file
  dance) failed with a sharing violation. The agent now retries rename, delete,
  truncate and open on transient share errors for about a second, as Git for
  Windows itself does.

- **From the final pre-release review:** a renamed directory's expansion is now
  split into frames and capped (past 100 000 entries the peers are told to
  rescan) — one huge `mv` could previously produce a frame above the protocol
  limit and drop every mount; the share-violation retry no longer waits on a
  read-only file or directory (a permanent refusal fails fast, ~255 ms budget
  otherwise); snapshot fetches are serialised so a rescan during the initial
  fetch cannot lose invalidations; an unauthenticated peer's frames are capped
  at 4 KiB and must complete within the handshake window, so a half-sent frame
  can no longer pin a session thread and 64 MiB of buffer.

- **Install routes, from the same review:** the release zip now keeps its
  `scripts\` folder (it was flattened, so the documented `scripts\install.ps1`
  did not exist and the Linux agent was not found); the Linux binaries are built
  on Ubuntu 22.04 with static libstdc++ and checked against a glibc ceiling, so
  they load on every distro the README lists; an unknown or stopped distro is
  refused with the list of installed ones instead of producing a task that can
  never mount; a staging copy that fails is an error, not a silent no-op; the
  wizard checks for WinFsp before copying anything and names the log
  (`%TEMP%\wsldrive-install.log`) when `install.ps1` fails; uninstall now ends
  running mounts and removes the staged copies from the distro and
  `%LOCALAPPDATA%\wsldrive`; Direction B warns when the distro lacks `fuse3`.

### Security

- **Peers authenticate** with a per-mount 128-bit shared secret from the platform
  CSPRNG, passed through the environment (never a command line) and compared in
  constant time. `wsldrived` refuses to run unauthenticated unless you pass
  `--insecure-no-auth`.
- Peer paths are confined to the served root: `..`, absolute and drive-letter
  paths are rejected and the resolved path is re-checked against the root.
- Wire-count-driven allocations are bounded, so a malformed frame cannot make
  either side commit gigabytes.
- Agent sessions are capped and reaped, and a peer that does not authenticate
  within 10 s is dropped, so idle connections cannot hold the session slots.
- Release binaries are **unsigned** (SmartScreen will warn); every release
  publishes `SHA256SUMS.txt`.

### Known limitations

- The mount follows a **symlink inside the tree** to its target, even outside the
  served root — so do not serve a tree containing links you would not expose.
- **Symlinks can be read but not created** through the mount: `readlink` and
  following a link work (a relative target resolves on the mount; an absolute
  target names a path on the *serving* side, so from the other OS it dangles),
  but `ln -s` on the mount is not supported yet.
- Hard links are reported as independent files (`st_nlink` is not preserved).
- Names the tree cannot represent — a backslash, which systemd uses in unit
  filenames — are dropped from the mount and reported as `N entries dropped`.
- Direction B needs a one-time elevated Hyper-V socket registration and a single
  `wsl --shutdown`; the installer does both.
- WSL2 only. WSL1 has no VM boundary and is unsupported by design.
