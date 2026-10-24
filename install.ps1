# Install opfsolve (compiled ExaModelsPower AC OPF models + the IDAES build of
# Ipopt) on Windows and solve AC OPF cases with it. In PowerShell:
#
#   irm https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.ps1 | iex
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.ps1))) case2000_goc --form=rect
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.ps1))) all
#
# Arguments are passed to opfsolve (see `opfsolve --help`); with none, it solves
# pglib_opf_case118_ieee. Pass --install-only to stop after installing.
#
# Environment:
#   OPFSOLVE_DIR      install directory (default .\opfsolve)
#   OPFSOLVE_VERSION  release tag (default: latest)
#   OPFSOLVE_ARCHIVE  local release archive to install instead of downloading one
#   IDAES_VERSION     idaes-ext release tag (default 3.4.2)

# Native tools report through $LASTEXITCODE; with 'Stop', PowerShell 5 would
# turn their stderr output into terminating errors.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$Repo = 'madsuite-org/opfsolve'
$Dir = if ($env:OPFSOLVE_DIR) { $env:OPFSOLVE_DIR } else { Join-Path (Get-Location) 'opfsolve' }
$Version = if ($env:OPFSOLVE_VERSION) { $env:OPFSOLVE_VERSION } else { 'latest' }
$IdaesVersion = if ($env:IDAES_VERSION) { $env:IDAES_VERSION } else { '3.4.2' }

function Say($msg) { Write-Host "==> $msg" }
# `throw`, not `exit`: under `irm | iex`, exit would close the PowerShell window.
function Fail($msg) { throw "opfsolve install: $msg" }

foreach ($tool in 'curl.exe', 'tar.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool is required (Windows 10 1803 or newer)" }
}

$Opts = @($args)
$InstallOnly = $false
if ($Opts.Count -gt 0 -and $Opts[0] -eq '--install-only') {
    $InstallOnly = $true
    $Opts = if ($Opts.Count -gt 1) { $Opts[1..($Opts.Count - 1)] } else { @() }
}
if ($Opts.Count -eq 0 -and -not $InstallOnly) { $Opts = @('case118_ieee') }

$arch = $env:PROCESSOR_ARCHITECTURE
if ($arch -ne 'AMD64') { Fail "unsupported architecture: $arch (Windows x86_64 only)" }
$Plat = 'windows-x86_64'
$IPlat = 'windows-x86_64'
Say "platform: $Plat"
Say "install directory: $Dir"
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
$Exe = Join-Path $Dir 'bin\opfsolve.exe'

if (-not (Test-Path $Exe)) {
    $tgz = Join-Path $Dir "opfsolve-$Plat.tar.gz"
    if ($env:OPFSOLVE_ARCHIVE) {
        Say "installing opfsolve from $env:OPFSOLVE_ARCHIVE"
        Copy-Item $env:OPFSOLVE_ARCHIVE $tgz
    } else {
        $url = if ($Version -eq 'latest') { "https://github.com/$Repo/releases/latest/download/opfsolve-$Plat.tar.gz" }
               else { "https://github.com/$Repo/releases/download/$Version/opfsolve-$Plat.tar.gz" }
        Say "downloading opfsolve ($Version): $url"
        curl.exe -fSL --progress-bar -o $tgz $url
        if ($LASTEXITCODE -ne 0) { Fail "no opfsolve release for $Plat at $url" }
    }
    tar.exe -xzf $tgz -C $Dir --strip-components 1
    if ($LASTEXITCODE -ne 0) { Fail "could not unpack $tgz" }
    Remove-Item $tgz
    Say "installed $Exe and the compiled model library $(Join-Path $Dir 'emp')"
} else {
    Say "opfsolve already installed: $(& $Exe --version)"
}

$IpoptDir = Join-Path $Dir 'ipopt'
$IpoptLib = Join-Path $IpoptDir 'libipopt-3.dll'
if (-not (Test-Path $IpoptLib)) {
    $iurl = "https://github.com/IDAES/idaes-ext/releases/download/$IdaesVersion/idaes-solvers-$IPlat.tar.gz"
    Say "downloading Ipopt (IDAES $IdaesVersion build with HSL MA27): $iurl"
    New-Item -ItemType Directory -Force -Path $IpoptDir | Out-Null
    $itgz = Join-Path $IpoptDir 'idaes-solvers.tar.gz'
    curl.exe -fSL --progress-bar -o $itgz $iurl
    if ($LASTEXITCODE -ne 0) { Fail "no IDAES Ipopt build for $IPlat" }
    tar.exe -xzf $itgz -C $IpoptDir 2>$null
    Remove-Item $itgz
    if (-not (Test-Path $IpoptLib)) { Fail "libipopt-3.dll not found in the IDAES archive" }
    "idaes-ext $IdaesVersion $IPlat" | Set-Content (Join-Path $IpoptDir 'SOURCE')
    Say "installed $IpoptLib"
} else {
    Say "Ipopt already installed: $(Get-Content (Join-Path $IpoptDir 'SOURCE') -ErrorAction SilentlyContinue)"
}

Say "run it again with: $Exe <case>... [options]   (see --help)"
if ($InstallOnly) { return }

# Output directory, as opfsolve will choose it, for the machine description.
$Out = 'opfsolve-results'
for ($i = 0; $i -lt $Opts.Count; $i++) {
    if ($Opts[$i] -like '--output=*') { $Out = $Opts[$i].Substring(9) }
    elseif ($Opts[$i] -eq '--output' -and $i + 1 -lt $Opts.Count) { $Out = $Opts[$i + 1] }
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$os = Get-CimInstance Win32_OperatingSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
@(
    "date: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))",
    "os: $($os.Caption) $($os.Version)",
    "cpu: $($cpu.Name)",
    "cores: $($cpu.NumberOfCores)",
    "opfsolve: $(& $Exe --version)",
    "ipopt: $(Get-Content (Join-Path $IpoptDir 'SOURCE') -ErrorAction SilentlyContinue)",
    "command: opfsolve $($Opts -join ' ')"
) | Set-Content (Join-Path $Out 'system.txt')

Say "running: opfsolve $($Opts -join ' ')"
& $Exe @Opts 2>&1 | Tee-Object -FilePath (Join-Path $Out 'opfsolve.log')
$rc = $LASTEXITCODE
Say "results in ${Out}: summary.csv, <case>\solution.json, <case>\ipopt.log, opfsolve.log, system.txt"
$global:LASTEXITCODE = $rc
