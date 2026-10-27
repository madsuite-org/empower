#!/bin/sh
# Install the built archives with install.sh --backends=cpu,cuda (EMPOWER_CUDA=12|13) (which downloads the NVIDIA libraries
# from the Julia packages' release URLs), then solve cases on the GPU with each formulation.
set -eu
cuda=${1:?usage: tools/test-cuda.sh 12|13}
cd "$(dirname "$0")/.."
root=$(pwd)
t=$(mktemp -d)
cd "$t"
export EMPOWER_ARCHIVE="$root/empower-linux-x86_64.tar.gz" EMPOWER_CUDA_ARCHIVE="$root/empower-cuda$cuda-linux-x86_64.tar.gz" EMPOWER_CUDA=$cuda
export EMPOWER_SKIP_HSL=1
sh "$root/install.sh" -y --path "$t/empower" --add-to-path=no --backends=cpu,cuda
op="$t/empower/bin/empower"
for f in polar rect dc; do
    "$op" case14_ieee case118_ieee --device=cuda --form=$f --output=$f
done
for f in polar rect dc; do
    cat "$f/summary.csv"
    for c in pglib_opf_case14_ieee pglib_opf_case118_ieee; do
        grep -q "status = LOCALLY_SOLVED" "$f/$c/solution.m"
        test "$(grep -c '^[a-z_]*,[-0-9.e+]*$' "$f/$c/violations.csv")" -eq 6
        test -s "$f/$c/madnlp.log"
    done
done
# Objectives against the AC values in the PGLib-OPF README (5 significant digits), and
# against PowerModels' DC OPF (DCPPowerModel) for dc.
awk -F, 'FNR > 1 {
    dc = ($2 == "dc")
    ref = ($1 ~ /case14_/) ? (dc ? 2051.5 : 2178.1) : (dc ? 93101 : 97214)
    if (($5 - ref) / ref > 1e-4 || ($5 - ref) / ref < -1e-4) { print "objective mismatch: " $0; bad = 1 }
    # constraint_violation, then the six violation_* groups
    for (k = NF - 6; k <= NF; k++)
        if ($k == "" || $k > 1e-5) { print "violation (column " k "): " $0; bad = 1 }
} END { exit bad }' polar/summary.csv rect/summary.csv dc/summary.csv
# The executable compiles nothing at run time: a stale gpu/precompile_statements.jl or a
# workload that misses a code path shows up here as compiled methods.
# Once with the output in a file and once on a terminal (script's pseudo-terminal), since
# printing specializes on the stream.
EMPOWER_TRACE_COMPILE=1 "$op" case14_ieee case300_ieee --device=cuda --form=polar --output=trace > trace.log 2>&1
script -q -e -c "EMPOWER_TRACE_COMPILE=1 '$op' case14_ieee case300_ieee --device=cuda --form=polar --output=trace" trace-tty.log > /dev/null
cat trace-tty.log >> trace.log
n=$(grep -c "precompile(" trace.log || true)
if [ "$n" -ne 0 ]; then
    echo "compiled $n methods at run time:"; grep "precompile(" trace.log | cut -c1-300; exit 1
fi
echo "all checks passed"
