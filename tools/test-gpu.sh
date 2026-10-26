#!/bin/sh
# Install the built archives with install.sh --gpu (which downloads the NVIDIA libraries
# from the Julia packages' release URLs), then solve cases on the GPU with each formulation.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
t=$(mktemp -d)
cd "$t"
export EMPOWER_ARCHIVE="$root/empower-linux-x86_64.tar.gz" EMPOWER_GPU_ARCHIVE="$root/empower-gpu-linux-x86_64.tar.gz"
export EMPOWER_SKIP_HSL=1
sh "$root/install.sh" -y --path "$t/empower" --add-to-path=no --gpu
op="$t/empower/bin/empower"
for f in polar rect dc; do
    "$op" case14_ieee case118_ieee --device=gpu --form=$f --output=$f
done
for f in polar rect dc; do
    cat "$f/summary.csv"
    for c in pglib_opf_case14_ieee pglib_opf_case118_ieee; do
        grep -q "status = LOCALLY_SOLVED" "$f/$c/solution.m"
        test -s "$f/$c/madnlp.log"
    done
done
# Objectives against the AC values in the PGLib-OPF README (5 significant digits), and
# against PowerModels' DC OPF (DCPPowerModel) for dc.
awk -F, 'FNR > 1 {
    dc = ($2 == "dc")
    ref = ($1 ~ /case14_/) ? (dc ? 2051.5 : 2178.1) : (dc ? 93101 : 97214)
    if (($5 - ref) / ref > 1e-4 || ($5 - ref) / ref < -1e-4) { print "objective mismatch: " $0; bad = 1 }
} END { exit bad }' polar/summary.csv rect/summary.csv dc/summary.csv
echo "all checks passed"
