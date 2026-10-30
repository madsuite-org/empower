"""The Python package against an install (EMPOWER_DIR): case118 in each form."""
import math
import sys

import empower

device = sys.argv[1] if len(sys.argv) > 1 else "cpu"
options = dict(o.split("=", 1) for o in sys.argv[2:])
bad = 0
assert empower.version() == empower.__version__, (empower.version(), empower.__version__)
for form, ref in (("polar", 97214), ("rect", 97214), ("dc", 93101)):
    r = empower.solve("case118_ieee", form=form, device=device, options=options, time_limit=600)
    print(r, r.bus["vm"][0], r.gen["pg"][0], r.branch["pf"][0])
    if not r.solved or abs(r.objective - ref) > 1e-4 * ref or len(r.bus["vm"]) != 118 \
            or len(r.gen["pg"]) != 54 or len(r.branch["pf"]) != 186 or not math.isfinite(r.bus["va"][5]):
        bad = 1
try:
    empower.solve("no_such_case_xyz", device=device, options=options)
    bad = 1
except empower.EmpowerError as e:
    print("unknown case:", e)
print("Python: FAILED" if bad else "Python: ok")
sys.exit(bad)
