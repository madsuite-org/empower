# Collect the license files of everything bundled in the release archive:
# the Julia runtime and the Julia packages compiled into the model library.
#
#   julia --project=compile compile/licenses.jl <outdir>
using Pkg, Downloads

out = abspath(ARGS[1])
mkpath(joinpath(out, "julia-packages"))

jl = dirname(Sys.BINDIR)
cp(joinpath(jl, "LICENSE.md"), joinpath(out, "julia-LICENSE.md"); force = true)
Downloads.download(
    "https://raw.githubusercontent.com/JuliaLang/julia/v$(VERSION)/THIRDPARTY.md",
    joinpath(out, "julia-THIRDPARTY.md"),
)

missing_license = String[]
for (uuid, d) in sort(collect(Pkg.dependencies()); by = x -> x[2].name)
    d.is_direct_dep && d.name in ("JuliaC", "ExaModelsCompiler") && continue
    # Standard libraries and the libraries Julia ships are covered by
    # julia-LICENSE.md and julia-THIRDPARTY.md.
    (d.source === nothing || startswith(d.source, Sys.STDLIB)) && continue
    files = filter(f -> occursin(r"^(LICEN[CS]E|COPYING)"i, f), readdir(d.source))
    if isempty(files)
        push!(missing_license, d.name)
        continue
    end
    for f in files
        cp(joinpath(d.source, f), joinpath(out, "julia-packages", "$(d.name)-$(f)"); force = true)
    end
end
isempty(missing_license) || error("no license file found for: $(join(missing_license, ", "))")
println("license files written to $out")
