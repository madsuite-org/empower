#!/bin/sh
# Install opfsolve (compiled ExaModelsPower AC OPF models) with Ipopt from
# conda-forge, optionally build the user's own HSL MA27, and solve AC OPF cases.
#
#   curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- case2000_goc --form=rect
#   curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | OPFSOLVE_HSL='<your HSL download link>' sh
#   wget -qO- https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh     # without curl
#
# Arguments are passed to opfsolve (see `opfsolve --help`); with none, it solves
# pglib_opf_case118_ieee. Pass --install-only to stop after installing.
#
# HSL (MA27). opfsolve never distributes HSL. Without it Ipopt uses MUMPS. To use
# MA27, request the free personal licence at
#   https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/
# and either
#   OPFSOLVE_HSL=<download link from the email, or the downloaded .tar.gz/.zip>
#       to have this script compile it, optimized, on this machine, or
#   OPFSOLVE_HSL_LIB=<path to an HSL library you already have (libhsl, libcoinhsl, HSL_jll)>
#       to use that library as is.
#
# Environment:
#   OPFSOLVE_DIR      install directory (default ./opfsolve)
#   OPFSOLVE_VERSION  release tag (default: latest)
#   OPFSOLVE_ARCHIVE  local release archive to install instead of downloading one
#   OPFSOLVE_HSL, OPFSOLVE_HSL_LIB   see above
set -eu

REPO="madsuite-org/opfsolve"
DIR="${OPFSOLVE_DIR:-$PWD/opfsolve}"
REL="${OPFSOLVE_VERSION:-latest}"
IPOPT_SPEC="ipopt=3.14.20"
MAMBA_VERSION="2.9.0-0"
MA27_URL="https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/"

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# get URL FILE (FILE "-" for stdout), with curl or, on systems without it such
# as a fresh Ubuntu desktop, wget.
if command -v curl >/dev/null; then
    get() { curl -fSL --progress-bar -o "$2" "$1"; }
elif command -v wget >/dev/null; then
    get() { wget -q --show-progress -O "$2" "$1" 2>/dev/null || wget -q -O "$2" "$1"; }
else
    die "curl or wget is required"
fi
command -v tar >/dev/null || die "tar is required"

install_only=0
if [ "${1:-}" = "--install-only" ]; then install_only=1; shift; fi
[ $# -eq 0 ] && [ $install_only -eq 0 ] && set -- case118_ieee

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
        cplat=$([ "$arch" = x86_64 ] && echo linux-64 || echo linux-aarch64)
        ext=so ;;
    Darwin)
        [ "$arch" = aarch64 ] || die "macOS on Intel is not supported; opfsolve needs Apple silicon (arm64)"
        plat="macos-$arch"; cplat=osx-arm64; ext=dylib ;;
    *) die "unsupported OS: $os (on Windows, use install.ps1)" ;;
esac
say "platform: $plat"
say "install directory: $DIR"
mkdir -p "$DIR"

# --- opfsolve ----------------------------------------------------------------
if [ ! -x "$DIR/bin/opfsolve" ]; then
    if [ -n "${OPFSOLVE_ARCHIVE:-}" ]; then
        say "installing opfsolve from $OPFSOLVE_ARCHIVE"
        tar xzf "$OPFSOLVE_ARCHIVE" -C "$DIR" --strip-components 1
    else
        if [ "$REL" = latest ]; then
            url="https://github.com/$REPO/releases/latest/download/opfsolve-$plat.tar.gz"
        else
            url="https://github.com/$REPO/releases/download/$REL/opfsolve-$plat.tar.gz"
        fi
        say "downloading opfsolve ($REL): $url"
        get "$url" - | tar xz -C "$DIR" --strip-components 1 ||
            die "no opfsolve release for $plat at $url"
    fi
    say "installed $DIR/bin/opfsolve and the compiled model library $DIR/emp"
else
    say "opfsolve already installed: $("$DIR/bin/opfsolve" --version)"
fi

# --- conda-forge tools (micromamba) ------------------------------------------
MM="$DIR/.mamba/micromamba"
export MAMBA_ROOT_PREFIX="$DIR/.mamba/root"
mamba() {
    if [ ! -x "$MM" ]; then
        murl="https://github.com/mamba-org/micromamba-releases/releases/download/$MAMBA_VERSION/micromamba-$cplat"
        say "downloading micromamba $MAMBA_VERSION (conda-forge package manager): $murl"
        mkdir -p "$DIR/.mamba"
        get "$murl" "$MM" || die "could not download micromamba"
        chmod +x "$MM"
    fi
    "$MM" "$@"
}

# --- Ipopt from conda-forge --------------------------------------------------
if [ ! -e "$DIR/ipopt/lib/libipopt.$ext" ]; then
    say "installing Ipopt from conda-forge ($IPOPT_SPEC, with MUMPS and OpenBLAS)"
    mamba create -y -q -p "$DIR/ipopt" -c conda-forge --override-channels "$IPOPT_SPEC" >/dev/null ||
        die "could not install Ipopt from conda-forge"
    [ -e "$DIR/ipopt/lib/libipopt.$ext" ] || die "libipopt.$ext missing after installing Ipopt"
    echo "conda-forge $IPOPT_SPEC" > "$DIR/ipopt/SOURCE"
    say "installed $DIR/ipopt/lib/libipopt.$ext"
else
    say "Ipopt already installed: $(cat "$DIR/ipopt/SOURCE" 2>/dev/null || echo "$DIR/ipopt")"
fi

# --- HSL ----------------------------------------------------------------------
hsl="$DIR/hsl/libhsl.$ext"

# Compile the user's own HSL package, optimized for this machine. HSL's personal
# licence permits modifying the software only to optimise its performance for
# the licensee's use on a particular platform, which is what this does; the
# result stays on this machine.
build_hsl() {
    src=$1
    work="$DIR/hsl/build"
    rm -rf "$work"; mkdir -p "$work"; chmod 700 "$DIR/hsl" "$work"
    case "$src" in
        http://* | https://*)
            # A download link from HSL's email is a page; the archive is under it.
            case "$src" in *.tar.gz | *.tgz | *.zip) ;; */) src="${src}package.tar.gz" ;; *) src="$src/package.tar.gz" ;; esac
            say "downloading your HSL package"
            get "$src" "$work/pkg" || die "could not download the HSL package (download links expire after 7 days)"
            pkg="$work/pkg" ;;
        *) [ -f "$src" ] || die "HSL package not found: $src"; pkg=$src ;;
    esac
    case "$(head -c 2 "$pkg" | od -An -tx1 | tr -d ' \n')" in
        1f8b) tar xzf "$pkg" -C "$work" ;;
        504b) command -v unzip >/dev/null || die "unzip is needed for a .zip HSL package"; unzip -q "$pkg" -d "$work" ;;
        *) die "the HSL package is neither .tar.gz nor .zip (an expired or already used download link returns a web page)" ;;
    esac
    top=$(find "$work" -mindepth 1 -maxdepth 1 -type d | head -1)
    # Coin-HSL builds with meson; the standalone MA27 package needs only gfortran.
    tools=gfortran
    [ -f "$top/meson.build" ] && tools="gfortran meson ninja pkg-config"
    say "installing a Fortran compiler from conda-forge to build HSL ($tools)"
    mamba create -y -q -p "$work/tools" -c conda-forge --override-channels $tools >/dev/null ||
        die "could not install the Fortran compiler"
    fc=$(ls "$work"/tools/bin/*gfortran | head -1)
    mkdir -p "$DIR/hsl"
    if [ -f "$top/meson.build" ]; then
        # Coin-HSL and Coin-HSL Archive.
        say "building $(basename "$top") with meson, buildtype=release"
        (cd "$top" && PATH="$work/tools/bin:$PATH" FC="$fc" meson setup build --buildtype=release \
            --prefix="$work/inst" >"$work/build.log" 2>&1 && PATH="$work/tools/bin:$PATH" meson install -C build >>"$work/build.log" 2>&1) ||
            die "HSL build failed; see $work/build.log"
        lib=$(find "$work/inst" -name "libcoinhsl*.$ext" -type f | head -1)
        [ -n "$lib" ] || die "no libcoinhsl in the HSL build; see $work/build.log"
        cp "$lib" "$hsl"
    elif [ -f "$top/src/ma27d.f" ]; then
        # The standalone MA27 package.
        say "compiling $(basename "$top") (double precision) with -O3"
        if [ "$os" = Darwin ]; then shared="-dynamiclib"; else shared="-shared"; fi
        "$fc" -O3 -fPIC $shared -o "$hsl" "$top/src/ma27d.f" -Wl,-rpath,"$DIR/ipopt/lib" ||
            die "compiling MA27 failed"
    else
        die "unrecognised HSL package: expected the MA27 package (src/ma27d.f) or Coin-HSL (meson.build)"
    fi
    echo "built from $(basename "$top") on $(date -u +%Y-%m-%d) with $("$fc" --version | head -1)" > "$DIR/hsl/SOURCE"
    rm -rf "$work"
    # Drop the compiler's packages from micromamba's cache; Ipopt keeps what it uses.
    mamba clean -a -y -q >/dev/null 2>&1 || true
    say "installed $hsl"
}

if [ -n "${OPFSOLVE_HSL_LIB:-}" ]; then
    [ -f "$OPFSOLVE_HSL_LIB" ] || die "OPFSOLVE_HSL_LIB not found: $OPFSOLVE_HSL_LIB"
    mkdir -p "$DIR/hsl"
    ln -sf "$OPFSOLVE_HSL_LIB" "$hsl"
    echo "your library $OPFSOLVE_HSL_LIB" > "$DIR/hsl/SOURCE"
    say "using your HSL library $OPFSOLVE_HSL_LIB"
elif [ -n "${OPFSOLVE_HSL:-}" ]; then
    build_hsl "$OPFSOLVE_HSL"
elif [ -e "$hsl" ]; then
    say "HSL: $(cat "$DIR/hsl/SOURCE" 2>/dev/null || echo "$hsl")"
else
    say "no HSL library: Ipopt will use MUMPS. For MA27, request the free licence at"
    say "  $MA27_URL"
    say "then run this script again with OPFSOLVE_HSL='<download link from the email>'"
fi

say "run it again with: $DIR/bin/opfsolve <case>... [options]   (see --help)"
[ $install_only -eq 1 ] && exit 0

# --- solve --------------------------------------------------------------------
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
    echo "hsl: $(cat "$DIR/hsl/SOURCE" 2>/dev/null || echo none)"
    echo "command: opfsolve $*"
} > "$out/system.txt"

say "running: opfsolve $*"
# Stream the output and keep a copy; POSIX sh has no pipefail, so the exit
# status goes through a file.
{ "$DIR/bin/opfsolve" "$@" && echo 0 > "$out/.rc" || echo $? > "$out/.rc"; } 2>&1 | tee "$out/opfsolve.log"
rc=$(cat "$out/.rc"); rm -f "$out/.rc"
say "results in $out: summary.csv, <case>/solution.json, <case>/ipopt.log, opfsolve.log, system.txt"
exit "$rc"
