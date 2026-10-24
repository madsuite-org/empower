#!/bin/sh
# Write explicit conda-forge lock files for this platform into DIR (default
# stage/opfsolve/locks): the exact package URLs and checksums of the Ipopt and
# Fortran-compiler environments the installer creates. Installing from a lock
# skips downloading conda-forge's index and solving, and gives every install of
# a release the same packages.
#
#   tools/lock.sh [DIR]
set -eu
cd "$(dirname "$0")/.."
out=${1:-stage/opfsolve/locks}
ipopt_spec=$(sed -n 's/^IPOPT_SPEC="\(.*\)"$/\1/p' install.sh)
mamba_version=$(sed -n 's/^MAMBA_VERSION="\(.*\)"$/\1/p' install.sh)
[ -n "$ipopt_spec" ] && [ -n "$mamba_version" ] || { echo "cannot read IPOPT_SPEC/MAMBA_VERSION from install.sh" >&2; exit 1; }

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) cplat=linux-64 ;;
    Linux-aarch64) cplat=linux-aarch64 ;;
    Darwin-arm64) cplat=osx-arm64 ;;
    MINGW*-x86_64 | MSYS*-x86_64 | CYGWIN*-x86_64) cplat=win-64 ;;
    *) echo "unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
t=$(mktemp -d)
mm="$t/micromamba"
[ "$cplat" = win-64 ] && mm="$t/micromamba.exe"
curl -fsSL -o "$mm" "https://github.com/mamba-org/micromamba-releases/releases/download/$mamba_version/micromamba-$cplat"
chmod +x "$mm"
export MAMBA_ROOT_PREFIX="$t/root"
mkdir -p "$out"
lock() {   # name spec...
    name=$1; shift
    "$mm" create -y -q -p "$t/$name" -c conda-forge --override-channels "$@" >/dev/null
    "$mm" env export -p "$t/$name" --explicit > "$out/$name.txt"
    n=$(grep -c '^https://' "$out/$name.txt")
    [ "$n" -gt 0 ] || { echo "empty lock for $name" >&2; exit 1; }
    echo "locked $name ($*): $n packages"
}
lock ipopt "$ipopt_spec"
lock gfortran gfortran
# Coin-HSL builds with meson (Linux and macOS).
[ "$cplat" = win-64 ] || lock gfortran-meson gfortran meson ninja pkg-config
rm -rf "$t"
