# Changelog

## v0.9.1

- `empower-cuda` loads its kernels on 4 threads (about 0.25 s, from 0.8 s) and then solves the built-in case14 once, silently, in the run's formulation and options (`warm-up`, about 0.13 s; `EMPOWER_NO_WARMUP=1` skips it): the first use of cuDSS, of the kernels and of MadNLP's solver setup no longer falls in the first case. On an RTX 3080, case118 as the first case of a run now solves as fast as the second.
- `EMPOWER_TRACE_COMPILE=1` also reports each case's garbage collection (pauses, mark and sweep time) and compile time, and `EMPOWER_GC_LOG=1` turns on Julia's log of each collection.

## v0.9.0

- The first case of a cuda run is as fast as the next ones: `empower-cuda` loads its GPU's precompiled kernels into the driver, and creates the cuBLAS, cuSPARSE and cuDSS handles, when it starts (shown as `kernels`), instead of on first use. Loading them one by one during the first case cost about a second, mostly in garbage collections set off by CUDA.jl allocating a 1 MiB error-log buffer for each kernel; at startup they load with garbage collection paused. On an RTX 3080, case14 took 0.59 s to build and 0.79 s to solve as the first case of a run, and now about 0.02 s and 0.13 s, against 0.01 s and 0.10 s as the second.
- `EMPOWER_PROFILE=FILE` profiles the first case of a cuda run and writes where the time went to FILE.
- Ctrl-C stops a cuda run at once: `empower` kills `empower-cuda`, which kept solving (and printing) after `empower` had exited.
- Without `--backends`, the installer installs the cuda backend as well on Linux x86_64 when `nvidia-smi` finds an NVIDIA GPU whose driver supports CUDA 12.8 or newer, and says so before installing.
- An HSL library already on the machine is used: the installer looks in `/usr/local/lib`, `/usr/local/lib64`, `/opt/homebrew/lib`, `/usr/lib`, `/usr/lib64`, `LD_LIBRARY_PATH` and the `ldconfig` cache for `libhsl` or `libcoinhsl` and offers it (Enter accepts it; `-y` uses it), and `empower` itself, with no HSL installed or given, uses one from the standard library directories when Ipopt can load MA27 from it.
- An HSL library that does not load as it is, such as one from a Julia package (`HSL_jll`, which links Julia's `libblastrampoline`), is installed as an adapted copy that loads the Ipopt environment's OpenBLAS (`patchelf` on Linux, `install_name_tool` on macOS). Checked with `HSL_jll` 2025.1.19's `libhsl_subset.so`: MA27, MA57 and MA86 solve.

## v0.8.0

- Without HSL, the cpu backend runs Ipopt with MUMPS instead of refusing to run, and warns, on install and on each run, that MUMPS is not the most efficient linear solver for OPF and not appropriate for benchmarking solver performance. Pressing Enter at the installer's HSL prompt skips HSL, as does a `-y` install with no HSL link.
- `empower hsl [LINK|FILE|LIB]` adds HSL MA27 to an existing install: the download link from HSL's email, the downloaded package, or an HSL library. It runs the installer for the installed release, which downloads nothing else.
- `empower download` downloads the whole PGLib-OPF v23.07 library: the typical operating conditions, `api` and `sad` cases.
- The summary at the end of a run, and `summary.csv`, give the largest absolute constraint violation at the solution, max(cl - c(x), c(x) - cu), and split the solve time into model evaluations and the solver; the summary names the linear solver.
- Tidier output in both backends: a ruled heading per case, the summary as a ruled table, dimmed timestamps and the status in green or red on a terminal (`NO_COLOR` turns colours off); the solver's own log stays on screen.
- The cuda backend compiles nothing on a terminal either: printing to a terminal compiled 9 methods (about 0.13 s) on each run.

## v0.7.1

- `empower up` (or `empower update`) updates empower to the latest release: it runs the installer again into the same directory, for the backends installed there, keeping the HSL library and leaving the shell startup files alone.
- The cuda backend compiles nothing at run time: the first case no longer pays for compiling model building and the solver (about 40 s with the updated dependencies). CI fails if a traced run compiles anything.
- The cuda backend runs on glibc 2.28 and newer (RHEL 8 and similar): the executable's launcher was linked against the build machine's glibc 2.34.
- Built on ExaModelsPower 0.4.3, ExaModels 0.13, ExaModelsCompiler 0.1.2, GPUCompiler 2.18, and MadNLP from madsuite-org/MadNLP.jl#668 at its current head.

## v0.7.0

- The cuda backend: `empower --device=cuda` solves on an NVIDIA GPU with MadNLP and cuDSS, with the same arguments and output (`madnlp.log` in place of `ipopt.log`). Linux x86_64, with an NVIDIA driver for CUDA 12.8 or newer. It is a separate archive, `empower-cuda12-linux-x86_64.tar.gz`, compiled ahead of time with GPU kernels for compute capabilities 7.0 to 12.0, and contains no NVIDIA library: the installer downloads them as the artifacts of the Julia packages that wrap them, from the URLs and SHA-256 sums those packages publish.
- `install.sh --backends=cpu,cuda` selects the solvers to install (default `cpu`); a cuda-only install needs no Ipopt or HSL. `empower --device=cpu|cuda` selects one at run time, defaulting to cpu when it is installed.
- `--no-hsl` (`EMPOWER_SKIP_HSL=1` on Windows), or `skip` at the prompt, installs the cpu backend without HSL; the prompt also takes an HSL library you already have.

## v0.6.0

- Renamed from opfsolve to empower: the command, the repository (madsuite-org/empower), the install directory (`~/.empower`), and the environment variables (`EMPOWER_*`).
- `--form=dc` solves the DC OPF (ExaModelsPower's `dcp` model).
- The result is the solved case in MATPOWER's `savecase` layout, `<case>/solution.m`, written by ExaModelsPower's `write_matpower_solution` compiled into the model library, in place of `solution.json`.
- MATPOWER cases with commented-out or blank table rows, CRLF line endings, or `rateA = 0` (no limit) are read correctly (ExaPowerIO 0.3.2).
- `summary.csv` is rewritten each run instead of appended to, so it describes one run, like `system.txt`.

## v0.5.0

- Installers: install atomically (download, verify, unpack aside, swap in), upgrade on re-run, verify release archives against `SHA256SUMS` and micromamba against its published SHA-256, HTTPS only, `--uninstall`, install directories with spaces, and the whole script in one function so a truncated download runs nothing. The PowerShell installer restores the session's preferences and keeps `%VARIABLES%` in the user PATH.
- opfsolve: solver options with whole-number values for real-valued Ipopt options (`tol=1`) are accepted; safer MATPOWER table reading, CSV quoting, and error handling.
- Releases: `SHA256SUMS`, build provenance attestations, generated release notes, the corresponding source of the bundled GPL and LGPL libraries, and a check that the tag matches the version.
- CI: commit-pinned actions with Dependabot, read-only token by default, pinned Julia 1.12.7 with caches, installer tests from the released commit, and a job for re-install, spaces, and uninstall.

## v0.4.3

- Show Ipopt's own output.

## v0.4.2

- Conda-forge lock files for Ipopt and the compiler; HSL compiled in the background while Ipopt installs.

## v0.4.1

- Use an existing gfortran; clearer installer and solver output.

## v0.4.0

- The installer installs the `opfsolve` command only, juliaup-style, into `~/.opfsolve` with a PATH block.

## v0.3.0

- HSL MA27 required; the installer asks for the HSL download link.

## v0.2.0

- Ipopt from conda-forge; HSL compiled on the user's machine.
