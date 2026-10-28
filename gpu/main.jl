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
        # A statement naming something the current dependencies no longer define (an
        # ExaModels closure renumbered by an update, say) does nothing; say so in the build log.
        try
            Core.eval(@__MODULE__, Meta.parse(line))
        catch err
            println(stderr, "stale precompile statement (", sprint(showerror, err; context = :limit => true)[1:min(end, 100)], "): ", line[1:min(end, 160)])
        end
    end
end

# CUDA.jl keys its launched kernels by (objectid, hash, function) and compares those keys
# with isequal, specialized on the kernel function's type: precompile it for every GPU
# kernel function (KernelAbstractions names them gpu_*) of the loaded modules.
for m in Base.loaded_modules_array(), n in names(m; all = true)
    startswith(string(n), "gpu_") && isdefined(m, n) || continue
    f = getfield(m, n)
    f isa Function || continue
    K = Tuple{UInt64,UInt64,typeof(f)}
    precompile(isequal, (K, K))
end

# The compiled kernels of every architecture, recorded in the executable so that it loads
# those for its GPU when it starts rather than one by one on first use.
append!(EmpowerGPU.PRECOMPILED, EmpowerKernels70.compiled_results())
println(stderr, "kernels recorded for loading at startup: ", length(EmpowerGPU.PRECOMPILED))

(@main)(args::Vector{String})::Cint = EmpowerGPU.main(args)
