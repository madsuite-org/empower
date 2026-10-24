# opfsolve

Solve AC optimal power flow problems with [ExaModelsPower.jl](https://github.com/madsuite-org/ExaModelsPower.jl) models compiled ahead of time, and Ipopt. No Julia installation is needed.

```sh
curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh
```

The script installs into `./opfsolve`:

- `bin/opfsolve` and the compiled ExaModelsPower model library for your platform,
- the [IDAES](https://github.com/IDAES/idaes-ext) build of Ipopt, which includes the HSL MA27 linear solver,

then solves `pglib_opf_case118_ieee` from [PGLib-OPF](https://github.com/power-grid-lib/pglib-opf). Arguments after `sh -s --` go to `opfsolve`:

```sh
curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- case2000_goc case9241_pegase --form=rect
curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- all
curl -fsSL https://raw.githubusercontent.com/madsuite-org/opfsolve/main/install.sh | sh -s -- --install-only
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
| `--options` | Solver options separated by spaces or commas, for example `--options="tol=1e-8 max_iter=500 linear_solver=mumps"`. `--options=@FILE` reads one `NAME VALUE` per line. May be repeated. The default linear solver is `ma27`, or `mumps` if the Ipopt library has no HSL. |
| `--output` | Output directory (default `./opfsolve-results`). |
| `--ipopt-lib`, `--model-lib` | Use another Ipopt (3.13 or later) or model library. |

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

## Platforms

| Platform | Ipopt build |
|---|---|
| Linux x86_64 and aarch64, glibc 2.28 or newer | idaes-ext `ubuntu1804`/`ubuntu2004`/`ubuntu2204`/`el7`/`el8`, chosen from `/etc/os-release` |
| macOS arm64 and x86_64 | idaes-ext `darwin-aarch64`/`darwin-x86_64` |

On Windows, use WSL. On Linux, the IDAES Ipopt build uses the system Fortran runtime and LAPACK; if they are missing, `install.sh` stops and prints the command that installs them:

```sh
sudo apt-get install libgfortran5 liblapack3 libblas3 libgomp1   # Debian, Ubuntu
sudo dnf install libgfortran lapack blas libgomp                 # RHEL, Fedora
```

## How it is built

- `emp/lib/libemp.so` (`.dylib` on macOS) holds the ExaModelsPower recipes `acp` (polar) and `acr` (rectangular), compiled by `ExaModelsCompiler.compile_all(ExaModelsPower)` with `juliac --trim=safe` into one shared library with a private Julia runtime. It implements the [cnlp ABI](https://github.com/madsuite-org/cnlp-abi): given a MATPOWER path it parses the case, builds the model, and evaluates the objective, constraints, and their derivatives.
- `bin/opfsolve` is a C program that loads the model library and Ipopt at run time and connects them through Ipopt's C interface.

`tools/build.sh <plat>` builds `opfsolve-<plat>.tar.gz` (Julia 1.12 and a C compiler are needed), and `tools/test.sh <plat>` installs it with `install.sh` and solves case14 and case118 with both forms. The `Release` workflow builds and tests every platform on each push and publishes the archives on a `v*` tag. The `Install` workflow runs the one-line installer against the latest release on Ubuntu 22.04 and 24.04 (x86_64 and arm64), macOS 14 and 15 (arm64) and macOS 15 (x86_64), Rocky Linux 8 and 9, Fedora, Debian 12, and Ubuntu 20.04.

## License

`opfsolve` is MIT licensed. The release archives also contain the compiled ExaModelsPower model library and the Julia runtime, whose components keep their own licenses (MIT, MPL-2.0, BSD, LGPL, and, for parts of SuiteSparse, GPL-2.0+); [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists them, and each archive carries their license files in `licenses/`. Ipopt, the HSL solvers, and the PGLib-OPF data are downloaded from their publishers by `install.sh` and are not distributed with `opfsolve`.

## Citation

`opfsolve` combines four components. If you use it in published work, please cite each of them; the BibTeX entries are in [`CITATION.bib`](CITATION.bib).

| Component | What it provides | Cite |
|---|---|---|
| Algebraic modeling system | [ExaModels.jl](https://github.com/madsuite-org/ExaModels.jl), through [ExaModelsPower.jl](https://github.com/madsuite-org/ExaModelsPower.jl): the model and its derivatives | Shin, Anitescu, and Pacaud, "Accelerating optimal power flow with GPUs: SIMD abstraction of nonlinear programs and condensed-space interior-point methods," *Electric Power Systems Research* 236, 110651, 2024 |
| Solver | [Ipopt](https://github.com/coin-or/Ipopt), IDAES build | Wächter and Biegler, "On the implementation of a primal-dual interior point filter line search algorithm for large-scale nonlinear programming," *Mathematical Programming* 106(1), 25–57, 2006 |
| Problem formulation | The AC OPF problem formulation, as defined in [PowerModels.jl](https://github.com/lanl-ansi/PowerModels.jl) | Coffrin, Bent, Sundar, Ng, and Lubin, "PowerModels.jl: An Open-Source Framework for Exploring Power Flow Formulations," PSCC 2018 |
| Data | [PGLib-OPF](https://github.com/power-grid-lib/pglib-opf) v23.07 | Babaeinejadsarookolaee et al., "The Power Grid Library for Benchmarking AC Optimal Power Flow Algorithms," arXiv:1908.02788, 2019; also the source documents named in each case file header |

PGLib-OPF asks that the version be stated, since the library changes between releases; `opfsolve` downloads v23.07.

## HSL

`opfsolve` never distributes compiled Ipopt or HSL, and every build checks its archive for them (`tools/check-no-ipopt-hsl.sh`). `install.sh` downloads Ipopt directly from the [IDAES idaes-ext releases](https://github.com/IDAES/idaes-ext/releases), as `idaes get-extensions` does. IDAES states that the National Energy Technology Laboratory (NETL) has obtained a distribution license for the HSL linear solvers that allows their inclusion in IDAES's compiled Ipopt ([IDAES binary packages](https://idaes-pse.readthedocs.io/en/stable/tutorials/getting_started/binaries.html)). The IDAES macOS x86_64 build has no HSL; there `opfsolve` uses MUMPS. Any Ipopt can be used instead with `--ipopt-lib`, for example one built against your own [HSL licence](https://hsl.rl.ac.uk/licensing.html).

## Acknowledgement

The HSL routines in the IDAES Ipopt build require the following acknowledgement in publications: HSL, a collection of Fortran codes for large-scale scientific computation. See https://www.hsl.rl.ac.uk.
