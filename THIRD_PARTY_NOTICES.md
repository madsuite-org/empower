# Third-party notices

`opfsolve` itself (the C driver, the build scripts, and `install.sh`) is under the MIT License; see [LICENSE](LICENSE).

## Bundled in the release archives

Each `opfsolve-<platform>.tar.gz` contains `licenses/` with the license file of every component listed here.

**The model library** `emp/lib/libemp.*` is compiled from these Julia packages, unmodified:

| Package | License | Source |
|---|---|---|
| ExaModels.jl | MIT | https://github.com/madsuite-org/ExaModels.jl |
| ExaModelsPower.jl | MIT | https://github.com/madsuite-org/ExaModelsPower.jl |
| ExaPowerIO.jl | MIT | https://github.com/madsuite-org/ExaPowerIO.jl |
| NLPModels.jl, SolverCore.jl, LinearOperators.jl | MPL-2.0 | https://github.com/JuliaSmoothOptimizers |
| Other dependencies (JSON, PrettyTables, Tables, TimerOutputs, and others) | MIT | see `licenses/julia-packages/` |

The MPL-2.0 packages are used unmodified; their source code is available at the addresses above.

**The Julia runtime** (`emp/lib/*libjulia*` and `emp/lib/julia/`) is Julia 1.12, MIT License, with the external libraries listed in Julia's [THIRDPARTY.md](https://github.com/JuliaLang/julia/blob/master/THIRDPARTY.md) (included as `licenses/julia-THIRDPARTY.md` for the exact Julia version used). These are the official Julia binaries of those libraries, unmodified. Among them:

- SuiteSparse: AMD, CAMD, CCOLAMD, COLAMD, and SuiteSparse_config are BSD-3-Clause; BTF, KLU, and LDL are LGPL-2.1+; CHOLMOD is LGPL-2.1+ and GPL-2.0+; RBio, SPQR, and UMFPACK are GPL-2.0+. Source: https://github.com/DrTimothyAldenDavis/SuiteSparse
- GMP (LGPL-3.0+ or GPL-2.0+) and MPFR (LGPL-3.0+). Source: https://gmplib.org, https://www.mpfr.org
- libgit2 (GPL-2.0+ with a linking exception). Source: https://github.com/libgit2/libgit2
- OpenBLAS (BSD-3-Clause), libblastrampoline, libuv, libunwind, nghttp2, curl (MIT or similar), OpenSSL (Apache-2.0), PCRE2 (BSD-3-Clause), zlib, dSFMT, OpenLibm.
- GCC runtime libraries (libgcc_s, libstdc++, libgfortran, libgomp, libatomic, libssp): GPL-3.0 with the [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html); libquadmath: LGPL-2.1+. Source: https://gcc.gnu.org

The Julia runtime loads SuiteSparse when it starts, so these libraries are present in the archive even though `opfsolve` does not call them.

## Downloaded by `install.sh`, not distributed by `opfsolve`

These are fetched from their publishers' own release pages onto the user's machine. `opfsolve` does not include, host, or redistribute them.

| Component | License | Source |
|---|---|---|
| Ipopt, IDAES build (`idaes-solvers-<platform>.tar.gz`) | EPL-2.0 (Ipopt), plus the licenses in that archive's `license.txt` | https://github.com/IDAES/idaes-ext/releases |
| HSL linear solvers (MA27 and others) inside the IDAES Ipopt | Distributed by IDAES under a distribution license obtained by NETL ([IDAES documentation](https://idaes-pse.readthedocs.io/en/stable/tutorials/getting_started/binaries.html)) | https://www.hsl.rl.ac.uk |
| PGLib-OPF v23.07 case files | Creative Commons Attribution 4.0 (CC BY 4.0) | https://github.com/power-grid-lib/pglib-opf |

**Rule: `opfsolve` never distributes compiled Ipopt or HSL.** No release archive, container image, or other package of `opfsolve` may contain an Ipopt or HSL binary; both are always downloaded by the user from their publisher. `tools/check-no-ipopt-hsl.sh` enforces this on every build: it fails if an archive contains an Ipopt or HSL library file, or a binary that defines Ipopt or HSL symbols. The free HSL licences do not permit redistribution. All technical papers, sales and publicity material resulting from use of the HSL codes within Ipopt must contain the following acknowledgement: HSL, a collection of Fortran codes for large-scale scientific computation. See http://www.hsl.rl.ac.uk.

**PGLib-OPF.** CC BY 4.0 requires attribution; see the Citation section of [README.md](README.md).
