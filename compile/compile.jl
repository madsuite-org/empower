# Compile the ExaModelsPower OPF recipes into one cnlp-ABI shared library,
# bundled with its Julia runtime.
#
#   julia --project=compile compile/compile.jl <outdir>
using ExaModels, ExaModelsCompiler, ExaModelsPower

out = abspath(get(ARGS, 1, "dist/emp"))
case = joinpath(pkgdir(ExaModelsPower), "data", "pglib_opf_case14_ieee.m")
println("compiling ExaModelsPower recipes acp (polar) and acr (rectangular) into $out")
t = @elapsed r = ExaModelsCompiler.compile_all(
    ExaModelsPower; path = out, case, only = [:acp, :acr], bundle = true, verbose = false,
)
println("built $(r.libpath) in $(round(t; digits = 1)) s")
