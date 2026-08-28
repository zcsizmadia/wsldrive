<#
.SYNOPSIS
  Interactive installer for wsldrive. Gathers and CONFIRMS everything it needs
  (drive letter, distro, paths, whether to restart WSL), then applies it so that
  after a reboot the drive(s) are mounted automatically with no manual tweaking.

  What it sets up:
    * installs the wsldrive binaries into Program Files
    * checks for WinFsp (Direction A) and offers to chain-install it
    * registers the Hyper-V socket service GUIDs (the fast transport)
    * restarts WSL once (with your confirmation) so it picks up the registration
    * registers an at-logon scheduled task that mounts the drive(s) on every boot
      and re-discovers the dynamic WSL VM GUID each time

  RUN ELEVATED (Administrator). Re-runnable / idempotent.

.EXAMPLE
  # interactive (recommended): asks and confirms each choice
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1

.EXAMPLE
  # unattended: Direction A, map Ubuntu ~ to W: over hvsocket
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Unattended `
    -DriveLetter W -Distro Ubuntu -WslRoot '~' -Yes

.EXAMPLE
  # preview only, change nothing
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -DryRun

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Uninstall
#>
[CmdletBinding()]
param(
  [switch] $Unattended,                       # no prompts; use the values below / defaults
  [switch] $Advanced,                         # also offer Direction B (mount a Windows folder in WSL)
  [switch] $Yes,                              # auto-confirm destructive steps (WSL restart)
  [switch] $DryRun,                           # print the plan, change nothing
  [switch] $Uninstall,                        # remove tasks + binaries + registration

  [string] $InstallDir = "$env:ProgramFiles\wsldrive",
  [string] $BinDir,                           # source of built binaries (auto-detected)

  # Direction A: mount a WSL ext4 tree as a Windows drive letter
  [string] $DriveLetter,                      # e.g. W  (empty in -Unattended => skip Direction A)
  [string] $Distro,                           # WSL distro (default: the WSL default distro)
  [string] $WslRoot = '~',                    # path inside the distro to serve
  [string] $LinuxAgent,                       # Linux wsldrived (agent) to stage into WSL (Direction A)

  # Direction B: mount a Windows path inside WSL
  [string] $WinRoot,                          # e.g. C:\projects  (empty => skip Direction B)
  [string] $Mountpoint = '~/win',             # mount point inside the distro
  [string] $LinuxBin,                         # Linux wsldrive (client) to stage into WSL (Direction B)

  [switch] $NoHvsocket,                       # use loopback TCP instead of Hyper-V sockets
  [switch] $NoShutdown,                        # never run `wsl --shutdown` (installer drives this)
  [string] $WinFspMsi,                        # path to WinFsp MSI to chain-install if missing
  [int]    $FirstPort = 5700,
  [int]    $PortCount = 10
)

$ErrorActionPreference = 'Stop'
$env:WSL_UTF8 = '1'   # make wsl.exe emit UTF-8 instead of UTF-16
$root = Split-Path -Parent $PSScriptRoot
$TaskA = 'wsldrive-mount-A'
$TaskB = 'wsldrive-mount-B'

# ---- tiny UI helpers -------------------------------------------------------
function Say  ($m) { Write-Host $m }
function Ok   ($m) { Write-Host "[ok]   $m"   -ForegroundColor Green }
function Warn ($m) { Write-Host "[warn] $m"   -ForegroundColor Yellow }
function Step ($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Plan ($m) { if ($DryRun) { Write-Host "[dry]  would $m" -ForegroundColor DarkGray } else { Write-Host "       $m" } }

function Ask([string]$prompt, [string]$default) {
  if ($Unattended) { return $default }
  $suffix = if ($default) { " [$default]" } else { "" }
  $a = Read-Host "$prompt$suffix"
  if ([string]::IsNullOrWhiteSpace($a)) { return $default } else { return $a.Trim() }
}
function AskYN([string]$prompt, [bool]$default) {
  if ($Unattended) { return $default }
  $d = if ($default) { 'Y/n' } else { 'y/N' }
  while ($true) {
    $a = (Read-Host "$prompt [$d]").Trim().ToLower()
    if ($a -eq '')             { return $default }
    if ($a -in 'y','yes')      { return $true }
    if ($a -in 'n','no')       { return $false }
  }
}

# ---- environment probes ----------------------------------------------------
function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Test-WinFsp {
  $k = 'HKLM:\SOFTWARE\WOW6432Node\WinFsp'
  (Test-Path $k) -and ($null -ne (Get-ItemProperty -Path $k -Name 'InstallDir' -ErrorAction SilentlyContinue).InstallDir)
}
function Get-DefaultDistro {
  try { $d = (& wsl.exe --status 2>$null | Select-String 'Default Distribution:').ToString() }
  catch { $d = $null }
  if ($d) { return ($d -replace '.*:\s*', '').Trim() }
  try { return ((& wsl.exe -l -q 2>$null) | Where-Object { $_.Trim() } | Select-Object -First 1).Trim() }
  catch { return '' }
}
function Get-FreeDriveLetter {
  foreach ($c in 'W','X','Y','Z','V','U','T') {
    if (-not (Test-Path "${c}:")) { return $c }
  }
  return 'W'
}
# Windows path -> the /mnt/<drive>/... path a WSL process sees.
function To-WslPath([string]$winPath) {
  $p = (Resolve-Path -LiteralPath $winPath -ErrorAction SilentlyContinue)
  $full = if ($p) { $p.Path } else { $winPath }
  if ($full -match '^([A-Za-z]):[\\/](.*)$') {
    return '/mnt/' + $Matches[1].ToLower() + '/' + ($Matches[2] -replace '\\','/')
  }
  return $full
}

# ===========================================================================
Step 'wsldrive installer'

if (-not $DryRun -and -not (Test-Admin)) {
  Warn 'This installer needs Administrator rights (driver, HKLM registry, scheduled task).'
  Warn 'Relaunching elevated...'
  # Rebuild the original invocation (named + switch params) so nothing is lost on relaunch.
  $argline = @('-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
  foreach ($kv in $PSBoundParameters.GetEnumerator()) {
    if ($kv.Value -is [switch]) { if ($kv.Value.IsPresent) { $argline += "-$($kv.Key)" } }
    else { $argline += "-$($kv.Key)"; $argline += "`"$($kv.Value)`"" }
  }
  Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argline
  exit 0
}
if ($DryRun) { Warn 'DRY RUN — nothing will be changed.' }

# ---- uninstall path --------------------------------------------------------
if ($Uninstall) {
  Step 'Uninstalling'
  foreach ($t in $TaskA, $TaskB) {
    if (Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue) {
      Plan "remove scheduled task $t"
      if (-not $DryRun) { Unregister-ScheduledTask -TaskName $t -Confirm:$false }
    }
  }
  Plan "unregister hvsocket service GUIDs (ports $FirstPort..$($FirstPort+$PortCount-1))"
  if (-not $DryRun) { & "$PSScriptRoot\register-hvsocket.ps1" -FirstPort $FirstPort -Count $PortCount -Unregister | Out-Null }
  if (Test-Path $InstallDir) {
    Plan "delete $InstallDir"
    if (-not $DryRun) { Remove-Item -Recurse -Force $InstallDir }
  }
  Ok 'Uninstalled. (WinFsp was left installed; remove it from Apps if you no longer need it.)'
  Warn 'Any mount running right now stays up until you end its process (Ctrl+C in its window) or reboot.'
  exit 0
}

# ---- locate the built binaries --------------------------------------------
if (-not $BinDir) {
  foreach ($c in 'msvc-release','msvc-relwithdebinfo','msvc-debug') {
    $p = Join-Path $root "build\$c\src\tools"
    if (Test-Path (Join-Path $p 'wsldrive.exe')) { $BinDir = $p; break }
  }
}
$srcCli      = if ($BinDir) { Join-Path $BinDir 'wsldrive.exe' }  else { '' }
$srcAgent    = if ($BinDir) { Join-Path $BinDir 'wsldrived.exe' } else { '' }
$srcLauncher = if ($BinDir) { Join-Path $BinDir 'wsldrivew.exe' } else { '' }  # windowless launcher (optional)
if (-not (Test-Path $srcCli) -or -not (Test-Path $srcAgent)) {
  throw "Could not find wsldrive.exe / wsldrived.exe. Build first (.\scripts\build.ps1) or pass -BinDir."
}
$linuxCli   = if ($LinuxBin)   { $LinuxBin }   else { Join-Path $root 'build\linux-release\src\tools\wsldrive' }   # Direction B client
$linuxAgent = if ($LinuxAgent) { $LinuxAgent } else { Join-Path $root 'build\linux-release\src\tools\wsldrived' }  # Direction A agent

# Copy a Windows-side Linux binary into the distro at an absolute path under the
# user's home, and return that WSL path. (Absolute, not ~, because the agent path
# is passed through single-quoting downstream where ~ would not expand.)
function Stage-Into-Wsl([string]$srcWin, [string]$name, [string]$wslHome) {
  $staged = "$env:LOCALAPPDATA\wsldrive\$name"
  New-Item -ItemType Directory -Force -Path (Split-Path $staged) | Out-Null
  Copy-Item -Force $srcWin $staged
  $srcWsl = To-WslPath $staged
  $dest = "$wslHome/.local/bin/$name"
  & wsl.exe -d $distro -- bash -lc "mkdir -p '$wslHome/.local/bin' && cp '$srcWsl' '$dest' && chmod +x '$dest'" 2>$null
  return $dest
}

# ===========================================================================
# 1. Gather + confirm configuration
# ===========================================================================
Step 'Configuration'

$useHv = -not $NoHvsocket
$distro = if ($Distro) { $Distro } else { Get-DefaultDistro }

# Direction A
$doA = $false; $letter = ''
if ($Unattended) {
  $doA = [bool]$DriveLetter
  $letter = ($DriveLetter -replace ':','').ToUpper()
} else {
  $doA = AskYN 'Mount a WSL folder as a Windows drive letter (Direction A)?' $true
  if ($doA) {
    $defLetter = if ($DriveLetter) { ($DriveLetter -replace ':','').ToUpper() } else { Get-FreeDriveLetter }
    $letter = (Ask 'Drive letter' $defLetter).TrimEnd(':').ToUpper()
    $distro = Ask 'WSL distro to serve from' $distro
    $WslRoot = Ask 'Folder inside the distro to expose' $WslRoot
  }
}

# Direction B (advanced): mounts a Windows folder *inside WSL* (a Linux path, not
# a drive letter). Only offered in advanced mode; easy mode is Direction A only.
$doB = $false
if ($Unattended) {
  $doB = [bool]$WinRoot   # passing -WinRoot opts into Direction B
} elseif ($Advanced -or $WinRoot) {
  $doB = AskYN 'Advanced: also mount a Windows folder inside WSL (Direction B)?' ([bool]$WinRoot)
  if ($doB) {
    $WinRoot = Ask 'Windows folder to expose (e.g. C:\projects)' $WinRoot
    $Mountpoint = Ask 'Mount point inside the distro' $Mountpoint
    if (-not $distro) { $distro = Ask 'WSL distro to mount into' (Get-DefaultDistro) }
  }
}
if ($doA -and -not (Test-Path $linuxAgent)) {
  Warn "Direction A needs the Linux build of the agent (wsldrived) at:`n         $linuxAgent"
  Warn "Build it in WSL:  cmake --preset linux-release && cmake --build --preset linux-release"
  if (-not (AskYN 'Continue and set up Direction A anyway (the task will fail until it exists)?' $false)) { $doA = $false }
}
if ($doB -and -not (Test-Path $linuxCli)) {
  Warn "Direction B needs the Linux build of wsldrive at:`n         $linuxCli"
  Warn "Build it in WSL:  cmake --preset linux-release && cmake --build --preset linux-release"
  if (-not (AskYN 'Continue and set up Direction B anyway (the task will fail until it exists)?' $false)) { $doB = $false }
}

if (-not $doA -and -not $doB) { Warn 'Nothing selected to mount. Exiting.'; exit 0 }

# Absolute WSL home (agent/client are staged under it); resolved once here.
$wslHome = if ($distro) { (& wsl.exe -d $distro -- bash -lc 'echo $HOME' 2>$null).Trim() } else { '' }

# Expand a leading ~ in the served WSL root to an absolute path: it is
# single-quoted when passed to the agent, so ~ would not expand in the distro
# shell and the agent would try to open a literal "~" directory and exit.
if ($doA -and $wslHome) {
  if ($WslRoot -eq '~') { $WslRoot = $wslHome }
  elseif ($WslRoot -like '~/*') { $WslRoot = $wslHome + $WslRoot.Substring(1) }
}

# WSL restart is only needed the first time we register hvsocket services, which
# only happens for Direction B (Direction A uses loopback TCP).
$needShutdown = $useHv -and $doB
$agentWslPath = To-WslPath (Join-Path $InstallDir 'wsldrived.exe')  # Windows agent (Direction B), seen from WSL

# ---- confirmation summary --------------------------------------------------
Step 'Review — this is everything the installer will do'
Say  "  install dir      : $InstallDir"
Say  "  binaries         : $srcCli"
if ($doA) {
  Say "  Direction A      : ${letter}:  <=  $distro : $WslRoot   (loopback TCP, auto-start '$TaskA')"
}
if ($doB) {
  Say "  Direction B      : $distro : $Mountpoint  <=  $WinRoot   ($(if($useHv){'hvsocket'}else{'TCP'}), auto-start '$TaskB')"
  if ($useHv) { Say "  hvsocket ports   : $FirstPort..$($FirstPort+$PortCount-1)  (HKLM registration)" }
}
if (-not (Test-WinFsp)) { Say "  WinFsp           : NOT installed -> $(if ($WinFspMsi) {'will chain-install'} else {'you will be prompted'})" }
if ($needShutdown) { Say "  WSL restart      : REQUIRED once (closes running WSL sessions) so it picks up the socket registration" }
Say  ""

if (-not $DryRun -and -not $Unattended) {
  if (-not (AskYN 'Proceed with the above?' $true)) { Warn 'Aborted; nothing changed.'; exit 0 }
}

# ===========================================================================
# 2. Apply
# ===========================================================================

# --- WinFsp (Direction A only) ---
if ($doA) {
  Step 'WinFsp'
  if (Test-WinFsp) {
    Ok 'WinFsp runtime already installed.'
  } elseif ($WinFspMsi -and (Test-Path $WinFspMsi)) {
    Plan "chain-install WinFsp from $WinFspMsi"
    if (-not $DryRun) { Start-Process msiexec.exe -Wait -ArgumentList '/i',"`"$WinFspMsi`"",'/qn' }
    if (-not $DryRun -and -not (Test-WinFsp)) { throw 'WinFsp install did not complete.' }
    Ok 'WinFsp installed.'
  } else {
    Warn 'WinFsp is required for Direction A and is not installed.'
    Warn 'Install it from https://winfsp.dev (or re-run with -WinFspMsi <path>), then run this installer again.'
    if (-not (AskYN 'Skip Direction A for now and continue?' $false)) { throw 'WinFsp missing.' }
    $doA = $false
  }
}

# --- copy binaries (a no-op when a packaged installer already placed them here) ---
Step 'Install binaries'
$sameDir = (Test-Path $InstallDir) -and ((Resolve-Path $BinDir).Path -eq (Resolve-Path $InstallDir).Path)
if ($sameDir) {
  Ok "Binaries already in place ($InstallDir)."
} else {
  Plan "copy wsldrive.exe / wsldrived.exe / wsldrivew.exe -> $InstallDir"
  if (-not $DryRun) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -Force $srcCli, $srcAgent $InstallDir
    if (Test-Path $srcLauncher) { Copy-Item -Force $srcLauncher $InstallDir }
  }
}
# Stage the Linux binaries into the distro. Direction A needs the agent
# (wsldrived) to serve the WSL tree; Direction B needs the client (wsldrive).
$agentInWsl = ''  # absolute path of the staged Linux agent, for the Direction A task
if ($doA) {
  Plan "stage Linux wsldrived into ${distro}:$wslHome/.local/bin/wsldrived"
  if (-not $DryRun -and (Test-Path $linuxAgent)) { $agentInWsl = Stage-Into-Wsl $linuxAgent 'wsldrived' $wslHome }
}
if ($doB) {
  Plan "stage Linux wsldrive into ${distro}:$wslHome/.local/bin/wsldrive"
  if (-not $DryRun -and (Test-Path $linuxCli)) { [void](Stage-Into-Wsl $linuxCli 'wsldrive' $wslHome) }
}
Ok 'Binaries installed.'

# --- hvsocket registration (Direction B only; Direction A uses loopback TCP) ---
if ($useHv -and $doB) {
  Step 'Hyper-V socket registration'
  Plan "register service GUIDs for ports $FirstPort..$($FirstPort+$PortCount-1)"
  if (-not $DryRun) { & "$PSScriptRoot\register-hvsocket.ps1" -FirstPort $FirstPort -Count $PortCount | Out-Null }
  Ok 'hvsocket services registered.'
}

# --- scheduled tasks (auto-start at logon) ---
# Direction A maps a drive letter, which must be visible to the user's normal
# (non-elevated) apps, so its task runs at the user's normal level (Limited).
# Direction B mounts inside WSL and needs Hyper-V admin for VM-GUID discovery, so
# its task runs Highest — elevation there does not affect any drive letter.
function Register-MountTask([string]$name, [string]$exe, [string]$argline, [bool]$Elevated) {
  # Run through the windowless launcher so no console window appears at logon.
  # (The mount process runs for the life of the mount; a visible console could be
  # closed by accident, which would unmount the drive.) Fall back to direct exec
  # if the launcher isn't present.
  $launcher = Join-Path $InstallDir 'wsldrivew.exe'
  if (Test-Path $launcher) {
    $taskExe = $launcher
    $taskArg = ('"{0}" {1}' -f $exe, $argline)
  } else {
    $taskExe = $exe
    $taskArg = $argline
  }
  Plan "register logon task '$name' ($(if($Elevated){'elevated'}else{'normal'}), windowless): $taskExe $taskArg"
  if ($DryRun) { return }
  $action    = New-ScheduledTaskAction -Execute $taskExe -Argument $taskArg
  $trigger   = New-ScheduledTaskTrigger -AtLogOn
  $principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) `
                 -LogonType Interactive -RunLevel $(if ($Elevated) { 'Highest' } else { 'Limited' })
  $settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                 -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
  Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Force | Out-Null
}

Step 'Auto-start'
if ($doA) {
  # Direction A is already fast over loopback TCP and needs no VM GUID, so it does
  # not use hvsocket (which would force an elevated, non-visible drive). Non-elevated.
  $exe = Join-Path $InstallDir 'wsldrive.exe'
  $agentArg = if ($agentInWsl) { " --agent $agentInWsl" } elseif ($wslHome) { " --agent $wslHome/.local/bin/wsldrived" } else { '' }
  $arg = "mount ${letter}: --distro $distro --wsl-root $WslRoot$agentArg"
  Register-MountTask $TaskA $exe $arg $false
  Ok "Direction A will mount ${letter}: at logon (loopback TCP)."
}
if ($doB) {
  # Runs the Linux client inside WSL; it launches the Windows agent over interop.
  $hvArgB = if ($useHv) { ' --hvsocket' } else { '' }
  $winRootFwd = $WinRoot -replace '\\','/'
  $bcmd = "~/.local/bin/wsldrive mount $Mountpoint --win-root '$winRootFwd' --win-agent '$agentWslPath'$hvArgB"
  $arg  = "-d $distro -- bash -lc `"mkdir -p $Mountpoint; $bcmd`""
  Register-MountTask $TaskB 'wsl.exe' $arg $true
  Ok "Direction B will mount $Mountpoint at logon$(if($useHv){' (hvsocket)'}else{' (TCP)'})."
}

# --- WSL restart ---
if ($needShutdown) {
  Step 'Restart WSL'
  $go = if ($NoShutdown) { $false } else { $Yes -or (AskYN 'Restart WSL now so it picks up the socket registration? (closes running WSL sessions)' $true) }
  if ($go) {
    Plan 'wsl --shutdown'
    if (-not $DryRun) { & wsl.exe --shutdown }
    Ok 'WSL restarted.'
  } else {
    Warn 'Skipped. Run "wsl --shutdown" yourself before the fast transport will work.'
  }
}

# ===========================================================================
Step 'Done'
Ok 'wsldrive is installed and will mount automatically at logon.'
if (-not $DryRun) {
  Say 'Start now without logging off:'
  if ($doA) { Say "    Start-ScheduledTask -TaskName $TaskA" }
  if ($doB) { Say "    Start-ScheduledTask -TaskName $TaskB" }
  Say 'Verify the environment:  wsldrive doctor'
  Say "Uninstall:               powershell -File scripts\install.ps1 -Uninstall"
}
