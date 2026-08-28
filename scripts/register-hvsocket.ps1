<#
.SYNOPSIS
  Register wsldrive's Hyper-V socket service GUIDs so a WSL2 guest can reach the
  Windows host over AF_VSOCK / AF_HYPERV (bypassing the TCP/localhost relay).

  WSL2 only routes guest<->host hvsocket connections whose service GUID is listed
  under GuestCommunicationServices. A vsock port P maps to the template GUID
  {PPPPPPPP-facb-11e6-bd58-64006a7986d3} (P in hex). This registers a small port
  range once; the wsldrive installer will do this in the future.

  RUN ELEVATED (Administrator). Re-runnable/idempotent. Use -Unregister to remove.

.EXAMPLE
  # from an elevated PowerShell:
  powershell -ExecutionPolicy Bypass -File scripts\register-hvsocket.ps1
#>
[CmdletBinding()]
param(
  [int] $FirstPort = 5700,
  [int] $Count = 10,
  [switch] $Unregister
)

$ErrorActionPreference = 'Stop'
$key = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Error "Run this from an elevated (Administrator) PowerShell."
  exit 1
}

foreach ($port in $FirstPort..($FirstPort + $Count - 1)) {
  $guid = ('{0:X8}-facb-11e6-bd58-64006a7986d3' -f $port)
  $path = Join-Path $key $guid
  if ($Unregister) {
    Remove-Item -Path $path -Force -ErrorAction SilentlyContinue
    Write-Host "unregistered port $port ($guid)"
  } else {
    New-Item -Path $path -Force | Out-Null
    Set-ItemProperty -Path $path -Name 'ElementName' -Value "wsldrive-$port"
    Write-Host "registered port $port -> $guid"
  }
}
Write-Host "done. (no WSL restart needed)"
