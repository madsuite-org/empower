# Changelog

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
