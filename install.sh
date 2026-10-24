#!/bin/sh
# Install the opfsolve command: compiled ExaModelsPower AC OPF models, Ipopt from
# conda-forge, and the user's own HSL MA27 compiled on this machine.
#
#   curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- -y --hsl='<link>'
#
# Then run `opfsolve <case>...` (see `opfsolve --help`).
#
# Like juliaup, it installs into ~/.opfsolve, puts ~/.opfsolve/bin on PATH in
# the shell startup files inside a marked block, and asks for confirmation
# unless given -y.
#
# Options:
#   -y, --yes                    accept the defaults without asking
#   -p, --path PATH              install directory (default ~/.opfsolve)
#       --add-to-path yes|no     add PATH/bin to PATH in shell startup files (default yes)
#       --hsl LINK|FILE          the HSL download link from HSL's email, or the
#                                downloaded package, to compile on this machine
#       --hsl-lib FILE           an HSL library you already have, used as is
#   -h, --help
#
# HSL MA27 is required, and opfsolve never distributes it. Request the free
# personal licence at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/ ; HSL emails
# a download link, which the script asks for if --hsl/--hsl-lib is not given.
#
# Environment (same as the options): OPFSOLVE_DIR, OPFSOLVE_HSL, OPFSOLVE_HSL_LIB;
# also OPFSOLVE_VERSION (release tag, default latest), OPFSOLVE_ARCHIVE (a local
# release archive), and OPFSOLVE_SKIP_HSL=1 (install without HSL, for testing only).
set -eu

REPO="madsuite-org/opfsolve"
DIR="${OPFSOLVE_DIR:-$HOME/.opfsolve}"
REL="${OPFSOLVE_VERSION:-latest}"
IPOPT_SPEC="ipopt=3.14.20"
MAMBA_VERSION="2.9.0-0"
MA27_URL="https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/"

say() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() { sed -n '2,/^set -eu/p' "$0" 2>/dev/null | sed '$d; s/^# \{0,1\}//' || echo "see https://github.com/$REPO"; }

yes=0
add_to_path=yes
while [ $# -gt 0 ]; do
    case "$1" in
        -y | --yes) yes=1 ;;
        -p | --path) shift; DIR=${1:?--path needs a directory} ;;
        --path=*) DIR=${1#*=} ;;
        --add-to-path) shift; add_to_path=${1:?--add-to-path needs yes or no} ;;
        --add-to-path=*) add_to_path=${1#*=} ;;
        --hsl) shift; OPFSOLVE_HSL=${1:?--hsl needs a link or file} ;;
        --hsl=*) OPFSOLVE_HSL=${1#*=} ;;
        --hsl-lib) shift; OPFSOLVE_HSL_LIB=${1:?--hsl-lib needs a file} ;;
        --hsl-lib=*) OPFSOLVE_HSL_LIB=${1#*=} ;;
        -h | --help) usage; exit 0 ;;
        *) die "unknown option $1 (see --help)" ;;
    esac
    shift
done
case "$add_to_path" in yes | 1) add_to_path=yes ;; no | 0) add_to_path=no ;; *) die "--add-to-path takes yes or no" ;; esac
case "$DIR" in /*) ;; *) DIR="$PWD/$DIR" ;; esac

# Under `curl ... | sh`, standard input is the script: prompts read the terminal.
have_tty=0
(: < /dev/tty) 2>/dev/null && have_tty=1
ask() {   # question default -> answer
    [ $have_tty -eq 1 ] || { echo "$2"; return; }
    printf '%s ' "$1" > /dev/tty
    read -r ans < /dev/tty || ans=""
    echo "${ans:-$2}"
}

command -v curl >/dev/null || die "curl is required"
get() { curl -fSL --progress-bar -o "$2" "$1"; }   # get URL FILE ("-" for stdout)
command -v tar >/dev/null || die "tar is required"


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
say "add $DIR/bin to PATH in shell startup files: $add_to_path"
if [ $yes -eq 0 ]; then
    [ $have_tty -eq 1 ] || die "unable to ask for confirmation; run with -y to accept the defaults (--help for options)"
    case "$(ask '==> proceed? [Y/n]' y)" in
        y | Y | yes | Yes | YES) ;;
        *) die "cancelled" ;;
    esac
fi
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
elif [ "${OPFSOLVE_SKIP_HSL:-}" = 1 ]; then
    say "OPFSOLVE_SKIP_HSL=1: installing without HSL (testing only)"
else
    say "opfsolve needs HSL MA27, which you download yourself under HSL's free personal licence:"
    say "  1. request it at $MA27_URL"
    say "  2. HSL emails you a download link (valid for 7 days)"
    # Under `curl ... | sh`, standard input is the script, so ask on the terminal.
    link=""
    if [ $have_tty -eq 1 ]; then
        # Open the request page for the user; on a machine without a desktop (ssh,
        # a server) the URL above is all there is.
        if [ "$os" = Darwin ]; then
            open "$MA27_URL" >/dev/null 2>&1 && say "opened the request page in your browser"
        elif { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; } && command -v xdg-open >/dev/null; then
            xdg-open "$MA27_URL" >/dev/null 2>&1 && say "opened the request page in your browser"
        fi
        printf '==> paste the download link (or a path to the package or an HSL library): ' > /dev/tty
        read -r link < /dev/tty || link=""
    fi
    [ -n "$link" ] || die "no HSL download link. Rerun with --hsl='<download link from the email>'"
    case "$link" in
        *.so | *.so.* | *.dylib)
            [ -f "$link" ] || die "HSL library not found: $link"
            mkdir -p "$DIR/hsl"; ln -sf "$link" "$hsl"
            echo "your library $link" > "$DIR/hsl/SOURCE"
            say "using your HSL library $link" ;;
        *) build_hsl "$link" ;;
    esac
fi

# --- PATH, the way juliaup does it ---------------------------------------------
bindir="$DIR/bin"
block_begin="# >>> opfsolve initialize >>>"
block_end="# <<< opfsolve initialize <<<"
write_block() {   # file: replace any earlier block, then append the current one
    f=$1
    if [ -f "$f" ] && grep -qF "$block_begin" "$f"; then
        awk -v b="$block_begin" -v e="$block_end" '$0 == b {skip = 1} !skip {print} $0 == e {skip = 0}' "$f" > "$f.opfsolve-tmp" &&
            cat "$f.opfsolve-tmp" > "$f" && rm -f "$f.opfsolve-tmp"
    fi
    cat >> "$f" <<BLOCK
$block_begin

# !! Contents within this block are managed by opfsolve !!

case ":\$PATH:" in
    *:$bindir:*)
        ;;

    *)
        export PATH=$bindir\${PATH:+:\${PATH}}
        ;;
esac

$block_end
BLOCK
    say "added $bindir to PATH in $f"
}
if [ "$add_to_path" = yes ]; then
    write_block "$HOME/.bashrc"
    if [ -f "$HOME/.bash_profile" ]; then write_block "$HOME/.bash_profile"; else write_block "$HOME/.profile"; fi
    case "${SHELL:-}" in */zsh) zsh_user=1 ;; *) zsh_user=0 ;; esac
    { [ -f "$HOME/.zshrc" ] || [ $zsh_user -eq 1 ] || [ "$os" = Darwin ]; } && write_block "$HOME/.zshrc"
fi

say "installed: $("$bindir/opfsolve" --version)"
case ":$PATH:" in
    *":$bindir:"*) say "run: opfsolve case118_ieee     (opfsolve --help for options)" ;;
    *)
        if [ "$add_to_path" = yes ]; then
            say "opfsolve is on PATH in new shells. To use it in this one, run:"
            say "  export PATH=\"$bindir:\$PATH\""
        fi
        say "run: $bindir/opfsolve case118_ieee     (opfsolve --help for options)" ;;
esac
