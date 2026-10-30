/* empower: AC and DC optimal power flow with compiled ExaModelsPower models and Ipopt.
 *
 * libempower solves a case with the empower install it is part of and returns the
 * solution in memory. Each solve runs that install's empower executable in a process of
 * its own (about 0.1 s of overhead), on the cpu (Ipopt, with HSL MA27 or MUMPS) or on an
 * NVIDIA GPU (MadNLP with cuDSS): its Julia runtime and libraries never meet the calling
 * program's. Usable from C and C++.
 *
 *     empower *e = empower_open(NULL);
 *     if (!e) { fprintf(stderr, "%s\n", empower_error()); return 1; }
 *     empower_result *r = empower_solve(e, "case118_ieee", "polar", "cpu", "tol=1e-8", NULL);
 *     if (r) printf("%s %g\n", r->status, r->objective);
 *     empower_free_result(r);
 *     empower_close(e);
 *
 * Not thread-safe: one call at a time per process (empower_error() is shared).
 */
#ifndef EMPOWER_H
#define EMPOWER_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define EMPOWER_API __declspec(dllexport)
#else
#define EMPOWER_API __attribute__((visibility("default")))
#endif

typedef struct empower empower;

typedef struct {
    const char *case_name;    /* pglib_opf_case118_ieee */
    const char *form;         /* polar, rect or dc */
    const char *status;       /* LOCALLY_SOLVED, ALMOST_LOCALLY_SOLVED, LOCALLY_INFEASIBLE,
                                 ITERATION_LIMIT, TIME_LIMIT, ... */
    int iterations, nvar, ncon;
    double objective;
    double constraint_violation;   /* max(cl - c(x), c(x) - cu, 0) at the solution */
    double time_build, time_solve; /* seconds */
    double time_evals, time_init, time_linear_algebra;   /* parts of time_solve */
    double base_mva;

    /* The solution, in the order of the case file's tables: per unit and radians.
     * Out-of-service generators and branches have zeros. */
    int nbus;
    const double *bus_i;       /* bus numbers */
    const double *vm, *va;
    const double *lam_p, *lam_q;   /* power balance multipliers, as MATPOWER reports them */
    int ngen;
    const double *gen_bus, *pg, *qg;
    int nbranch;
    const double *f_bus, *t_bus;
    const double *pf, *qf, *pt, *qt;

    const char *dir;   /* the case's output directory (solution.m, solution.json, ipopt.log), or NULL */
} empower_result;

/* install: the empower directory, or NULL for the one this library is in. NULL on
 * failure; empower_error() says why. */
EMPOWER_API empower *empower_open(const char *install);

/* Solve a case: a MATPOWER file, or a PGLib-OPF case name (downloaded on first use).
 * form: "polar" (NULL), "rect" or "dc". device: "cpu" (NULL) or "cuda". options: solver
 * options, "NAME=VALUE ..." separated by spaces or commas (max_wall_time=60 for a time
 * limit), or NULL. output: a directory for the case's files (<output>/<case>/solution.m,
 * solution.json, the solver log), or NULL to keep none.
 * NULL if the case could not be solved at all (not found, not a valid case); a result
 * whose status is not LOCALLY_SOLVED otherwise. */
EMPOWER_API empower_result *empower_solve(empower *e, const char *casefile, const char *form,
                                          const char *device, const char *options, const char *output);

EMPOWER_API void empower_free_result(empower_result *r);
EMPOWER_API void empower_close(empower *e);

/* The last error, or "" */
EMPOWER_API const char *empower_error(void);
EMPOWER_API const char *empower_version(void);

#ifdef __cplusplus
}
#endif
#endif
