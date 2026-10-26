# Install the empower command on Windows: compiled ExaModelsPower AC OPF models,
# Ipopt from conda-forge, and the user's own HSL MA27 compiled on this machine.
# In PowerShell:
#
#   irm https://raw.githubusercontent.com/madsuite-org/empower/main/install.ps1 | iex
#
# Then run `empower <case>...` (see `empower --help`).
#
# Like juliaup, it installs into ~\.empower, adds ~\.empower\bin to the user
# PATH (and to this session), and asks for confirmation unless EMPOWER_YES=1.
# Running it again upgrades to the latest release.
#
# Environment:
#   EMPOWER_YES=1          accept the defaults without asking
#   EMPOWER_DIR            install directory (default ~\.empower)
#   EMPOWER_ADD_TO_PATH    yes (default) or no
#   EMPOWER_HSL            your HSL download link (from HSL's email) or the
#                           downloaded .tar.gz/.zip, compiled on this machine
#   EMPOWER_HSL_LIB        an HSL library you already have, used as is
#   EMPOWER_UNINSTALL=1    remove the install directory and its user PATH entry
#   EMPOWER_VERSION        release tag (default: latest)
#   EMPOWER_ARCHIVE        local release archive to install instead of downloading one
#   EMPOWER_FC             the Fortran compiler to try first (default gfortran on PATH)
#   EMPOWER_SKIP_HSL=1     install without HSL (as Enter at the prompt); Ipopt then uses
#                          MUMPS until `empower hsl` adds MA27
#   EMPOWER_BACKENDS       cpu (the default, and the only backend on Windows)
#
# HSL MA27 is required, and empower never distributes it. Request the free
# personal licence at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/; HSL emails
# a download link, which the script asks for if EMPOWER_HSL is not set.
#
# Everything runs inside one script block: a download cut short does not parse
# and so runs nothing, and nothing leaks into the calling session apart from
# PATH.

& {
$SavedErrorAction = $ErrorActionPreference
$SavedProgress = $ProgressPreference
try {
# Native tools report through $LASTEXITCODE; with 'Stop', PowerShell 5 would
# turn their stderr output into terminating errors.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$Repo = 'madsuite-org/empower'
$Dir = if ($env:EMPOWER_DIR) { [IO.Path]::GetFullPath($env:EMPOWER_DIR) } else { Join-Path $HOME '.empower' }
$AddToPath = if ($env:EMPOWER_ADD_TO_PATH) { $env:EMPOWER_ADD_TO_PATH } else { 'yes' }
$Interactive = [Environment]::UserInteractive -and -not [Console]::IsInputRedirected
$Version = if ($env:EMPOWER_VERSION) { $env:EMPOWER_VERSION } else { 'latest' }
$IpoptSpec = 'ipopt=3.14.20'
$MambaVersion = '2.9.0-0'
$MambaSha256 = 'a6d804394b2418991c4e29562853eaace2f2ce9d9da661a98e74e02e8dbb44b0'
$Ma27Url = 'https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/'
$Plat = 'windows-x86_64'
$BinDir = Join-Path $Dir 'bin'

function Say($msg) { Write-Host "    $msg" }
function Section($msg) { Write-Host ''; Write-Host $msg -ForegroundColor Cyan }
# `throw`, not `exit`: under `irm | iex`, exit would close the PowerShell window.
function Fail($msg) { throw "empower install: $msg" }
if ($env:EMPOWER_BACKENDS -and $env:EMPOWER_BACKENDS -ne 'cpu') {
    Fail "the cuda backend is available on Linux x86_64 only; on Windows EMPOWER_BACKENDS can only be cpu"
}
function Get-Url($url, $out) {
    curl.exe --proto '=https' --tlsv1.2 -fsSL -o $out $url
    return $LASTEXITCODE -eq 0
}
function Test-Sha256($file, $want, $what) {
    $got = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLower()
    if ($got -ne $want.ToLower()) { Fail "checksum mismatch for $what (expected $want, got $got); the download may be corrupt or tampered with" }
}

# The user PATH, read and written raw (REG_EXPAND_SZ, %VARIABLES% kept), as
# juliaup and rustup do; [Environment]::SetEnvironmentVariable would expand them.
function Get-UserPath {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
    $raw = $key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $key.Close()
    return @($raw -split ';' | Where-Object { $_ })
}
function Set-UserPath([string[]]$parts) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    $key.SetValue('Path', ($parts -join ';'), [Microsoft.Win32.RegistryValueKind]::ExpandString)
    $key.Close()
    # Tell running programs (Explorer, new terminals) that the environment changed.
    [Environment]::SetEnvironmentVariable('EMPOWER_PATH_REFRESH', '1', 'User')
    [Environment]::SetEnvironmentVariable('EMPOWER_PATH_REFRESH', $null, 'User')
}

foreach ($tool in 'curl.exe', 'tar.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool is required (Windows 10 1803 or newer)" }
}

# --- uninstall ---
if ($env:EMPOWER_UNINSTALL -eq '1') {
    Write-Host 'empower uninstall' -ForegroundColor Cyan
    Say "remove: $Dir, and $BinDir from the user PATH"
    if ($env:EMPOWER_YES -ne '1') {
        if (-not $Interactive) { Fail 'unable to ask for confirmation; set $env:EMPOWER_YES=1' }
        $answer = Read-Host '    proceed? [y/N]'
        if ($answer -notmatch '^(y|yes)$') { Fail 'cancelled' }
    }
    if ((Test-Path $Dir) -and -not (Test-Path (Join-Path $BinDir 'empower.exe'))) { Fail "$Dir does not look like an empower install; not removing it" }
    Remove-Item -Recurse -Force $Dir -ErrorAction SilentlyContinue
    $parts = Get-UserPath
    if ($parts -contains $BinDir) { Set-UserPath @($parts | Where-Object { $_ -ne $BinDir }) }
    $env:Path = (@($env:Path -split ';' | Where-Object { $_ -and $_ -ne $BinDir })) -join ';'
    Say 'done'
    return
}

$arch = $env:PROCESSOR_ARCHITECTURE
if ($arch -ne 'AMD64') { Fail "unsupported architecture: $arch (Windows x86_64 only)" }

# The release to install: the latest one unless EMPOWER_VERSION names a tag.
if ($env:EMPOWER_ARCHIVE) { $Tag = 'local archive' }
elseif ($Version -eq 'latest') {
    $eff = curl.exe --proto '=https' --tlsv1.2 -fsSLI -o NUL -w '%{url_effective}' "https://github.com/$Repo/releases/latest"
    $Tag = ($eff -replace '.*/tag/', '')
    if ($Tag -notmatch '^v') { Fail 'could not find the latest empower release' }
} else { $Tag = $Version }

Write-Host 'empower installer' -ForegroundColor Cyan
Say "platform:      $Plat"
Say "release:       $Tag"
Say "install into:  $Dir"
Say "add to PATH:   $AddToPath ($BinDir, in the user PATH)"
if ($env:EMPOWER_YES -ne '1') {
    if (-not $Interactive) { Fail 'unable to ask for confirmation; set $env:EMPOWER_YES=1 to accept the defaults' }
    $answer = Read-Host '    proceed? [Y/n]'
    if ($answer -and $answer -notmatch '^(y|yes)$') { Fail 'cancelled' }
}
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
$Exe = Join-Path $BinDir 'empower.exe'
$HslDir = Join-Path $Dir 'hsl'
$HslLib = Join-Path $HslDir 'libhsl.dll'

# --- conda-forge tools (micromamba) ---
$MambaDir = Join-Path $Dir '.mamba'
$Mamba = Join-Path $MambaDir 'micromamba.exe'
$env:MAMBA_ROOT_PREFIX = Join-Path $MambaDir 'root'
function Get-Mamba {
    if ((Test-Path $Mamba) -and ((Get-FileHash -Algorithm SHA256 -LiteralPath $Mamba).Hash.ToLower() -eq $MambaSha256)) { return }
    New-Item -ItemType Directory -Force -Path $MambaDir | Out-Null
    $part = "$Mamba.part"
    if (-not (Get-Url "https://github.com/mamba-org/micromamba-releases/releases/download/$MambaVersion/micromamba-win-64" $part)) { Fail 'could not download micromamba' }
    Test-Sha256 $part $MambaSha256 "micromamba $MambaVersion"
    Move-Item -Force $part $Mamba
}
function Invoke-Mamba { Get-Mamba; & $Mamba @args }
# From the release's lock file when it has one (no index download, no solve, the
# same packages for everyone), else by spec.
function Lock-Args($name, [string[]]$spec) {
    $lock = Join-Path $Dir "locks\$name.txt"
    if (Test-Path $lock) { return @('--file', $lock) }
    return @('-c', 'conda-forge', '--override-channels') + $spec
}

# --- 1. empower: download, verify, unpack aside, then swap in ---
Section '[1/5] empower'
$ReleaseFile = Join-Path $Dir 'RELEASE'
$Installed = if (Test-Path $ReleaseFile) { (Get-Content $ReleaseFile -TotalCount 1) } else { '' }
if ((Test-Path $Exe) -and ($Installed -eq $Tag) -and -not $env:EMPOWER_ARCHIVE) {
    Say "$Tag is installed"
} else {
    $Stage = Join-Path $Dir '.staging'
    Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Join-Path $Stage 'new') | Out-Null
    $tgz = Join-Path $Stage 'archive.tar.gz'
    if ($env:EMPOWER_ARCHIVE) {
        Say "from $env:EMPOWER_ARCHIVE"
        Copy-Item $env:EMPOWER_ARCHIVE $tgz
    } else {
        $base = "https://github.com/$Repo/releases/download/$Tag"
        Say "downloading $base/empower-$Plat.tar.gz"
        if (-not (Get-Url "$base/empower-$Plat.tar.gz" $tgz)) { Fail "no empower $Tag release for $Plat" }
        $sums = Join-Path $Stage 'SHA256SUMS'
        if (-not (Get-Url "$base/SHA256SUMS" $sums)) { Fail "could not download the checksums of release $Tag" }
        $line = Get-Content $sums | Where-Object { ($_ -split '\s+')[1] -match "^\*?empower-$Plat\.tar\.gz$" } | Select-Object -First 1
        if (-not $line) { Fail "release $Tag lists no checksum for empower-$Plat.tar.gz" }
        Test-Sha256 $tgz (($line -split '\s+')[0]) "empower-$Plat.tar.gz"
        Say 'checksum verified'
    }
    tar.exe -xzf $tgz -C (Join-Path $Stage 'new') --strip-components 1
    if ($LASTEXITCODE -ne 0) { Fail 'could not unpack the empower archive' }
    if (-not (Test-Path (Join-Path $Stage 'new\bin\empower.exe'))) { Fail 'the empower archive has no bin\empower.exe' }
    # Replace what the archive provides; keep ipopt, hsl, cases, and .mamba.
    foreach ($item in Get-ChildItem -Force (Join-Path $Stage 'new')) {
        $dest = Join-Path $Dir $item.Name
        Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
        Move-Item $item.FullName $dest
    }
    $Tag | Set-Content $ReleaseFile
    Remove-Item -Recurse -Force $Stage
    if ($Installed -and $Installed -ne $Tag) { Say "upgraded from $Installed" }
}
Say "$(& $Exe --version) and its compiled model library"

# --- 2. HSL source: asked for first, so its compiler downloads alongside Ipopt ---
# Compile the user's own HSL package, optimized for this machine. HSL's personal
# licence permits modifying the software only to optimise its performance for
# the licensee's use on a particular platform; the result stays on this machine.
# The cpu backend without HSL; the same words as empower's own warning.
function Write-NoHslWarning {
    Write-Host ''
    Write-Host 'warning: without HSL MA27, Ipopt uses MUMPS. MUMPS is not the most efficient' -ForegroundColor Red
    Say 'linear solver for OPF, and its timings are not appropriate for benchmarking solver'
    Say "performance. Requesting HSL's free MA27 licence takes about 2 minutes:"
    Say "  $Ma27Url"
    Say "then add it with: empower hsl '<download link from HSL''s email>'"
}
Section '[2/5] HSL MA27 package'
$HslMode = ''; $HslSrc = ''; $UserLib = ''
if ($env:EMPOWER_HSL_LIB) { $HslMode = 'lib'; $UserLib = [IO.Path]::GetFullPath($env:EMPOWER_HSL_LIB); Say "your library $UserLib" }
elseif ($env:EMPOWER_HSL) { $HslMode = 'build'; $HslSrc = $env:EMPOWER_HSL }
elseif (Test-Path $HslLib) { $HslMode = 'keep'; Say 'already installed' }
elseif ($env:EMPOWER_SKIP_HSL -eq '1') { $HslMode = 'skip'; Say 'skipped (EMPOWER_SKIP_HSL=1)'; Write-NoHslWarning }
else {
    Say "empower needs HSL MA27, which you download yourself under HSL's free personal licence:"
    Say "  1. request it at $Ma27Url (about 2 minutes)"
    Say '  2. HSL emails you a download link (valid for 7 days)'
    $link = ''
    if ($Interactive) {
        try { Start-Process $Ma27Url; Say 'opened the request page in your browser' } catch { }
        $link = Read-Host '    paste the download link, a path to the package or to an HSL library, or press Enter to skip'
    }
    if (-not $link -or $link -eq 'skip') { $HslMode = 'skip'; Say 'skipped'; Write-NoHslWarning }
    else {
        if ($link -notmatch '^https?://') { $link = [IO.Path]::GetFullPath($link) }
        if ($link -match '\.dll$') { $HslMode = 'lib'; $UserLib = $link } else { $HslMode = 'build'; $HslSrc = $link }
    }
}

# The build works in a private temporary directory, under its short (8.3) name
# when the path has spaces: conda-forge's gfortran cannot run from such a path.
$Work = Join-Path ([IO.Path]::GetTempPath()) ("empower-hsl-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $Work | Out-Null
if ($Work -match ' ') {
    try { $Work = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($Work).ShortPath } catch { }
}
New-Item -ItemType Directory -Force -Path $HslDir | Out-Null
$Fc = $null; $ToolsProc = $null; $Ma27 = $null
function Start-Tools {
    Get-Mamba
    Say 'compiler: downloading gfortran from conda-forge, alongside Ipopt'
    # Its own package cache, so it does not wait on Ipopt's. Start-Process joins
    # its arguments with spaces, so each one is quoted.
    $env:MAMBA_ROOT_PREFIX = Join-Path $Work 'mamba-root'
    $argv = @('create', '-y', '-q', '-p', (Join-Path $Work 'tools')) + (Lock-Args 'gfortran' @('gfortran'))
    $proc = Start-Process -FilePath $Mamba -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $Work 'tools.log') -RedirectStandardError (Join-Path $Work 'tools.err') `
        -ArgumentList (($argv | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $env:MAMBA_ROOT_PREFIX = Join-Path $MambaDir 'root'
    return $proc
}
if ($HslMode -eq 'build') {
    if ($HslSrc -match '^https?://') {
        # HSL's emailed links are http://; fetch them over https. The link is a
        # page; the archive is under it.
        $HslSrc = 'https://' + ($HslSrc -replace '^https?://', '')
        if ($HslSrc -notmatch '\.(tar\.gz|tgz|zip)$') { $HslSrc = $HslSrc.TrimEnd('/') + '/package.tar.gz' }
        Say 'downloading your HSL package'
        $pkg = Join-Path $Work 'pkg'
        if (-not (Get-Url $HslSrc $pkg)) { Fail 'could not download the HSL package (download links expire after 7 days)' }
    } else {
        if (-not (Test-Path $HslSrc)) { Fail "HSL package not found: $HslSrc" }
        $pkg = Convert-Path $HslSrc
    }
    $fs = [IO.File]::OpenRead($pkg); $magic = New-Object byte[] 2; [void]$fs.Read($magic, 0, 2); $fs.Close()
    if ($magic[0] -eq 0x1f -and $magic[1] -eq 0x8b) { tar.exe -xzf $pkg -C $Work }
    elseif ($magic[0] -eq 0x50 -and $magic[1] -eq 0x4b) { Expand-Archive -Path $pkg -DestinationPath $Work }
    else { Fail 'the HSL package is neither .tar.gz nor .zip (an expired or already used download link returns a web page)' }
    $top = Get-ChildItem -Directory $Work | Where-Object { $_.Name -notin @('tools', 'mamba-root') } | Select-Object -First 1
    if ($top) { $Ma27 = Join-Path $top.FullName 'src\ma27d.f' }
    if (-not $top -or -not (Test-Path $Ma27)) { Fail 'unrecognised HSL package: expected the MA27 package (src\ma27d.f)' }
    Say "package: $($top.Name)"
    # An existing gfortran first ($env:EMPOWER_FC, or gfortran on PATH).
    $sys = if ($env:EMPOWER_FC) { $env:EMPOWER_FC } else { (Get-Command gfortran -ErrorAction SilentlyContinue).Source }
    if ($sys) { $Fc = $sys; Say "compiler: $((& $sys --version)[0]) (yours)" } else { $ToolsProc = Start-Tools }
}

# --- 3. Ipopt from conda-forge; recreated when the release's lock changes ---
Section '[3/5] Ipopt'
$IpoptDir = Join-Path $Dir 'ipopt'
$IpoptLib = Join-Path $IpoptDir 'Library\bin\ipopt-3.dll'
$Lock = Join-Path $Dir 'locks\ipopt.txt'
$LockUsed = Join-Path $IpoptDir 'LOCK'
$current = (Test-Path $IpoptLib) -and (-not (Test-Path $Lock) -or ((Test-Path $LockUsed) -and ((Get-FileHash $Lock).Hash -eq (Get-FileHash $LockUsed).Hash)))
if (-not $current) {
    if (Test-Path $Lock) { Say "installing $IpoptSpec from conda-forge (locked: $(@(Select-String -Path $Lock -Pattern '^https://').Count) packages)" }
    else { Say "installing $IpoptSpec from conda-forge" }
    $new = "$IpoptDir.new"
    Remove-Item -Recurse -Force $new -ErrorAction SilentlyContinue
    $largs = Lock-Args 'ipopt' @($IpoptSpec)
    Invoke-Mamba create -y -q -p $new @largs | Out-Null
    if (-not (Test-Path (Join-Path $new 'Library\bin\ipopt-3.dll'))) { Fail 'could not install Ipopt from conda-forge' }
    "conda-forge $IpoptSpec" | Set-Content (Join-Path $new 'SOURCE')
    if (Test-Path $Lock) { Copy-Item $Lock (Join-Path $new 'LOCK') }
    Remove-Item -Recurse -Force $IpoptDir -ErrorAction SilentlyContinue
    Move-Item $new $IpoptDir
}
Say "$(Get-Content (Join-Path $IpoptDir 'SOURCE') -ErrorAction SilentlyContinue): $IpoptLib"

# --- 4. HSL: build, link, or keep ---
function Test-Hsl { & $Exe --check-hsl "--hsl-lib=$HslLib" *> (Join-Path $Work 'check.log'); return $LASTEXITCODE -eq 0 }
function Build-Ma27($compiler) {
    Say 'compiling MA27 (double precision) with -O3'
    # A static Fortran runtime, so the DLL needs nothing from the compiler environment.
    $out = Join-Path $Work 'libhsl.dll'
    & $compiler -O3 -shared -static-libgfortran -static-libgcc -static -o $out $Ma27 *> (Join-Path $Work 'build.log')
    if (($LASTEXITCODE -ne 0) -or -not (Test-Path $out)) { return $false }
    Copy-Item -Force $out $HslLib
    return $true
}
switch ($HslMode) {
    'lib' {
        Section '[4/5] HSL MA27'
        if (-not (Test-Path $UserLib)) { Fail "HSL library not found: $UserLib" }
        Copy-Item $UserLib $HslLib -Force
        "your library $UserLib" | Set-Content (Join-Path $HslDir 'SOURCE')
        if (-not (Test-Hsl)) { Fail "Ipopt cannot use MA27 from $UserLib; see $(Join-Path $Work 'check.log')" }
        Say "your library $UserLib (MA27 checked)"
    }
    'build' {
        Section '[4/5] HSL MA27'
        $ok = $false
        if ($Fc) {
            $ok = (Build-Ma27 $Fc) -and (Test-Hsl)
            if (-not $ok) { Say "the build with your compiler did not work with Ipopt; using conda-forge's gfortran instead"; $ToolsProc = Start-Tools }
        }
        if (-not $ok) {
            $ToolsProc.WaitForExit()
            $Fc = (Get-ChildItem -Recurse -Path (Join-Path $Work 'tools') -Filter '*gfortran.exe' -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
            if (-not $Fc) { Fail "could not install the Fortran compiler; see $(Join-Path $Work 'tools.err')" }
            if (-not (Build-Ma27 $Fc)) { Fail "compiling MA27 failed; see $(Join-Path $Work 'build.log')" }
            if (-not (Test-Hsl)) { Fail "the HSL library built, but Ipopt cannot use its MA27; see $(Join-Path $Work 'check.log')" }
        }
        "built from $($top.Name) on $((Get-Date).ToString('yyyy-MM-dd')) with $((& $Fc --version)[0])" | Set-Content (Join-Path $HslDir 'SOURCE')
        Say "$HslLib (MA27 checked)"
    }
    'keep' {
        Section '[4/5] HSL MA27'
        if (-not (Test-Hsl)) { Fail 'the installed HSL library no longer works with Ipopt; rerun with $env:EMPOWER_HSL set' }
        Say "$(Get-Content (Join-Path $HslDir 'SOURCE') -ErrorAction SilentlyContinue): $HslLib (MA27 checked)"
    }
}
Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue

# --- 5. PATH, as juliaup does it on Windows: the user PATH, and this session ---
Section '[5/5] PATH'
if ($AddToPath -match '^(yes|1)$') {
    $parts = Get-UserPath
    if ($parts -notcontains $BinDir) { Set-UserPath (@($BinDir) + $parts) }
    if (@($env:Path -split ';') -notcontains $BinDir) { $env:Path = "$BinDir;$env:Path" }
    Say "$BinDir added to the user PATH and to this session"
} else {
    Say 'left unchanged (EMPOWER_ADD_TO_PATH=no)'
}

# Drop downloaded packages that are no longer needed.
if (Test-Path $Mamba) { Invoke-Mamba clean -a -y -q | Out-Null }

Write-Host ''
Write-Host 'empower is installed.' -ForegroundColor Cyan
if ($AddToPath -match '^(yes|1)$') { Say 'try:  empower case118_ieee        (empower --help for options)' }
else { Say "try:  & '$Exe' case118_ieee" }
} finally {
    $ErrorActionPreference = $SavedErrorAction
    $ProgressPreference = $SavedProgress
}
}
