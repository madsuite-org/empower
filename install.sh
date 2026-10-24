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
# release archive), OPFSOLVE_FC (the Fortran compiler to try first; default gfortran
# on PATH), and OPFSOLVE_SKIP_HSL=1 (install without HSL, for testing only).
set -eu

REPO="madsuite-org/opfsolve"
DIR="${OPFSOLVE_DIR:-$HOME/.opfsolve}"
REL="${OPFSOLVE_VERSION:-latest}"
IPOPT_SPEC="ipopt=3.14.20"
MAMBA_VERSION="2.9.0-0"
MA27_URL="https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/"

# Bold section headings and highlighted errors on a terminal; plain text otherwise.
if [ -t 1 ]; then B=$(printf '\033[1m'); R=$(printf '\033[0m'); E=$(printf '\033[1;31m'); else B=""; R=""; E=""; fi
say() { printf '    %s\n' "$*"; }
section() { printf '\n%s%s%s\n' "$B" "$*" "$R"; }
die() { printf '\n%serror:%s %s\n' "$E" "$R" "$*" >&2; exit 1; }

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
    printf '    %s ' "$1" > /dev/tty
    read -r ans < /dev/tty || ans=""
    echo "${ans:-$2}"
}

command -v curl >/dev/null || die "curl is required"
# get URL FILE ("-" for stdout); a progress bar only on a terminal.
if [ -t 2 ]; then get() { curl -fSL --progress-bar -o "$2" "$1"; }; else get() { curl -fsSL -o "$2" "$1"; }; fi
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
printf '%sopfsolve installer%s\n' "$B" "$R"
say "platform:      $plat"
say "install into:  $DIR"
say "add to PATH:   $add_to_path ($DIR/bin, in the shell startup files)"
if [ $yes -eq 0 ]; then
    [ $have_tty -eq 1 ] || die "unable to ask for confirmation; run with -y to accept the defaults (--help for options)"
    case "$(ask 'proceed? [Y/n]' y)" in
        y | Y | yes | Yes | YES) ;;
        *) die "cancelled" ;;
    esac
fi
mkdir -p "$DIR"
hsl="$DIR/hsl/libhsl.$ext"

# --- conda-forge tools (micromamba) ------------------------------------------
MM="$DIR/.mamba/micromamba"
export MAMBA_ROOT_PREFIX="$DIR/.mamba/root"
mamba() { "$MM" "$@"; }
# create_env PREFIX NAME SPEC...: from the release's lock file when it has one
# (no index download, no solve, the same packages for everyone), else by spec.
create_env() {
    p=$1; name=$2; shift 2
    lockf="$DIR/locks/$name.txt"
    if [ -f "$lockf" ]; then mamba create -y -q -p "$p" --file "$lockf"
    else mamba create -y -q -p "$p" -c conda-forge --override-channels "$@"; fi
}
get_mamba() {
    [ -x "$MM" ] && return 0
    mkdir -p "$DIR/.mamba"
    get "https://github.com/mamba-org/micromamba-releases/releases/download/$MAMBA_VERSION/micromamba-$cplat" "$MM" ||
        die "could not download micromamba"
    chmod +x "$MM"
}

# --- 1. opfsolve ------------------------------------------------------------------
section "[1/5] opfsolve"
if [ ! -x "$DIR/bin/opfsolve" ]; then
    if [ -n "${OPFSOLVE_ARCHIVE:-}" ]; then
        say "from $OPFSOLVE_ARCHIVE"
        tar xzf "$OPFSOLVE_ARCHIVE" -C "$DIR" --strip-components 1
    else
        if [ "$REL" = latest ]; then
            url="https://github.com/$REPO/releases/latest/download/opfsolve-$plat.tar.gz"
        else
            url="https://github.com/$REPO/releases/download/$REL/opfsolve-$plat.tar.gz"
        fi
        say "downloading $url"
        get "$url" - | tar xz -C "$DIR" --strip-components 1 || die "no opfsolve release for $plat at $url"
    fi
fi
say "$("$DIR/bin/opfsolve" --version) and its compiled model library"

# compile_hsl FC TOOLS_BIN: build the extracted package in $top into $hsl.
compile_hsl() {
    cfc=$1; tbin=$2
    if [ -f "$top/meson.build" ]; then
        # Coin-HSL and Coin-HSL Archive.
        rm -rf "$top/build" "$work/inst"
        (cd "$top" && PATH="${tbin:+$tbin:}$PATH" FC="$cfc" meson setup build --buildtype=release \
            --prefix="$work/inst" >"$work/build.log" 2>&1 &&
            PATH="${tbin:+$tbin:}$PATH" meson install -C build >>"$work/build.log" 2>&1) || return 1
        lib=$(find "$work/inst" -name "libcoinhsl*.$ext" -type f | head -1)
        [ -n "$lib" ] || return 1
        cp "$lib" "$hsl"
    else
        # The standalone MA27 package.
        if [ "$os" = Darwin ]; then shared="-dynamiclib"; else shared="-shared"; fi
        "$cfc" -O3 -fPIC $shared -o "$hsl" "$top/src/ma27d.f" -Wl,-rpath,"$DIR/ipopt/lib" >"$work/build.log" 2>&1 || return 1
    fi
}
# check_hsl: Ipopt solves a small problem with MA27 from $hsl.
check_hsl() { "$DIR/bin/opfsolve" --check-hsl --hsl-lib="$hsl" >"$work/check.log" 2>&1; }

# --- 2. HSL source: asked for first, so its compiler downloads alongside Ipopt ------
# Compile the user's own HSL package, optimized for this machine. HSL's personal
# licence permits modifying the software only to optimise its performance for
# the licensee's use on a particular platform, which is what this does; the
# result stays on this machine.
hsl_src=""          # a package to build (link or file), if any
hsl_mode=""         # build | lib | keep | skip
section "[2/5] HSL MA27 package"
if [ -n "${OPFSOLVE_HSL_LIB:-}" ]; then
    hsl_mode=lib; hsl_lib=$OPFSOLVE_HSL_LIB; say "your library $hsl_lib"
elif [ -n "${OPFSOLVE_HSL:-}" ]; then
    hsl_mode=build; hsl_src=$OPFSOLVE_HSL
elif [ -e "$hsl" ]; then
    hsl_mode=keep; say "already installed"
elif [ "${OPFSOLVE_SKIP_HSL:-}" = 1 ]; then
    hsl_mode=skip; say "skipped (OPFSOLVE_SKIP_HSL=1)"
else
    say "opfsolve needs HSL MA27, which you download yourself under HSL's free personal licence:"
    say "  1. request it at $MA27_URL"
    say "  2. HSL emails you a download link (valid for 7 days)"
    link=""
    if [ $have_tty -eq 1 ]; then
        # Open the request page for the user; without a desktop (ssh, a server)
        # the URL above is all there is.
        if [ "$os" = Darwin ]; then
            open "$MA27_URL" >/dev/null 2>&1 && say "opened the request page in your browser"
        elif { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; } && command -v xdg-open >/dev/null; then
            xdg-open "$MA27_URL" >/dev/null 2>&1 && say "opened the request page in your browser"
        fi
        link=$(ask 'paste the download link (or a path to the package or an HSL library):' "")
    fi
    [ -n "$link" ] || die "no HSL download link. Rerun with --hsl='<download link from the email>'"
    case "$link" in
        *.so | *.so.* | *.dylib) hsl_mode=lib; hsl_lib=$link ;;
        *) hsl_mode=build; hsl_src=$link ;;
    esac
fi

work="$DIR/hsl/build"
if [ "$hsl_mode" = build ]; then
    rm -rf "$work"; mkdir -p "$work"; chmod 700 "$DIR/hsl" "$work"
    case "$hsl_src" in
        http://* | https://*)
            # A download link from HSL's email is a page; the archive is under it.
            case "$hsl_src" in *.tar.gz | *.tgz | *.zip) ;; */) hsl_src="${hsl_src}package.tar.gz" ;; *) hsl_src="$hsl_src/package.tar.gz" ;; esac
            say "downloading your HSL package"
            get "$hsl_src" "$work/pkg" || die "could not download the HSL package (download links expire after 7 days)"
            pkg="$work/pkg" ;;
        *) [ -f "$hsl_src" ] || die "HSL package not found: $hsl_src"; pkg=$hsl_src ;;
    esac
    case "$(head -c 2 "$pkg" | od -An -tx1 | tr -d ' \n')" in
        1f8b) tar xzf "$pkg" -C "$work" ;;
        504b) command -v unzip >/dev/null || die "unzip is needed for a .zip HSL package"; unzip -q "$pkg" -d "$work" ;;
        *) die "the HSL package is neither .tar.gz nor .zip (an expired or already used download link returns a web page)" ;;
    esac
    top=$(find "$work" -mindepth 1 -maxdepth 1 -type d | head -1)
    [ -f "$top/meson.build" ] || [ -f "$top/src/ma27d.f" ] ||
        die "unrecognised HSL package: expected the MA27 package (src/ma27d.f) or Coin-HSL (meson.build)"
    say "package: $(basename "$top")"

    # An existing gfortran first ($OPFSOLVE_FC, or gfortran on PATH; Coin-HSL also
    # needs meson and ninja); otherwise conda-forge's, downloaded in the background
    # while Ipopt installs, into its own package cache so the two do not contend.
    sysfc="${OPFSOLVE_FC:-$(command -v gfortran 2>/dev/null || true)}"
    use_sys=0
    if [ -n "$sysfc" ] && "$sysfc" --version >/dev/null 2>&1; then
        if [ -f "$top/src/ma27d.f" ] || { command -v meson >/dev/null && command -v ninja >/dev/null; }; then
            use_sys=1
            say "compiler: $("$sysfc" --version | head -1) (yours)"
        fi
    fi
    # Download the compiler if needed, then compile, all in the background while
    # Ipopt installs; only the final check needs Ipopt. The compiler gets its own
    # package cache so the two downloads do not wait on each other's lock.
    start_build() {   # sys|conda
        if [ "$1" = sys ]; then
            say "compiling with -O3 in the background, alongside Ipopt"
            ( compile_hsl "$sysfc" "" && echo 0 > "$work/build.rc" || echo 1 > "$work/build.rc" ) &
        else
            tools=gfortran; lname=gfortran
            [ -f "$top/meson.build" ] && { tools="gfortran meson ninja pkg-config"; lname=gfortran-meson; }
            get_mamba
            say "compiler: downloading $tools from conda-forge, then compiling with -O3, alongside Ipopt"
            ( if MAMBA_ROOT_PREFIX="$work/mamba-root" create_env "$work/tools" $lname $tools >"$work/tools.log" 2>&1; then
                  fc=$(ls "$work"/tools/bin/*gfortran | head -1)
                  "$fc" --version | head -1 > "$work/fc.txt"
                  compile_hsl "$fc" "$work/tools/bin" && echo 0 > "$work/build.rc" || echo 1 > "$work/build.rc"
              else echo 2 > "$work/build.rc"; fi ) &
        fi
        build_pid=$!
    }
    if [ $use_sys -eq 1 ]; then "$sysfc" --version | head -1 > "$work/fc.txt"; start_build sys; else start_build conda; fi
fi

# --- 3. Ipopt from conda-forge -----------------------------------------------------
section "[3/5] Ipopt"
if [ ! -e "$DIR/ipopt/lib/libipopt.$ext" ]; then
    get_mamba
    if [ -f "$DIR/locks/ipopt.txt" ]; then say "installing $IPOPT_SPEC from conda-forge (locked: $(grep -c '^https://' "$DIR/locks/ipopt.txt") packages)"
    else say "installing $IPOPT_SPEC from conda-forge"; fi
    create_env "$DIR/ipopt" ipopt "$IPOPT_SPEC" >/dev/null || die "could not install Ipopt from conda-forge"
    [ -e "$DIR/ipopt/lib/libipopt.$ext" ] || die "libipopt.$ext missing after installing Ipopt"
    echo "conda-forge $IPOPT_SPEC" > "$DIR/ipopt/SOURCE"
fi
say "$(cat "$DIR/ipopt/SOURCE" 2>/dev/null): $DIR/ipopt/lib/libipopt.$ext"

# --- HSL: build, link, or keep -------------------------------------------------------

case "$hsl_mode" in
    lib)
        [ -f "$hsl_lib" ] || die "HSL library not found: $hsl_lib"
        mkdir -p "$DIR/hsl"; ln -sf "$hsl_lib" "$hsl"
        echo "your library $hsl_lib" > "$DIR/hsl/SOURCE"
        mkdir -p "$work"
        check_hsl || die "Ipopt cannot use MA27 from $hsl_lib"
        rm -rf "$work"
        section "[4/5] HSL MA27"; say "your library $hsl_lib (MA27 checked)" ;;
    build)
        section "[4/5] HSL MA27"
        wait "$build_pid" 2>/dev/null || true
        rc=$(cat "$work/build.rc" 2>/dev/null || echo 1)
        if [ "$rc" = 0 ] && check_hsl; then
            :
        elif [ $use_sys -eq 1 ]; then
            say "the build with your compiler did not work with Ipopt; using conda-forge's gfortran instead"
            rm -f "$hsl" "$work/build.rc"
            use_sys=0; start_build conda; wait "$build_pid" 2>/dev/null || true
            rc=$(cat "$work/build.rc" 2>/dev/null || echo 1)
            [ "$rc" = 0 ] || die "HSL build failed; see $work/tools.log and $work/build.log"
            check_hsl || die "the HSL library built, but Ipopt cannot use its MA27; see $work/check.log"
        else
            [ "$rc" = 2 ] && die "could not install the Fortran compiler; see $work/tools.log"
            [ "$rc" = 0 ] || die "HSL build failed; see $work/build.log"
            die "the HSL library built, but Ipopt cannot use its MA27; see $work/check.log"
        fi
        echo "built from $(basename "$top") on $(date -u +%Y-%m-%d) with $(cat "$work/fc.txt")" > "$DIR/hsl/SOURCE"
        rm -rf "$work"
        say "$hsl (MA27 checked)" ;;
    keep) section "[4/5] HSL MA27"; say "$(cat "$DIR/hsl/SOURCE" 2>/dev/null): $hsl" ;;
    skip) ;;
esac

# --- 4. PATH, the way juliaup does it ----------------------------------------------
section "[5/5] PATH"
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
    say "$bindir added in $f"
}
if [ "$add_to_path" = yes ]; then
    write_block "$HOME/.bashrc"
    if [ -f "$HOME/.bash_profile" ]; then write_block "$HOME/.bash_profile"; else write_block "$HOME/.profile"; fi
    case "${SHELL:-}" in */zsh) zsh_user=1 ;; *) zsh_user=0 ;; esac
    { [ -f "$HOME/.zshrc" ] || [ $zsh_user -eq 1 ] || [ "$os" = Darwin ]; } && write_block "$HOME/.zshrc"
else
    say "left unchanged (--add-to-path no)"
fi

# Drop downloaded packages that are no longer needed.
[ -x "$MM" ] && mamba clean -a -y -q >/dev/null 2>&1 || true

printf '\n%sopfsolve is installed.%s\n' "$B" "$R"
case ":$PATH:" in
    *":$bindir:"*) say "try:  opfsolve case118_ieee        (opfsolve --help for options)" ;;
    *)
        if [ "$add_to_path" = yes ]; then
            say "new terminals have opfsolve on PATH. In this one, first run:"
            say "  export PATH=\"$bindir:\$PATH\""
            say "then try:  opfsolve case118_ieee        (opfsolve --help for options)"
        else
            say "try:  $bindir/opfsolve case118_ieee"
        fi ;;
esac
