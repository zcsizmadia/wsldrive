<#
.SYNOPSIS
  Build wsldrive-windows-x64.zip - the "script route" download - with the layout
  install.ps1 expects: the binaries at the root and the scripts under scripts\.

  Compress-Archive flattens a list of file paths, which is how the first release
  candidate shipped a zip with install.ps1 at the root and no scripts\ folder -
  exactly the layout README documents did not exist. Building a staging tree and
  zipping THAT keeps the folder; release.yml and the CI installer job both call
  this, so CI tests the zip a release actually produces.

.EXAMPLE
  .\scripts\package-zip.ps1 -WinBin build\msvc-release\src\tools -LinuxBin build\linux-release\src\tools -Out staging\wsldrive-windows-x64.zip
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string] $WinBin,    # folder with wsldrive.exe, wsldrived.exe, wsldrivew.exe
  [Parameter(Mandatory)][string] $LinuxBin,  # folder with wsldrive and wsldrived (Linux builds)
  [Parameter(Mandatory)][string] $Out        # zip to write
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stage = Join-Path ([IO.Path]::GetTempPath()) "wsldrive-zip-$PID"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$stage\scripts" | Out-Null

foreach ($f in 'wsldrive.exe', 'wsldrived.exe', 'wsldrivew.exe') {
  $p = Join-Path $WinBin $f
  if (-not (Test-Path $p)) { throw "missing Windows binary: $p" }
  Copy-Item $p $stage\
}
foreach ($pair in @(@('wsldrive', 'wsldrive-linux-x64'), @('wsldrived', 'wsldrived-linux-x64'))) {
  $p = Join-Path $LinuxBin $pair[0]
  if (-not (Test-Path $p)) { throw "missing Linux binary: $p (install.ps1 needs it to set up either direction)" }
  Copy-Item $p (Join-Path $stage $pair[1])
}
Copy-Item (Join-Path $root 'scripts\install.ps1'), (Join-Path $root 'scripts\register-hvsocket.ps1') "$stage\scripts\"
Copy-Item (Join-Path $root 'wsldrive.example.json'), (Join-Path $root 'README.md'), (Join-Path $root 'CHANGELOG.md'), (Join-Path $root 'LICENSE') $stage\

New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Resolve-Path -LiteralPath (Split-Path -Parent $Out) -ErrorAction SilentlyContinue) ) -ErrorAction SilentlyContinue | Out-Null
if (Test-Path $Out) { Remove-Item -Force $Out }
# Zip the CONTENTS of the staging dir (a directory entry keeps its subfolders).
Compress-Archive -Path "$stage\*" -DestinationPath $Out
Remove-Item -Recurse -Force $stage

# Prove the layout before anyone downloads it.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$entries = [IO.Compression.ZipFile]::OpenRead((Resolve-Path $Out).Path).Entries.FullName
foreach ($must in 'scripts/install.ps1', 'scripts/register-hvsocket.ps1', 'wsldrive.exe', 'wsldrived.exe', 'wsldrivew.exe', 'wsldrive-linux-x64', 'wsldrived-linux-x64', 'wsldrive.example.json', 'README.md', 'LICENSE') {
  if ($entries -notcontains $must) { throw "zip is missing $must (entries: $($entries -join ', '))" }
}
Write-Host "[ok]   $Out ($($entries.Count) entries, scripts\ folder present)"
