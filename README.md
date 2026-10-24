# exaacopf

Solve AC optimal power flow problems with [ExaModelsPower.jl](https://github.com/madsuite-org/ExaModelsPower.jl) models compiled ahead of time, and Ipopt. No Julia installation is needed.

```sh
curl -fsSL https://raw.githubusercontent.com/madsuite-org/exaacopf/main/install.sh | sh
```

The script installs into `./exaacopf`:

- `bin/exaacopf` and the compiled ExaModelsPower model library for your platform,
- the [IDAES](https://github.com/IDAES/idaes-ext) build of Ipopt, which includes the HSL MA27 linear solver,

then solves `pglib_opf_case118_ieee` from [PGLib-OPF](https://github.com/power-grid-lib/pglib-opf). Arguments after `sh -s --` go to `exaacopf`:

```sh
curl -fsSL https://raw.githubusercontent.com/madsuite-org/exaacopf/main/install.sh | sh -s -- case2000_goc case9241_pegase --form=rect
curl -fsSL https://raw.githubusercontent.com/madsuite-org/exaacopf/main/install.sh | sh -s -- all
curl -fsSL https://raw.githubusercontent.com/madsuite-org/exaacopf/main/install.sh | sh -s -- --install-only
```

## Usage

```
exaacopf <case>... [--form=polar|rect] [--solver=ipopt] [--options="NAME=VALUE ..."] [--output=DIR]
```

| Argument | Meaning |
|---|---|
| `<case>` | A MATPOWER file, or a PGLib-OPF case name (`case118_ieee`, `pglib_opf_case118_ieee.m`), downloaded into `exaacopf/cases` on first use. Several cases can be given; `all` solves every PGLib-OPF case for typical operating conditions. |
| `--form` | Voltage formulation: `polar` (default) or `rect`. |
| `--solver` | `ipopt` (default, and the only solver in this release). |
| `--options` | Solver options separated by spaces or commas, for example `--options="tol=1e-8 max_iter=500 linear_solver=mumps"`. `--options=@FILE` reads one `NAME VALUE` per line. May be repeated. The default linear solver is `ma27`. |
| `--output` | Output directory (default `./exaacopf-results`). |
| `--ipopt-lib`, `--model-lib` | Use another Ipopt (3.13 or later) or model library. |

```sh
exaacopf/bin/exaacopf case14_ieee case118_ieee mycase.m --form=polar --solver=ipopt --options="tol=1e-8"
```

`exaacopf` prints each step with its elapsed time (downloading, parsing the case and building the model, setting up Ipopt, solving, writing the output), with the full Ipopt log in between.

## Output

| File | Contents |
|---|---|
| `summary.csv` | One row per case: termination status, objective, iterations, problem size, and the time to parse and build the model and to solve (split into model callbacks and Ipopt). |
| `<case>/solution.json` | The result in the layout PowerModels.jl uses: `termination_status`, `primal_status`, `objective`, `solve_time`, and `solution` with `bus` (`vm`, `va`), `gen` (`pg`, `qg`), and `branch` (`pf`, `qf`, `pt`, `qt`) in per unit, keyed by MATPOWER bus number and by generator and branch row. Out-of-service components are omitted. Ipopt's constraint multipliers are under `constraint_multipliers`, by model constraint block. |
| `<case>/ipopt.log` | Full Ipopt output. |
| `exaacopf.log`, `system.txt` | Written by `install.sh`: the console output, and the date, OS, CPU, and versions used. |

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
- `bin/exaacopf` is a C program that loads the model library and Ipopt at run time and connects them through Ipopt's C interface.

`tools/build.sh <plat>` builds `exaacopf-<plat>.tar.gz` (Julia 1.12 and a C compiler are needed), and `tools/test.sh <plat>` installs it with `install.sh` and solves case14 and case118 with both forms. The `Release` workflow builds and tests every platform on each push and publishes the archives on a `v*` tag. The `Install` workflow runs the one-line installer against the latest release on Ubuntu 22.04 and 24.04 (x86_64 and arm64), macOS 13 to 15, Rocky Linux 8 and 9, Fedora, Debian 12, and Ubuntu 20.04.

## Acknowledgement

The HSL routines in the IDAES Ipopt build require the following acknowledgement in publications: HSL, a collection of Fortran codes for large-scale scientific computation. See https://www.hsl.rl.ac.uk.
