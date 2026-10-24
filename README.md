# opfsolve

Solve AC optimal power flow problems with [ExaModelsPower.jl](https://github.com/madsuite-org/ExaModelsPower.jl) models compiled ahead of time, and Ipopt. No Julia installation is needed.

opfsolve uses the HSL MA27 linear solver, which you download yourself under HSL's free personal licence: first request it at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/. HSL emails a download link, valid for 7 days, which the installer asks for (see [HSL MA27](#hsl-ma27)).

Linux and macOS (Apple silicon):

```sh
curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh
```

or, where there is no `curl` (a fresh Ubuntu desktop has only `wget`):

```sh
wget -qO- https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh
```

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.ps1 | iex
```

The script installs into `./opfsolve` (`.\opfsolve` on Windows):

- `bin/opfsolve` and the compiled ExaModelsPower model library for your platform,
- Ipopt 3.14 from [conda-forge](https://anaconda.org/conda-forge/ipopt), installed with [micromamba](https://github.com/mamba-org/micromamba-releases),
- HSL MA27, compiled with `-O3` on your machine from your own licensed copy,

then solves `pglib_opf_case118_ieee` from [PGLib-OPF](https://github.com/power-grid-lib/pglib-opf). Arguments after `sh -s --` go to `opfsolve`:

```sh
curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- case2000_goc case9241_pegase --form=rect
curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- all
curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- --install-only
```

On Windows, pass arguments through a script block:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.ps1))) case2000_goc --form=rect
```

## Usage

```
opfsolve <case>... [--form=polar|rect] [--solver=ipopt] [--options="NAME=VALUE ..."] [--output=DIR]
```

| Argument | Meaning |
|---|---|
| `<case>` | A MATPOWER file, or a PGLib-OPF case name (`case118_ieee`, `pglib_opf_case118_ieee.m`), downloaded from PGLib-OPF v23.07 into `opfsolve/cases` on first use. Several cases can be given; `all` solves every PGLib-OPF case for typical operating conditions. |
| `--form` | Voltage formulation: `polar` (default) or `rect`. |
| `--solver` | `ipopt` (default, and the only solver in this release). |
| `--options` | Solver options separated by spaces or commas, for example `--options="tol=1e-8 max_iter=500"`. `--options=@FILE` reads one `NAME VALUE` per line. May be repeated. The linear solver is `ma27`; with an HSL library that has them, `linear_solver=ma57` (or `ma86`, `ma97`) selects another. |
| `--output` | Output directory (default `./opfsolve-results`). |
| `--hsl-lib` | HSL library to use (default `opfsolve/hsl`, or `OPFSOLVE_HSL_LIB`). |
| `--ipopt-lib`, `--model-lib` | Use another Ipopt (3.14 or later) or model library. |

```sh
opfsolve/bin/opfsolve case14_ieee case118_ieee mycase.m --form=polar --solver=ipopt --options="tol=1e-8"
```

`opfsolve` prints each step with its elapsed time (downloading, parsing the case and building the model, setting up Ipopt, solving, writing the output), with the full Ipopt log in between.

## Output

| File | Contents |
|---|---|
| `summary.csv` | One row per case: termination status, objective, iterations, problem size, and the time to parse and build the model and to solve (split into model callbacks and Ipopt). |
| `<case>/solution.json` | The result in the layout PowerModels.jl uses: `termination_status`, `primal_status`, `objective`, `solve_time`, and `solution` with `bus` (`vm`, `va`), `gen` (`pg`, `qg`), and `branch` (`pf`, `qf`, `pt`, `qt`) in per unit, keyed by MATPOWER bus number and by generator and branch row. Out-of-service components are omitted. Ipopt's constraint multipliers are under `constraint_multipliers`, by model constraint block. |
| `<case>/ipopt.log` | Full Ipopt output. |
| `opfsolve.log`, `system.txt` | Written by `install.sh`: the console output, and the date, OS, CPU, and versions used. |

## HSL (MA27)

opfsolve requires MA27. Its licence is free for personal use, but `opfsolve` never distributes it, so each user downloads it:

1. Request the free personal licence for MA27 at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/. HSL emails a download link, valid for 7 days.
2. Run the installer and paste the link when it asks, or pass it in `OPFSOLVE_HSL`. The installer downloads the package, installs a Fortran compiler from conda-forge, compiles MA27 with `-O3` on your machine into `opfsolve/hsl/`, and removes the compiler again:

```sh
curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | OPFSOLVE_HSL='<download link>' sh
```

```powershell
$env:OPFSOLVE_HSL = '<download link>'; irm https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.ps1 | iex
```

`OPFSOLVE_HSL` also accepts the downloaded `.tar.gz` or `.zip`, and a Coin-HSL or Coin-HSL Archive package, which is built with meson (Linux and macOS). To use an HSL library you already have instead (for example `libcoinhsl` or the one from HSL_jll), set `OPFSOLVE_HSL_LIB=/path/to/libhsl.so` when installing, or pass `--hsl-lib=PATH` to `opfsolve`. With a library that has them, `--options=linear_solver=ma57` (or `ma86`, `ma97`) selects another HSL solver.

## Platforms

| Platform | Ipopt from conda-forge |
|---|---|
| Linux x86_64 and aarch64, glibc 2.28 or newer | `linux-64`, `linux-aarch64` |
| macOS arm64 (Apple silicon) | `osx-arm64` |
| Windows x86_64 (Windows 10 1803 or newer) | `win-64` |

No system packages are needed: conda-forge supplies Ipopt's Fortran runtime and BLAS.

## How it is built

- `emp/lib/libemp.so` (`.dylib` on macOS) holds the ExaModelsPower recipes `acp` (polar) and `acr` (rectangular), compiled by `ExaModelsCompiler.compile_all(ExaModelsPower)` with `juliac --trim=safe` into one shared library with a private Julia runtime. It implements the [cnlp ABI](https://github.com/madsuite-org/cnlp-abi): given a MATPOWER path it parses the case, builds the model, and evaluates the objective, constraints, and their derivatives.
- `bin/opfsolve` is a C program that loads the model library and Ipopt at run time and connects them through Ipopt's C interface.

`tools/build.sh <plat>` builds `opfsolve-<plat>.tar.gz` (Julia 1.12 and a C compiler are needed), and `tools/test.sh <plat>` installs it with `install.sh` and solves case14 and case118 with both forms. The `Release` workflow builds and tests every platform on each push and publishes the archives on a `v*` tag. After publishing, it runs the one-line installers against the release on Ubuntu 22.04 and 24.04 (x86_64 and arm64), macOS 14 and 15, Windows Server 2022 and 2025, Rocky Linux 8 and 9, Fedora, Debian 12, and Ubuntu 20.04. CI has no HSL licence, so it installs with `OPFSOLVE_SKIP_HSL=1` (a switch for testing only), solves with `linear_solver=mumps`, and checks that `opfsolve` refuses to run without MA27; the HSL build is tested by hand.

## License

`opfsolve` is MIT licensed. The release archives also contain the compiled ExaModelsPower model library and the Julia runtime, whose components keep their own licenses (MIT, MPL-2.0, BSD, LGPL, and, for parts of SuiteSparse, GPL-2.0+); [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists them, and each archive carries their license files in `licenses/`. Ipopt and its dependencies (from conda-forge), your HSL package (from HSL), and the PGLib-OPF data are downloaded from their publishers by the installer and are not distributed with `opfsolve`.

## Citation

`opfsolve` combines these components. If you use it in published work, please cite each of them; the BibTeX entries are in [`CITATION.bib`](CITATION.bib).

| Component | What it provides | Cite |
|---|---|---|
| Algebraic modeling system | [ExaModels.jl](https://github.com/madsuite-org/ExaModels.jl), through [ExaModelsPower.jl](https://github.com/madsuite-org/ExaModelsPower.jl): the model and its derivatives | Shin, Anitescu, and Pacaud, "Accelerating optimal power flow with GPUs: SIMD abstraction of nonlinear programs and condensed-space interior-point methods," *Electric Power Systems Research* 236, 110651, 2024 |
| Solver | [Ipopt](https://github.com/coin-or/Ipopt) | Wächter and Biegler, "On the implementation of a primal-dual interior point filter line search algorithm for large-scale nonlinear programming," *Mathematical Programming* 106(1), 25–57, 2006 |
| Linear solver | HSL MA27, from your own licensed copy | The acknowledgement HSL requires in all technical papers: "HSL, a collection of Fortran codes for large-scale scientific computation. See http://www.hsl.rl.ac.uk."; and for MA27, Duff and Reid, "The multifrontal solution of indefinite sparse symmetric linear equations," *ACM Transactions on Mathematical Software* 9(3), 302–325, 1983 |
| Problem formulation | The AC OPF problem formulation, as defined in [PowerModels.jl](https://github.com/lanl-ansi/PowerModels.jl) | Coffrin, Bent, Sundar, Ng, and Lubin, "PowerModels.jl: An Open-Source Framework for Exploring Power Flow Formulations," PSCC 2018 |
| Data | [PGLib-OPF](https://github.com/power-grid-lib/pglib-opf) v23.07 | Babaeinejadsarookolaee et al., "The Power Grid Library for Benchmarking AC Optimal Power Flow Algorithms," arXiv:1908.02788, 2019; also the source documents named in each case file header |

PGLib-OPF asks that the version be stated, since the library changes between releases; `opfsolve` downloads v23.07.

## No Ipopt or HSL in the release

`opfsolve` never distributes compiled Ipopt or HSL. Every build checks its archive for them (`tools/check-no-ipopt-hsl.sh`) and fails if it finds either. Ipopt comes from conda-forge, and HSL is compiled on the user's machine from the user's own licensed copy, which HSL's personal licence allows ("to optimise its performance for the Licensee's use on a particular platform").
