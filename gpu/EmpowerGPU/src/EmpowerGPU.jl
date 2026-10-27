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
# CUDA itself, not only CUDACore: ExaModelsPower's CUDA extension, which precompiles model
# building and evaluation on the GPU, loads with it.
import CUDA
import GPUCompiler
using PrecompileTools: @compile_workload
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

# Output goes out as one string per line: print(io, ...) compiles per stream type (a file,
# a pipe, a terminal), and the executable must not compile on a terminal it was not built
# on; string(...) does not depend on the stream, and write(::IO, ::String) is one method.
say(io::IO, args...) = (write(io, string(args..., "\n")); nothing)
say(args...) = say(stdout, args...)

# Styles on a terminal (unless NO_COLOR is set), as the empower driver prints them.
const STYLE = Dict{Symbol,String}()
function init_style()
    on = stdout isa Base.TTY && !haskey(ENV, "NO_COLOR")
    for (k, v) in (:bold => "\e[1m", :dim => "\e[2m", :rst => "\e[0m", :green => "\e[32m", :red => "\e[31m")
        STYLE[k] = on ? v : ""
    end
end
sty(k) = get(STYLE, k, "")
const T_START = Ref(0.0)
step(args...) = say("  ", sty(:dim), lpad(Base.Ryu.writefixed(time() - T_START[], 3), 8), "s", sty(:rst), "  ", args...)
kv(label, args...) = say("  ", rpad(label, 9), args...)
heading(t) = say("\n", sty(:bold), "\u2500\u2500 ", t, " ", "\u2500"^max(78 - length(t) - 4, 0), sty(:rst))

# The summary's numbers, as C's printf writes them in the cpu backend's summary.
fixed3(t) = isfinite(t) ? Base.Ryu.writefixed(t, 3) : "-"
function fmt_g(x)   # %.8g
    isfinite(x) || return string(x)
    x == 0 && return "0"
    e = floor(Int, log10(abs(x)))
    -4 <= e < 8 || return Base.Ryu.writeexp(x, 7)
    t = Base.Ryu.writefixed(x, max(7 - e, 0))
    occursin('.', t) ? rstrip(rstrip(t, '0'), '.') : t
end

# A table with a ruled header, as the cpu backend prints it; align[k] is 'l' or 'r'.
function print_table(head, align, rows)
    w = [maximum(length(r[k]) for r in [[head]; rows]) for k in eachindex(head)]
    rule(l, m, r) = say("  ", l, join(("\u2500"^(w[k] + 2) for k in eachindex(w)), m), r)
    line(row) = say("  \u2502 ", join((align[k] == 'l' ? rpad(row[k], w[k]) : lpad(row[k], w[k])
                                      for k in eachindex(w)), " \u2502 "), " \u2502")
    rule("\u250c", "\u252c", "\u2510")
    line(head)
    rule("\u251c", "\u253c", "\u2524")
    foreach(line, rows)
    rule("\u2514", "\u2534", "\u2518")
end

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
    step("model: ", model.meta.nvar, " variables, ", model.meta.ncon, " constraints (",
            round(t_build; digits = 3), " s)")
    log = joinpath(dir, "madnlp.log")
    t0 = time()
    r = madnlp(model; output_file = log, opts...)
    t_solve = time() - t0
    st = termination_status(r.status)
    ok = r.status in (MadNLP.SOLVE_SUCCEEDED, MadNLP.SOLVED_TO_ACCEPTABLE_LEVEL)
    step(sty(:bold), sty(ok ? :green : :red), st, sty(:rst), ": objective ", fmt_g(r.objective), ", ",
         r.iter, " iterations, ", fixed3(t_solve), " s (",
         fixed3(r.counters.eval_function_time), " s in model evaluations)")
    sol = joinpath(dir, "solution.m")
    write_matpower_solution(sol, file, vars, cons, Array(r.solution), Array(r.multipliers),
                            Array(r.multipliers_L), Array(r.multipliers_U);
                            form = FORMS[form], objective = r.objective, status = st)
    step("wrote ", dir, "/{solution.m, madnlp.log}")
    t_cb = r.counters.eval_function_time
    # The largest absolute constraint violation, max(cl - c(x), c(x) - cu, 0), unscaled.
    c = Array(r.constraints)::Vector{Float64}
    cl = Array(model.meta.lcon)::Vector{Float64}
    cu = Array(model.meta.ucon)::Vector{Float64}
    viol = 0.0
    for i in eachindex(c)
        viol = max(viol, cl[i] - c[i], c[i] - cu[i])
    end
    csv(summary, name)
    println(summary, ",", form, ",", st, ",", r.status, ",", r.objective, ",", r.iter, ",",
            model.meta.nvar, ",", model.meta.ncon, ",", t_build, ",", t_solve, ",", t_cb, ",",
            t_solve - t_cb, ",", viol)
    flush(summary)
    return (; name, st, obj = r.objective, iter = r.iter, t_build, t_solve, t_cb, viol,
            ok = r.status in (MadNLP.SOLVE_SUCCEEDED, MadNLP.SOLVED_TO_ACCEPTABLE_LEVEL))
end

function main(args::Vector{String})::Cint
    rc = run(args)
    flush(stdout); flush(stderr)
    # Skip runtime teardown (CUDA context destruction, finalizers, atexit hooks): the
    # process is done and the OS reclaims everything.
    ccall(:_exit, Cvoid, (Cint,), rc)
    return rc
end

# The app: solve each case and write the results; the exit code.
function run(args::Vector{String})::Cint
    init_style()
    # Warnings and errors only: the packages' @info messages ("Loading matpower file") are
    # noise between empower's own lines.
    Base.CoreLogging.global_logger(Base.CoreLogging.SimpleLogger(stderr, Base.CoreLogging.Warn))
    T_START[] = time() - something(tryparse(Float64, get(ENV, "EMPOWER_ELAPSED", "")), 0.0)
    # EMPOWER_TRACE_COMPILE=1: print every method compiled at run time (as
    # `precompile(...)` statements with their compile time) to stderr, to find what the
    # executable still compiles on first use.
    get(ENV, "EMPOWER_TRACE_COMPILE", "") == "1" &&
        ccall(:jl_force_trace_compile_timing_enable, Cvoid, ())
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
    haskey(FORMS, form) || (say(stderr, "error: --form must be polar, rect or dc"); return Cint(2))
    t_gpu = @elapsed (dev = CUDACore.device(); CUDACore.synchronize())
    kv("gpu", CUDACore.name(dev), " (context ", fixed3(t_gpu), " s)")
    mkpath(out)
    path = joinpath(out, "summary.csv")
    results = []
    open(path, "w") do summary
        println(summary, "case,form,termination_status,raw_status,objective,iterations,nvar,ncon,",
                "time_parse_and_build,time_solve,time_solve_callbacks,time_solve_solver,constraint_violation")
        for (k, f) in enumerate(files)
            heading(string("[", k, "/", length(files), "] ", casename(f)))
            step("case ", f)
            try
                push!(results, solve_case(f, form, out, opts, summary))
            catch err
                say(stderr, "error: ", f, ": ", sprint(showerror, err))
                push!(results, (; name = casename(f), st = "not solved (see the error above)",
                                obj = NaN, iter = 0, t_build = NaN, t_solve = NaN, t_cb = NaN,
                                viol = NaN, ok = false))
            end
        end
    end
    head = ["case", "status", "objective", "max viol", "iter", "build (s)", "solve (s)",
            "evals (s)", "solver (s)"]
    rows = map(results) do r
        r.ok || isfinite(r.t_solve) || return vcat([r.name, "not solved"], fill("-", 7))
        [r.name, r.st, fmt_g(r.obj), Base.Ryu.writeexp(r.viol, 2), string(r.iter),
         fixed3(r.t_build), fixed3(r.t_solve), fixed3(r.t_cb), fixed3(r.t_solve - r.t_cb)]
    end
    heading("Summary")
    kv("written", path)
    print_table(head, "llrrrrrrr", rows)
    nok = count(r -> r.ok, results)
    nbad = length(results) - nok
    say("  ", nok > 0 ? sty(:green) : "", nok, " solved", sty(:rst), ", ", nbad > 0 ? sty(:red) : "", nbad,
        " not solved", sty(:rst), ", ", Base.Ryu.writefixed(time() - T_START[], 1), " s in total")
    say("  ", sty(:dim), "MadNLP with cuDSS. max viol: max(cl - c(x), c(x) - cu) at the solution. solve = evals")
    say("  (model functions and derivatives) + solver (MadNLP and cuDSS).", sty(:rst))
    return Cint(nok == length(results) ? 0 : 1)
end

# The app's own run on the bundled case14, in each formulation, so the executable compiles
# nothing on a first run: building the model, solving it, and writing the results. Needs
# the GPU this package is built on.
@compile_workload begin
    if CUDACore.functional()
        case = joinpath(pkgdir(ExaModelsPower), "data", "pglib_opf_case14_ieee.m")
        out = mktempdir()
        redirect_stdout(devnull) do
            for form in ("polar", "rect", "dc")
                # No options (the usual run), then a common set.
                run(["--form=$form", "--output=$out", case])
                run(["--form=$form", "--output=$out", "--option", "print_level=WARN",
                     "--option", "tol=1e-8", "--option", "max_iter=3000", case])
            end
        end
        rm(out; recursive = true)
        # case14 is too small for the multi-block sort of the sparsity patterns that every
        # larger case uses; a model with 5000 variables sorts with it.
        EM = ExaModelsPower.ExaModels
        n = 5000
        c = EM.ExaCore(; backend = CUDABackend())
        c, x = EM.add_var(c, n)
        c, _ = EM.add_obj(c, x[i]^2 + x[i] for i in 1:n)
        c, _ = EM.add_con(c, x[i] * x[i+1] for i in 1:n-1)
        EM.ExaModel(c)
    end
end

end # module EmpowerGPU
