"""Optimal power flow with empower: compiled ExaModelsPower models, solved with Ipopt
(cpu) or MadNLP on an NVIDIA GPU (cuda), by the empower command of an install.

Each solve runs empower in its own process (about 0.1 s of overhead) and reads the
solution back: an empower process loads its own Julia runtime and libraries, which can
conflict with those already in a Python process (such as numpy's OpenSSL).

    import empower
    r = empower.solve("case118_ieee")
    r.status, r.objective, r.bus["vm"], r.gen["pg"]

Values are per unit and radians, in the order of the case file's tables.
"""

import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

__version__ = "0.10.1"

__all__ = ["solve", "Result", "EmpowerError", "install_dir", "version"]

try:
    import numpy as _np
except ImportError:  # lists instead of arrays
    _np = None


class EmpowerError(RuntimeError):
    pass


def install_dir():
    """The empower install: EMPOWER_DIR, or ~/.empower."""
    return os.environ.get("EMPOWER_DIR") or os.path.join(os.path.expanduser("~"), ".empower")


class Result:
    """A solved case.

    status: LOCALLY_SOLVED, ALMOST_LOCALLY_SOLVED, LOCALLY_INFEASIBLE, ITERATION_LIMIT,
    TIME_LIMIT, ...; objective; iterations; time_build, time_solve, and its parts
    time_evals, time_init, time_linear_algebra (seconds);
    constraint_violation; base_mva; bus, gen, branch: dicts of arrays (per unit, radians);
    dir: the case's output directory, if one was kept.
    """

    def __init__(self, **kw):
        self.__dict__.update(kw)

    @property
    def solved(self):
        return self.status in ("LOCALLY_SOLVED", "ALMOST_LOCALLY_SOLVED")

    def __repr__(self):
        return "<empower.Result %s %s %s objective=%.10g iterations=%d>" % (
            self.case, self.form, self.status, self.objective, self.iterations)


def _array(xs):
    xs = list(xs)
    return _np.array(xs, dtype=float) if _np is not None else xs


def _exe():
    exe = os.path.join(install_dir(), "bin", "empower.exe" if sys.platform == "win32" else "empower")
    if not os.path.exists(exe):
        raise EmpowerError("empower is not installed at %s (set EMPOWER_DIR)" % install_dir())
    return exe


def version():
    """The version of the empower install."""
    return subprocess.run([_exe(), "--version"], stdout=subprocess.PIPE, text=True,
                          check=True).stdout.split()[-1]


def _options(options, time_limit):
    opts = dict(options or {})
    if time_limit is not None:
        opts["max_wall_time"] = float(time_limit)
    for k, v in opts.items():
        if any(c in str(k) + str(v) for c in " ,\t="):
            raise ValueError("option %r=%r: names and values cannot hold spaces, commas or '='" % (k, v))
    return " ".join("%s=%s" % (k, repr(v) if isinstance(v, float) else v) for k, v in opts.items())


# --------------------------------------------------------------------- solve

def _read_matpower(path):
    """baseMVA and the bus, gen and branch tables (rows of numbers) of a MATPOWER file."""
    mc = {"baseMVA": 100.0, "bus": [], "gen": [], "branch": []}
    table = None
    with open(path) as f:
        for line in f:
            code = line.split("%", 1)[0]
            if table is None:
                m = re.match(r"\s*mpc\.(\w+)\s*=\s*(.*)", code)
                if not m:
                    continue
                if m.group(1) == "baseMVA":
                    mc["baseMVA"] = float(m.group(2).strip().rstrip(";"))
                    continue
                if m.group(1) not in ("bus", "gen", "branch") or "[" not in m.group(2):
                    continue
                table, code = m.group(1), m.group(2).split("[", 1)[1]
            end = "]" in code
            for row in code.split("]", 1)[0].split(";"):
                nums = row.replace(",", " ").split()
                if nums:
                    mc[table].append([float(x) for x in nums])
            if end:
                table = None
    return mc


def _run(case, form, device, options, output, start):
    exe = _exe()
    out = output or tempfile.mkdtemp(prefix="empower-")
    try:
        cmd = [exe, str(case), "--device=" + device, "--form=" + form, "--output=" + out, "--quiet"]
        if options:
            cmd.append("--options=" + options)
        if start:
            cmd.append("--start=" + os.fspath(start))
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        rows = []
        summary = os.path.join(out, "summary.csv")
        if os.path.exists(summary):
            import csv
            with open(summary) as f:
                rows = list(csv.DictReader(f))
        if not rows:
            msg = "\n".join(l for l in p.stderr.splitlines() if l.strip()) or p.stdout.strip()
            raise EmpowerError(msg.replace("error: ", "", 1))
        s = rows[0]
        d = os.path.join(out, s["case"])
        mc = _read_matpower(os.path.join(d, "solution.m"))
        bus, gen, br, b = mc["bus"], mc["gen"], mc["branch"], 1.0 / mc["baseMVA"]
        col = lambda t, c, k=1.0: _array((r[c - 1] if len(r) >= c else 0.0) * k for r in t)
        return Result(
            case=s["case"], form=s["form"], status=s["termination_status"], raw_status=None,
            objective=float(s["objective"]), iterations=int(s["iterations"]),
            nvar=int(s["nvar"]), ncon=int(s["ncon"]),
            constraint_violation=float(s["constraint_violation"]),
            time_build=float(s["time_parse_and_build"]), time_solve=float(s["time_solve"]),
            time_evals=float(s["time_solve_callbacks"]), time_init=float(s["time_solve_init"]),
            time_linear_algebra=float(s["time_solve_linear_algebra"] or "nan"),
            base_mva=mc["baseMVA"],
            bus={"i": col(bus, 1), "vm": col(bus, 8), "va": col(bus, 9, math.pi / 180),
                 "lam_p": col(bus, 14), "lam_q": col(bus, 15)},
            gen={"bus": col(gen, 1), "pg": col(gen, 2, b), "qg": col(gen, 3, b)},
            branch={"f_bus": col(br, 1), "t_bus": col(br, 2), "pf": col(br, 14, b),
                    "qf": col(br, 15, b), "pt": col(br, 16, b), "qt": col(br, 17, b)},
            dir=d if output else None,
        )
    finally:
        if not output:
            shutil.rmtree(out, ignore_errors=True)


def solve(case, form="polar", device="cpu", options=None, time_limit=None, output=None, start=None):
    """Solve the optimal power flow problem of a case.

    case: a MATPOWER file, or a PGLib-OPF case name ("case118_ieee"), downloaded on first
    use. form: "polar", "rect" or "dc". device: "cpu" (Ipopt with HSL MA27, or MUMPS) or
    "cuda" (MadNLP with cuDSS). options:
    solver options, {"tol": 1e-8, "max_iter": 500}. time_limit: seconds. output: a
    directory to keep the case's files in (solution.m, solution.json, the solver log).
    start: a solved case of the same network (such as a Result's dir + "/solution.m") whose
    bus voltages and generator dispatch the solve starts from.

    Every array is in the order of the case file's tables; out-of-service generators and
    branches have zeros.
    """
    if form not in ("polar", "rect", "dc"):
        raise ValueError("form must be polar, rect or dc, got %r" % form)
    opts = _options(options, time_limit)
    if output:
        os.makedirs(output, exist_ok=True)
    if device not in ("cpu", "cuda"):
        raise ValueError("device must be cpu or cuda, got %r" % device)
    return _run(case, form, device, opts, output, start)
