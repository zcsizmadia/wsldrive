<#
.SYNOPSIS
  Install a pinned chocolatey package, retrying transient failures.

  Two reasons this exists.

  Supply chain: every GitHub action in these workflows is SHA-pinned, but the
  WinFsp SDK and the Inno Setup compiler were pulled at whatever version was
  current. wsldrive.exe links against the downloaded winfsp-x64.lib and
  wsldrive-setup.exe is produced by the downloaded compiler, so a hijacked or
  simply broken package lands straight inside a release artifact. Pin the
  versions here and bump them deliberately.

  Flakiness: choco is the least reliable step in both workflows, and a network
  blip failed the whole run.

.EXAMPLE
  pwsh scripts/ci/choco-install.ps1 -Package winfsp
  pwsh scripts/ci/choco-install.ps1 -Package innosetup -Version 6.7.1
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string] $Package,
  # Defaults to the pin below. Pass explicitly only to test a different one.
  [string] $Version,
  [int]    $Retries = 3
)

$ErrorActionPreference = 'Continue'

# The pinned toolchain. One place to bump, used by ci.yml and release.yml alike.
# Last verified green: winfsp 2.1.25156, innosetup 6.7.1 (2026-09-05).
$pinned = @{
  winfsp    = '2.1.25156'
  innosetup = '6.7.1'
}

if (-not $Version) {
  if (-not $pinned.ContainsKey($Package)) {
    throw "no pinned version for '$Package'; add one to scripts/ci/choco-install.ps1 or pass -Version"
  }
  $Version = $pinned[$Package]
}

for ($attempt = 1; $attempt -le $Retries; $attempt++) {
  choco install $Package --version $Version -y --no-progress
  # 3010 is "installed, reboot pending", which is a success for our purposes.
  if ($LASTEXITCODE -eq 3010) { $global:LASTEXITCODE = 0 }
  if ($LASTEXITCODE -eq 0) {
    Write-Host "$Package $Version installed (attempt $attempt)"
    exit 0
  }
  Write-Host "::warning::$Package $Version install attempt $attempt failed (exit $LASTEXITCODE)"
  if ($attempt -lt $Retries) { Start-Sleep -Seconds (5 * $attempt) }
}

throw "$Package $Version failed to install after $Retries attempts"
