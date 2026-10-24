#!/bin/sh
# Install a built archive with install.sh, then solve cases with both forms.
set -eu
plat=$1
cd "$(dirname "$0")/.."
root=$(pwd)
t=$(mktemp -d)
cd "$t"
# CI has no HSL licence: install without HSL and ask for MUMPS explicitly.
export OPFSOLVE_ARCHIVE="$root/opfsolve-$plat.tar.gz" OPFSOLVE_SKIP_HSL=1
sh "$root/install.sh" -y --path "$t/opfsolve" --add-to-path=no
op="$t/opfsolve/bin/opfsolve"
"$op" case14_ieee case118_ieee --form=polar --options="tol=1e-8 max_iter=500 linear_solver=mumps" --output=polar
"$op" case14_ieee case118_ieee --form=rect --options=linear_solver=mumps --output=rect
"$op" --help > /dev/null
# Without HSL and without an explicit linear solver, opfsolve must refuse and say how to get MA27.
if "$op" case14_ieee --output=nohsl > nohsl.log 2>&1; then
    echo "opfsolve ran without MA27"; exit 1
fi
grep -q "hsl.rl.ac.uk/download/MA27" nohsl.log
for f in polar rect; do
    cat "$f/summary.csv"
    grep -q "^opfsolve: " "$f/system.txt"
    for c in pglib_opf_case14_ieee pglib_opf_case118_ieee; do
        grep -q '"termination_status": "LOCALLY_SOLVED"' "$f/$c/solution.json"
        grep -qi "linear solver mumps" "$f/$c/ipopt.log"
    done
done
# Objectives against the AC values in the PGLib-OPF README (5 significant digits).
awk -F, 'FNR > 1 {
    ref = ($1 ~ /case14_/) ? 2178.1 : 97214
    if (($5 - ref) / ref > 1e-4 || ($5 - ref) / ref < -1e-4) { print "objective mismatch: " $0; bad = 1 }
} END { exit bad }' polar/summary.csv rect/summary.csv
echo "all checks passed"
