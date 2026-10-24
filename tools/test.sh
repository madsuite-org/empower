#!/bin/sh
# Install a built archive with install.sh and solve cases with both forms.
set -eu
plat=$1
cd "$(dirname "$0")/.."
root=$(pwd)
t=$(mktemp -d)
cd "$t"
export OPFSOLVE_DIR="$t/opfsolve" OPFSOLVE_ARCHIVE="$root/opfsolve-$plat.tar.gz"
sh "$root/install.sh" case14_ieee case118_ieee --form=polar --options="tol=1e-8 max_iter=500" --output=polar
sh "$root/install.sh" case14_ieee case118_ieee --form=rect --output=rect
"$OPFSOLVE_DIR/bin/opfsolve" --help > /dev/null
for f in polar rect; do
    cat "$f/summary.csv"
    for c in pglib_opf_case14_ieee pglib_opf_case118_ieee; do
        grep -q '"termination_status": "LOCALLY_SOLVED"' "$f/$c/solution.json"
        # IDAES ships no HSL for macOS x86_64; opfsolve falls back to MUMPS there.
        if [ "$plat" = macos-x86_64 ]; then ls=mumps; else ls=ma27; fi
        grep -q "linear solver $ls" "$f/$c/ipopt.log"
    done
done
# Objectives against the AC values in the PGLib-OPF README (5 significant digits).
awk -F, 'FNR > 1 {
    ref = ($1 ~ /case14_/) ? 2178.1 : 97214
    if (($5 - ref) / ref > 1e-4 || ($5 - ref) / ref < -1e-4) { print "objective mismatch: " $0; bad = 1 }
} END { exit bad }' polar/summary.csv rect/summary.csv
echo "all checks passed"
