# Install the opfsolve command on Windows: compiled ExaModelsPower AC OPF models,
# Ipopt from conda-forge, and the user's own HSL MA27 compiled on this machine.
# In PowerShell:
#
#   irm https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.ps1 | iex
#
# Then run `opfsolve <case>...` (see `opfsolve --help`).
#
# Like juliaup, it installs into ~\.opfsolve, adds ~\.opfsolve\bin to the user
# PATH (and to this session), and asks for confirmation unless OPFSOLVE_YES=1.
#
# Environment:
#   OPFSOLVE_YES=1          accept the defaults without asking
#   OPFSOLVE_DIR            install directory (default ~\.opfsolve)
#   OPFSOLVE_ADD_TO_PATH    yes (default) or no
#   OPFSOLVE_HSL            your HSL download link (from HSL's email) or the
#                           downloaded .tar.gz/.zip, compiled on this machine
#   OPFSOLVE_HSL_LIB        an HSL library you already have, used as is
#   OPFSOLVE_VERSION        release tag (default: latest)
#   OPFSOLVE_ARCHIVE        local release archive to install instead of downloading one
#   OPFSOLVE_SKIP_HSL=1     install without HSL (for testing only)
#
# HSL MA27 is required, and opfsolve never distributes it. Request the free
# personal licence at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/; HSL emails
# a download link, which the script asks for if OPFSOLVE_HSL is not set.

# Native tools report through $LASTEXITCODE; with 'Stop', PowerShell 5 would
# turn their stderr output into terminating errors.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$Repo = 'madsuite-org/opfsolve'
$Dir = if ($env:OPFSOLVE_DIR) { $env:OPFSOLVE_DIR } else { Join-Path $HOME '.opfsolve' }
$AddToPath = if ($env:OPFSOLVE_ADD_TO_PATH) { $env:OPFSOLVE_ADD_TO_PATH } else { 'yes' }
$Interactive = [Environment]::UserInteractive -and -not [Console]::IsInputRedirected
$Version = if ($env:OPFSOLVE_VERSION) { $env:OPFSOLVE_VERSION } else { 'latest' }
$IpoptSpec = 'ipopt=3.14.20'
$MambaVersion = '2.9.0-0'
$Ma27Url = 'https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/'

function Say($msg) { Write-Host "    $msg" }
function Section($msg) { Write-Host ''; Write-Host $msg -ForegroundColor Cyan }
# `throw`, not `exit`: under `irm | iex`, exit would close the PowerShell window.
function Fail($msg) { throw "opfsolve install: $msg" }

foreach ($tool in 'curl.exe', 'tar.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool is required (Windows 10 1803 or newer)" }
}


$arch = $env:PROCESSOR_ARCHITECTURE
if ($arch -ne 'AMD64') { Fail "unsupported architecture: $arch (Windows x86_64 only)" }
$Plat = 'windows-x86_64'
Write-Host 'opfsolve installer' -ForegroundColor Cyan
Say "platform:      $Plat"
Say "install into:  $Dir"
Say "add to PATH:   $AddToPath ($Dir\bin, in the user PATH)"
if ($env:OPFSOLVE_YES -ne '1') {
    if (-not $Interactive) { Fail 'unable to ask for confirmation; set $env:OPFSOLVE_YES=1 to accept the defaults' }
    $answer = Read-Host '    proceed? [Y/n]'
    if ($answer -and $answer -notmatch '^(y|yes)$') { Fail 'cancelled' }
}
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
$Exe = Join-Path $Dir 'bin\opfsolve.exe'
$HslDir = Join-Path $Dir 'hsl'
$HslLib = Join-Path $HslDir 'libhsl.dll'
$Work = Join-Path $HslDir 'build'

# --- conda-forge tools (micromamba) ---
$MambaDir = Join-Path $Dir '.mamba'
$Mamba = Join-Path $MambaDir 'micromamba.exe'
$env:MAMBA_ROOT_PREFIX = Join-Path $MambaDir 'root'
function Get-Mamba {
    if (Test-Path $Mamba) { return }
    New-Item -ItemType Directory -Force -Path $MambaDir | Out-Null
    curl.exe -fsSL -o $Mamba "https://github.com/mamba-org/micromamba-releases/releases/download/$MambaVersion/micromamba-win-64.exe"
    if ($LASTEXITCODE -ne 0) { Fail 'could not download micromamba' }
}
function Invoke-Mamba { Get-Mamba; & $Mamba @args }

# --- 1. opfsolve ---
Section '[1/5] opfsolve'
if (-not (Test-Path $Exe)) {
    $tgz = Join-Path $Dir "opfsolve-$Plat.tar.gz"
    if ($env:OPFSOLVE_ARCHIVE) {
        Say "from $env:OPFSOLVE_ARCHIVE"
        Copy-Item $env:OPFSOLVE_ARCHIVE $tgz
    } else {
        $url = if ($Version -eq 'latest') { "https://github.com/$Repo/releases/latest/download/opfsolve-$Plat.tar.gz" }
               else { "https://github.com/$Repo/releases/download/$Version/opfsolve-$Plat.tar.gz" }
        Say "downloading $url"
        curl.exe -fsSL -o $tgz $url
        if ($LASTEXITCODE -ne 0) { Fail "no opfsolve release for $Plat at $url" }
    }
    tar.exe -xzf $tgz -C $Dir --strip-components 1
    if ($LASTEXITCODE -ne 0) { Fail "could not unpack $tgz" }
    Remove-Item $tgz
}
Say "$(& $Exe --version) and its compiled model library"

# --- 2. HSL source: asked for first, so its compiler downloads alongside Ipopt ---
# Compile the user's own HSL package, optimized for this machine. HSL's personal
# licence permits modifying the software only to optimise its performance for
# the licensee's use on a particular platform; the result stays on this machine.
Section '[2/5] HSL MA27 package'
$HslMode = ''; $HslSrc = ''; $UserLib = ''
if ($env:OPFSOLVE_HSL_LIB) { $HslMode = 'lib'; $UserLib = $env:OPFSOLVE_HSL_LIB; Say "your library $UserLib" }
elseif ($env:OPFSOLVE_HSL) { $HslMode = 'build'; $HslSrc = $env:OPFSOLVE_HSL }
elseif (Test-Path $HslLib) { $HslMode = 'keep'; Say 'already installed' }
elseif ($env:OPFSOLVE_SKIP_HSL -eq '1') { $HslMode = 'skip'; Say 'skipped (OPFSOLVE_SKIP_HSL=1)' }
else {
    Say "opfsolve needs HSL MA27, which you download yourself under HSL's free personal licence:"
    Say "  1. request it at $Ma27Url"
    Say '  2. HSL emails you a download link (valid for 7 days)'
    $link = ''
    if ($Interactive) {
        try { Start-Process $Ma27Url; Say 'opened the request page in your browser' } catch { }
        $link = Read-Host '    paste the download link (or a path to the package or an HSL library)'
    }
    if (-not $link) { Fail "no HSL download link. Rerun with `$env:OPFSOLVE_HSL='<download link from the email>'" }
    if ($link -match '\.dll$') { $HslMode = 'lib'; $UserLib = $link } else { $HslMode = 'build'; $HslSrc = $link }
}

$Fc = $null; $ToolsProc = $null; $Ma27 = $null
function Start-Tools {
    Get-Mamba
    Say 'compiler: downloading gfortran from conda-forge, alongside Ipopt'
    # Its own package cache, so it does not wait on Ipopt's.
    $env:MAMBA_ROOT_PREFIX = Join-Path $Work 'mamba-root'
    $proc = Start-Process -FilePath $Mamba -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $Work 'tools.log') -RedirectStandardError (Join-Path $Work 'tools.err') `
        -ArgumentList @('create', '-y', '-q', '-p', (Join-Path $Work 'tools'), '-c', 'conda-forge', '--override-channels', 'gfortran')
    $env:MAMBA_ROOT_PREFIX = Join-Path $MambaDir 'root'
    return $proc
}
if ($HslMode -eq 'build') {
    Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $Work | Out-Null
    if ($HslSrc -match '^https?://') {
        if ($HslSrc -notmatch '\.(tar\.gz|tgz|zip)$') { $HslSrc = $HslSrc.TrimEnd('/') + '/package.tar.gz' }
        Say 'downloading your HSL package'
        $pkg = Join-Path $Work 'pkg'
        curl.exe -fsSL -o $pkg $HslSrc
        if ($LASTEXITCODE -ne 0) { Fail 'could not download the HSL package (download links expire after 7 days)' }
    } else {
        if (-not (Test-Path $HslSrc)) { Fail "HSL package not found: $HslSrc" }
        $pkg = $HslSrc
    }
    $magic = [System.IO.File]::ReadAllBytes($pkg)[0..1]
    if ($magic[0] -eq 0x1f -and $magic[1] -eq 0x8b) { tar.exe -xzf $pkg -C $Work }
    elseif ($magic[0] -eq 0x50 -and $magic[1] -eq 0x4b) { Expand-Archive -Path $pkg -DestinationPath $Work }
    else { Fail 'the HSL package is neither .tar.gz nor .zip (an expired or already used download link returns a web page)' }
    $top = Get-ChildItem -Directory $Work | Select-Object -First 1
    $Ma27 = Join-Path $top.FullName 'src\ma27d.f'
    if (-not (Test-Path $Ma27)) { Fail 'unrecognised HSL package: expected the MA27 package (src\ma27d.f)' }
    Say "package: $($top.Name)"
    # An existing gfortran first ($env:OPFSOLVE_FC, or gfortran on PATH).
    $sys = if ($env:OPFSOLVE_FC) { $env:OPFSOLVE_FC } else { (Get-Command gfortran -ErrorAction SilentlyContinue).Source }
    if ($sys) { $Fc = $sys; Say "compiler: $((& $sys --version)[0]) (yours)" } else { $ToolsProc = Start-Tools }
}

# --- 3. Ipopt from conda-forge ---
Section '[3/5] Ipopt'
$IpoptDir = Join-Path $Dir 'ipopt'
$IpoptLib = Join-Path $IpoptDir 'Library\bin\ipopt-3.dll'
if (-not (Test-Path $IpoptLib)) {
    Say "installing $IpoptSpec from conda-forge"
    Invoke-Mamba create -y -q -p $IpoptDir -c conda-forge --override-channels $IpoptSpec | Out-Null
    if (-not (Test-Path $IpoptLib)) { Fail 'could not install Ipopt from conda-forge' }
    "conda-forge $IpoptSpec" | Set-Content (Join-Path $IpoptDir 'SOURCE')
}
Say "$(Get-Content (Join-Path $IpoptDir 'SOURCE') -ErrorAction SilentlyContinue): $IpoptLib"

# --- 4. HSL: build, link, or keep ---
function Test-Hsl { & $Exe --check-hsl "--hsl-lib=$HslLib" *> (Join-Path $Work 'check.log'); return $LASTEXITCODE -eq 0 }
function Build-Ma27($compiler) {
    Say 'compiling MA27 (double precision) with -O3'
    # A static Fortran runtime, so the DLL needs nothing from the compiler environment.
    & $compiler -O3 -shared -static-libgfortran -static-libgcc -static -o $HslLib $Ma27 *> (Join-Path $Work 'build.log')
    return ($LASTEXITCODE -eq 0) -and (Test-Path $HslLib)
}
New-Item -ItemType Directory -Force -Path $HslDir | Out-Null
switch ($HslMode) {
    'lib' {
        Section '[4/5] HSL MA27'
        if (-not (Test-Path $UserLib)) { Fail "HSL library not found: $UserLib" }
        Copy-Item $UserLib $HslLib -Force
        "your library $UserLib" | Set-Content (Join-Path $HslDir 'SOURCE')
        New-Item -ItemType Directory -Force -Path $Work | Out-Null
        if (-not (Test-Hsl)) { Fail "Ipopt cannot use MA27 from $UserLib" }
        Remove-Item -Recurse -Force $Work
        Say "your library $UserLib (MA27 checked)"
    }
    'build' {
        Section '[4/5] HSL MA27'
        $ok = $false
        if ($Fc) {
            $ok = (Build-Ma27 $Fc) -and (Test-Hsl)
            if (-not $ok) { Say "that build did not work with Ipopt; using conda-forge's gfortran instead"; $ToolsProc = Start-Tools }
        }
        if (-not $ok) {
            $ToolsProc.WaitForExit()
            $Fc = (Get-ChildItem -Recurse -Path (Join-Path $Work 'tools') -Filter '*gfortran.exe' | Select-Object -First 1).FullName
            if (-not $Fc) { Fail "could not install the Fortran compiler; see $(Join-Path $Work 'tools.err')" }
            if (-not (Build-Ma27 $Fc)) { Fail "compiling MA27 failed; see $(Join-Path $Work 'build.log')" }
            if (-not (Test-Hsl)) { Fail "the HSL library built, but Ipopt cannot use its MA27; see $(Join-Path $Work 'check.log')" }
        }
        "built from $($top.Name) on $((Get-Date).ToString('yyyy-MM-dd')) with $((& $Fc --version)[0])" | Set-Content (Join-Path $HslDir 'SOURCE')
        Remove-Item -Recurse -Force $Work
        Say "$HslLib (MA27 checked)"
    }
    'keep' { Section '[4/5] HSL MA27'; Say "$(Get-Content (Join-Path $HslDir 'SOURCE') -ErrorAction SilentlyContinue): $HslLib" }
}

# --- 5. PATH, as juliaup does it on Windows: the user PATH, and this session ---
Section '[5/5] PATH'
$BinDir = Join-Path $Dir 'bin'
if ($AddToPath -match '^(yes|1)$') {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @($userPath -split ';' | Where-Object { $_ })
    if ($parts -notcontains $BinDir) {
        [Environment]::SetEnvironmentVariable('Path', (@($BinDir) + $parts) -join ';', 'User')
    }
    if (@($env:Path -split ';') -notcontains $BinDir) { $env:Path = "$BinDir;$env:Path" }
    Say "$BinDir added to the user PATH and to this session"
} else {
    Say 'left unchanged (OPFSOLVE_ADD_TO_PATH=no)'
}

# Drop downloaded packages that are no longer needed.
if (Test-Path $Mamba) { Invoke-Mamba clean -a -y -q | Out-Null }

Write-Host ''
Write-Host 'opfsolve is installed.' -ForegroundColor Cyan
if ($AddToPath -match '^(yes|1)$') { Say 'try:  opfsolve case118_ieee        (opfsolve --help for options)' }
else { Say "try:  $Exe case118_ieee" }
