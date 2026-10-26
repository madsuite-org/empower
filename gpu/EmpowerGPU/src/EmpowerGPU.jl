# empower-cuda: solve OPF cases on an NVIDIA GPU with ExaModelsPower and MadNLP (cuDSS).
#
# Called by the empower driver with --device=cuda, which resolves the cases to files:
#
#   empower-cuda --form=polar|rect|dc --output=DIR [--option NAME=VALUE]... FILE...
#
# For each case it writes DIR/<case>/solution.m (the solved case, MATPOWER savecase
# layout) and DIR/<case>/madnlp.log, and a row of DIR/summary.csv.
module EmpowerGPU

using ExaModelsPower, MadNLP, MadNLPGPU, CUDACore, cuBLAS, cuSPARSE, cuSOLVER, CUDSS
import GPUCompiler
const CUDABackend = CUDACore.CUDAKernels.CUDABackend

# Specializations that a traced run still compiled at run time; compiling them here
# puts them in the executable. Evaluated in a module that binds every loaded module by
# name, since several statements refer to modules this file does not import.


const FORMS = Dict("polar" => Polar(), "rect" => Rect(), "dc" => DC())

# The same names the CPU build writes, from MadNLP's status.
function termination_status(s)
    s == MadNLP.SOLVE_SUCCEEDED && return "LOCALLY_SOLVED"
    s == MadNLP.SOLVED_TO_ACCEPTABLE_LEVEL && return "ALMOST_LOCALLY_SOLVED"
    s == MadNLP.INFEASIBLE_PROBLEM_DETECTED && return "LOCALLY_INFEASIBLE"
    s == MadNLP.MAXIMUM_ITERATIONS_EXCEEDED && return "ITERATION_LIMIT"
    s == MadNLP.MAXIMUM_WALLTIME_EXCEEDED && return "TIME_LIMIT"
    s == MadNLP.DIVERGING_ITERATES && return "NORM_LIMIT"
    s == MadNLP.USER_REQUESTED_STOP && return "INTERRUPTED"
    return "NUMERICAL_ERROR"
end

# NAME=VALUE, with VALUE converted to the type of MadNLP's option field.
function option(spec)
    name, value = split(spec, '='; limit = 2)
    key = Symbol(name)
    T = key in fieldnames(MadNLP.MadNLPOptions) ? fieldtype(MadNLP.MadNLPOptions, key) : Any
    v = if T <: Bool
        parse(Bool, value)
    elseif T <: Integer
        parse(Int, value)
    elseif T <: AbstractFloat
        parse(Float64, value)
    elseif T <: Enum
        only(i for i in instances(T) if string(i) == value)
    elseif T <: Type || T == Any
        # A solver or KKT type by name, such as linear_solver=CUDSSSolver.
        isdefined(MadNLPGPU, Symbol(value)) ? getfield(MadNLPGPU, Symbol(value)) :
        isdefined(MadNLP, Symbol(value)) ? getfield(MadNLP, Symbol(value)) :
        something(tryparse(Int, value), tryparse(Float64, value), value)
    else
        value
    end
    return key => v
end

casename(path) = first(splitext(basename(path)))

function csv(io, v)
    s = string(v)
    occursin(r"[,\"\r\n]", s) ? print(io, '"', replace(s, "\"" => "\"\""), '"') : print(io, s)
end

function solve_case(file, form, out, opts, summary)
    name = casename(file)
    dir = mkpath(joinpath(out, name))
    t0 = time()
    model, vars, cons = opf_model(file; form = FORMS[form], backend = CUDABackend())
    t_build = time() - t0
    println("model: ", model.meta.nvar, " variables, ", model.meta.ncon, " constraints (",
            round(t_build; digits = 3), " s)")
    log = joinpath(dir, "madnlp.log")
    t0 = time()
    r = madnlp(model; output_file = log, opts...)
    t_solve = time() - t0
    st = termination_status(r.status)
    println(st, ": objective ", r.objective, ", ", r.iter, " iterations, ", round(t_solve; digits = 3), " s")
    sol = joinpath(dir, "solution.m")
    write_matpower_solution(sol, file, vars, cons, Array(r.solution), Array(r.multipliers),
                            Array(r.multipliers_L), Array(r.multipliers_U);
                            form = FORMS[form], objective = r.objective, status = st)
    println("wrote ", dir, "/{solution.m, madnlp.log}")
    t_cb = r.counters.eval_function_time
    csv(summary, name)
    println(summary, ",", form, ",", st, ",", r.status, ",", r.objective, ",", r.iter, ",",
            model.meta.nvar, ",", model.meta.ncon, ",", t_build, ",", t_solve, ",", t_cb, ",",
            t_solve - t_cb)
    flush(summary)
    return (; name, st, obj = r.objective, iter = r.iter, t_build, t_solve,
            ok = r.status in (MadNLP.SOLVE_SUCCEEDED, MadNLP.SOLVED_TO_ACCEPTABLE_LEVEL))
end

function main(args::Vector{String})::Cint
    t_start = time()
    form, out, opts, files = "polar", "empower-results", Pair{Symbol,Any}[], String[]
    i = 1
    while i <= length(args)
        a = args[i]
        if startswith(a, "--form=")
            form = a[8:end]
        elseif startswith(a, "--output=")
            out = a[10:end]
        elseif a == "--option" && i < length(args)
            push!(opts, option(args[i += 1]))
        else
            push!(files, a)
        end
        i += 1
    end
    haskey(FORMS, form) || (println(stderr, "error: --form must be polar, rect or dc"); return Cint(2))
    println("GPU: ", CUDACore.name(CUDACore.device()))
    mkpath(out)
    path = joinpath(out, "summary.csv")
    results = []
    open(path, "w") do summary
        println(summary, "case,form,termination_status,raw_status,objective,iterations,nvar,ncon,",
                "time_parse_and_build,time_solve,time_solve_callbacks,time_solve_solver")
        for (k, f) in enumerate(files)
            println("\n[", k, "/", length(files), "] ", f)
            try
                push!(results, solve_case(f, form, out, opts, summary))
            catch err
                println(stderr, "error: ", f, ": ", sprint(showerror, err))
                push!(results, (; name = casename(f), st = "not solved (see the error above)",
                                obj = NaN, iter = 0, t_build = NaN, t_solve = NaN, ok = false))
            end
        end
    end
    w = maximum(length(r.name) for r in results; init = 4)
    println("\nSummary (", path, ")")
    println("  ", rpad("case", w), "  ", rpad("status", 22), lpad("objective", 14), lpad("iter", 6),
            lpad("build", 9), lpad("solve", 9))
    for r in results
        println("  ", rpad(r.name, w), "  ", rpad(r.st, 22), lpad(string(round(r.obj; sigdigits = 8)), 14),
                lpad(r.iter, 6), lpad(string(round(r.t_build; digits = 3), "s"), 9),
                lpad(string(round(r.t_solve; digits = 3), "s"), 9))
    end
    nok = count(r -> r.ok, results)
    println("  ", nok, " solved, ", length(results) - nok, " not solved, ",
            round(time() - t_start; digits = 1), " s in total")
    flush(stdout); flush(stderr)
    # Skip runtime teardown (CUDA context destruction, finalizers, atexit hooks): the
    # process is done and the OS reclaims everything.
    ccall(:_exit, Cvoid, (Cint,), nok == length(results) ? 0 : 1)
    return Cint(0)
end

# The GPU kernels compiled for the other architectures empower-cuda supports (V100, RTX
# 20 to 50 series, A100, L40, H100, B200), one package per architecture: loading them
# brings their images, and so the cubins, into the executable.
import EmpowerKernels70, EmpowerKernels75, EmpowerKernels80, EmpowerKernels86, EmpowerKernels89, EmpowerKernels90, EmpowerKernels100, EmpowerKernels120

end # module EmpowerGPU
