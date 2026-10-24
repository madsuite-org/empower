# Install opfsolve (compiled ExaModelsPower AC OPF models) with Ipopt from
# conda-forge on Windows, optionally build the user's own HSL MA27, and solve AC
# OPF cases. In PowerShell:
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
#   OPFSOLVE_HSL      your HSL download link (from HSL's email) or the downloaded
#                     .tar.gz/.zip, to compile it, optimized, on this machine
#   OPFSOLVE_HSL_LIB  an HSL library you already have, used as is
#   OPFSOLVE_SKIP_HSL=1  install without HSL (for testing only)
#
# HSL MA27 is required, and opfsolve never distributes it. Request the free
# personal licence at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/; HSL emails
# a download link, which the script asks for if OPFSOLVE_HSL is not set.

# Native tools report through $LASTEXITCODE; with 'Stop', PowerShell 5 would
# turn their stderr output into terminating errors.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$Repo = 'madsuite-org/opfsolve'
$Dir = if ($env:OPFSOLVE_DIR) { $env:OPFSOLVE_DIR } else { Join-Path (Get-Location) 'opfsolve' }
$Version = if ($env:OPFSOLVE_VERSION) { $env:OPFSOLVE_VERSION } else { 'latest' }
$IpoptSpec = 'ipopt=3.14.20'
$MambaVersion = '2.9.0-0'
$Ma27Url = 'https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/'

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

# --- conda-forge tools (micromamba) ---
$MambaDir = Join-Path $Dir '.mamba'
$Mamba = Join-Path $MambaDir 'micromamba.exe'
$env:MAMBA_ROOT_PREFIX = Join-Path $MambaDir 'root'
function Invoke-Mamba {
    if (-not (Test-Path $Mamba)) {
        $murl = "https://github.com/mamba-org/micromamba-releases/releases/download/$MambaVersion/micromamba-win-64.exe"
        Say "downloading micromamba $MambaVersion (conda-forge package manager): $murl"
        New-Item -ItemType Directory -Force -Path $MambaDir | Out-Null
        curl.exe -fSL --progress-bar -o $Mamba $murl
        if ($LASTEXITCODE -ne 0) { Fail 'could not download micromamba' }
    }
    & $Mamba @args
}

# --- Ipopt from conda-forge ---
$IpoptDir = Join-Path $Dir 'ipopt'
$IpoptLib = Join-Path $IpoptDir 'Library\bin\ipopt-3.dll'
if (-not (Test-Path $IpoptLib)) {
    Say "installing Ipopt from conda-forge ($IpoptSpec, with MUMPS and OpenBLAS)"
    Invoke-Mamba create -y -q -p $IpoptDir -c conda-forge --override-channels $IpoptSpec | Out-Null
    if (-not (Test-Path $IpoptLib)) { Fail 'could not install Ipopt from conda-forge' }
    "conda-forge $IpoptSpec" | Set-Content (Join-Path $IpoptDir 'SOURCE')
    Say "installed $IpoptLib"
} else {
    Say "Ipopt already installed: $(Get-Content (Join-Path $IpoptDir 'SOURCE') -ErrorAction SilentlyContinue)"
}

# --- HSL ---
$HslDir = Join-Path $Dir 'hsl'
$HslLib = Join-Path $HslDir 'libhsl.dll'

# Compile the user's own HSL package, optimized for this machine. HSL's personal
# licence permits modifying the software only to optimise its performance for
# the licensee's use on a particular platform; the result stays on this machine.
function Build-Hsl($src) {
    $work = Join-Path $HslDir 'build'
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    if ($src -match '^https?://') {
        if ($src -notmatch '\.(tar\.gz|tgz|zip)$') { $src = $src.TrimEnd('/') + '/package.tar.gz' }
        Say 'downloading your HSL package'
        $pkg = Join-Path $work 'pkg'
        curl.exe -fsSL -o $pkg $src
        if ($LASTEXITCODE -ne 0) { Fail 'could not download the HSL package (download links expire after 7 days)' }
    } else {
        if (-not (Test-Path $src)) { Fail "HSL package not found: $src" }
        $pkg = $src
    }
    $magic = [System.IO.File]::ReadAllBytes($pkg)[0..1]
    if ($magic[0] -eq 0x1f -and $magic[1] -eq 0x8b) { tar.exe -xzf $pkg -C $work }
    elseif ($magic[0] -eq 0x50 -and $magic[1] -eq 0x4b) { Expand-Archive -Path $pkg -DestinationPath $work }
    else { Fail 'the HSL package is neither .tar.gz nor .zip (an expired or already used download link returns a web page)' }
    $top = Get-ChildItem -Directory $work | Where-Object { $_.Name -ne 'tools' } | Select-Object -First 1
    $ma27 = Join-Path $top.FullName 'src\ma27d.f'
    if (-not (Test-Path $ma27)) { Fail 'unrecognised HSL package: expected the MA27 package (src\ma27d.f)' }
    Say 'installing a Fortran compiler from conda-forge to build HSL'
    $tools = Join-Path $work 'tools'
    Invoke-Mamba create -y -q -p $tools -c conda-forge --override-channels gfortran | Out-Null
    $fc = Get-ChildItem -Recurse -Path $tools -Filter '*gfortran.exe' | Select-Object -First 1
    if (-not $fc) { Fail 'could not install the Fortran compiler' }
    Say "compiling $($top.Name) (double precision) with -O3"
    # A static Fortran runtime, so the DLL needs nothing from the compiler environment.
    & $fc.FullName -O3 -shared -static-libgfortran -static-libgcc -static -o $HslLib $ma27
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $HslLib)) { Fail 'compiling MA27 failed' }
    "built from $($top.Name) on $((Get-Date).ToString('yyyy-MM-dd')) with $((& $fc.FullName --version)[0])" | Set-Content (Join-Path $HslDir 'SOURCE')
    Remove-Item -Recurse -Force $work
    # Drop the compiler's packages from micromamba's cache; Ipopt keeps what it uses.
    Invoke-Mamba clean -a -y -q | Out-Null
    Say "installed $HslLib"
}

New-Item -ItemType Directory -Force -Path $HslDir | Out-Null
if ($env:OPFSOLVE_HSL_LIB) {
    if (-not (Test-Path $env:OPFSOLVE_HSL_LIB)) { Fail "OPFSOLVE_HSL_LIB not found: $env:OPFSOLVE_HSL_LIB" }
    Copy-Item $env:OPFSOLVE_HSL_LIB $HslLib -Force
    "your library $env:OPFSOLVE_HSL_LIB" | Set-Content (Join-Path $HslDir 'SOURCE')
    Say "using your HSL library $env:OPFSOLVE_HSL_LIB"
} elseif ($env:OPFSOLVE_HSL) {
    Build-Hsl $env:OPFSOLVE_HSL
} elseif (Test-Path $HslLib) {
    Say "HSL: $(Get-Content (Join-Path $HslDir 'SOURCE') -ErrorAction SilentlyContinue)"
} elseif ($env:OPFSOLVE_SKIP_HSL -eq '1') {
    Say 'OPFSOLVE_SKIP_HSL=1: installing without HSL (testing only)'
} else {
    Say "opfsolve needs HSL MA27, which you download yourself under HSL's free personal licence:"
    Say "  1. request it at $Ma27Url"
    Say '  2. HSL emails you a download link (valid for 7 days)'
    $link = ''
    if ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
        $link = Read-Host '==> paste the download link (or a path to the package or an HSL library)'
    }
    if (-not $link) { Fail "no HSL download link. Rerun with `$env:OPFSOLVE_HSL='<download link from the email>'" }
    if ($link -match '\.dll$') {
        if (-not (Test-Path $link)) { Fail "HSL library not found: $link" }
        Copy-Item $link $HslLib -Force
        "your library $link" | Set-Content (Join-Path $HslDir 'SOURCE')
        Say "using your HSL library $link"
    } else {
        Build-Hsl $link
    }
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
    "hsl: $(if (Test-Path $HslLib) { Get-Content (Join-Path $HslDir 'SOURCE') -ErrorAction SilentlyContinue } else { 'none' })",
    "command: opfsolve $($Opts -join ' ')"
) | Set-Content (Join-Path $Out 'system.txt')

Say "running: opfsolve $($Opts -join ' ')"
& $Exe @Opts 2>&1 | Tee-Object -FilePath (Join-Path $Out 'opfsolve.log')
$rc = $LASTEXITCODE
Say "results in ${Out}: summary.csv, <case>\solution.json, <case>\ipopt.log, opfsolve.log, system.txt"
$global:LASTEXITCODE = $rc
