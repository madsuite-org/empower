#!/bin/sh
# Build empower-<plat>.tar.gz:
#   empower/bin/empower       C driver: loads the model library and Ipopt at run time
#   empower/emp/lib/libemp.*   ExaModelsPower OPF recipes acp (polar), acr (rectangular)
#                               and dcp (DC) with their MATPOWER writers, compiled by
#                               ExaModelsCompiler.compile_all with juliac --trim=safe,
#                               plus a private Julia runtime
set -eu
plat=$1
cd "$(dirname "$0")/.."

# PrettyTables v3 loads REPL; with ExaPowerIO's LazyArtifacts loading Pkg, Pkg's REPL
# extension then enters the trimmed image and fails verification. compile_all pins
# only path-tracked packages into the library build, so v2 is developed from a clone.
prettytables=99595fcad38a5181e5888e32267cd045fa5b8669   # v2.4.0
if [ "$(git -C compile/deps/PrettyTables rev-parse HEAD 2>/dev/null)" != "$prettytables" ]; then
    rm -rf compile/deps/PrettyTables
    git clone -q https://github.com/ronisbr/PrettyTables.jl compile/deps/PrettyTables
    git -C compile/deps/PrettyTables checkout -q "$prettytables"
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
mkdir -p stage/empower/bin
julia --startup-file=no --project=compile compile/compile.jl stage/empower/emp
# The driver, and libempower (the C API: driver/empower.h) from the same source.
mkdir -p stage/empower/lib stage/empower/include
lib="-DEMPOWER_LIB -Wno-unused-function -Wno-unused-variable"
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        # Static, so the driver adds no MinGW runtime DLLs of its own.
        cc -O2 -Wall -Wextra -static -o stage/empower/bin/empower.exe driver/empower.c
        cc -O2 -Wall -Wextra $lib -shared -static-libgcc -o stage/empower/bin/empower.dll driver/empower.c \
            -Wl,--out-implib,stage/empower/lib/libempower.dll.a ;;
    Darwin)
        cc -O2 -Wall -Wextra -o stage/empower/bin/empower driver/empower.c -ldl -lm
        cc -O2 -Wall -Wextra $lib -dynamiclib -fvisibility=hidden -install_name @rpath/libempower.dylib \
            -o stage/empower/lib/libempower.dylib driver/empower.c -lm ;;
    *)
        cc -O2 -Wall -Wextra -o stage/empower/bin/empower driver/empower.c -ldl -lm
        cc -O2 -Wall -Wextra $lib -D_GNU_SOURCE -fPIC -shared -fvisibility=hidden -Wl,-soname,libempower.so \
            -o stage/empower/lib/libempower.so driver/empower.c -ldl -lm ;;
esac
cp driver/empower.h stage/empower/include/
# The Python package (pip install <install>/python).
mkdir -p stage/empower/python
cp -R python/empower python/pyproject.toml python/README.md stage/empower/python/
find stage/empower/python -name __pycache__ -prune -exec rm -rf {} +
find stage/empower -maxdepth 3 -name 'libemp*' -o -maxdepth 3 -name 'empower*' -type f
cp LICENSE README.md THIRD_PARTY_NOTICES.md CITATION.bib stage/empower/
julia --startup-file=no --project=compile compile/licenses.jl stage/empower/licenses
sh tools/lock.sh stage/empower/locks
tar czf "empower-$plat.tar.gz" -C stage empower
sh tools/check-no-ipopt-hsl.sh "empower-$plat.tar.gz"
ls -la "empower-$plat.tar.gz"
