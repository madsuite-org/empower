#!/bin/sh
# Collect the corresponding source of the GPL and LGPL libraries bundled with the
# Julia runtime in the release archives (SuiteSparse, GMP, MPFR, libgit2), at the
# exact versions Julia JULIA_VERSION ships, into OUTDIR/opfsolve-sources-gpl.tar.
# The release attaches it next to the archives.
#
#   tools/gpl-sources.sh JULIA_VERSION OUTDIR
set -eu
jv=$1
out=$2
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
get() { curl --proto '=https' --tlsv1.2 -fsSL -o "$2" "$1"; }

# Julia records the version of each external library in deps/<name>.version.
ver() {   # file variable
    get "https://raw.githubusercontent.com/JuliaLang/julia/v$jv/deps/$1.version" "$t/$1.version"
    v=$(sed -n "s/^$2 *:\{0,1\}= *//p" "$t/$1.version" | head -1)
    [ -n "$v" ] || { echo "no $2 in deps/$1.version of Julia $jv" >&2; exit 1; }
    printf '%s\n' "$v"
}
ss=$(ver libsuitesparse LIBSUITESPARSE_VER)
gmp=$(ver gmp GMP_VER)
mpfr=$(ver mpfr MPFR_VER)
git2=$(ver libgit2 LIBGIT2_BRANCH)

src="$t/opfsolve-sources-gpl"
mkdir -p "$src"
get "https://github.com/DrTimothyAldenDavis/SuiteSparse/archive/refs/tags/v$ss.tar.gz" "$src/SuiteSparse-$ss.tar.gz"
get "https://ftp.gnu.org/gnu/gmp/gmp-$gmp.tar.xz" "$src/gmp-$gmp.tar.xz"
get "https://ftp.gnu.org/gnu/mpfr/mpfr-$mpfr.tar.xz" "$src/mpfr-$mpfr.tar.xz"
get "https://github.com/libgit2/libgit2/archive/refs/tags/$git2.tar.gz" "$src/libgit2-$git2.tar.gz"
cat > "$src/README.md" <<README
# Source of the GPL and LGPL libraries in the opfsolve release archives

The Julia runtime bundled in the opfsolve archives (Julia $jv) includes these
libraries, built by Julia's BinaryBuilder recipes from the following upstream
sources, which are included here:

| Library | Version | License | File |
|---|---|---|---|
| SuiteSparse (CHOLMOD, SPQR, UMFPACK, ...) | $ss | mix of BSD-3, LGPL-2.1+, GPL-2.0+ | SuiteSparse-$ss.tar.gz |
| GMP | $gmp | LGPL-3.0+ or GPL-2.0+ | gmp-$gmp.tar.xz |
| MPFR | $mpfr | LGPL-3.0+ | mpfr-$mpfr.tar.xz |
| libgit2 | $git2 | GPL-2.0 with linking exception | libgit2-$git2.tar.gz |

The build recipes, including any patches applied, are in
https://github.com/JuliaPackaging/Yggdrasil (directories S/SuiteSparse, G/GMP,
M/MPFR, L/LibGit2), and Julia's own record of these versions is in
https://github.com/JuliaLang/julia/tree/v$jv/deps.
README
mkdir -p "$out"
tar cf "$out/opfsolve-sources-gpl.tar" -C "$t" opfsolve-sources-gpl
ls -la "$out/opfsolve-sources-gpl.tar"
