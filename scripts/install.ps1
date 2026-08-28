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
  [switch] $Yes,                              # auto-confirm destructive steps (WSL restart)
  [switch] $DryRun,                           # print the plan, change nothing
  [switch] $Uninstall,                        # remove tasks + binaries + registration

  [string] $InstallDir = "$env:ProgramFiles\wsldrive",
  [string] $BinDir,                           # source of built binaries (auto-detected)

  # Direction A: mount a WSL ext4 tree as a Windows drive letter
  [string] $DriveLetter,                      # e.g. W  (empty in -Unattended => skip Direction A)
  [string] $Distro,                           # WSL distro (default: the WSL default distro)
  [string] $WslRoot = '~',                    # path inside the distro to serve

  # Direction B: mount a Windows path inside WSL
  [string] $WinRoot,                          # e.g. C:\projects  (empty => skip Direction B)
  [string] $Mountpoint = '~/win',             # mount point inside the distro
  [string] $LinuxBin,                         # Linux wsldrive binary to stage into WSL (Direction B)

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
$srcCli   = if ($BinDir) { Join-Path $BinDir 'wsldrive.exe' }  else { '' }
$srcAgent = if ($BinDir) { Join-Path $BinDir 'wsldrived.exe' } else { '' }
if (-not (Test-Path $srcCli) -or -not (Test-Path $srcAgent)) {
  throw "Could not find wsldrive.exe / wsldrived.exe. Build first (.\scripts\build.ps1) or pass -BinDir."
}
$linuxCli = if ($LinuxBin) { $LinuxBin } else { Join-Path $root 'build\linux-release\src\tools\wsldrive' }  # Direction B

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

# Direction B
$doB = $false
if ($Unattended) {
  $doB = [bool]$WinRoot
} else {
  $doB = AskYN 'Mount a Windows folder inside WSL (Direction B)?' ([bool]$WinRoot)
  if ($doB) {
    $WinRoot = Ask 'Windows folder to expose (e.g. C:\projects)' $WinRoot
    $Mountpoint = Ask 'Mount point inside the distro' $Mountpoint
    if (-not $distro) { $distro = Ask 'WSL distro to mount into' (Get-DefaultDistro) }
  }
}
if ($doB -and -not (Test-Path $linuxCli)) {
  Warn "Direction B needs the Linux build of wsldrive at:`n         $linuxCli"
  Warn "Build it in WSL:  cmake --preset linux-release && cmake --build --preset linux-release"
  if (-not (AskYN 'Continue and set up Direction B anyway (the task will fail until it exists)?' $false)) { $doB = $false }
}

if (-not $doA -and -not $doB) { Warn 'Nothing selected to mount. Exiting.'; exit 0 }

# WSL restart is only needed the first time we register hvsocket services.
$needShutdown = $useHv
$agentWslPath = To-WslPath (Join-Path $InstallDir 'wsldrived.exe')

# ---- confirmation summary --------------------------------------------------
Step 'Review — this is everything the installer will do'
Say  "  install dir      : $InstallDir"
Say  "  binaries         : $srcCli"
Say  "  transport        : $(if ($useHv) {'Hyper-V sockets (fast)'} else {'loopback TCP'})"
if ($useHv) { Say "  hvsocket ports   : $FirstPort..$($FirstPort+$PortCount-1)  (HKLM registration)" }
if ($doA) {
  Say "  Direction A      : ${letter}:  <=  $distro : $WslRoot"
  Say "                     auto-start at logon (task '$TaskA')"
}
if ($doB) {
  Say "  Direction B      : $distro : $Mountpoint  <=  $WinRoot"
  Say "                     auto-start at logon (task '$TaskB')"
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
  Plan "copy wsldrive.exe / wsldrived.exe -> $InstallDir"
  if (-not $DryRun) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -Force $srcCli, $srcAgent $InstallDir
  }
}
if ($doB) {
  $linuxDest = "$env:LOCALAPPDATA\wsldrive\wsldrive-linux"   # staged where WSL can read it
  Plan "stage Linux wsldrive -> $linuxDest and copy into ${distro}:~/.local/bin/wsldrive"
  if (-not $DryRun -and (Test-Path $linuxCli)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $linuxDest) | Out-Null
    Copy-Item -Force $linuxCli $linuxDest
    $linuxSrcWsl = To-WslPath $linuxDest
    & wsl.exe -d $distro -- bash -lc "mkdir -p ~/.local/bin && cp '$linuxSrcWsl' ~/.local/bin/wsldrive && chmod +x ~/.local/bin/wsldrive" 2>$null
  }
}
Ok 'Binaries installed.'

# --- hvsocket registration ---
if ($useHv) {
  Step 'Hyper-V socket registration'
  Plan "register service GUIDs for ports $FirstPort..$($FirstPort+$PortCount-1)"
  if (-not $DryRun) { & "$PSScriptRoot\register-hvsocket.ps1" -FirstPort $FirstPort -Count $PortCount | Out-Null }
  Ok 'hvsocket services registered.'
}

# --- scheduled tasks (auto-start at logon) ---
function Register-MountTask([string]$name, [string]$exe, [string]$argline) {
  Plan "register logon task '$name': $exe $argline"
  if ($DryRun) { return }
  $action    = New-ScheduledTaskAction -Execute $exe -Argument $argline
  $trigger   = New-ScheduledTaskTrigger -AtLogOn
  $principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) `
                 -LogonType Interactive -RunLevel Highest
  $settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                 -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
  Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Force | Out-Null
}

Step 'Auto-start'
$hvArg = if ($useHv) { ' --hvsocket' } else { '' }
if ($doA) {
  $exe = Join-Path $InstallDir 'wsldrive.exe'
  $arg = "mount ${letter}: --distro $distro --wsl-root $WslRoot$hvArg"
  Register-MountTask $TaskA $exe $arg
  Ok "Direction A will mount ${letter}: at logon."
}
if ($doB) {
  # Runs the Linux client inside WSL; it launches the Windows agent over interop.
  $winRootFwd = $WinRoot -replace '\\','/'
  $bcmd = "~/.local/bin/wsldrive mount $Mountpoint --win-root '$winRootFwd' --win-agent '$agentWslPath'$hvArg"
  $arg  = "-d $distro -- bash -lc `"mkdir -p $Mountpoint; $bcmd`""
  Register-MountTask $TaskB 'wsl.exe' $arg
  Ok "Direction B will mount $Mountpoint at logon."
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
