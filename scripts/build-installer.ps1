<#
.SYNOPSIS
  Compile the wsldrive GUI installer (installer\wsldrive.iss) with Inno Setup.

  Requires the Windows + Linux binaries to be built first:
    .\scripts\build.ps1                                         # wsldrive.exe / wsldrived.exe
    wsl:  cmake --preset linux-release && cmake --build --preset linux-release   # Linux client (Direction B)

  Locates ISCC.exe (the Inno Setup compiler); if missing, offers to install it
  via winget. Output: installer\dist\wsldrive-setup.exe

.EXAMPLE
  .\scripts\build-installer.ps1
  .\scripts\build-installer.ps1 -Version 0.2.0
#>
[CmdletBinding()]
param(
  [string] $Config  = 'release',
  # Defaults to the project version in CMakeLists.txt. Hard-coding it here is
  # how the script came to stamp 0.1.0 onto a 0.9.0 build.
  [string] $Version = '',
  [switch] $InstallInno
)

$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent $PSScriptRoot

if (-not $Version) {
  $cml = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
  if ($cml -match '(?m)^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$') { $Version = $Matches[1] }
  else { throw "could not read VERSION from CMakeLists.txt; pass -Version explicitly" }
}
Write-Host "building installer for wsldrive $Version"
$iss     = Join-Path $root 'installer\wsldrive.iss'
$binDir  = Join-Path $root "build\msvc-$Config\src\tools"
$linux   = Join-Path $root 'build\linux-release\src\tools\wsldrive'    # client (Direction B)
$linuxAgent = Join-Path $root 'build\linux-release\src\tools\wsldrived' # agent (Direction A)

if (-not (Test-Path (Join-Path $binDir 'wsldrive.exe'))) {
  throw "wsldrive.exe not found in $binDir. Build first: .\scripts\build.ps1 -Config $Config"
}
if (-not (Test-Path $linux)) {
  Write-Host "[warn] Linux client not found ($linux); the installer will omit Direction B support." -ForegroundColor Yellow
}
if (-not (Test-Path $linuxAgent)) {
  Write-Host "[warn] Linux agent not found ($linuxAgent); the installer will omit Direction A auto-launch." -ForegroundColor Yellow
}

function Find-Iscc {
  $c = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
         Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($c) { return $c }
  $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  return $null
}

$iscc = Find-Iscc
if (-not $iscc) {
  if ($InstallInno) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
      throw "winget not available. Install Inno Setup manually from https://jrsoftware.org/isdl.php"
    }
    Write-Host "Installing Inno Setup via winget..." -ForegroundColor Cyan
    winget install --id JRSoftware.InnoSetup -e --accept-source-agreements --accept-package-agreements
    $iscc = Find-Iscc
  }
  if (-not $iscc) {
    throw "Inno Setup (ISCC.exe) not found. Re-run with -InstallInno, or install it from https://jrsoftware.org/isdl.php"
  }
}
Write-Host "[ok]   ISCC: $iscc" -ForegroundColor Green

$out = Join-Path $root 'installer\dist'
New-Item -ItemType Directory -Force -Path $out | Out-Null

& $iscc `
  "/DBinDir=$binDir" `
  "/DLinuxBin=$linux" `
  "/DLinuxAgent=$linuxAgent" `
  "/DAppVersion=$Version" `
  $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

Write-Host "[ok]   Built $out\wsldrive-setup.exe" -ForegroundColor Green
