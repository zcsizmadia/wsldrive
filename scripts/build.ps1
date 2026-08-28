<#
.SYNOPSIS
  Configure, build, test and optionally benchmark wsldrive with MSVC + Ninja.

.EXAMPLE
  .\scripts\build.ps1                    # RelWithDebInfo build + tests
  .\scripts\build.ps1 -Config debug      # Debug build + tests
  .\scripts\build.ps1 -Bench             # also run micro-benchmarks
  .\scripts\build.ps1 -NoTests -Clean    # wipe the build dir first, skip tests
#>
[CmdletBinding()]
param(
  [ValidateSet('debug', 'release')] [string] $Config = 'release',
  [switch] $NoTests,
  [switch] $Bench,
  [string] $BenchFilter = '',
  [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$preset = "msvc-$Config"

# Locate Visual Studio and enter its x64 developer environment.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; is Visual Studio 2022 installed?" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with the C++ toolset was found." }
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

Push-Location $root
try {
  if ($Clean) { Remove-Item -Recurse -Force (Join-Path $root "build\$preset") -ErrorAction SilentlyContinue }
  cmake --preset $preset
  if ($LASTEXITCODE -ne 0) { throw "configure failed" }
  cmake --build --preset $preset --parallel
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
  if (-not $NoTests) {
    ctest --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
  }
  if ($Bench) {
    $exe = Join-Path $root "build\$preset\bench\core_bench.exe"
    $args = @('--benchmark_min_time=0.2s')
    if ($BenchFilter) { $args += "--benchmark_filter=$BenchFilter" }
    & $exe @args
  }
}
finally {
  Pop-Location
}
