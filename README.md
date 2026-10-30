# empower

Solve AC and DC optimal power flow problems with [ExaModelsPower.jl](https://github.com/madsuite-org/ExaModelsPower.jl) models compiled ahead of time: with Ipopt on the CPU, or with MadNLP and cuDSS on an NVIDIA GPU (see [Backends](#backends)). No Julia installation is needed. (empower was called opfsolve before v0.6.0.)

On the CPU, empower uses the HSL MA27 linear solver, which you download yourself under HSL's free personal licence: request it at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/ (about 2 minutes), and HSL emails a download link, valid for 7 days, which the installer asks for. An HSL library already on the machine is found and offered instead. Press Enter to skip HSL for now; Ipopt then uses MUMPS, and `empower hsl` adds MA27 later (see [HSL MA27](#hsl-ma27)).

Linux and macOS (Apple silicon):

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/madsuite-org/empower/main/install.sh | sh
```

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/madsuite-org/empower/main/install.ps1 | iex
```

The installer works like juliaup's. It shows the install directory, asks for confirmation (`-y` accepts the defaults), and installs into `~/.empower`:

- `bin/empower` and the compiled ExaModelsPower model library for your platform,
- Ipopt 3.14 from [conda-forge](https://anaconda.org/conda-forge/ipopt), installed with [micromamba](https://github.com/mamba-org/micromamba-releases),
- HSL MA27: compiled with `-O3` on your machine from your own licensed copy, or an HSL library you already have,
- on Linux with an NVIDIA GPU (or with `--backends=cpu,cuda`), the GPU solver and the NVIDIA libraries it uses (see [Backends](#backends)).

It then puts `~/.empower/bin` on `PATH`: in a marked `# >>> empower initialize >>>` block in `~/.bashrc`, `~/.bash_profile` or `~/.profile`, and `~/.zshrc` (zsh and macOS), and in the user `PATH` on Windows, where it also applies to the current PowerShell session. In a new terminal, run:

```sh
empower case118_ieee
```

A shell cannot change the `PATH` of the terminal that started it, so on Linux and macOS the installer prints the one `export` line that makes `empower` available in the current terminal as well.

Installer options (after `sh -s --`): `-y` (no prompts), `--path DIR` (install directory), `--add-to-path no` (leave the startup files alone), `--hsl LINK` (the download link from HSL's email), `--hsl-lib FILE` (an HSL library you already have), `--no-hsl` (install without HSL), `--backends LIST` (`cpu`, `cuda`, or `cpu,cuda`; see [Backends](#backends)), and `--uninstall`; `--help` lists them. On Windows, set the environment variables `EMPOWER_YES=1`, `EMPOWER_DIR`, `EMPOWER_ADD_TO_PATH=no`, `EMPOWER_HSL`, `EMPOWER_HSL_LIB`, `EMPOWER_SKIP_HSL=1`, or `EMPOWER_UNINSTALL=1` before running it.

**Upgrade** by running the installer again: it installs the latest release (or `EMPOWER_VERSION=vX.Y.Z`), keeps your HSL library, and recreates the Ipopt environment only if the release pins different packages. **Uninstall** with `sh -s -- --uninstall` (or `$env:EMPOWER_UNINSTALL='1'` on Windows), which removes `~/.empower` and the PATH entries.

**Integrity.** The installer downloads only over HTTPS. It checks each release archive against the release's `SHA256SUMS` and micromamba against its published SHA-256, unpacks into a staging directory, and swaps the result in only when it is complete. Ipopt and the compiler come from conda-forge through lock files in the archive, which pin each package by URL and MD5. Each archive also has a signed build provenance attestation; check it with `gh attestation verify empower-<platform>.tar.gz --repo madsuite-org/empower` (GitHub CLI 2.49 or newer).

## Usage

```
empower <case>... [--form=polar|rect|dc] [--device=cpu|cuda] [--options="NAME=VALUE ..."] [--output=DIR]
                  [--time-limit=SECONDS] [--jobs=N] [--quiet]
empower json SOLUTION.m [OUT.json]   a solved case as JSON (each run writes solution.json)
empower up                     update to the latest release, for the backends installed
empower hsl [LINK|FILE|LIB]    add HSL MA27: the link from HSL's email, the package, or a library
empower download               download the whole PGLib-OPF v23.07 library
empower check                  solve case14 with each installed backend (the installer runs it)
```

| Argument | Meaning |
|---|---|
| `<case>` | A MATPOWER file, or a PGLib-OPF case name (`case118_ieee`, `pglib_opf_case118_ieee.m`), downloaded from PGLib-OPF v23.07 into `~/.empower/cases` on first use. Several cases can be given; `all` solves every PGLib-OPF case for typical operating conditions. `empower download` downloads the whole library (typical, `api` and `sad` cases) at once. |
| `--form` | Formulation: `polar` (default) or `rect` for AC OPF, or `dc` for DC OPF. |
| `--device` | `cpu`: Ipopt with HSL MA27 (MUMPS without HSL). `cuda`: MadNLP with cuDSS on an NVIDIA GPU (see [Backends](#backends)). Default: `cpu` if it is installed, else `cuda`. |
| `--solver` | `ipopt` on the CPU, `madnlp` on the GPU (the defaults, and the only solvers in this release). |
| `--options` | Solver options separated by spaces or commas, for example `--options="tol=1e-8 max_iter=500"`. `--options=@FILE` reads one `NAME VALUE` per line. May be repeated. On the CPU the linear solver is `ma27` (`mumps` without HSL); with an HSL library that has them, `linear_solver=ma57` (or `ma86`, `ma97`) selects another. On the GPU they are MadNLP options. |
| `--output` | Output directory (default `./empower-results`). |
| `--time-limit` | Wall-clock limit per case, in seconds (the solver's `max_wall_time`). A case that reaches it ends with status `TIME_LIMIT`. |
| `-j`, `--jobs` | Solve up to N cases at a time, each in its own process (`--device=cpu` only). Each case's console output goes to `<case>/console.log`; the console shows a line per finished case and the summary. Each process solves with one thread, so N up to the number of physical cores is reasonable. |
| `-q`, `--quiet` | Print only warnings, errors, one line per case, and the summary. The solver logs are written in full either way. |
| `--hsl-lib` | HSL library to use (default `~/.empower/hsl`, or `EMPOWER_HSL_LIB`, or `libhsl`/`libcoinhsl` in `/usr/local/lib` and the other standard library directories). |
| `--ipopt-lib`, `--model-lib` | Use another Ipopt (3.14 or later) or model library. |

```sh
empower case14_ieee case118_ieee mycase.m --form=polar --solver=ipopt --options="tol=1e-8"
```

`empower` prints each step with its elapsed time (downloading, parsing the case and building the model, solving, writing the output) under a heading per case, the solver's own output for each solve (`--options=print_level=N` sets how much), and a summary table at the end:

```
── Summary ───────────────────────────────────────────────────────────────────
  written  empower-results/summary.csv
  ┌────────────────────────┬────────────────┬───────────┬──────────┬──────┬───────────┬───────────┬───────────┬────────────┐
  │ case                   │ status         │ objective │ max viol │ iter │ build (s) │ solve (s) │ evals (s) │ solver (s) │
  ├────────────────────────┼────────────────┼───────────┼──────────┼──────┼───────────┼───────────┼───────────┼────────────┤
  │ pglib_opf_case118_ieee │ LOCALLY_SOLVED │ 97213.607 │ 9.83e-09 │   29 │     0.001 │     0.260 │     0.004 │      0.256 │
  └────────────────────────┴────────────────┴───────────┴──────────┴──────┴───────────┴───────────┴───────────┴────────────┘
```

A second table gives the violations by group (see `violations.csv` below). `max viol` is the largest absolute constraint violation at the solution, max(cl - c(x), c(x) - cu); `solve` is split into `evals` (the model's functions and derivatives) and `solver` (the solver itself, with its linear solver). On a terminal the status is coloured; set `NO_COLOR` to turn colours off.

To update to the latest release, run `empower up` (or `empower update`). It runs the installer again into the same directory, for the backends you have installed, keeping your HSL library and leaving your shell startup files alone.

## C, C++ and Python

The install has a C API, `libempower` (`include/empower.h`; `lib/libempower.so`, `lib/libempower.dylib`, or `bin/empower.dll` with `lib/libempower.dll.a`), usable from C and C++, and a Python package (`python/`). Both solve a case with the install's backends and return the solution in memory: status, objective, iterations, times, and the bus voltages, generator dispatch and branch flows, per unit and radians, in the order of the case file's tables (out-of-service generators and branches have zeros).

```c
#include "empower.h"

empower *e = empower_open(NULL);   /* NULL: the install this library is in */
if (!e) { fprintf(stderr, "%s\n", empower_error()); return 1; }
empower_result *r = empower_solve(e, "case118_ieee", "polar", "cpu", "tol=1e-8 max_wall_time=60", NULL);
if (r) printf("%s %g, vm[0] = %g, pg[0] = %g\n", r->status, r->objective, r->vm[0], r->pg[0]);
empower_free_result(r);
empower_close(e);
```

```sh
cc -I ~/.empower/include app.c -L ~/.empower/lib -lempower -Wl,-rpath,$HOME/.empower/lib
```

```python
import sys; sys.path.insert(0, "/home/me/.empower/python")   # or: pip install ~/.empower/python
import empower
r = empower.solve("case118_ieee", form="polar", device="cpu", options={"tol": 1e-8}, time_limit=60)
r.status, r.objective, r.bus["vm"], r.gen["pg"], r.branch["pf"]   # numpy arrays if numpy is installed
```

`device="cuda"` (`"cuda"` in C) solves on the GPU. Each solve runs the install's `empower` in a process of its own, about 0.1 s of overhead, and reads the solution back: the Julia runtime in the model library brings its own libraries (OpenSSL among them), and in the same process as a program that has already loaded other versions (numpy loads the system's OpenSSL) it cannot start. The API is not thread-safe; solve one case at a time per process, or use `empower -j` for many cases.

## Output

| File | Contents |
|---|---|
| `summary.csv` | Rewritten each run, one row per case: termination status, objective, iterations, problem size, the time to parse and build the model and to solve (split into model callbacks and the solver), and the largest absolute constraint violation at the solution, max(cl - c(x), c(x) - cu). The end of the run prints the same table. |
| `<case>/solution.m` | The solved case, in the layout MATPOWER's `savecase` writes for an OPF result: the input case with `VM`, `VA`, `PG`, `QG`, `VG` updated and the result columns appended, `PF`, `QF`, `PT`, `QT` and the multipliers `LAM_P`, `LAM_Q`, `MU_VMAX`, `MU_VMIN` (bus), `MU_PMAX`, `MU_PMIN`, `MU_QMAX`, `MU_QMIN` (gen) and `MU_SF`, `MU_ST`, `MU_ANGMIN`, `MU_ANGMAX` (branch), in MATPOWER's units. The objective and termination status are in a comment at the top. For `dc`, as in MATPOWER's DC OPF, `VM` is 1 and the reactive columns are 0. Written by ExaModelsPower's `write_matpower_solution`, compiled into the model library. |
| `<case>/solution.json` | The same solution in PowerModels' result layout: `termination_status`, `objective`, and `solution` with `bus` (`vm`, `va`), `gen` (`pg`, `qg`) and `branch` (`pf`, `qf`, `pt`, `qt`), in per unit and radians. Buses are keyed by their number, generators and branches by their position in the case (only those in service). `empower json` converts a `solution.m` the same way. |
| `<case>/violations.csv` | The largest violation of each group of constraints and bounds at the solution, `max(l - v, v - u, 0)`: power balance, power flow equations, branch flow limits, angle differences, voltage and generation bounds (ExaModelsPower's `opf_violations`). The same numbers are the `violation_*` columns of `summary.csv`, and the end of the run prints them as a second table. |
| `<case>/ipopt.log` | Full Ipopt output (`madnlp.log`, MadNLP's, with `--device=cuda`). |
| `<case>/console.log` | With `--jobs`: what the case's process printed. |
| `system.txt` | The date, OS, CPU, empower version, the Ipopt and HSL libraries used (and where they came from), and the command. |

## HSL (MA27)

On the CPU, empower uses MA27. Its licence is free for personal use, but `empower` never distributes it, so each user downloads it (or uses an HSL library they already have, below):

1. Request the free personal licence for MA27 at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/. HSL emails a download link, valid for 7 days.
2. Run the installer. Without `--hsl`, it opens the request page in your browser (where there is a desktop) and waits for you to paste the link from the email. Or pass the link with `--hsl` (`EMPOWER_HSL` on Windows). The installer downloads the package and compiles MA27 with `-O3` into `~/.empower/hsl/`. It uses your `gfortran` if you have one (or the compiler in `EMPOWER_FC`); otherwise it downloads gfortran from conda-forge. The compiler download and the compile run in the background while Ipopt installs, and the compiler is removed afterwards. Either way it checks that Ipopt can use the result:

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/madsuite-org/empower/main/install.sh | sh -s -- --hsl='<download link>'
```

```powershell
$env:EMPOWER_HSL = '<download link>'; irm https://raw.githubusercontent.com/madsuite-org/empower/main/install.ps1 | iex
```

To install the `cpu` backend without HSL, press Enter at the prompt, or pass `--no-hsl` (`EMPOWER_SKIP_HSL=1` on Windows). Ipopt then uses MUMPS, which is not the most efficient linear solver for OPF and whose timings are not appropriate for benchmarking solver performance; `empower` warns about it on each run. Add MA27 at any time with `empower hsl '<download link>'` (or a downloaded package, or an HSL library), which runs the installer for the installed release and downloads nothing else.

If an HSL library is already on the machine (`libhsl` or `libcoinhsl` in `/usr/local/lib`, `/opt/homebrew/lib`, `/usr/lib`, on `LD_LIBRARY_PATH`, or in the `ldconfig` cache), the installer finds it and offers it; Enter accepts it, and `-y` uses it. A library that does not load as it is, such as one from the `HSL_jll` Julia package (which links Julia's `libblastrampoline`), is installed as a copy adapted to load the Ipopt environment's OpenBLAS; for HSL_jll's full `libhsl.so`, which also links MPI, the installer adds MPICH from conda-forge. With HSL_jll 2025.1.19, MA27, MA57 and MA86 work this way; MA97 does not (its first iteration fails, likely a mismatch between that HSL release and the HSL interface conda-forge's Ipopt was built with).

`--hsl` (`EMPOWER_HSL`) also accepts the downloaded `.tar.gz` or `.zip`, and a Coin-HSL or Coin-HSL Archive package, which is built with meson (Linux and macOS). To use an HSL library you already have instead (for example `libcoinhsl` or the one from HSL_jll), pass `--hsl-lib=/path/to/libhsl.so` to the installer (`EMPOWER_HSL_LIB` on Windows) or to `empower`. With a library that has them, `--options=linear_solver=ma57` (or `ma86`, `ma97`) selects another HSL solver.

## Backends

The installer's `--backends` selects the solvers to install, comma-separated. Without it, the installer installs `cpu`, and `cuda` as well on Linux x86_64 when `nvidia-smi` finds an NVIDIA GPU whose driver supports CUDA 12.8 or newer; it says so before installing, and `--backends=cpu` keeps to the CPU. `empower --device` selects one at run time (default `cpu` if it is installed, else `cuda`).

| Backend | Solver | Platforms |
|---|---|---|
| `cpu` | Ipopt with HSL MA27 | Linux x86_64 and aarch64, macOS arm64, Windows x86_64 |
| `cuda` | MadNLP with NVIDIA cuDSS, on an NVIDIA GPU | Linux x86_64, with an NVIDIA driver for CUDA 12.8 or newer |

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/madsuite-org/empower/main/install.sh | sh -s -- --backends=cpu,cuda
empower case118_ieee --device=cuda
```

`--device=cuda` takes the same arguments and writes the same output (`madnlp.log` in place of `ipopt.log`); `--options` are then MadNLP options, for example `--options="tol=1e-6 max_iter=1000"`. The `cuda` backend is `empower-cuda12-linux-x86_64.tar.gz`, built with CUDA 12.8, which runs on every driver for CUDA 12.8 or newer, CUDA 13 included. (`tools/build-cuda.sh 13` builds a CUDA 13.0 archive as well, which needs a GPU of compute capability 7.5 or newer to build; once a release carries it, the installer picks it for a CUDA 13 driver and a non-Volta GPU, and `EMPOWER_CUDA=12` or `13` chooses.) It is the solver compiled ahead of time with its Julia runtime, with GPU kernels compiled for compute capabilities 7.0, 7.5, 8.0, 8.6, 8.9, 9.0, 10.0 and 12.0 (V100, RTX 20 to 50 series, A100, L40, H100, B200), so they run without compiling; so far it has been tested on 7.0 (Quadro GV100), 7.5 (RTX 2080 Ti), 8.0 (A100), 8.6 (RTX 3080), 8.9 (L40S) and 12.0 (RTX PRO 6000 Blackwell). On another GPU it compiles its kernels when it starts, which makes the first solve slower. On a supported GPU nothing is compiled at run time; the first case of a run is still slower than the next ones, mostly because each precompiled kernel is loaded into the GPU driver on its first use.

It needs about 8 GB of memory: with less, the system keeps re-reading its 2 GB executable and the NVIDIA libraries, and runs are many times slower. On a cluster, request memory with the job (Slurm's default is often 1 GB), for example `salloc --gres=gpu:1 -c 8 --mem=32G`; `empower` warns when its memory limit is below 8 GB. The installer's last step, `empower check`, solves case14 with each backend. The first run on a machine or node takes longer (about 7 s on a cluster node with the files on NFS, against about 2 s later): `empower-cuda` loads its whole 2 GB image when it starts, which is read from disk the first time.

The installer requires an NVIDIA driver for CUDA 12.8 or newer. On an older CUDA 12 driver, `EMPOWER_CUDA=12` skips that check; whether the backend then runs depends on CUDA's minor-version compatibility and has not been tested.

The archive does not contain the NVIDIA libraries it uses (CUDA runtime, cuBLAS, cuSPARSE, cuSOLVER, NVRTC, nvJitLink, cuDSS). The installer downloads them, about 1.7 GB, as the artifacts of the Julia packages that wrap them (`CUDA_Runtime_jll`, `CUDA_Compiler_jll`, `CUDSS_jll`), from the URLs and SHA-256 sums those packages publish, into `~/.empower/cuda-depot`, under NVIDIA's licences.

## Platforms

| Platform | Ipopt from conda-forge |
|---|---|
| Linux x86_64 and aarch64, glibc 2.28 or newer | `linux-64`, `linux-aarch64` |
| macOS arm64 (Apple silicon) | `osx-arm64` |
| Windows x86_64 (Windows 10 1803 or newer) | `win-64` |

No system packages are needed: conda-forge supplies Ipopt's Fortran runtime and BLAS.

## How it is built

- `emp/lib/libemp.so` (`emp/lib/libemp.dylib` on macOS, `emp/bin/libemp.dll` on Windows) holds the ExaModelsPower recipes `acp` (polar), `acr` (rectangular) and `dcp` (DC), with ExaModelsPower's MATPOWER writer, compiled by `ExaModelsCompiler.compile_all(ExaModelsPower)` with `juliac --trim=safe` into one shared library with a private Julia runtime. It implements the [cnlp ABI](https://github.com/madsuite-org/cnlp-abi): given a MATPOWER path it parses the case, builds the model, and evaluates the objective, constraints, and their derivatives.
- `bin/empower` is a C program that loads the model library and Ipopt at run time and connects them through Ipopt's C interface.
- `cuda/bin/empower-cuda` (the `cuda` backend) is `gpu/EmpowerGPU`, which builds the ExaModelsPower model on the GPU and solves it with MadNLP and cuDSS, compiled by `juliac` with its Julia runtime; `bin/empower --device=cuda` runs it. `tools/build-cuda.sh 12` builds it on a machine with an NVIDIA GPU, and `tools/test-cuda.sh 12` installs it and checks the solutions, and that a run, on a file and on a terminal, compiles nothing.

`tools/build.sh <plat>` builds `empower-<plat>.tar.gz` (Julia 1.12.7 and a C compiler are needed), including `locks/`: explicit conda-forge lock files (`tools/lock.sh`) with which the installer creates the Ipopt and compiler environments without downloading conda-forge's index or solving, so every install of a release gets the same packages; and `tools/test.sh <plat>` installs it with `install.sh` and solves case14 and case118 with each formulation. The `Release` workflow builds and tests every platform on each push, the `cuda` archive on a self-hosted runner with an NVIDIA GPU. On a `v*` tag it checks that the tag matches the version in `driver/empower.c` and has a `CHANGELOG.md` entry, then publishes the archives with `SHA256SUMS`, build provenance attestations, and the corresponding source of the bundled GPL and LGPL libraries. After publishing, it runs the one-line installers against the release, then `empower` from a new shell, on Ubuntu 22.04 and 24.04 (x86_64 and arm64), macOS 14 and 15, Windows Server 2022 and 2025, Rocky Linux 8 and 9, Fedora, Debian 12, and Ubuntu 20.04. A further job installs twice into a directory with a space, checks the PATH block, solves from a new shell, and uninstalls. CI has no HSL licence, so it installs with `-y` and `EMPOWER_SKIP_HSL=1` (a switch for testing only), solves with `linear_solver=mumps`, and checks that `empower` falls back to MUMPS with its warning without HSL; the HSL build is tested by hand.

## License

`empower` is MIT licensed. The release archives also contain the compiled ExaModelsPower model library and the Julia runtime, whose components keep their own licenses (MIT, MPL-2.0, BSD, LGPL, and, for parts of SuiteSparse, GPL-2.0+); [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists them, each archive carries their license files in `licenses/`, and each release attaches the corresponding source of the GPL and LGPL libraries (`empower-sources-gpl.tar`). Ipopt and its dependencies (from conda-forge) and your HSL package (from HSL) are downloaded by the installer, and PGLib-OPF cases by `empower` when it first needs them; none of them is distributed with `empower`.

## Citation

`empower` combines these components. If you use it in published work, please cite each of them; the BibTeX entries are in [`CITATION.bib`](CITATION.bib).

| Component | What it provides | Cite |
|---|---|---|
| Power system models | [ExaModelsPower.jl](https://github.com/madsuite-org/ExaModelsPower.jl): the AC and DC OPF models, built from the MATPOWER case | Johnson, Lauinger, Shin, and Pacaud, "ExaModelsPower.jl: A GPU-Compatible Modeling Library for Nonlinear Power System Optimization," arXiv:2510.12897, 2025 |
| Algebraic modeling system | [ExaModels.jl](https://github.com/madsuite-org/ExaModels.jl): the model's functions and derivatives, on the CPU and the GPU | Shin, Schanen, Pacaud, Montoison, and Anitescu, "ExaModels.jl: An Algebraic Modeling System for Nonlinear Programming on GPUs," arXiv:2608.16265, 2026; and Shin, Anitescu, and Pacaud, "Accelerating optimal power flow with GPUs: SIMD abstraction of nonlinear programs and condensed-space interior-point methods," *Electric Power Systems Research* 236, 110651, 2024 |
| Solver (`cpu`) | [Ipopt](https://github.com/coin-or/Ipopt) | Wächter and Biegler, "On the implementation of a primal-dual interior point filter line search algorithm for large-scale nonlinear programming," *Mathematical Programming* 106(1), 25–57, 2006 |
| Linear solver (`cpu`) | HSL MA27, from your own licensed copy | The acknowledgement HSL requires in all technical papers: "HSL, a collection of Fortran codes for large-scale scientific computation. See http://www.hsl.rl.ac.uk."; and for MA27, Duff and Reid, "The multifrontal solution of indefinite sparse symmetric linear equations," *ACM Transactions on Mathematical Software* 9(3), 302–325, 1983 |
| GPU solver (`cuda`) | [MadNLP.jl](https://github.com/madsuite-org/MadNLP.jl), its condensed-space interior-point method on the GPU | Shin, Anitescu, and Pacaud, "Accelerating optimal power flow with GPUs: SIMD abstraction of nonlinear programs and condensed-space interior-point methods," *Electric Power Systems Research* 236, 110651, 2024 |
| GPU linear solver (`cuda`) | NVIDIA [cuDSS](https://developer.nvidia.com/cudss), the sparse direct solver MadNLP uses on the GPU | NVIDIA cuDSS, https://developer.nvidia.com/cudss (no paper; cite the library and its version, which `empower` records in `nvidia-artifacts.tsv`) |
| Graph partitioning (`cuda`) | [METIS](https://github.com/KarypisLab/METIS), which cuDSS uses to order the matrix before factorizing it | Karypis and Kumar, "A fast and high quality multilevel scheme for partitioning irregular graphs," *SIAM Journal on Scientific Computing* 20(1), 359–392, 1998 |
| GPU programming (`cuda`) | [CUDA.jl](https://github.com/JuliaGPU/CUDA.jl), which compiles the model's and MadNLP's GPU kernels | Besard, Foket, and De Sutter, "Effective Extensible Programming: Unleashing Julia on GPUs," *IEEE Transactions on Parallel and Distributed Systems*, 2018, doi:10.1109/TPDS.2018.2872064 |
| Problem formulation | The AC and DC OPF problem formulations, as defined in [PowerModels.jl](https://github.com/lanl-ansi/PowerModels.jl) | Coffrin, Bent, Sundar, Ng, and Lubin, "PowerModels.jl: An Open-Source Framework for Exploring Power Flow Formulations," PSCC 2018 |
| Data | [PGLib-OPF](https://github.com/power-grid-lib/pglib-opf) v23.07 | Babaeinejadsarookolaee et al., "The Power Grid Library for Benchmarking AC Optimal Power Flow Algorithms," arXiv:1908.02788, 2019; also the source documents named in each case file header |

PGLib-OPF asks that the version be stated, since the library changes between releases; `empower` downloads v23.07.

## No Ipopt or HSL in the release

`empower` never distributes compiled Ipopt or HSL. Every build checks its archive for them (`tools/check-no-ipopt-hsl.sh`) and fails if it finds either. Ipopt comes from conda-forge, and HSL is compiled on the user's machine from the user's own licensed copy, which HSL's personal licence allows ("to optimise its performance for the Licensee's use on a particular platform").
