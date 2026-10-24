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

rm -rf stage
mkdir -p stage/opfsolve/bin
julia --startup-file=no --project=compile compile/compile.jl stage/opfsolve/emp
cc -O2 -Wall -o stage/opfsolve/bin/opfsolve driver/opfsolve.c -ldl -lm
cp LICENSE README.md stage/opfsolve/
tar czf "opfsolve-$plat.tar.gz" -C stage opfsolve
ls -la "opfsolve-$plat.tar.gz"
