# empower for Python

Solves optimal power flow cases with an empower install (see the empower README) and
returns the solution as arrays. Each solve runs the `empower` command in its own process
(about 0.1 s of overhead), on the cpu (Ipopt) or on an NVIDIA GPU (`device="cuda"`).

```python
import empower
r = empower.solve("case118_ieee", form="polar", options={"tol": 1e-8}, time_limit=60)
print(r.status, r.objective)
r.bus["vm"], r.gen["pg"], r.branch["pf"]   # per unit and radians
```

The install is found from `EMPOWER_DIR`, or `~/.empower`.
