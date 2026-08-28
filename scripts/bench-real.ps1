<#
.SYNOPSIS
  Real-world cross-boundary benchmark for wsldrive (see bench/real_world.md).

  Direction A: Windows accessing a WSL2 ext4 tree
      native (in WSL, reference)  vs  \\wsl.localhost (Plan 9)  vs  wsldrive drive
  Direction B: WSL accessing a Windows NTFS tree
      native (on Windows, reference)  vs  /mnt/c  vs  wsldrive FUSE mount

  Workloads: walk (enumerate all files), read (read every file's bytes).
  Reports the median wall-clock (ms) of several warm runs. All WSL-side work is
  in scripts/bench/*.sh to avoid shell-quoting pitfalls.
#>
[CmdletBinding()]
param(
  [string] $Distro = 'Ubuntu',
  [int] $Files = 3000,
  [int] $Runs = 5,
  # Keep ports outside Windows' reserved TCP ranges (see
  # `netsh interface ipv4 show excludedportrange protocol=tcp`); WSL2's NAT
  # honours them, so a reserved port fails to bind.
  [int] $PortA = 51888,
  [int] $PortB = 51890
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$winExe   = Join-Path $root 'build\msvc-release\src\tools\wsldrive.exe'
$winAgent = Join-Path $root 'build\msvc-release\src\tools\wsldrived.exe'
$linAgent = '/mnt/c/OpenSource/wsldrive/build/linux-release/src/tools/wsldrived'
$linCli   = '/mnt/c/OpenSource/wsldrive/build/linux-release/src/tools/wsldrive'
$bench    = '/mnt/c/OpenSource/wsldrive/scripts/bench'

function Wsl([string[]]$argv) { & wsl.exe -d $Distro -- @argv }
function WslSh([string]$script, [string[]]$a) { (& wsl.exe -d $Distro -- bash "$bench/$script" @a) }

$wslHome = (Wsl @('bash','-c','echo $HOME')).Trim()
$winTree = Join-Path $env:LOCALAPPDATA 'wsldrive-bench'   # NTFS, but not under C:\ root
$winTreeWsl = '/mnt/c' + ($winTree.Substring(2) -replace '\\','/')  # /mnt/c/... path for WSL

function Median([double[]]$xs) { $s = @($xs | Sort-Object); $s[[int]([math]::Floor($s.Count/2))] }

function Win-Walk([string]$p) { (Measure-Command { [void]([System.IO.Directory]::EnumerateFiles($p,'*','AllDirectories') | Measure-Object).Count }).TotalMilliseconds }
function Win-Read([string]$p) { (Measure-Command { $s=0L; foreach ($f in [System.IO.Directory]::EnumerateFiles($p,'*','AllDirectories')) { $s += [System.IO.File]::ReadAllBytes($f).Length } }).TotalMilliseconds }

function Med-Win([scriptblock]$fn, [string]$arg) { & $fn $arg | Out-Null; $t=@(); for($i=0;$i -lt $Runs;$i++){$t+=(& $fn $arg)}; [math]::Round((Median $t),1) }
function Med-Wsl([string]$script, [string]$p)     { WslSh $script @($p) | Out-Null; $t=@(); for($i=0;$i -lt $Runs;$i++){$t+=[double](WslSh $script @($p))}; [math]::Round((Median $t),1) }

$rows = @()
function Row($d,$s,$w,$r) { $script:rows += [pscustomobject]@{Direction=$d; Scenario=$s; walk_ms=$w; read_ms=$r} }

Write-Host "Generating trees ($Files files)..." -ForegroundColor Cyan
$nWsl = (WslSh 'gen-tree.sh' @("$wslHome/wsldrive-bench", "$Files")).Trim()
$curWin = 0
if (Test-Path $winTree) { $curWin = ([System.IO.Directory]::EnumerateFiles($winTree,'*','AllDirectories')|Measure-Object).Count }
if ($curWin -ne $Files) {
  Remove-Item -Recurse -Force $winTree -ErrorAction SilentlyContinue
  $per=[math]::Ceiling($Files/60)
  for($d=1;$d -le 60;$d++){ $dd=Join-Path $winTree "dir$d"; New-Item -ItemType Directory -Force $dd|Out-Null; for($f=1;$f -le $per;$f++){ [System.IO.File]::WriteAllText((Join-Path $dd "file$f.txt"), ([guid]::NewGuid().ToString()*40)) } }
}
$nWin = ([System.IO.Directory]::EnumerateFiles($winTree,'*','AllDirectories')|Measure-Object).Count
Write-Host "  WSL tree: $nWsl files;  Windows tree: $nWin files"

# ---- Direction A: Windows -> WSL ext4 ----
Write-Host "`n=== Direction A: Windows accessing WSL2 ext4 ===" -ForegroundColor Green
Row 'A' 'native (in WSL, ref)' (Med-Wsl 'walk.sh' "$wslHome/wsldrive-bench") (Med-Wsl 'read.sh' "$wslHome/wsldrive-bench")

$unc = "\\wsl.localhost\$Distro" + ($wslHome -replace '/','\') + "\wsldrive-bench"
if (Test-Path $unc) { Row 'A' '\\wsl.localhost (9P)' (Med-Win ${function:Win-Walk} $unc) (Med-Win ${function:Win-Read} $unc) }
else { Row 'A' '\\wsl.localhost (9P)' 'N/A' 'N/A' }

$mp = Start-Process $winExe -ArgumentList "mount","X:","--distro",$Distro,"--wsl-root","$wslHome/wsldrive-bench","--agent",$linAgent,"--port",$PortA -PassThru -NoNewWindow -RedirectStandardOutput "$env:TEMP\benchA.out"
$ok=$false; for($i=0;$i -lt 60;$i++){Start-Sleep -Milliseconds 300; if(Test-Path X:\){$ok=$true;break}}
if($ok){ Row 'A' 'wsldrive drive' (Med-Win ${function:Win-Walk} 'X:\') (Med-Win ${function:Win-Read} 'X:\') }
else   { Row 'A' 'wsldrive drive' 'MOUNT FAILED' 'MOUNT FAILED'; Get-Content "$env:TEMP\benchA.out" -EA SilentlyContinue }
Stop-Process -Id $mp.Id -Force -EA SilentlyContinue; Start-Sleep -Seconds 1

# ---- Direction B: WSL -> Windows NTFS ----
Write-Host "=== Direction B: WSL accessing Windows NTFS ===" -ForegroundColor Green
Row 'B' 'native (on Win, ref)' (Med-Win ${function:Win-Walk} $winTree) (Med-Win ${function:Win-Read} $winTree)
Row 'B' '/mnt/c (9P/virtiofs)' (Med-Wsl 'walk.sh' $winTreeWsl) (Med-Wsl 'read.sh' $winTreeWsl)

# Reverse roles: the WSL client listens and the Windows agent dials in, so the
# connection is Windows->WSL (localhost forwarding) and needs no firewall change.
# The mount runs under a persistent wsl.exe process so its listener stays up.
$mountProc = Start-Process wsl.exe -ArgumentList '-d',$Distro,'--',$linCli,'mount','/tmp/benchB','--listen',"tcp://0.0.0.0:$PortB" -PassThru -NoNewWindow -RedirectStandardOutput "$env:TEMP\benchBm.out" -RedirectStandardError "$env:TEMP\benchBm.err"
Start-Sleep -Milliseconds 1200  # let the WSL listener bind
$agent = Start-Process $winAgent -ArgumentList "--root",$winTree,"--connect","tcp://127.0.0.1:$PortB","--exit-when-idle" -PassThru -NoNewWindow -RedirectStandardError "$env:TEMP\benchB.err"
$up=$false; for($i=0;$i -lt 40;$i++){ Start-Sleep -Milliseconds 300; if((WslSh 'is-mounted.sh' @()).Trim() -eq 'yes'){$up=$true;break} }
if($up){
  Row 'B' 'wsldrive FUSE mount' (Med-Wsl 'walk.sh' '/tmp/benchB') (Med-Wsl 'read.sh' '/tmp/benchB')
  Wsl @('bash','-c','fusermount3 -u /tmp/benchB 2>/dev/null; true') | Out-Null
} else {
  Row 'B' 'wsldrive FUSE mount' 'N/A' 'N/A'
  Write-Host "  (mount did not come up; check $env:TEMP\benchBm.err)" -ForegroundColor Yellow
}
Stop-Process -Id $agent.Id -Force -EA SilentlyContinue
Stop-Process -Id $mountProc.Id -Force -EA SilentlyContinue
Wsl @('bash','-c','fusermount3 -u /tmp/benchB 2>/dev/null; true') | Out-Null

Write-Host "`n===== RESULTS (median of $Runs warm runs, ms; lower is better) =====" -ForegroundColor Cyan
$rows | Format-Table -AutoSize
