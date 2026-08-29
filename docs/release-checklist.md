# Release checklist

CI covers what can run unattended: the C++ suites, a mounted filesystem battery on
Linux (libfuse3) and on Windows (WinFsp), every `install.ps1` code path in
`-DryRun`, and compiling the GUI installer. What it **cannot** do is an elevated
install with a real WSL distro on a machine with a desktop session. That part is a
short manual pass, done once per release on a Windows machine with WSL2:

1. **Build a release candidate** — run the *Release* workflow manually
   (`workflow_dispatch`, version `x.y.z-rc`) and download `wsldrive-release-assets`.
2. **GUI route.** Run `wsldrive-setup.exe`, accept the defaults (Direction A, `W:`).
   - The drive appears in a *non-elevated* Explorer/terminal within a few seconds.
   - `wsldrive doctor` passes.
   - Reboot. The drive is back without touching anything.
3. **Uninstall from Add/Remove Programs.** Afterwards, all of these are gone:
   `Get-ScheduledTask wsldrive-mount-*`, `C:\Program Files\wsldrive`, and the
   `HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices`
   entries for ports 5700–5709.
4. **Script route.** Unpack `wsldrive-windows-x64.zip`, run
   `scripts\install.ps1 -Unattended -DriveLetter W -Distro <name> -WslRoot '~' -Yes`
   from an elevated prompt; same three checks as step 2, then `install.ps1 -Uninstall`.
5. **Advanced (Direction B), if it changed:** `install.ps1 -Advanced`, mount a Windows
   folder at `~/win`, confirm `ls ~/win` inside the distro after the WSL restart.
6. **Checksums.** `SHA256SUMS.txt` matches the downloaded files.

Then tag `vx.y.z`; the Release workflow publishes the assets.
