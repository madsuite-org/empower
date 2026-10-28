#!/bin/sh
# Build empower-cuda12-linux-x86_64.tar.gz or empower-cuda13-linux-x86_64.tar.gz, the
# cuda backend for drivers that support CUDA 12.8 or 13.0 and newer:
#   empower-cuda/bin/empower-cuda the GPU solver (gpu/EmpowerGPU, MadNLP with cuDSS), built
#                                 with juliac --bundle, with its Julia runtime
#   empower-cuda/nvidia-artifacts.tsv
#                                 the NVIDIA libraries it loads, which the archive does
#                                 not contain: the installer downloads each one, as the
#                                 Julia package (JLL) artifact the build selected, from
#                                 the URL in that package's Artifacts.toml and checks its
#                                 SHA-256
#
#   tools/build-cuda.sh 12|13
#
# The CUDA runtime is fixed per archive (12.8 or 13.0), in a copy of gpu/ with its own
# LocalPreferences.toml, since a binary that is distributed has to name the libraries it
# loads. Needs an NVIDIA GPU: the precompile workloads compile the kernels on it, and
# EmpowerGPU compiles them again for the other architectures empower-cuda supports.
set -eu
cuda=${1:?usage: tools/build-cuda.sh 12|13}
case "$cuda" in 12) runtime=12.8 ;; 13) runtime=13.0 ;; *) echo "CUDA major version must be 12 or 13" >&2; exit 2 ;; esac
cd "$(dirname "$0")/.."
name=empower-cuda$cuda-linux-x86_64
env=build-cuda$cuda
stage=stage-cuda$cuda

# Compile for the CPU targets of the official Julia binaries, as tools/build.sh does, not
# for the build machine's CPU (a runner's AVX-512 image is rejected on a CPU without it).
JULIA_CPU_TARGET='generic;sandybridge,-xsaveopt,clone_all;haswell,-rdrnd,base(1);x86-64-v4,-rdrnd,base(1)'
export JULIA_CPU_TARGET
# No persistent compiled-code store (CompilerCaching's LMDB object cache): a build does
# not reuse it, and the kernel packages precompile in parallel, which hung on the CI
# runners, whose depot is on NFS, where LMDB's locking across processes is unreliable.
export JULIA_OBJCACHE=0

rm -rf "$env" "$stage"
mkdir -p "$env" "$stage"
cp -r gpu/Project.toml gpu/Manifest.toml gpu/main.jl gpu/precompile_statements.jl gpu/EmpowerGPU gpu/kernels "$env/"
# Julia applies a project's preferences only to the packages it lists, so the runtime and
# compiler packages join as extras; each reads its own preference namespace first, so
# both are set (the compiler's otherwise follows a global preference, if one exists).
printf '\n[extras]\nCUDA_Compiler_jll = "d1e2174e-dfdc-576e-b43e-73b79eb1aca8"\nCUDA_Runtime_jll = "76a88914-d11a-5bdc-97e0-2f5a05c973a2"\nKernelAbstractions = "63c18a36-062a-441e-b654-da1e3ab1ce7c"\n' >> "$env/Project.toml"
printf '[CUDA_Compiler_jll]\nversion = "%s"\n\n[CUDA_Runtime_jll]\nversion = "%s"\n' "$runtime" "$runtime" > "$env/LocalPreferences.toml"
# KernelAbstractions 0.10 launches a kernel on its POCL (CPU) back-end while it precompiles,
# which puts GPUCompiler's device runtime into its package image; juliac cannot link that
# into an executable (undefined julia.gpu.state_getter). empower-cuda uses only the CUDA
# back-end, whose kernels its own workloads precompile.
printf '\n[KernelAbstractions]\nprecompile_workload = false\n' >> "$env/LocalPreferences.toml"

julia --startup-file=no --project="$env" -e 'using Pkg; Pkg.instantiate(); Pkg.precompile()'
julia --startup-file=no --project=gpu/juliac -e 'using Pkg; Pkg.instantiate()'

# juliac compiles the executable's launcher with a C compiler, which ties it to the build
# machine's glibc (__libc_start_main@GLIBC_2.34 on newer systems). conda-forge's gcc with
# a glibc 2.28 sysroot links it for 2.28 and newer, as the cpu archives are built.
toolchain="$(pwd)/build-toolchain"
cc="$toolchain/env/bin/x86_64-conda-linux-gnu-gcc"
if [ ! -x "$cc" ]; then
    mkdir -p "$toolchain"
    curl --proto '=https' --tlsv1.2 -fsSL -o "$toolchain/micromamba" \
        https://github.com/mamba-org/micromamba-releases/releases/download/2.9.0-0/micromamba-linux-64
    echo "366cd9cd8be14df1ab8ed50352a82111082a36686b2d389fdb79a92c3fafb3e3  $toolchain/micromamba" | sha256sum -c -
    chmod +x "$toolchain/micromamba"
    MAMBA_ROOT_PREFIX="$toolchain/root" "$toolchain/micromamba" create -y -q -p "$toolchain/env" \
        -c conda-forge --override-channels gcc_linux-64 "sysroot_linux-64=2.28"
fi
# KernelAbstractions 0.10's CPU back-end is POCL, and the packages that precompile on it
# (ExaModelsPower's KernelAbstractions extension) put GPUCompiler's device runtime for it
# into their package images. juliac compiles that code into the executable, where the
# device intrinsics it calls do not exist. empower-cuda never runs it (only the CUDA
# back-end), so the link gets definitions that trap if they are ever called.
stubs="$toolchain/device-stubs.o"
cat > "$toolchain/device-stubs.s" <<'STUBS'
    .text
    .globl "julia.gpu.state_getter"
    .type "julia.gpu.state_getter", @function
"julia.gpu.state_getter":
    ud2
    .globl "julia.opencl.random_keys"
    .type "julia.opencl.random_keys", @function
"julia.opencl.random_keys":
    ud2
    .globl "julia.opencl.random_counters"
    .type "julia.opencl.random_counters", @function
"julia.opencl.random_counters":
    ud2
    .section .note.GNU-stack,"",@progbits
STUBS
"$cc" -c "$toolchain/device-stubs.s" -o "$stubs"
cat > "$toolchain/cc-link" <<WRAP
#!/bin/sh
# The C compiler juliac uses; the link of the executable gets the device stubs.
for a in "\$@"; do
    case "\$a" in *.o.a) exec "$cc" "\$@" "$stubs" ;; esac
done
exec "$cc" "\$@"
WRAP
chmod +x "$toolchain/cc-link"
export JULIA_CC="$toolchain/cc-link"
# juliac generates the executable's code on JULIA_IMAGE_THREADS threads; leave headroom for
# others on a shared machine.
JULIA_IMAGE_THREADS=${JULIA_IMAGE_THREADS:-$(n=$(($(nproc) / 2)); [ "$n" -gt 32 ] && n=32; [ "$n" -lt 1 ] && n=1; echo $n)}
export JULIA_IMAGE_THREADS
echo "juliac: $JULIA_IMAGE_THREADS threads"
julia --startup-file=no --project=gpu/juliac -e 'using JuliaC; JuliaC.main(ARGS)' -- \
    --project="$env" --output-exe empower-cuda --bundle "$stage/empower-cuda" \
    --trim=no --experimental --jl-option threads=4 "$env/main.jl"

# The launcher must run on glibc 2.28.
newest=$("$toolchain/env/bin/x86_64-conda-linux-gnu-objdump" -T "$stage/empower-cuda/bin/empower-cuda" |
    grep -o 'GLIBC_[0-9.]*' | sort -t. -k2,2n -k3,3n | tail -1)
case "$newest" in GLIBC_2.[0-9] | GLIBC_2.[0-9].* | GLIBC_2.1[0-9]* | GLIBC_2.2[0-8] | GLIBC_2.2[0-8].*) echo "launcher needs $newest" ;;
    *) echo "the launcher needs $newest, newer than glibc 2.28"; exit 1 ;; esac

# The NVIDIA artifacts the build selected, as each package reports it: recorded for the
# installer; every artifact of those packages is then removed from the bundle.
julia --startup-file=no --project="$env" -e '
    using TOML
    arts, out = ARGS[1], open(ARGS[2], "w")
    manifest = TOML.parsefile(joinpath(ARGS[3], "Manifest.toml"))["deps"]
    for jll in ("CUDA_Runtime_jll", "CUDA_Compiler_jll", "CUDA_Driver_jll", "CUDSS_jll")
        e = only(manifest[jll])
        id = Base.PkgId(Base.UUID(e["uuid"]), jll)
        mod = Base.require(id)
        h = basename(mod.artifact_dir)
        toml = TOML.parsefile(joinpath(dirname(dirname(Base.locate_package(id))), "Artifacts.toml"))
        a = only(a for a in toml[replace(jll, "_jll" => "")] if a["git-tree-sha1"] == h)
        d = only(a["download"])
        println(out, replace(jll, "_jll" => ""), "\t", e["version"], "\t", h, "\t", d["url"], "\t", d["sha256"])
        # Every artifact of the package: the bundler selects in its own process, which
        # may pick another CUDA version (from a global preference) than the build did.
        for b in toml[replace(jll, "_jll" => "")]
            rm(joinpath(arts, b["git-tree-sha1"]); recursive = true, force = true)
        end
    end
    close(out)' "$stage/empower-cuda/share/julia/artifacts" "$stage/empower-cuda/nvidia-artifacts.tsv" "$env"
cat "$stage/empower-cuda/nvidia-artifacts.tsv"
grep -q "cuda+$runtime\|cuda+$cuda" "$stage/empower-cuda/nvidia-artifacts.tsv" ||
    { echo "the NVIDIA artifacts are not for CUDA $runtime"; exit 1; }

# No NVIDIA library may remain in the archive.
left=$(find "$stage/empower-cuda" -name 'libcuda*' -o -name 'libcublas*' -o -name 'libcusparse*' \
    -o -name 'libcusolver*' -o -name 'libcudss*' -o -name 'libcufft*' -o -name 'libcurand*' \
    -o -name 'libcupti*' -o -name 'libnvrtc*' -o -name 'libnvJitLink*' \
    -o -name 'libnvvm*' -o -name 'ptxas' | head -5)
[ -z "$left" ] || { echo "NVIDIA files left in the archive: $left"; exit 1; }

printf '%s\n' "$cuda" > "$stage/empower-cuda/CUDA"
julia --startup-file=no --project="$env" compile/licenses.jl "$stage/empower-cuda/licenses"
cp LICENSE README.md THIRD_PARTY_NOTICES.md CITATION.bib "$stage/empower-cuda/"
# pigz writes the same gzip format in parallel.
if command -v pigz >/dev/null; then
    tar -C "$stage" -cf - empower-cuda | pigz > "$name.tar.gz"
else
    tar -C "$stage" -czf "$name.tar.gz" empower-cuda
fi
echo "built $name.tar.gz ($(($(wc -c < "$name.tar.gz") / 1048576)) MiB)"
