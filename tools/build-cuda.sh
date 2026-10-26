#!/bin/sh
# Build empower-cuda-linux-x86_64.tar.gz, the cuda backend:
#   empower-cuda/bin/empower-cuda the GPU solver (gpu/EmpowerGPU, MadNLP with cuDSS), built
#                                 with juliac --bundle, with its Julia runtime
#   empower-cuda/nvidia-artifacts.tsv
#                                 the NVIDIA libraries it loads, which the archive does
#                                 not contain: the installer downloads each one, as the
#                                 Julia package (JLL) artifact the build used, from the
#                                 URL in that package's Artifacts.toml and checks its
#                                 SHA-256
# Needs an NVIDIA GPU: the precompile workloads compile the kernels on it, and EmpowerGPU
# compiles them again for the other architectures empower-cuda supports.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)

julia --startup-file=no --project=gpu -e 'using Pkg; Pkg.instantiate(); Pkg.precompile()'
julia --startup-file=no --project=compile -e 'using Pkg; Pkg.instantiate()'

rm -rf stage-cuda
mkdir -p stage-cuda
julia --startup-file=no --project=compile -e 'using JuliaC; JuliaC.main(ARGS)' -- \
    --project=gpu --output-exe empower-cuda --bundle stage-cuda/empower-cuda \
    --trim=no --experimental gpu/main.jl

# The NVIDIA artifacts: recorded for the installer, then removed.
julia --startup-file=no --project=gpu -e '
    using TOML
    arts = ARGS[1]
    jlls = ("CUDA_Runtime_jll", "CUDA_Compiler_jll", "CUDA_Driver_jll", "CUDSS_jll")
    manifest = TOML.parsefile("gpu/Manifest.toml")["deps"]
    out = open(ARGS[2], "w")
    for jll in jlls
        e = only(manifest[jll])
        pkg = Base.locate_package(Base.PkgId(Base.UUID(e["uuid"]), jll))
        toml = TOML.parsefile(joinpath(dirname(dirname(pkg)), "Artifacts.toml"))
        name = replace(jll, "_jll" => "")
        hit = false
        for a in toml[name]
            h = a["git-tree-sha1"]
            isdir(joinpath(arts, h)) || continue
            d = only(a["download"])
            println(out, name, "\t", e["version"], "\t", h, "\t", d["url"], "\t", d["sha256"])
            rm(joinpath(arts, h); recursive = true)
            hit = true
        end
        hit || error("no artifact of $jll in the bundle")
    end
    close(out)' stage-cuda/empower-cuda/share/julia/artifacts stage-cuda/empower-cuda/nvidia-artifacts.tsv
cat stage-cuda/empower-cuda/nvidia-artifacts.tsv

# No NVIDIA library may remain in the archive.
left=$(find stage-cuda/empower-cuda -name 'libcuda*' -o -name 'libcublas*' -o -name 'libcusparse*' \
    -o -name 'libcusolver*' -o -name 'libcudss*' -o -name 'libcufft*' -o -name 'libcurand*' \
    -o -name 'libcupti*' -o -name 'libnvrtc*' -o -name 'libnvJitLink*' \
    -o -name 'libnvvm*' -o -name 'ptxas' | head -5)
[ -z "$left" ] || { echo "NVIDIA files left in the archive: $left"; exit 1; }

julia --startup-file=no --project=gpu compile/licenses.jl stage-cuda/empower-cuda/licenses
cp LICENSE README.md THIRD_PARTY_NOTICES.md CITATION.bib stage-cuda/empower-cuda/
tar -C stage-cuda -czf empower-cuda-linux-x86_64.tar.gz empower-cuda
echo "built empower-cuda-linux-x86_64.tar.gz ($(du -h empower-cuda-linux-x86_64.tar.gz | cut -f1))"
