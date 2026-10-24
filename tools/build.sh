#!/bin/sh
# Build opfsolve-<plat>.tar.gz:
#   opfsolve/bin/opfsolve       C driver: loads the model library and Ipopt at run time
#   opfsolve/emp/lib/libemp.*   ExaModelsPower AC OPF recipes acp (polar) and acr
#                               (rectangular), compiled by ExaModelsCompiler.compile_all
#                               with juliac --trim=safe, plus a private Julia runtime
set -eu
plat=$1
cd "$(dirname "$0")/.."

# PrettyTables v3 loads REPL; with ExaPowerIO's LazyArtifacts loading Pkg, Pkg's REPL
# extension then enters the trimmed image and fails verification. compile_all pins
# only path-tracked packages into the library build, so v2 is developed from a clone.
if [ ! -d compile/deps/PrettyTables ]; then
    git clone -q --depth 1 -b v2.4.0 https://github.com/ronisbr/PrettyTables.jl compile/deps/PrettyTables
fi
julia --startup-file=no --project=compile -e 'using Pkg; Pkg.instantiate()'

# Compile for the CPU targets of the official Julia binaries (JuliaCI/julia-buildkite,
# utilities/build_envs.sh), not for the build machine's CPU, so the library runs on
# any CPU of the architecture.
case "$plat" in
    *-x86_64) JULIA_CPU_TARGET='generic;sandybridge,-xsaveopt,clone_all;haswell,-rdrnd,base(1);x86-64-v4,-rdrnd,base(1)' ;;
    macos-aarch64) JULIA_CPU_TARGET='generic;apple-m1,clone_all' ;;
    linux-aarch64) JULIA_CPU_TARGET='generic;cortex-a57;thunderx2t99;carmel,clone_all;apple-m1,base(3);neoverse-512tvb,-rand,-fpac,base(3)' ;;
    *) echo "unknown platform $plat" >&2; exit 1 ;;
esac
export JULIA_CPU_TARGET
echo "JULIA_CPU_TARGET=$JULIA_CPU_TARGET"

rm -rf stage
mkdir -p stage/opfsolve/bin
julia --startup-file=no --project=compile compile/compile.jl stage/opfsolve/emp
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        # Static, so the driver adds no MinGW runtime DLLs of its own.
        cc -O2 -Wall -static -o stage/opfsolve/bin/opfsolve.exe driver/opfsolve.c ;;
    *)
        cc -O2 -Wall -o stage/opfsolve/bin/opfsolve driver/opfsolve.c -ldl -lm ;;
esac
find stage/opfsolve -maxdepth 3 -name 'libemp*' -o -maxdepth 3 -name 'opfsolve*' -type f
cp LICENSE README.md THIRD_PARTY_NOTICES.md CITATION.bib stage/opfsolve/
julia --startup-file=no --project=compile compile/licenses.jl stage/opfsolve/licenses
tar czf "opfsolve-$plat.tar.gz" -C stage opfsolve
sh tools/check-no-ipopt-hsl.sh "opfsolve-$plat.tar.gz"
ls -la "opfsolve-$plat.tar.gz"
