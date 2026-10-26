# empower

Solve AC and DC optimal power flow problems with [ExaModelsPower.jl](https://github.com/madsuite-org/ExaModelsPower.jl) models compiled ahead of time, and Ipopt. No Julia installation is needed. (empower was called opfsolve before v0.6.0.)

empower uses the HSL MA27 linear solver, which you download yourself under HSL's free personal licence: first request it at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/. HSL emails a download link, valid for 7 days, which the installer asks for (see [HSL MA27](#hsl-ma27)).

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
- HSL MA27, compiled with `-O3` on your machine from your own licensed copy.

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
empower <case>... [--form=polar|rect|dc] [--solver=ipopt] [--options="NAME=VALUE ..."] [--output=DIR]
```

| Argument | Meaning |
|---|---|
| `<case>` | A MATPOWER file, or a PGLib-OPF case name (`case118_ieee`, `pglib_opf_case118_ieee.m`), downloaded from PGLib-OPF v23.07 into `~/.empower/cases` on first use. Several cases can be given; `all` solves every PGLib-OPF case for typical operating conditions. |
| `--form` | Formulation: `polar` (default) or `rect` for AC OPF, or `dc` for DC OPF. |
| `--device` | `cpu` (default): Ipopt with HSL MA27. `gpu`: MadNLP with cuDSS on an NVIDIA GPU (see [GPU](#gpu)). |
| `--solver` | `ipopt` on the CPU, `madnlp` on the GPU (the defaults, and the only solvers in this release). |
| `--options` | Solver options separated by spaces or commas, for example `--options="tol=1e-8 max_iter=500"`. `--options=@FILE` reads one `NAME VALUE` per line. May be repeated. The linear solver is `ma27`; with an HSL library that has them, `linear_solver=ma57` (or `ma86`, `ma97`) selects another. |
| `--output` | Output directory (default `./empower-results`). |
| `--hsl-lib` | HSL library to use (default `~/.empower/hsl`, or `EMPOWER_HSL_LIB`). |
| `--ipopt-lib`, `--model-lib` | Use another Ipopt (3.14 or later) or model library. |

```sh
empower case14_ieee case118_ieee mycase.m --form=polar --solver=ipopt --options="tol=1e-8"
```

`empower` prints each step with its elapsed time (downloading, parsing the case and building the model, solving, writing the output), Ipopt's own output for each solve (`--options=print_level=N` sets how much), and a summary table at the end.

## Output

| File | Contents |
|---|---|
| `summary.csv` | Rewritten each run, one row per case: termination status, objective, iterations, problem size, and the time to parse and build the model and to solve (split into model callbacks and Ipopt). |
| `<case>/solution.m` | The solved case, in the layout MATPOWER's `savecase` writes for an OPF result: the input case with `VM`, `VA`, `PG`, `QG`, `VG` updated and the result columns appended, `PF`, `QF`, `PT`, `QT` and the multipliers `LAM_P`, `LAM_Q`, `MU_VMAX`, `MU_VMIN` (bus), `MU_PMAX`, `MU_PMIN`, `MU_QMAX`, `MU_QMIN` (gen) and `MU_SF`, `MU_ST`, `MU_ANGMIN`, `MU_ANGMAX` (branch), in MATPOWER's units. The objective and termination status are in a comment at the top. For `dc`, as in MATPOWER's DC OPF, `VM` is 1 and the reactive columns are 0. Written by ExaModelsPower's `write_matpower_solution`, compiled into the model library. |
| `<case>/ipopt.log` | Full Ipopt output. |
| `system.txt` | The date, OS, CPU, empower version, the Ipopt and HSL libraries used (and where they came from), and the command. |

## HSL (MA27)

empower requires MA27. Its licence is free for personal use, but `empower` never distributes it, so each user downloads it:

1. Request the free personal licence for MA27 at https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/. HSL emails a download link, valid for 7 days.
2. Run the installer. Without `--hsl`, it opens the request page in your browser (where there is a desktop) and waits for you to paste the link from the email. Or pass the link with `--hsl` (`EMPOWER_HSL` on Windows). The installer downloads the package and compiles MA27 with `-O3` into `~/.empower/hsl/`. It uses your `gfortran` if you have one (or the compiler in `EMPOWER_FC`); otherwise it downloads gfortran from conda-forge. The compiler download and the compile run in the background while Ipopt installs, and the compiler is removed afterwards. Either way it checks that Ipopt can use the result:

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/madsuite-org/empower/main/install.sh | sh -s -- --hsl='<download link>'
```

```powershell
$env:EMPOWER_HSL = '<download link>'; irm https://raw.githubusercontent.com/madsuite-org/empower/main/install.ps1 | iex
```

To install the `cpu` backend without HSL, pass `--no-hsl` (`EMPOWER_SKIP_HSL=1` on Windows) or answer `skip` at the prompt; `empower` then needs `--hsl-lib`, or another linear solver in `--options` (the conda-forge Ipopt has MUMPS: `--options=linear_solver=mumps`).

`--hsl` (`EMPOWER_HSL`) also accepts the downloaded `.tar.gz` or `.zip`, and a Coin-HSL or Coin-HSL Archive package, which is built with meson (Linux and macOS). To use an HSL library you already have instead (for example `libcoinhsl` or the one from HSL_jll), pass `--hsl-lib=/path/to/libhsl.so` to the installer (`EMPOWER_HSL_LIB` on Windows) or to `empower`. With a library that has them, `--options=linear_solver=ma57` (or `ma86`, `ma97`) selects another HSL solver.

## Backends

The installer's `--backends` selects the solvers to install, comma-separated (default `cpu`); `empower --device` selects one at run time (default `cpu` if it is installed, else `cuda`).

| Backend | Solver | Platforms |
|---|---|---|
| `cpu` | Ipopt with HSL MA27 | Linux x86_64 and aarch64, macOS arm64, Windows x86_64 |
| `cuda` | MadNLP with NVIDIA cuDSS, on an NVIDIA GPU | Linux x86_64, with an NVIDIA driver for CUDA 12.8 or newer |

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/madsuite-org/empower/main/install.sh | sh -s -- --backends=cpu,cuda
empower case118_ieee --device=cuda
```

`--device=cuda` takes the same arguments and writes the same output (`madnlp.log` in place of `ipopt.log`); `--options` are then MadNLP options, for example `--options="tol=1e-6 max_iter=1000"`. The `cuda` backend comes in two archives, `empower-cuda12-linux-x86_64.tar.gz` (CUDA 12.8) and `empower-cuda13-linux-x86_64.tar.gz` (CUDA 13.0); the installer picks the one for the newest CUDA your driver supports, as `nvidia-smi` reports it (`EMPOWER_CUDA=12` or `13` chooses instead). Each is the solver compiled ahead of time with its Julia runtime, with GPU kernels compiled for compute capabilities 7.0, 7.5, 8.0, 8.6, 8.9 and 9.0 (V100, RTX 20, 30 and 40 series, A100, L40, H100), so they run without compiling; so far it has been tested on 7.0 (Quadro GV100), 7.5 (RTX 2080 Ti) and 8.6 (RTX 3080). On another GPU it compiles its kernels when it starts, which makes the first solve slower.

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

`tools/build.sh <plat>` builds `empower-<plat>.tar.gz` (Julia 1.12.7 and a C compiler are needed), including `locks/`: explicit conda-forge lock files (`tools/lock.sh`) with which the installer creates the Ipopt and compiler environments without downloading conda-forge's index or solving, so every install of a release gets the same packages; and `tools/test.sh <plat>` installs it with `install.sh` and solves case14 and case118 with each formulation. The `Release` workflow builds and tests every platform on each push. On a `v*` tag it checks that the tag matches the version in `driver/empower.c` and has a `CHANGELOG.md` entry, then publishes the archives with `SHA256SUMS`, build provenance attestations, and the corresponding source of the bundled GPL and LGPL libraries. After publishing, it runs the one-line installers against the release, then `empower` from a new shell, on Ubuntu 22.04 and 24.04 (x86_64 and arm64), macOS 14 and 15, Windows Server 2022 and 2025, Rocky Linux 8 and 9, Fedora, Debian 12, and Ubuntu 20.04. A further job installs twice into a directory with a space, checks the PATH block, solves from a new shell, and uninstalls. CI has no HSL licence, so it installs with `-y` and `EMPOWER_SKIP_HSL=1` (a switch for testing only), solves with `linear_solver=mumps`, and checks that `empower` refuses to run without MA27; the HSL build is tested by hand.

## License

`empower` is MIT licensed. The release archives also contain the compiled ExaModelsPower model library and the Julia runtime, whose components keep their own licenses (MIT, MPL-2.0, BSD, LGPL, and, for parts of SuiteSparse, GPL-2.0+); [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists them, each archive carries their license files in `licenses/`, and each release attaches the corresponding source of the GPL and LGPL libraries (`empower-sources-gpl.tar`). Ipopt and its dependencies (from conda-forge) and your HSL package (from HSL) are downloaded by the installer, and PGLib-OPF cases by `empower` when it first needs them; none of them is distributed with `empower`.

## Citation

`empower` combines these components. If you use it in published work, please cite each of them; the BibTeX entries are in [`CITATION.bib`](CITATION.bib).

| Component | What it provides | Cite |
|---|---|---|
| Algebraic modeling system | [ExaModels.jl](https://github.com/madsuite-org/ExaModels.jl), through [ExaModelsPower.jl](https://github.com/madsuite-org/ExaModelsPower.jl): the model and its derivatives | Shin, Anitescu, and Pacaud, "Accelerating optimal power flow with GPUs: SIMD abstraction of nonlinear programs and condensed-space interior-point methods," *Electric Power Systems Research* 236, 110651, 2024 |
| Solver | [Ipopt](https://github.com/coin-or/Ipopt) | Wächter and Biegler, "On the implementation of a primal-dual interior point filter line search algorithm for large-scale nonlinear programming," *Mathematical Programming* 106(1), 25–57, 2006 |
| Linear solver | HSL MA27, from your own licensed copy | The acknowledgement HSL requires in all technical papers: "HSL, a collection of Fortran codes for large-scale scientific computation. See http://www.hsl.rl.ac.uk."; and for MA27, Duff and Reid, "The multifrontal solution of indefinite sparse symmetric linear equations," *ACM Transactions on Mathematical Software* 9(3), 302–325, 1983 |
| Problem formulation | The AC and DC OPF problem formulations, as defined in [PowerModels.jl](https://github.com/lanl-ansi/PowerModels.jl) | Coffrin, Bent, Sundar, Ng, and Lubin, "PowerModels.jl: An Open-Source Framework for Exploring Power Flow Formulations," PSCC 2018 |
| Data | [PGLib-OPF](https://github.com/power-grid-lib/pglib-opf) v23.07 | Babaeinejadsarookolaee et al., "The Power Grid Library for Benchmarking AC Optimal Power Flow Algorithms," arXiv:1908.02788, 2019; also the source documents named in each case file header |

PGLib-OPF asks that the version be stated, since the library changes between releases; `empower` downloads v23.07.

## No Ipopt or HSL in the release

`empower` never distributes compiled Ipopt or HSL. Every build checks its archive for them (`tools/check-no-ipopt-hsl.sh`) and fails if it finds either. Ipopt comes from conda-forge, and HSL is compiled on the user's machine from the user's own licensed copy, which HSL's personal licence allows ("to optimise its performance for the Licensee's use on a particular platform").
