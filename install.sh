#!/bin/sh
# Install the empower command: compiled ExaModelsPower AC OPF models, Ipopt from
# conda-forge, and the user's own HSL MA27 compiled on this machine.
#
#   curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/madsuite-org/empower/main/install.sh | sh
#
# Everything is inside main(), called on the last line, so a download cut short
# runs nothing.
set -eu

REPO="madsuite-org/empower"
IPOPT_SPEC="ipopt=3.14.20"
MAMBA_VERSION="2.9.0-0"
MA27_URL="https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/"

usage() {
    cat <<'EOF'
Install empower (compiled ExaModelsPower AC OPF models, Ipopt from conda-forge,
and your own HSL MA27, compiled on this machine).

Like juliaup, it installs into ~/.empower, puts ~/.empower/bin on PATH in the
shell startup files inside a marked block, and asks for confirmation unless -y.
Running it again upgrades to the latest release.

  curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/madsuite-org/empower/main/install.sh | sh
  curl ... | sh -s -- -y --hsl='<download link from HSL's email>'

Options:
  -y, --yes                accept the defaults without asking
  -p, --path DIR           install directory (default ~/.empower)
      --add-to-path yes|no add DIR/bin to PATH in shell startup files (default yes)
      --backends LIST      the solvers to install, comma-separated (default cpu):
                             cpu  Ipopt with HSL MA27 (empower --device=cpu)
                             cuda MadNLP with cuDSS on an NVIDIA GPU (empower
                                  --device=cuda; Linux x86_64, NVIDIA driver for CUDA
                                  12.8 or newer). It downloads the NVIDIA libraries
                                  it uses (CUDA runtime, cuBLAS, cuSPARSE, cuSOLVER,
                                  cuDSS; about 1.7 GB) as the Julia packages'
                                  artifacts, from the URLs and SHA-256 sums those
                                  packages publish; empower does not distribute them
      --hsl LINK|FILE      the HSL download link from HSL's email, or the
                           downloaded package, to compile on this machine
      --hsl-lib FILE       an HSL library you already have, used as is
      --no-hsl             install the cpu backend without HSL; empower then needs
                           --hsl-lib, or another linear solver in --options
      --uninstall          remove DIR and the PATH blocks from the startup files
  -h, --help

The cpu backend uses HSL MA27, which empower never distributes. Request the free
personal licence at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/ ; HSL emails a
download link, which the installer asks for unless --hsl, --hsl-lib or --no-hsl is
given.

Environment: EMPOWER_DIR, EMPOWER_BACKENDS, EMPOWER_HSL, EMPOWER_HSL_LIB,
EMPOWER_SKIP_HSL=1 (as the options);
EMPOWER_VERSION (release tag, default latest); EMPOWER_ARCHIVE (a local release
archive, for testing; EMPOWER_CUDA_ARCHIVE likewise); EMPOWER_FC (the Fortran compiler to try first; default
gfortran on PATH).
EOF
}

# Bold section headings and highlighted errors on a terminal; plain text otherwise.
if [ -t 1 ]; then B=$(printf '\033[1m'); R=$(printf '\033[0m'); E=$(printf '\033[1;31m'); else B=""; R=""; E=""; fi
say() { printf '    %s\n' "$*"; }
section() { printf '\n%s%s%s\n' "$B" "$*" "$R"; }
die() { printf '\n%serror:%s %s\n' "$E" "$R" "$*" >&2; exit 1; }

# get URL FILE ("-" for stdout): https only, a progress bar only on a terminal.
get() {
    if [ -t 2 ]; then curl --proto '=https' --tlsv1.2 -fSL --progress-bar -o "$2" "$1"
    else curl --proto '=https' --tlsv1.2 -fsSL -o "$2" "$1"; fi
}

sha256() {   # FILE -> hex digest
    if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null; then shasum -a 256 "$1" | cut -d' ' -f1
    else die "sha256sum or shasum is needed to verify downloads"; fi
}

verify() {   # FILE EXPECTED-SHA256 WHAT
    got=$(sha256 "$1")
    [ "$got" = "$2" ] || die "checksum mismatch for $3 (expected $2, got $got); the download may be corrupt or tampered with"
}

absolute() {   # PATH -> absolute path (the file need not exist)
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s/%s\n' "$PWD" "$1" ;;
    esac
}

# Startup files that hold the PATH block.
rc_files() {
    printf '%s\n' "$HOME/.bashrc"
    if [ -f "$HOME/.bash_profile" ]; then printf '%s\n' "$HOME/.bash_profile"; else printf '%s\n' "$HOME/.profile"; fi
    case "${SHELL:-}" in */zsh) zsh_user=1 ;; *) zsh_user=0 ;; esac
    if [ -f "$HOME/.zshrc" ] || [ $zsh_user -eq 1 ] || [ "$(uname -s)" = Darwin ]; then printf '%s\n' "$HOME/.zshrc"; fi
}

block_begin="# >>> empower initialize >>>"
block_end="# <<< empower initialize <<<"
remove_block() {   # FILE: drop an earlier empower block, if any
    [ -f "$1" ] && grep -qF "$block_begin" "$1" || return 0
    awk -v b="$block_begin" -v e="$block_end" '$0 == b {skip = 1} !skip {print} $0 == e {skip = 0}' "$1" > "$1.empower-tmp" &&
        cat "$1.empower-tmp" > "$1" && rm -f "$1.empower-tmp"
}

main() {
    DIR="${EMPOWER_DIR:-$HOME/.empower}"
    REL="${EMPOWER_VERSION:-latest}"
    yes=0
    add_to_path=yes
    uninstall=0
    while [ $# -gt 0 ]; do
        case "$1" in
            -y | --yes) yes=1 ;;
            -p | --path) shift; DIR=${1:?--path needs a directory} ;;
            --path=*) DIR=${1#*=} ;;
            --add-to-path) shift; add_to_path=${1:?--add-to-path needs yes or no} ;;
            --add-to-path=*) add_to_path=${1#*=} ;;
            --hsl) shift; EMPOWER_HSL=${1:?--hsl needs a link or file} ;;
            --hsl=*) EMPOWER_HSL=${1#*=} ;;
            --hsl-lib) shift; EMPOWER_HSL_LIB=${1:?--hsl-lib needs a file} ;;
            --hsl-lib=*) EMPOWER_HSL_LIB=${1#*=} ;;
            --backends) shift; EMPOWER_BACKENDS=${1:?--backends needs a list such as cpu,cuda} ;;
            --backends=*) EMPOWER_BACKENDS=${1#*=} ;;
            --no-hsl) EMPOWER_SKIP_HSL=1 ;;
            --uninstall) uninstall=1 ;;
            -h | --help) usage; exit 0 ;;
            *) die "unknown option $1 (see --help)" ;;
        esac
        shift
    done
    case "$add_to_path" in yes | 1) add_to_path=yes ;; no | 0) add_to_path=no ;; *) die "--add-to-path takes yes or no" ;; esac
    backends=${EMPOWER_BACKENDS:-cpu}
    cpu=0; cuda=0
    for b in $(echo "$backends" | tr ',' ' '); do
        case "$b" in
            cpu) cpu=1 ;;
            cuda) cuda=1 ;;
            amdgpu) die "the amdgpu backend is not available in this release" ;;
            *) die "unknown backend $b (--backends takes cpu and cuda, comma-separated)" ;;
        esac
    done
    [ $cpu -eq 1 ] || [ $cuda -eq 1 ] || die "--backends needs cpu, cuda, or both"
    # Sections: empower, then HSL package, Ipopt and HSL for cpu, then cuda, then PATH.
    nsec=$((2 + 3 * cpu + cuda)); isec=0
    sec() { isec=$((isec + 1)); section "[$isec/$nsec] $1"; }
    DIR=$(absolute "$DIR")
    [ -n "${EMPOWER_HSL_LIB:-}" ] && EMPOWER_HSL_LIB=$(absolute "$EMPOWER_HSL_LIB")
    case "${EMPOWER_HSL:-}" in "" | http://* | https://*) ;; *) EMPOWER_HSL=$(absolute "$EMPOWER_HSL") ;; esac

    # Under `curl ... | sh`, standard input is the script: prompts read the terminal.
    have_tty=0
    (: < /dev/tty) 2>/dev/null && have_tty=1
    ask() {   # question default -> answer
        [ $have_tty -eq 1 ] || { echo "$2"; return; }
        printf '    %s ' "$1" > /dev/tty
        read -r ans < /dev/tty || ans=""
        echo "${ans:-$2}"
    }

    if [ $uninstall -eq 1 ]; then
        printf '%sempower uninstall%s\n' "$B" "$R"
        say "remove:  $DIR"
        say "and the empower PATH block from: $(rc_files | tr '\n' ' ')"
        if [ $yes -eq 0 ]; then
            [ $have_tty -eq 1 ] || die "unable to ask for confirmation; run with -y"
            case "$(ask 'proceed? [y/N]' n)" in y | Y | yes | Yes | YES) ;; *) die "cancelled" ;; esac
        fi
        [ -x "$DIR/bin/empower" ] || [ ! -e "$DIR" ] || die "$DIR does not look like an empower install; not removing it"
        rm -rf "$DIR"
        rc_files | while IFS= read -r f; do remove_block "$f"; done
        say "done"
        return 0
    fi

    command -v curl >/dev/null || die "curl is required"
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
            if [ "$arch" = x86_64 ]; then
                cplat=linux-64; mamba_sha=366cd9cd8be14df1ab8ed50352a82111082a36686b2d389fdb79a92c3fafb3e3
            else
                cplat=linux-aarch64; mamba_sha=9f93b974adcb4d166996af969b6cd371287d1a3e52733704727884d9b74cb7a7
            fi
            ext=so ;;
        Darwin)
            [ "$arch" = aarch64 ] || die "macOS on Intel is not supported; empower needs Apple silicon (arm64)"
            plat="macos-$arch"; cplat=osx-arm64; ext=dylib
            mamba_sha=ec2a072f028e1a7cf20f3e2e74d5a8127cf5a5f27636375b5359811565f4e5be ;;
        *) die "unsupported OS: $os (on Windows, use install.ps1)" ;;
    esac

    # The release to install: the latest one unless EMPOWER_VERSION names a tag.
    if [ -n "${EMPOWER_ARCHIVE:-}" ]; then
        tag="local archive"
    elif [ "$REL" = latest ]; then
        tag=$(curl --proto '=https' --tlsv1.2 -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest" 2>/dev/null | sed 's|.*/tag/||') ||
            die "could not reach GitHub to find the latest empower release"
        case "$tag" in v*) ;; *) die "could not find the latest empower release" ;; esac
    else
        tag=$REL
    fi

    [ $cuda -eq 1 ] && [ "$plat" != linux-x86_64 ] && die "the cuda backend is available on Linux x86_64 only"
    printf '%sempower installer%s\n' "$B" "$R"
    say "platform:      $plat"
    say "release:       $tag"
    say "install into:  $DIR"
    say "add to PATH:   $add_to_path ($DIR/bin, in the shell startup files)"
    say "backends:      $(echo "$backends" | sed 's/,/, /g')"
    [ $cuda -eq 1 ] && say "               (cuda: and its NVIDIA libraries, about 1.7 GB)"
    if [ $yes -eq 0 ]; then
        [ $have_tty -eq 1 ] || die "unable to ask for confirmation; run with -y to accept the defaults (--help for options)"
        case "$(ask 'proceed? [Y/n]' y)" in
            y | Y | yes | Yes | YES) ;;
            *) die "cancelled" ;;
        esac
    fi
    mkdir -p "$DIR"
    hsl="$DIR/hsl/libhsl.$ext"

    # --- conda-forge tools (micromamba) ---------------------------------------
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
        [ -x "$MM" ] && [ "$(sha256 "$MM")" = "$mamba_sha" ] && return 0
        mkdir -p "$DIR/.mamba"
        get "https://github.com/mamba-org/micromamba-releases/releases/download/$MAMBA_VERSION/micromamba-$cplat" "$MM.part" ||
            die "could not download micromamba"
        verify "$MM.part" "$mamba_sha" "micromamba $MAMBA_VERSION"
        chmod +x "$MM.part" && mv -f "$MM.part" "$MM"
    }

    # --- 1. empower: download, verify, unpack aside, then swap in ----------------
    sec "empower"
    installed=$(cat "$DIR/RELEASE" 2>/dev/null || true)
    if [ -x "$DIR/bin/empower" ] && [ "$installed" = "$tag" ] && [ -z "${EMPOWER_ARCHIVE:-}" ]; then
        say "$tag is installed"
    else
        stage="$DIR/.staging"
        rm -rf "$stage"; mkdir -p "$stage"
        if [ -n "${EMPOWER_ARCHIVE:-}" ]; then
            say "from $EMPOWER_ARCHIVE"
            cp "$EMPOWER_ARCHIVE" "$stage/archive.tar.gz"
        else
            base="https://github.com/$REPO/releases/download/$tag"
            say "downloading $base/empower-$plat.tar.gz"
            get "$base/empower-$plat.tar.gz" "$stage/archive.tar.gz" || die "no empower $tag release for $plat"
            get "$base/SHA256SUMS" "$stage/SHA256SUMS" || die "could not download the checksums of release $tag"
            want=$(awk -v f="empower-$plat.tar.gz" '$2 == f || $2 == "*" f {print $1}' "$stage/SHA256SUMS")
            [ -n "$want" ] || die "release $tag lists no checksum for empower-$plat.tar.gz"
            verify "$stage/archive.tar.gz" "$want" "empower-$plat.tar.gz"
            say "checksum verified"
        fi
        mkdir -p "$stage/new"
        tar xzf "$stage/archive.tar.gz" -C "$stage/new" --strip-components 1 || die "could not unpack the empower archive"
        [ -x "$stage/new/bin/empower" ] || die "the empower archive has no bin/empower"
        # Replace what the archive provides; keep ipopt/, hsl/, cases/, and .mamba/.
        for item in "$stage/new"/* "$stage/new"/.[!.]*; do
            [ -e "$item" ] || continue
            name=$(basename "$item")
            rm -rf "${DIR:?}/${name:?}"
            mv "$item" "$DIR/$name"
        done
        printf '%s\n' "$tag" > "$DIR/RELEASE"
        rm -rf "$stage"
        [ -n "$installed" ] && [ "$installed" != "$tag" ] && say "upgraded from $installed"
    fi
    say "$("$DIR/bin/empower" --version) and its compiled model library"

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
    check_hsl() { "$DIR/bin/empower" --check-hsl --hsl-lib="$hsl" >"$work/check.log" 2>&1; }

    # --- the cpu backend: HSL, Ipopt ---------------------------------------------
    if [ $cpu -eq 1 ]; then
        # --- 2. HSL source: asked for first, so its compiler downloads alongside Ipopt --
        # Compile the user's own HSL package, optimized for this machine. HSL's personal
        # licence permits modifying the software only to optimise its performance for
        # the licensee's use on a particular platform, which is what this does; the
        # result stays on this machine.
        hsl_src=""          # a package to build (link or file), if any
        hsl_mode=""         # build | lib | keep | skip
        sec "HSL MA27 package"
        if [ -n "${EMPOWER_HSL_LIB:-}" ]; then
            hsl_mode=lib; hsl_lib=$EMPOWER_HSL_LIB; say "your library $hsl_lib"
        elif [ -n "${EMPOWER_HSL:-}" ]; then
            hsl_mode=build; hsl_src=$EMPOWER_HSL
        elif [ -e "$hsl" ]; then
            hsl_mode=keep; say "already installed"
        elif [ "${EMPOWER_SKIP_HSL:-}" = 1 ]; then
            hsl_mode=skip; say "skipped (--no-hsl): empower then needs --hsl-lib, or another linear solver in --options"
        else
            say "empower needs HSL MA27, which you download yourself under HSL's free personal licence:"
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
                link=$(ask 'paste the download link, a path to the package or to an HSL library, or "skip":' "")
            fi
            [ -n "$link" ] || die "no HSL download link. Rerun with --hsl='<download link from the email>', --hsl-lib=FILE, or --no-hsl"
            case "$link" in
                skip | Skip | SKIP) hsl_mode=skip; say "skipped" ;;
                http://* | https://*) hsl_mode=build; hsl_src=$link ;;
                *) link=$(absolute "$link")
                   case "$link" in
                       *.so | *.so.* | *.dylib) hsl_mode=lib; hsl_lib=$link ;;
                       *) hsl_mode=build; hsl_src=$link ;;
                   esac ;;
            esac
        fi

        # The build works in a private temporary directory: conda-forge's gfortran
        # cannot run from a path with spaces, which the install directory may have.
        work=$(mktemp -d "${TMPDIR:-/tmp}/empower-hsl.XXXXXX") || die "could not create a temporary directory"
        chmod 700 "$work"
        mkdir -p "$DIR/hsl"
        if [ "$hsl_mode" = build ]; then
            case "$hsl_src" in
                http://* | https://*)
                    # HSL's emailed links are http://; fetch them over https. The link is
                    # a page; the archive is under it.
                    hsl_src="https://${hsl_src#*://}"
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
            [ -n "$top" ] && { [ -f "$top/meson.build" ] || [ -f "$top/src/ma27d.f" ]; } ||
                die "unrecognised HSL package: expected the MA27 package (src/ma27d.f) or Coin-HSL (meson.build)"
            say "package: $(basename "$top")"

            # An existing gfortran first ($EMPOWER_FC, or gfortran on PATH; Coin-HSL also
            # needs meson and ninja); otherwise conda-forge's.
            sysfc="${EMPOWER_FC:-$(command -v gfortran 2>/dev/null || true)}"
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

        # --- 3. Ipopt from conda-forge; recreated when the release's lock changes ------
        sec "Ipopt"
        lock="$DIR/locks/ipopt.txt"
        if [ -e "$DIR/ipopt/lib/libipopt.$ext" ] && { [ ! -f "$lock" ] || cmp -s "$lock" "$DIR/ipopt/LOCK"; }; then
            :
        else
            get_mamba
            if [ -f "$lock" ]; then say "installing $IPOPT_SPEC from conda-forge (locked: $(grep -c '^https://' "$lock") packages)"
            else say "installing $IPOPT_SPEC from conda-forge"; fi
            rm -rf "$DIR/ipopt.new"
            create_env "$DIR/ipopt.new" ipopt "$IPOPT_SPEC" >/dev/null || die "could not install Ipopt from conda-forge"
            [ -e "$DIR/ipopt.new/lib/libipopt.$ext" ] || die "libipopt.$ext missing after installing Ipopt"
            echo "conda-forge $IPOPT_SPEC" > "$DIR/ipopt.new/SOURCE"
            [ -f "$lock" ] && cp "$lock" "$DIR/ipopt.new/LOCK"
            rm -rf "$DIR/ipopt" && mv "$DIR/ipopt.new" "$DIR/ipopt"
        fi
        say "$(cat "$DIR/ipopt/SOURCE" 2>/dev/null): $DIR/ipopt/lib/libipopt.$ext"

        # --- 4. HSL: build, link, or keep ----------------------------------------------
        case "$hsl_mode" in
            lib)
                sec "HSL MA27"
                [ -f "$hsl_lib" ] || die "HSL library not found: $hsl_lib"
                ln -sf "$hsl_lib" "$hsl"
                echo "your library $hsl_lib" > "$DIR/hsl/SOURCE"
                check_hsl || die "Ipopt cannot use MA27 from $hsl_lib; see $work/check.log"
                rm -rf "$work"
                say "your library $hsl_lib (MA27 checked)" ;;
            build)
                sec "HSL MA27"
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
            keep)
                sec "HSL MA27"
                check_hsl || die "the installed HSL library no longer works with Ipopt; rerun with --hsl"
                rm -rf "$work"
                say "$(cat "$DIR/hsl/SOURCE" 2>/dev/null): $hsl (MA27 checked)" ;;
            skip) rm -rf "$work" ;;
        esac
    fi

    # --- cuda backend: empower-cuda, then the NVIDIA libraries it loads -------------
    if [ $cuda -eq 1 ]; then
        sec "cuda: empower-cuda and its NVIDIA libraries"
        # The archive for the newest CUDA the driver supports: CUDA 13 (13.0 and newer)
        # or CUDA 12 (12.8 and newer). EMPOWER_CUDA=12|13 chooses instead.
        cmaj=${EMPOWER_CUDA:-}
        if [ -z "$cmaj" ]; then
            v=$(nvidia-smi 2>/dev/null | sed -n 's/.*CUDA Version: *\([0-9][0-9.]*\).*/\1/p' | head -1)
            [ -n "$v" ] || die "could not read the CUDA version of your NVIDIA driver (nvidia-smi); install the driver, or set EMPOWER_CUDA=12 or 13"
            vmaj=${v%%.*}; vmin=${v#*.}; vmin=${vmin%%.*}
            if [ "$vmaj" -ge 13 ]; then cmaj=13
            elif [ "$vmaj" -eq 12 ] && [ "$vmin" -ge 8 ]; then cmaj=12
            else die "your NVIDIA driver supports CUDA $v; the cuda backend needs CUDA 12.8 or newer, so update the driver"; fi
            # CUDA 13 dropped Volta (compute capability 7.0): such a GPU takes the CUDA 12 build.
            cc=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | sort -t. -k1,1n -k2,2n | head -1)
            if [ "$cmaj" = 13 ] && [ -n "$cc" ] && [ "${cc%%.*}" -lt 7 -o "$cc" = 7.0 -o "$cc" = 7.2 ]; then
                cmaj=12
                say "NVIDIA driver supports CUDA $v, GPU compute capability $cc: installing the CUDA 12 build (CUDA 13 has no Volta)"
            else
                say "NVIDIA driver supports CUDA $v: installing the CUDA $cmaj build"
            fi
        fi
        case "$cmaj" in 12 | 13) ;; *) die "EMPOWER_CUDA must be 12 or 13" ;; esac
        ginstalled=$(cat "$DIR/cuda/RELEASE" 2>/dev/null || true)
        if [ -x "$DIR/cuda/bin/empower-cuda" ] && [ "$ginstalled" = "$tag cuda$cmaj" ] && [ -z "${EMPOWER_CUDA_ARCHIVE:-}" ]; then
            say "$tag is installed"
        else
            gstage="$DIR/.staging-cuda"
            rm -rf "$gstage"; mkdir -p "$gstage/new"
            if [ -n "${EMPOWER_CUDA_ARCHIVE:-}" ]; then
                say "from $EMPOWER_CUDA_ARCHIVE"
                cp "$EMPOWER_CUDA_ARCHIVE" "$gstage/archive.tar.gz"
            else
                base="https://github.com/$REPO/releases/download/$tag"
                say "downloading $base/empower-cuda$cmaj-$plat.tar.gz"
                get "$base/empower-cuda$cmaj-$plat.tar.gz" "$gstage/archive.tar.gz" || die "no empower-cuda$cmaj $tag release for $plat"
                get "$base/SHA256SUMS" "$gstage/SHA256SUMS" || die "could not download the checksums of release $tag"
                want=$(awk -v f="empower-cuda$cmaj-$plat.tar.gz" '$2 == f || $2 == "*" f {print $1}' "$gstage/SHA256SUMS")
                [ -n "$want" ] || die "release $tag lists no checksum for empower-cuda$cmaj-$plat.tar.gz"
                verify "$gstage/archive.tar.gz" "$want" "empower-cuda$cmaj-$plat.tar.gz"
                say "checksum verified"
            fi
            tar xzf "$gstage/archive.tar.gz" -C "$gstage/new" --strip-components 1 || die "could not unpack the empower-cuda archive"
            [ -x "$gstage/new/bin/empower-cuda" ] || die "the empower-cuda archive has no bin/empower-cuda"
            rm -rf "$DIR/cuda"
            mv "$gstage/new" "$DIR/cuda"
            printf '%s\n' "$tag cuda$cmaj" > "$DIR/cuda/RELEASE"
            rm -rf "$gstage"
        fi
        # The NVIDIA libraries, kept across upgrades in their own depot. CUDA_Driver (a
        # forward-compatibility driver) is not downloaded: empower-cuda uses the system
        # driver, and its Julia package only needs the directory to exist.
        arts="$DIR/cuda-depot/artifacts"
        mkdir -p "$arts"
        while IFS="$(printf '\t')" read -r name version hash url sum; do
            [ -n "$hash" ] || continue
            if [ "$name" = CUDA_Driver ]; then
                mkdir -p "$arts/$hash"
                continue
            fi
            if [ -f "$arts/$hash/.empower-verified" ]; then
                say "$name $version: installed"
                continue
            fi
            say "$name $version: downloading $url"
            get "$url" "$arts/$hash.tar.gz" || die "could not download $name"
            verify "$arts/$hash.tar.gz" "$sum" "$name $version"
            rm -rf "$arts/$hash"; mkdir -p "$arts/$hash"
            tar xzf "$arts/$hash.tar.gz" -C "$arts/$hash" || die "could not unpack $name"
            rm -f "$arts/$hash.tar.gz"
            : > "$arts/$hash/.empower-verified"
            say "$name $version: checksum verified"
        done < "$DIR/cuda/nvidia-artifacts.tsv"
        # Artifacts the installed empower-cuda no longer uses.
        for d in "$arts"/*; do
            [ -d "$d" ] || continue
            grep -q "$(basename "$d")" "$DIR/cuda/nvidia-artifacts.tsv" || rm -rf "$d"
        done
        say "$("$DIR/bin/empower" --version) with the cuda backend (CUDA $cmaj)"
    fi

    # --- 5. PATH, the way juliaup does it ------------------------------------------
    sec "PATH"
    bindir="$DIR/bin"
    if [ "$add_to_path" = yes ]; then
        rc_files | while IFS= read -r f; do
            remove_block "$f"
            # The path is quoted, so an install directory with spaces works.
            cat >> "$f" <<BLOCK
$block_begin

# !! Contents within this block are managed by empower !!

case ":\$PATH:" in
    *:"$bindir":*)
        ;;

    *)
        export PATH="$bindir\${PATH:+:\${PATH}}"
        ;;
esac

$block_end
BLOCK
            say "$bindir added in $f"
        done
    else
        say "left unchanged (--add-to-path no)"
    fi

    # Drop downloaded packages that are no longer needed.
    if [ -x "$MM" ]; then mamba clean -a -y -q >/dev/null 2>&1 || true; fi

    printf '\n%sempower is installed.%s\n' "$B" "$R"
    case ":$PATH:" in
        *":$bindir:"*) say "try:  empower case118_ieee        (empower --help for options)" ;;
        *)
            if [ "$add_to_path" = yes ]; then
                say "new terminals have empower on PATH. In this one, first run:"
                say "  export PATH=\"$bindir:\$PATH\""
                say "then try:  empower case118_ieee        (empower --help for options)"
            else
                say "try:  \"$bindir/empower\" case118_ieee"
            fi ;;
    esac
}

main "$@"
