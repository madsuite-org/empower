#!/bin/sh
# Build empower-gpu-linux-x86_64.tar.gz:
#   empower-gpu/bin/empower-gpu   the GPU solver (gpu/EmpowerGPU, MadNLP with cuDSS), built
#                                 with juliac --bundle, with its Julia runtime
#   empower-gpu/nvidia-artifacts.tsv
#                                 the NVIDIA libraries it loads, which the archive does
#                                 not contain: the installer downloads each one, as the
#                                 Julia package (JLL) artifact the build used, from the
#                                 URL in that package's Artifacts.toml and checks its
#                                 SHA-256
# Needs an NVIDIA GPU: the precompile workloads compile the kernels on it, and EmpowerGPU
# compiles them again for the other architectures empower-gpu supports.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)

julia --startup-file=no --project=gpu -e 'using Pkg; Pkg.instantiate(); Pkg.precompile()'
julia --startup-file=no --project=compile -e 'using Pkg; Pkg.instantiate()'

rm -rf stage-gpu
mkdir -p stage-gpu
julia --startup-file=no --project=compile -e 'using JuliaC; JuliaC.main(ARGS)' -- \
    --project=gpu --output-exe empower-gpu --bundle stage-gpu/empower-gpu \
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
    close(out)' stage-gpu/empower-gpu/share/julia/artifacts stage-gpu/empower-gpu/nvidia-artifacts.tsv
cat stage-gpu/empower-gpu/nvidia-artifacts.tsv

# No NVIDIA library may remain in the archive.
left=$(find stage-gpu/empower-gpu -name 'libcuda*' -o -name 'libcublas*' -o -name 'libcusparse*' \
    -o -name 'libcusolver*' -o -name 'libcudss*' -o -name 'libcufft*' -o -name 'libcurand*' \
    -o -name 'libcupti*' -o -name 'libnvrtc*' -o -name 'libnvJitLink*' \
    -o -name 'libnvvm*' -o -name 'ptxas' | head -5)
[ -z "$left" ] || { echo "NVIDIA files left in the archive: $left"; exit 1; }

julia --startup-file=no --project=gpu compile/licenses.jl stage-gpu/empower-gpu/licenses
cp LICENSE README.md THIRD_PARTY_NOTICES.md CITATION.bib stage-gpu/empower-gpu/
tar -C stage-gpu -czf empower-gpu-linux-x86_64.tar.gz empower-gpu
echo "built empower-gpu-linux-x86_64.tar.gz ($(du -h empower-gpu-linux-x86_64.tar.gz | cut -f1))"
