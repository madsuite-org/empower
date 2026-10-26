#!/bin/sh
# Install a built archive with install.sh, then solve cases with each formulation.
set -eu
plat=$1
cd "$(dirname "$0")/.."
root=$(pwd)
t=$(mktemp -d)
cd "$t"
# CI has no HSL licence: install without HSL and ask for MUMPS explicitly.
export EMPOWER_ARCHIVE="$root/empower-$plat.tar.gz" EMPOWER_SKIP_HSL=1
sh "$root/install.sh" -y --path "$t/empower" --add-to-path=no
op="$t/empower/bin/empower"
"$op" case14_ieee case118_ieee --form=polar --options="tol=1e-8 max_iter=500 linear_solver=mumps" --output=polar
"$op" case14_ieee case118_ieee --form=rect --options=linear_solver=mumps --output=rect
"$op" case14_ieee case118_ieee --form=dc --options=linear_solver=mumps --output=dc
"$op" --help > /dev/null
# Without HSL and without an explicit linear solver, empower uses MUMPS and says how to get MA27.
"$op" case14_ieee --output=nohsl > nohsl.log 2>&1
grep -q "hsl.rl.ac.uk/download/MA27" nohsl.log
grep -q "empower hsl" nohsl.log
grep -qi "linear solver mumps" nohsl/pglib_opf_case14_ieee/ipopt.log
# The whole PGLib-OPF library: three sets of cases.
"$op" download > download.log
test "$(grep -c " cases, " download.log)" -eq 3
for f in polar rect dc; do
    cat "$f/summary.csv"
    grep -q "^empower: " "$f/system.txt"
    head -1 "$f/summary.csv" | grep -q ",constraint_violation$"
    for c in pglib_opf_case14_ieee pglib_opf_case118_ieee; do
        grep -q "status = LOCALLY_SOLVED" "$f/$c/solution.m"
        grep -q "^mpc.branch" "$f/$c/solution.m"
        grep -qi "linear solver mumps" "$f/$c/ipopt.log"
    done
done
# Objectives against the AC values in the PGLib-OPF README (5 significant digits), and
# against PowerModels' DC OPF (DCPPowerModel) for dc.
awk -F, 'FNR > 1 {
    dc = ($2 == "dc")
    ref = ($1 ~ /case14_/) ? (dc ? 2051.5 : 2178.1) : (dc ? 93101 : 97214)
    if (($5 - ref) / ref > 1e-4 || ($5 - ref) / ref < -1e-4) { print "objective mismatch: " $0; bad = 1 }
    if ($NF == "" || $NF > 1e-5) { print "constraint violation: " $0; bad = 1 }
} END { exit bad }' polar/summary.csv rect/summary.csv dc/summary.csv
echo "all checks passed"
