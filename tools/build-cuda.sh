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

rm -rf "$env" "$stage"
mkdir -p "$env" "$stage"
cp -r gpu/Project.toml gpu/Manifest.toml gpu/main.jl gpu/precompile_statements.jl gpu/EmpowerGPU "$env/"
# Julia applies a project's preferences only to the packages it lists, so the runtime
# package joins as an extra.
printf '\n[extras]\nCUDA_Runtime_jll = "76a88914-d11a-5bdc-97e0-2f5a05c973a2"\n' >> "$env/Project.toml"
printf '[CUDA_Runtime_jll]\nversion = "%s"\n' "$runtime" > "$env/LocalPreferences.toml"

julia --startup-file=no --project="$env" -e 'using Pkg; Pkg.instantiate(); Pkg.precompile()'
julia --startup-file=no --project=gpu/juliac -e 'using Pkg; Pkg.instantiate()'
julia --startup-file=no --project=gpu/juliac -e 'using JuliaC; JuliaC.main(ARGS)' -- \
    --project="$env" --output-exe empower-cuda --bundle "$stage/empower-cuda" \
    --trim=no --experimental "$env/main.jl"

# The NVIDIA artifacts the build selected, as each package reports it: recorded for the
# installer, then removed from the bundle if it carries them (lazy ones it may not).
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
        rm(joinpath(arts, h); recursive = true, force = true)
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
tar -C "$stage" -czf "$name.tar.gz" empower-cuda
echo "built $name.tar.gz ($(du -h "$name.tar.gz" | cut -f1))"
