# The GPU kernels of the packages empower-cuda uses, compiled for compute capability 8.9
# while this package precompiles, into its image. One package per architecture, so Pkg
# precompiles them in parallel (a precompile process has one thread).
module EmpowerKernels89

# EmpowerGPU first: its kernels (and those of the packages it loads, CUDA's extensions
# included) are the ones compiled for this architecture.
import EmpowerGPU
using ExaModelsPower, MadNLP, MadNLPGPU, CUDACore, cuBLAS, cuSPARSE, cuSOLVER, CUDSS
import GPUCompiler
using PrecompileTools: @compile_workload

include(joinpath(@__DIR__, "..", "..", "multiarch.jl"))

@compile_workload begin
    compile_for([v"8.9"])
end

end # module EmpowerKernels89
