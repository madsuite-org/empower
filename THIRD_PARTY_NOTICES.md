# Third-party notices

`opfsolve` itself (the C driver, the build scripts, `install.sh`, and `install.ps1`) is under the MIT License; see [LICENSE](LICENSE).

## Bundled in the release archives

Each `opfsolve-<platform>.tar.gz` contains `licenses/` with the license file of every component listed here.

**The model library** (`emp/lib/libemp.so` or `.dylib`, `emp/bin/libemp.dll` on Windows) is compiled from these Julia packages, unmodified:

| Package | License | Source |
|---|---|---|
| ExaModels.jl | MIT | https://github.com/madsuite-org/ExaModels.jl |
| ExaModelsPower.jl | MIT | https://github.com/madsuite-org/ExaModelsPower.jl |
| ExaPowerIO.jl | MIT | https://github.com/madsuite-org/ExaPowerIO.jl |
| NLPModels.jl, SolverCore.jl, LinearOperators.jl | MPL-2.0 | https://github.com/JuliaSmoothOptimizers |
| Other dependencies (JSON, PrettyTables, Tables, TimerOutputs, and others) | MIT | see `licenses/julia-packages/` |

The MPL-2.0 packages are used unmodified; their source code is available at the addresses above.

**The Julia runtime** (the `libjulia` libraries and `julia/` directory next to the model library) is Julia 1.12, MIT License, with the external libraries listed in Julia's [THIRDPARTY.md](https://github.com/JuliaLang/julia/blob/master/THIRDPARTY.md) (included as `licenses/julia-THIRDPARTY.md` for the exact Julia version used). These are the official Julia binaries of those libraries, unmodified. Among them:

- SuiteSparse: AMD, CAMD, CCOLAMD, COLAMD, and SuiteSparse_config are BSD-3-Clause; BTF, KLU, and LDL are LGPL-2.1+; CHOLMOD is LGPL-2.1+ and GPL-2.0+; RBio, SPQR, and UMFPACK are GPL-2.0+. Source: https://github.com/DrTimothyAldenDavis/SuiteSparse
- GMP (LGPL-3.0+ or GPL-2.0+) and MPFR (LGPL-3.0+). Source: https://gmplib.org, https://www.mpfr.org
- libgit2 (GPL-2.0+ with a linking exception). Source: https://github.com/libgit2/libgit2
- OpenBLAS (BSD-3-Clause), libblastrampoline, libuv, libunwind, nghttp2, curl (MIT or similar), OpenSSL (Apache-2.0), PCRE2 (BSD-3-Clause), zlib, dSFMT, OpenLibm.
- GCC runtime libraries (libgcc_s, libstdc++, libgfortran, libgomp, libatomic, libssp): GPL-3.0 with the [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html); libquadmath: LGPL-2.1+. Source: https://gcc.gnu.org

The Julia runtime loads SuiteSparse when it starts, so these libraries are present in the archive even though `opfsolve` does not call them.

**Corresponding source.** Each release attaches `opfsolve-sources-gpl.tar` with the source of the GPL and LGPL libraries above (SuiteSparse, GMP, MPFR, libgit2) at the exact versions the bundled Julia uses, made by `tools/gpl-sources.sh`; their build recipes are in [Yggdrasil](https://github.com/JuliaPackaging/Yggdrasil).

## Downloaded by the installer or by `opfsolve`, not distributed with it

The installer fetches Ipopt, micromamba, and the user's HSL package, and `opfsolve` fetches PGLib-OPF cases when it first needs them, all from their publishers onto the user's machine. `opfsolve` does not include, host, or redistribute them.

| Component | License | Source |
|---|---|---|
| Ipopt 3.14 and its dependencies (MUMPS, OpenBLAS, the GCC Fortran runtime, and others) | EPL-2.0 (Ipopt), CeCILL-C (MUMPS), BSD-3-Clause (OpenBLAS), and each package's own license | https://anaconda.org/conda-forge/ipopt |
| micromamba, which installs the above | BSD-3-Clause | https://github.com/mamba-org/micromamba-releases |
| HSL MA27 (required) | Your own HSL licence; the installer compiles your copy on your machine | https://www.hsl.rl.ac.uk |
| PGLib-OPF v23.07 case files | Creative Commons Attribution 4.0 (CC BY 4.0) | https://github.com/power-grid-lib/pglib-opf |

**Rule: `opfsolve` never distributes compiled Ipopt or HSL.** No release archive, container image, or other package of `opfsolve` may contain an Ipopt or HSL binary; Ipopt is always downloaded from conda-forge, and HSL is compiled on the user's machine from the user's own licensed copy (HSL's personal licence allows modifying it "to optimise its performance for the Licensee's use on a particular platform"). `tools/check-no-ipopt-hsl.sh` enforces this on every build: it fails if an archive contains an Ipopt or HSL library file, or a binary that defines Ipopt or HSL symbols. The free HSL licences do not permit redistribution. All technical papers, sales and publicity material resulting from use of the HSL codes within Ipopt must contain the following acknowledgement: HSL, a collection of Fortran codes for large-scale scientific computation. See http://www.hsl.rl.ac.uk.

**PGLib-OPF.** CC BY 4.0 requires attribution; see the Citation section of [README.md](README.md).
