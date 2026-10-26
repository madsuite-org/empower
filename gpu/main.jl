# The empower-cuda executable: EmpowerGPU.main, built with juliac.
using EmpowerGPU, CUDACore
# The GPU kernels compiled for the other architectures empower-cuda supports, one package
# per architecture.
import EmpowerKernels70, EmpowerKernels75, EmpowerKernels80, EmpowerKernels86, EmpowerKernels89, EmpowerKernels90, EmpowerKernels100, EmpowerKernels120

module PrecompileStatements
    for (id, m) in Base.loaded_modules
        name = Symbol(id.name)
        isdefined(@__MODULE__, name) || Core.eval(@__MODULE__, :(const $name = $m))
    end
    for line in eachline(joinpath(@__DIR__, "precompile_statements.jl"))
        try
            Core.eval(@__MODULE__, Meta.parse(line))
        catch
        end
    end
end

(@main)(args::Vector{String})::Cint = EmpowerGPU.main(args)
