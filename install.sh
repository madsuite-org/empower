#!/bin/sh
# Install opfsolve (compiled ExaModelsPower AC OPF models + the IDAES build of
# Ipopt) and solve AC OPF cases with it.
#
#   curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- case2000_goc --form=rect
#   curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- all
#
# Arguments are passed to opfsolve (see `opfsolve --help`); with none, it solves
# pglib_opf_case118_ieee. Pass --install-only to stop after installing.
#
# Environment:
#   OPFSOLVE_DIR      install directory (default ./opfsolve)
#   OPFSOLVE_VERSION  release tag (default: latest)
#   OPFSOLVE_ARCHIVE  local release archive to install instead of downloading one
#   IDAES_VERSION     idaes-ext release tag (default 3.4.2)
set -eu

REPO="madsuite-org/opfsolve"
DIR="${OPFSOLVE_DIR:-$PWD/opfsolve}"
VERSION="${OPFSOLVE_VERSION:-latest}"
IDAES_VERSION="${IDAES_VERSION:-3.4.2}"

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null || die "curl is required"
command -v tar >/dev/null || die "tar is required"

install_only=0
if [ "${1:-}" = "--install-only" ]; then install_only=1; shift; fi
[ $# -eq 0 ] && [ $install_only -eq 0 ] && set -- case118_ieee

# Platform names used by our release and by idaes-ext.
os=$(uname -s)
arch=$(uname -m)
case "$arch" in
    x86_64 | amd64) arch=x86_64 ;;
    aarch64 | arm64) arch=aarch64 ;;
    *) die "unsupported architecture: $arch" ;;
esac
case "$os" in
    Linux)
        plat="linux-$arch"
        id="" ; ver="" ; like=""
        if [ -r /etc/os-release ]; then
            . /etc/os-release
            id="${ID:-}" ; ver="${VERSION_ID:-}" ; like="${ID_LIKE:-}"
        fi
        major=${ver%%.*}
        case "$id $like" in
            *ubuntu*)
                if [ "${major:-22}" -ge 22 ] 2>/dev/null; then d=ubuntu2204
                elif [ "${major}" -ge 20 ] 2>/dev/null; then d=ubuntu2004
                else d=ubuntu1804; fi ;;
            *debian*)
                if [ "${major:-12}" -ge 12 ] 2>/dev/null; then d=ubuntu2204; else d=ubuntu2004; fi ;;
            *rhel* | *fedora* | *centos* | *rocky* | *almalinux*)
                if [ "${major:-8}" -ge 8 ] 2>/dev/null || [ "$id" = fedora ]; then d=el8; else d=el7; fi ;;
            *) d=ubuntu2204 ;;
        esac
        iplat="$d-$arch"
        libipopt=libipopt.so
        ;;
    Darwin)
        [ "$arch" = aarch64 ] || die "macOS on Intel is not supported; opfsolve needs Apple silicon (arm64)"
        plat="macos-$arch"
        iplat="darwin-$arch"
        libipopt=libipopt.dylib
        ;;
    *) die "unsupported OS: $os (use WSL on Windows)" ;;
esac
say "platform: $plat"
say "install directory: $DIR"

if [ ! -x "$DIR/bin/opfsolve" ]; then
    mkdir -p "$DIR"
    if [ -n "${OPFSOLVE_ARCHIVE:-}" ]; then
        say "installing opfsolve from $OPFSOLVE_ARCHIVE"
        tar xzf "$OPFSOLVE_ARCHIVE" -C "$DIR" --strip-components 1
    else
        if [ "$VERSION" = latest ]; then
            url="https://github.com/$REPO/releases/latest/download/opfsolve-$plat.tar.gz"
        else
            url="https://github.com/$REPO/releases/download/$VERSION/opfsolve-$plat.tar.gz"
        fi
        say "downloading opfsolve ($VERSION): $url"
        curl -fSL --progress-bar "$url" | tar xz -C "$DIR" --strip-components 1 ||
            die "no opfsolve release for $plat at $url"
    fi
    say "installed $DIR/bin/opfsolve and the compiled model library $DIR/emp"
else
    say "opfsolve already installed: $("$DIR/bin/opfsolve" --version)"
fi

if [ ! -e "$DIR/ipopt/$libipopt" ]; then
    iurl="https://github.com/IDAES/idaes-ext/releases/download/$IDAES_VERSION/idaes-solvers-$iplat.tar.gz"
    say "downloading Ipopt (IDAES $IDAES_VERSION build with HSL MA27): $iurl"
    mkdir -p "$DIR/ipopt"
    curl -fSL --progress-bar "$iurl" | tar xz -C "$DIR/ipopt" 2>/dev/null ||
        die "no IDAES Ipopt build for $iplat"
    [ -e "$DIR/ipopt/$libipopt" ] || die "$libipopt not found in the IDAES archive"
    echo "idaes-ext $IDAES_VERSION $iplat" > "$DIR/ipopt/SOURCE"
    say "installed $DIR/ipopt/$libipopt"
else
    say "Ipopt already installed: $(cat "$DIR/ipopt/SOURCE" 2>/dev/null || echo "$DIR/ipopt")"
fi

# The IDAES Linux build links against the system Fortran runtime and LAPACK.
if [ "$os" = Linux ] && command -v ldd >/dev/null; then
    missing=$(ldd "$DIR/ipopt/$libipopt" | awk '/not found/ {print $1}' | tr '\n' ' ')
    if [ -n "$missing" ]; then
        die "Ipopt needs system libraries: $missing
  Debian/Ubuntu: sudo apt-get install libgfortran5 liblapack3 libblas3 libgomp1
  RHEL/Fedora:   sudo dnf install libgfortran lapack blas libgomp"
    fi
fi

say "run it again with: $DIR/bin/opfsolve <case>... [options]   (see --help)"
[ $install_only -eq 1 ] && exit 0

# Output directory, as opfsolve will choose it, for the machine description.
out=opfsolve-results
prev=""
for a in "$@"; do
    case "$a" in --output=*) out=${a#--output=} ;; esac
    [ "$prev" = "--output" ] && out=$a
    prev=$a
done
mkdir -p "$out"
{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "uname: $(uname -a)"
    if [ "$os" = Linux ]; then
        echo "cpu: $(awk -F': ' '/model name/ {print $2; exit}' /proc/cpuinfo)"
        echo "cores: $(nproc 2>/dev/null || echo unknown)"
    else
        echo "cpu: $(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
        echo "cores: $(sysctl -n hw.ncpu 2>/dev/null)"
    fi
    echo "opfsolve: $("$DIR/bin/opfsolve" --version)"
    echo "ipopt: $(cat "$DIR/ipopt/SOURCE" 2>/dev/null)"
    echo "command: opfsolve $*"
} > "$out/system.txt"

say "running: opfsolve $*"
# Stream the output and keep a copy; POSIX sh has no pipefail, so the exit
# status goes through a file.
{ "$DIR/bin/opfsolve" "$@"; echo $? > "$out/.rc"; } 2>&1 | tee "$out/opfsolve.log"
rc=$(cat "$out/.rc"); rm -f "$out/.rc"
say "results in $out: summary.csv, <case>/solution.json, <case>/ipopt.log, opfsolve.log, system.txt"
exit "$rc"
