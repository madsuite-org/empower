/*
 * empower: solve AC optimal power flow cases with a compiled ExaModelsPower
 * model library (cnlp ABI v0.1) and any Ipopt shared library (>= 3.14, which has hsllib).
 *
 * Both libraries are loaded at run time, so the same driver works with the
 * conda-forge Ipopt, with or without the user's own HSL (hsllib), or a local one.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define MODEL_LIB "emp/bin/libemp.dll"
#define IPOPT_LIB "ipopt/Library/bin/ipopt-3.dll"
#define HSL_LIB "hsl/libhsl.dll"
#define NULL_DEVICE "NUL"
#define MKDIR(p) _mkdir(p)
#else
#include <dlfcn.h>
#include <spawn.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#define NULL_DEVICE "/dev/null"
#define MKDIR(p) mkdir(p, 0755)
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#define MODEL_LIB "emp/lib/libemp.dylib"
#define IPOPT_LIB "ipopt/lib/libipopt.dylib"
#define HSL_LIB "hsl/libhsl.dylib"
#else
#define MODEL_LIB "emp/lib/libemp.so"
#define IPOPT_LIB "ipopt/lib/libipopt.so"
#define HSL_LIB "hsl/libhsl.so"
#endif
extern char **environ;
#endif

#define VERSION "0.7.1"
#define MA27_URL "https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/"
#define PGLIB_VERSION "23.07"
#define PGLIB_RAW "https://raw.githubusercontent.com/power-grid-lib/pglib-opf/v" PGLIB_VERSION
#define PGLIB_TGZ "https://github.com/power-grid-lib/pglib-opf/archive/refs/tags/v" PGLIB_VERSION ".tar.gz"

/* ---------------------------------------------------------------- utilities */

static double now(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
#endif
}

/* Last path separator ('/' everywhere, also '\\' on Windows). */
static char *last_sep(const char *p) {
    char *a = strrchr(p, '/');
#ifdef _WIN32
    char *b = strrchr(p, '\\');
    if (!a || (b && b > a)) a = b;
#endif
    return a;
}

/* Shared-library loading. */
#ifdef _WIN32
static char dl_err[512];
static void *lib_open(const char *path) {
    char p[8192];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p; *c; c++)
        if (*c == '/') *c = '\\';
    /* Resolve the library's own DLL dependencies from its directory. */
    HMODULE h = LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h)
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                       GetLastError(), 0, dl_err, sizeof dl_err, NULL);
    return (void *)h;
}
static void *lib_sym(void *h, const char *name) { return (void *)GetProcAddress((HMODULE)h, name); }
static const char *lib_error(void) { return dl_err; }
#else
static void *lib_open(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *lib_sym(void *h, const char *name) { return dlsym(h, name); }
static const char *lib_error(void) { return dlerror(); }
#endif

static double t_start;

static void step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void step(const char *fmt, ...) {
    va_list ap;
    printf("  %8.3fs  ", now() - t_start);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(2);
}

/* A problem with one case: reported, and the batch moves on. */
static int case_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int case_error(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    return -1;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n ? n : 1, s);
    if (!p) die("out of memory");
    return p;
}

static void *sym(void *h, const char *prefix, const char *name) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s%s%s", prefix, *prefix ? "_" : "", name);
    void *p = lib_sym(h, buf);
    if (!p) die("symbol %s not found in a loaded library", buf);
    return p;
}

/* Run a program found on PATH and wait; returns its exit status, or -1. */
static int run(char *const args[]) {
#ifdef _WIN32
    /* _spawnvp re-joins argv with spaces, so quote each argument. */
    int n = 0;
    while (args[n]) n++;
    char **q = xmalloc((n + 1) * sizeof *q);
    for (int i = 0; i < n; i++) {
        size_t L = strlen(args[i]) + 3;
        q[i] = xmalloc(L);
        snprintf(q[i], L, "\"%s\"", args[i]);
    }
    q[n] = NULL;
    intptr_t rc = _spawnvp(_P_WAIT, args[0], (const char *const *)q);
    for (int i = 0; i < n; i++) free(q[i]);
    free(q);
    return (int)rc;
#else
    pid_t pid;
    int st;
    if (posix_spawnp(&pid, args[0], NULL, NULL, args, environ) != 0) return -1;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

/* Download url to path with curl. Returns 0 on success. */
static int download(const char *url, const char *path) {
    /* -s without -S: a failed download gets empower's own message, not curl's. */
    char *c[] = {"curl", "-fsL", "--proto", "=https", "--tlsv1.2", "-o", (char *)path, (char *)url, NULL};
    int rc = run(c);
    if (rc == -1) die("curl is needed to download %s", url);
    return rc;
}

/* Release layout: <root>/bin/empower and the model library MODEL_LIB; placed by
 * the installer: Ipopt (conda-forge) at IPOPT_LIB, the user's own compiled HSL at
 * HSL_LIB, and PGLib-OPF cases in <root>/cases/. */
static char root[4096];

static void find_root(void) {
    char exe[4096], real[4096];
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (k == 0 || k >= sizeof exe) return;
    if (!_fullpath(real, exe, sizeof real)) return;
#else
#ifdef __APPLE__
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0) return;
#else
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return;
    exe[k] = 0;
#endif
    if (!realpath(exe, real)) return;
#endif
    for (int up = 0; up < 2; up++) {
        char *slash = last_sep(real);
        if (!slash) return;
        *slash = 0;
    }
    snprintf(root, sizeof root, "%s", real);
}

/* <root>/<rel> if it exists, as a new string. */
static const char *under_root(const char *rel) {
    char b[8192];
    if (!*root) return NULL;
    snprintf(b, sizeof b, "%s/%s", root, rel);
    return access(b, R_OK) == 0 ? strdup(b) : NULL;
}

static const char *case_dir(void) {
    static char dir[8192];
    const char *env = getenv("EMPOWER_CASES");
    if (env) snprintf(dir, sizeof dir, "%s", env);
    else if (*root) snprintf(dir, sizeof dir, "%s/cases", root);
    else snprintf(dir, sizeof dir, "cases");
    MKDIR(dir);
    return dir;
}

/* ------------------------------------------------------------- cnlp model */

typedef struct {
    int32_t (*new_str)(const char *);
    int32_t (*nvar)(int32_t), (*ncon)(int32_t), (*nnzj)(int32_t), (*nnzh)(int32_t);
    int32_t (*meta)(int32_t, double *, double *, double *, double *, double *);
    int32_t (*obj)(int32_t, const double *, double *);
    int32_t (*grad)(int32_t, const double *, double *);
    int32_t (*cons)(int32_t, const double *, double *);
    int32_t (*jac_structure)(int32_t, int32_t *, int32_t *);
    int32_t (*jac)(int32_t, const double *, double *);
    int32_t (*hess_structure)(int32_t, int32_t *, int32_t *);
    int32_t (*hess)(int32_t, const double *, const double *, double, double *);
    /* ExaModelsPower's MATPOWER writer, compiled into the library. */
    int32_t (*write_matpower)(int32_t, const char *, const double *, const double *,
                              const double *, const double *, double, const char *);
    int32_t id;
    long nfev, ngev, nconsev, njev, nhev, iters;
    double tfev, tgev, tconsev, tjev, thev;
} Model;

static void load_model(Model *M, void *h, const char *p) {
    M->new_str = sym(h, p, "new_str");
    M->nvar = sym(h, p, "nvar");
    M->ncon = sym(h, p, "ncon");
    M->nnzj = sym(h, p, "nnzj");
    M->nnzh = sym(h, p, "nnzh");
    M->meta = sym(h, p, "meta");
    M->obj = sym(h, p, "obj");
    M->grad = sym(h, p, "grad");
    M->cons = sym(h, p, "cons");
    M->jac_structure = sym(h, p, "jac_structure");
    M->jac = sym(h, p, "jac");
    M->hess_structure = sym(h, p, "hess_structure");
    M->hess = sym(h, p, "hess");
    M->write_matpower = sym(h, p, "write_matpower");
}

/* --------------------------------------------------------------- Ipopt API */

typedef int Bool;
typedef Bool (*Eval_F)(int, double *, Bool, double *, void *);
typedef Bool (*Eval_Grad)(int, double *, Bool, double *, void *);
typedef Bool (*Eval_G)(int, double *, Bool, int, double *, void *);
typedef Bool (*Eval_Jac)(int, double *, Bool, int, int, int *, int *, double *, void *);
typedef Bool (*Eval_H)(int, double *, Bool, double, int, double *, Bool, int, int *, int *,
                       double *, void *);
typedef Bool (*Intermediate)(int, int, double, double, double, double, double, double, double,
                             double, int, void *);

typedef struct {
    void *(*create)(int, double *, double *, int, double *, double *, int, int, int, Eval_F,
                    Eval_G, Eval_Grad, Eval_Jac, Eval_H);
    void (*free)(void *);
    int (*solve)(void *, double *, double *, double *, double *, double *, double *, void *);
    Bool (*str_opt)(void *, const char *, const char *);
    Bool (*num_opt)(void *, const char *, double);
    Bool (*int_opt)(void *, const char *, int);
    Bool (*out_file)(void *, const char *, int);
    Bool (*intermediate)(void *, Intermediate);
} Ipopt;

#define TIMED(M, cnt, acc, call)            \
    do {                                    \
        double t0_ = now();                 \
        int32_t rc_ = (call);               \
        (M)->acc += now() - t0_;            \
        (M)->cnt++;                         \
        return rc_ == 0;                    \
    } while (0)

static Bool eval_f(int n, double *x, Bool nx, double *f, void *u) {
    (void)n; (void)nx;
    Model *M = u;
    TIMED(M, nfev, tfev, M->obj(M->id, x, f));
}
static Bool eval_grad(int n, double *x, Bool nx, double *g, void *u) {
    (void)n; (void)nx;
    Model *M = u;
    TIMED(M, ngev, tgev, M->grad(M->id, x, g));
}
static Bool eval_g(int n, double *x, Bool nx, int m, double *g, void *u) {
    (void)n; (void)nx; (void)m;
    Model *M = u;
    TIMED(M, nconsev, tconsev, M->cons(M->id, x, g));
}
static Bool eval_jac(int n, double *x, Bool nx, int m, int nnz, int *ir, int *jc, double *v,
                     void *u) {
    (void)n; (void)nx; (void)m; (void)nnz;
    Model *M = u;
    if (!v) return M->jac_structure(M->id, ir, jc) == 0;
    TIMED(M, njev, tjev, M->jac(M->id, x, v));
}
static Bool eval_h(int n, double *x, Bool nx, double sigma, int m, double *y, Bool ny, int nnz,
                   int *ir, int *jc, double *v, void *u) {
    (void)n; (void)nx; (void)m; (void)ny; (void)nnz;
    Model *M = u;
    /* cnlp: 1-based, lower triangle, which is what Ipopt's Fortran style wants. */
    if (!v) return M->hess_structure(M->id, ir, jc) == 0;
    TIMED(M, nhev, thev, M->hess(M->id, x, y, sigma, v));
}
static Bool on_iter(int alg, int iter, double f, double pr, double du, double mu, double d,
                    double reg, double adu, double apr, int ls, void *u) {
    (void)alg; (void)f; (void)pr; (void)du; (void)mu; (void)d; (void)reg; (void)adu;
    (void)apr; (void)ls;
    ((Model *)u)->iters = iter;
    return 1;
}

static const char *status_name(int s) {
    switch (s) {
    case 0: return "Solve_Succeeded";
    case 1: return "Solved_To_Acceptable_Level";
    case 2: return "Infeasible_Problem_Detected";
    case 3: return "Search_Direction_Becomes_Too_Small";
    case 4: return "Diverging_Iterates";
    case 5: return "User_Requested_Stop";
    case 6: return "Feasible_Point_Found";
    case -1: return "Maximum_Iterations_Exceeded";
    case -2: return "Restoration_Failed";
    case -3: return "Error_In_Step_Computation";
    case -4: return "Maximum_CpuTime_Exceeded";
    case -5: return "Maximum_WallTime_Exceeded";
    case -10: return "Not_Enough_Degrees_Of_Freedom";
    case -11: return "Invalid_Problem_Definition";
    case -12: return "Invalid_Option";
    case -13: return "Invalid_Number_Detected";
    case -100: return "Unrecoverable_Exception";
    case -101: return "NonIpopt_Exception_Thrown";
    case -102: return "Insufficient_Memory";
    default: return "Internal_Error";
    }
}

/* The MathOptInterface status names PowerModels reports, as Ipopt.jl maps them. */
static const char *termination_status(int s) {
    switch (s) {
    case 0: return "LOCALLY_SOLVED";
    case 1: return "ALMOST_LOCALLY_SOLVED";
    case 2: return "LOCALLY_INFEASIBLE";
    case 3: case 6: return "SLOW_PROGRESS";
    case 4: return "NORM_LIMIT";
    case 5: return "INTERRUPTED";
    case -1: return "ITERATION_LIMIT";
    case -4: case -5: return "TIME_LIMIT";
    case -2: case -3: case -13: return "NUMERICAL_ERROR";
    case -10: case -11: return "INVALID_MODEL";
    case -12: return "INVALID_OPTION";
    case -102: return "MEMORY_LIMIT";
    default: return "OTHER_ERROR";
    }
}

/* ---------------------------------------------------------------- options */

#define MAXOPTS 512
static char *opts[MAXOPTS];
static int nopts = 0;

static void add_opt(const char *name, const char *value) {
    if (nopts == MAXOPTS) die("too many solver options");
    size_t n = strlen(name) + strlen(value) + 2;
    opts[nopts] = xmalloc(n);
    snprintf(opts[nopts++], n, "%s=%s", name, value);
}

/* "tol=1e-8 max_iter=500", "tol=1e-8,max_iter=500", or "@file". */
static void parse_options(const char *spec) {
    if (spec[0] == '@') {
        FILE *f = fopen(spec + 1, "r");
        if (!f) die("cannot read options file %s", spec + 1);
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char *c = strchr(line, '#');
            if (c) *c = 0;
            for (char *q = line; *q; q++)
                if (*q == '=') *q = ' ';
            char name[512], value[512];
            if (sscanf(line, "%511s %511s", name, value) == 2) add_opt(name, value);
        }
        fclose(f);
        return;
    }
    char *s = strdup(spec);
    for (char *tok = strtok(s, " ,\t"); tok; tok = strtok(NULL, " ,\t")) {
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok) die("solver option must be NAME=VALUE, got \"%s\"", tok);
        *eq = 0;
        add_opt(tok, eq + 1);
    }
    free(s);
}

/* ------------------------------------------------------------------ cases */

typedef struct {
    char **v;
    int n, cap;
} List;

static void push(List *l, const char *s) {
    if (l->n == l->cap) l->v = xrealloc(l->v, (l->cap = l->cap ? 2 * l->cap : 16) * sizeof(char *));
    l->v[l->n++] = strdup(s);
}

/* PGLib-OPF names carry the bus count (pglib_opf_case2000_goc): smallest first. */
static long bus_count(const char *path) {
    const char *p = strstr(path, "pglib_opf_case");
    return p ? strtol(p + 14, NULL, 10) : 0;
}

static int cmpcase(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    long nx = bus_count(x), ny = bus_count(y);
    return nx != ny ? (nx < ny ? -1 : 1) : strcmp(x, y);
}

/* "all": every case at the top level of PGLib-OPF (typical operating conditions). */
static void add_all(List *cases) {
    const char *cd = case_dir();
    char dir[8300];
    snprintf(dir, sizeof dir, "%s/pglib-opf-" PGLIB_VERSION, cd);
    char marker[8400];
    snprintf(marker, sizeof marker, "%s/BASELINE.md", dir);
    if (access(marker, R_OK) != 0) {
        step("downloading the PGLib-OPF library: %s", PGLIB_TGZ);
        char tgz[8400];
        snprintf(tgz, sizeof tgz, "%s/pglib-opf-" PGLIB_VERSION ".tar.gz", cd);
        if (download(PGLIB_TGZ, tgz) != 0) die("could not download PGLib-OPF");
        char *untar[] = {"tar", "-xzf", tgz, "-C", (char *)cd, NULL};
        if (run(untar) != 0) die("could not unpack %s", tgz);
        unlink(tgz);
        step("saved %s", dir);
    } else {
        step("using cached PGLib-OPF library %s", dir);
    }
    DIR *d = opendir(dir);
    if (!d) die("cannot read %s", dir);
    struct dirent *e;
    List found = {0};
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (!strncmp(e->d_name, "pglib_opf_", 10) && L > 2 && !strcmp(e->d_name + L - 2, ".m")) {
            char p[8600];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            push(&found, p);
        }
    }
    closedir(d);
    qsort(found.v, found.n, sizeof(char *), cmpcase);
    step("%d PGLib-OPF cases selected", found.n);
    for (int i = 0; i < found.n; i++) push(cases, found.v[i]);
}

/* A path that exists is used as is; otherwise a PGLib-OPF case name. */
static int resolve_case(const char *c, char *path, size_t cap) {
    if (access(c, R_OK) == 0) {
        snprintf(path, cap, "%s", c);
        return 0;
    }
    if (last_sep(c)) return case_error("case file not found: %s", c);
    char name[1024];
    snprintf(name, sizeof name, "%s%s", strncmp(c, "pglib_opf_", 10) ? "pglib_opf_" : "", c);
    size_t L = strlen(name);
    if (L > 2 && !strcmp(name + L - 2, ".m")) name[L - 2] = 0;
    const char *cd = case_dir();
    /* One directory per PGLib-OPF release, shared with "all". */
    snprintf(path, cap, "%s/pglib-opf-" PGLIB_VERSION, cd);
    MKDIR(path);
    snprintf(path, cap, "%s/pglib-opf-" PGLIB_VERSION "/%s.m", cd, name);
    if (access(path, R_OK) == 0) {
        step("using cached case %s", path);
        return 0;
    }
    char url[2048], tmp[8300];
    snprintf(url, sizeof url, "%s/%s.m", PGLIB_RAW, name);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    step("downloading %s", url);
    if (download(url, tmp) != 0) {
        unlink(tmp);
        return case_error("%s is not a file, and no PGLib-OPF case %s could be downloaded", c, name);
    }
    rename(tmp, path);
    step("saved %s", path);
    return 0;
}

static void case_name(const char *path, char *out, size_t cap) {
    const char *b = last_sep(path);
    snprintf(out, cap, "%s", b ? b + 1 : path);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

/* ----------------------------------------------------------------- solving */

typedef struct {
    const char *form, *model, *linear_solver, *hsllib;
    Ipopt I;
    void *hm;
    FILE *summary;
} Ctx;

typedef struct {
    int status, iters, n, m;
    double obj, t_build, t_solve, t_cb, t_total;
} Result;

/* A CSV field, quoted when it holds a comma, quote, or newline. */
static void csv_field(FILE *f, const char *v) {
    if (!strpbrk(v, ",\"\r\n")) { fputs(v, f); return; }
    fputc('"', f);
    for (; *v; v++) {
        if (*v == '"') fputc('"', f);
        fputc(*v, f);
    }
    fputc('"', f);
}

static int solve_case(Ctx *C, const char *casefile, const char *outroot, Result *r) {
    double t_case = now();
    char name[1024], out[8192], path[8400];
    case_name(casefile, name, sizeof name);
    snprintf(out, sizeof out, "%s/%s", outroot, name);
    MKDIR(out);

    Model M = {0};
    load_model(&M, C->hm, C->model);

    step("parsing the case and building the %s-form model", C->form);
    double t0 = now();
    M.id = M.new_str(casefile);
    r->t_build = now() - t0;
    if (M.id <= 0) return case_error("the model library could not build a model from %s", casefile);
    int n = r->n = M.nvar(M.id), m = r->m = M.ncon(M.id);
    int nnzj = M.nnzj(M.id), nnzh = M.nnzh(M.id);
    step("model: %d variables, %d constraints, %d + %d Jacobian/Hessian nonzeros (%.3f s)", n,
         m, nnzj, nnzh, r->t_build);

    double *x = xmalloc(n * sizeof *x), *xl = xmalloc(n * sizeof *x), *xu = xmalloc(n * sizeof *x);
    double *gl = xmalloc(m * sizeof *x), *gu = xmalloc(m * sizeof *x), *g = xmalloc(m * sizeof *x);
    double *y = xcalloc(m, sizeof *x), *zl = xcalloc(n, sizeof *x), *zu = xcalloc(n, sizeof *x);
    int rc = 1;
    void *P = NULL;
    Ipopt *I = &C->I;
    if (M.meta(M.id, x, xl, xu, gl, gu) != 0) { rc = case_error("could not read model bounds"); goto done; }
    for (int i = 0; i < n; i++) {
        if (xl[i] < -1e20) xl[i] = -1e20;
        if (xu[i] > 1e20) xu[i] = 1e20;
    }
    for (int i = 0; i < m; i++) {
        if (gl[i] < -1e20) gl[i] = -1e20;
        if (gu[i] > 1e20) gu[i] = 1e20;
    }

    P = I->create(n, xl, xu, m, gl, gu, nnzj, nnzh, 1, eval_f, eval_g, eval_grad, eval_jac, eval_h);
    if (!P) { rc = case_error("CreateIpoptProblem failed"); goto done; }
    if (I->intermediate) I->intermediate(P, on_iter);
    snprintf(path, sizeof path, "%s/ipopt.log", out);
    I->out_file(P, path, 5);
    if (C->hsllib) I->str_opt(P, "hsllib", C->hsllib);
    I->str_opt(P, "linear_solver", C->linear_solver);
    for (int k = 0; k < nopts; k++) {
        char key[512];
        const char *eq = strchr(opts[k], '=');
        snprintf(key, sizeof key, "%.*s", (int)(eq - opts[k]), opts[k]);
        const char *v = eq + 1;
        char *end;
        long iv = strtol(v, &end, 10);
        /* Ipopt checks the option's type: try integer, then real, then string,
         * so that tol=1 or max_wall_time=3600 work for real-valued options. */
        int is_int = *v && !*end;
        double dv = strtod(v, &end);
        int is_num = *v && !*end;
        Bool ok = (is_int && I->int_opt(P, key, (int)iv)) || (is_num && I->num_opt(P, key, dv)) ||
                  I->str_opt(P, key, v);
        if (!ok) die("Ipopt rejected option %s", opts[k]);
    }

    step("solving with Ipopt, %s", C->linear_solver);
    printf("\n");
    fflush(stdout);
    t0 = now();
    r->status = I->solve(P, x, g, &r->obj, y, zl, zu, &M);
    r->t_solve = now() - t0;
    fflush(stdout);
    printf("\n");
    r->iters = M.iters;
    r->t_cb = M.tfev + M.tgev + M.tconsev + M.tjev + M.thev;
    step("%s: objective %.10g, %d iterations, %.3f s (%.3f s in model callbacks)",
         termination_status(r->status), r->obj, r->iters, r->t_solve, r->t_cb);

    snprintf(path, sizeof path, "%s/solution.m", out);
    if (M.write_matpower(M.id, path, x, y, zl, zu, r->obj, termination_status(r->status)) != 0)
        case_error("could not write %s", path);
    else
        step("wrote %s/{solution.m, ipopt.log}", out);
    r->t_total = now() - t_case;

    if (C->summary) {
        csv_field(C->summary, name);
        fprintf(C->summary, ",%s,%s,%s,%.17g,%d,%d,%d,%.6f,%.6f,%.6f,%.6f\n", C->form,
                termination_status(r->status), status_name(r->status), r->obj, r->iters, r->n,
                r->m, r->t_build, r->t_solve, r->t_cb, r->t_solve - r->t_cb);
        fflush(C->summary);
    }
    rc = (r->status == 0 || r->status == 1) ? 0 : 1;

done:
    if (P) I->free(P);
    free(x); free(xl); free(xu); free(gl); free(gu); free(g); free(y); free(zl); free(zu);
    return rc;
}

/* min (x - 1)^2: one variable, no constraints. */
static Bool p_f(int n, double *x, Bool nx, double *f, void *u) {
    (void)n; (void)nx; (void)u;
    *f = (x[0] - 1) * (x[0] - 1);
    return 1;
}
static Bool p_grad(int n, double *x, Bool nx, double *g, void *u) {
    (void)n; (void)nx; (void)u;
    g[0] = 2 * (x[0] - 1);
    return 1;
}
static Bool p_g(int n, double *x, Bool nx, int m, double *g, void *u) {
    (void)n; (void)x; (void)nx; (void)m; (void)g; (void)u;
    return 1;
}
static Bool p_jac(int n, double *x, Bool nx, int m, int nnz, int *ir, int *jc, double *v, void *u) {
    (void)n; (void)x; (void)nx; (void)m; (void)nnz; (void)ir; (void)jc; (void)v; (void)u;
    return 1;
}
static Bool p_h(int n, double *x, Bool nx, double s, int m, double *y, Bool ny, int nnz, int *ir,
                int *jc, double *v, void *u) {
    (void)n; (void)x; (void)nx; (void)m; (void)y; (void)ny; (void)nnz; (void)u;
    if (!v) { ir[0] = 1; jc[0] = 1; }
    else v[0] = 2 * s;
    return 1;
}

static int probe_ma27(Ipopt *I, const char *hsllib) {
    double xl = -1e20, xu = 1e20, x = 0, obj = 0, zl = 0, zu = 0;
    void *P = I->create(1, &xl, &xu, 0, NULL, NULL, 0, 1, 1, p_f, p_g, p_grad, p_jac, p_h);
    if (!P) return 0;
    I->int_opt(P, "print_level", 0);
    I->str_opt(P, "sb", "yes");
    if (hsllib) I->str_opt(P, "hsllib", hsllib);
    I->str_opt(P, "linear_solver", "ma27");
    /* Ipopt reports a missing solver on stdout even at print_level 0. */
    fflush(stdout);
    int saved = dup(1), devnull = saved >= 0 ? open(NULL_DEVICE, O_WRONLY) : -1;
    if (devnull >= 0) dup2(devnull, 1);
    int status = I->solve(P, &x, NULL, &obj, NULL, &zl, &zu, NULL);
    fflush(stdout);
    if (devnull >= 0) { dup2(saved, 1); close(devnull); }
    if (saved >= 0) close(saved);
    I->free(P);
    return status == 0;
}

/* The SOURCE note the installer leaves next to what it installed, or "". */
static const char *read_source(const char *kind, char *buf, size_t cap) {
    char path[8192];
    *buf = 0;
    if (!*root) return NULL;
    snprintf(path, sizeof path, "%s/%s/SOURCE", root, kind);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (!fgets(buf, (int)cap, f)) *buf = 0;
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return *buf ? buf : NULL;
}

/* Record the machine and the libraries of a run in <output>/system.txt. */
static void write_system(const char *out, const char *ipoptlib, const char *hsllib, int argc,
                         char **argv) {
    char path[8192], buf[512] = "";
    snprintf(path, sizeof path, "%s/system.txt", out);
    FILE *f = fopen(path, "w");
    if (!f) return;
    time_t t = time(NULL);
    char date[64];
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
    fprintf(f, "date: %s\nempower: %s\n", date, VERSION);
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const char *cpu = getenv("PROCESSOR_IDENTIFIER");
    fprintf(f, "os: Windows\ncpu: %s\nlogical cpus: %lu\n", cpu ? cpu : "unknown",
            (unsigned long)si.dwNumberOfProcessors);
#else
    struct utsname u;
    if (uname(&u) == 0) fprintf(f, "os: %s %s %s\n", u.sysname, u.release, u.machine);
#ifdef __APPLE__
    size_t L = sizeof buf;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &L, NULL, 0) != 0) snprintf(buf, sizeof buf, "unknown");
#else
    FILE *c = fopen("/proc/cpuinfo", "r");
    char line[512];
    while (c && fgets(line, sizeof line, c)) {
        if (!strncmp(line, "model name", 10) || !strncmp(line, "Model name", 10)) {
            char *v = strchr(line, ':');
            if (v) snprintf(buf, sizeof buf, "%s", v + 2);
            buf[strcspn(buf, "\n")] = 0;
            break;
        }
    }
    if (c) fclose(c);
#endif
    fprintf(f, "cpu: %s\nlogical cpus: %ld\n", *buf ? buf : "unknown", sysconf(_SC_NPROCESSORS_ONLN));
#endif
    /* The installer leaves a SOURCE file next to what it installed. */
    const char *libs[2][2] = {{"ipopt", ipoptlib}, {"hsl", hsllib}};
    for (int k = 0; k < 2; k++) {
        fprintf(f, "%s library: %s\n", libs[k][0], libs[k][1] ? libs[k][1] : "(none)");
        if (libs[k][1] && read_source(libs[k][0], buf, sizeof buf))
            fprintf(f, "%s: %s\n", libs[k][0], buf);
    }
    fprintf(f, "command:");
    for (int i = 0; i < argc; i++) fprintf(f, " %s", argv[i]);
    fprintf(f, "\n");
    fclose(f);
}

/* ------------------------------------------------------------------- cuda */

/* --device=cuda: resolve the cases here, then solve them all in one run of empower-cuda,
 * which writes the same files. */
static int run_cuda(List *args, const char *form, const char *out, int argc, char **argv) {
#if defined(_WIN32) || defined(__APPLE__)
    (void)args; (void)form; (void)out; (void)argc; (void)argv;
    die("--device=cuda is available on Linux x86_64 only");
#else
    const char *exe = under_root("cuda/bin/empower-cuda");
    if (!exe || access(exe, X_OK) != 0)
        die("the cuda backend is not installed: rerun the installer with --backends=cpu,cuda");
    /* The bundle's own depot, then the NVIDIA libraries the installer downloaded. */
    char depot[16400];
    snprintf(depot, sizeof depot, "%s/cuda/share/julia:%s/cuda-depot", root, root);
    printf("empower %s, %s form, MadNLP with cuDSS on the GPU\n", VERSION, form);
    printf("  solver   %s\n", exe);
    printf("  output   %s\n", out);
    for (int k = 0; k < nopts; k++) printf("  option   %s\n", opts[k]);
    List cases = {0};
    for (int i = 0; i < args->n; i++) {
        if (!strcmp(args->v[i], "all")) add_all(&cases);
        else push(&cases, args->v[i]);
    }
    List files = {0};
    int failed = 0;
    for (int i = 0; i < cases.n; i++) {
        char cf[8192];
        if (resolve_case(cases.v[i], cf, sizeof cf) == 0) push(&files, cf);
        else failed++;
    }
    MKDIR(out);
    write_system(out, NULL, NULL, argc, argv);
    if (!files.n) die("no case to solve");
    char formarg[64], outarg[8300];
    snprintf(formarg, sizeof formarg, "--form=%s", form);
    snprintf(outarg, sizeof outarg, "--output=%s", out);
    char **cmd = xcalloc(4 + 2 * nopts + files.n, sizeof *cmd);
    int n = 0;
    cmd[n++] = (char *)exe;
    cmd[n++] = formarg;
    cmd[n++] = outarg;
    for (int k = 0; k < nopts; k++) { cmd[n++] = "--option"; cmd[n++] = opts[k]; }
    for (int i = 0; i < files.n; i++) cmd[n++] = files.v[i];
    cmd[n] = NULL;
    /* The bundle finds its Julia libraries and artifacts in its own depot. */
    setenv("JULIA_DEPOT_PATH", depot, 1);
    /* As for the cpu model library: ExaModelsPower's __init__ otherwise creates a data
     * directory next to its source as it was on the build machine. */
    if (!getenv("EXA_MODELS_DEPOT")) setenv("EXA_MODELS_DEPOT", case_dir(), 1);
    unsetenv("JULIA_PROJECT");
    unsetenv("JULIA_LOAD_PATH");
    fflush(stdout);
    int rc = run(cmd);
    if (rc < 0) die("could not run %s", exe);
    return rc || failed ? 1 : 0;
#endif
}

/* --------------------------------------------------------------------- up */

/* empower up: run the installer again, into this install directory, for the backends
 * installed here, keeping the HSL library and leaving the shell startup files alone. */
static int self_update(void) {
    if (!*root) die("cannot find the empower install directory");
    int cpu = under_root(IPOPT_LIB) != NULL;
    int cuda = under_root("cuda/bin/empower-cuda") != NULL;
    int hsl = under_root(HSL_LIB) != NULL;
    if (!cpu && !cuda) cpu = 1;
    const char *backends = cpu && cuda ? "cpu,cuda" : cuda ? "cuda" : "cpu";
    printf("updating empower %s in %s (backends: %s)\n", VERSION, root, backends);
    fflush(stdout);
#ifdef _WIN32
    _putenv_s("EMPOWER_DIR", root);
    _putenv_s("EMPOWER_YES", "1");
    _putenv_s("EMPOWER_ADD_TO_PATH", "no");
    if (cpu && !hsl) _putenv_s("EMPOWER_SKIP_HSL", "1");
    char *cmd[] = {"powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                   "irm https://raw.githubusercontent.com/madsuite-org/empower/main/install.ps1 | iex", NULL};
#else
    if (strchr(root, '\'')) die("cannot update an install directory whose path has a single quote");
    char script[16400];
    snprintf(script, sizeof script,
             "curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/madsuite-org/empower/main/install.sh"
             " | sh -s -- -y --path '%s' --add-to-path=no --backends=%s%s",
             root, backends, cpu && !hsl ? " --no-hsl" : "");
    char *cmd[] = {"sh", "-c", script, NULL};
#endif
    int rc = run(cmd);
    if (rc != 0) die("the update failed");
    return 0;
}

/* ------------------------------------------------------------------- main */

static const char *usage =
    "usage: empower up           update empower to the latest release (same backends)\n"
    "       empower <case>... [--form=polar|rect|dc] [--device=cpu|cuda] [--solver=ipopt] [--options=\"NAME=VALUE ...\"]\n"
    "                [--output=DIR] [--hsl-lib=PATH] [--ipopt-lib=PATH] [--model-lib=PATH]\n"
    "\n"
    "Solves the AC (or DC) optimal power flow problem for one or more MATPOWER cases.\n"
    "\n"
    "  <case>       a MATPOWER file, or a PGLib-OPF case name (case118_ieee,\n"
    "               pglib_opf_case118_ieee.m), downloaded on first use; \"all\" solves\n"
    "               every PGLib-OPF v" PGLIB_VERSION " case for typical operating conditions\n"
    "  --form       formulation: polar (default) or rect (AC), or dc\n"
    "  --device     cpu (Ipopt with HSL MA27) or cuda (MadNLP with cuDSS on an NVIDIA\n"
    "               GPU; Linux x86_64); the installer's --backends selects which are\n"
    "               installed. Default: cpu if installed, else cuda\n"
    "  --solver     ipopt on cpu, madnlp on cuda (the defaults, and the only ones)\n"
    "  --options    solver options separated by spaces or commas,\n"
    "               --options=\"tol=1e-8 max_iter=500\"; --options=@FILE reads one\n"
    "               \"NAME VALUE\" per line; may be repeated\n"
    "  --output     output directory (default ./empower-results)\n"
    "  --hsl-lib    HSL library providing MA27 (and MA57, MA86, MA97 if it has them),\n"
    "               passed to Ipopt's hsllib option; default <install>/hsl, or\n"
    "               $EMPOWER_HSL_LIB. MA27 is the linear solver unless --options sets\n"
    "               linear_solver\n"
    "  --ipopt-lib  Ipopt shared library, 3.14 or newer (default <install>/ipopt, or\n"
    "               $EMPOWER_IPOPT_LIB)\n"
    "  --model-lib  compiled ExaModelsPower library (default <install>/emp, from the release)\n"
    "\n"
    "For each case, writes <output>/<case>/solution.m (the solved case, in MATPOWER's\n"
    "savecase format) and <output>/<case>/ipopt.log, and a row of <output>/summary.csv\n"
    "(rewritten each run);\n"
    "<output>/system.txt records the machine, the libraries, and the command.\n";

/* Accept both --name=value and --name value. */
static const char *flag(const char *a, const char *name, int *i, int argc, char **argv) {
    size_t n = strlen(name);
    if (strncmp(a, name, n)) return NULL;
    if (a[n] == '=') return a + n + 1;
    if (a[n] == 0) {
        if (*i + 1 >= argc) die("%s needs a value", name);
        return argv[++*i];
    }
    return NULL;
}

int main(int argc, char **argv) {
    t_start = now();
    find_root();
    if (argc == 2 && (!strcmp(argv[1], "up") || !strcmp(argv[1], "update"))) return self_update();
    const char *modellib = getenv("EMPOWER_MODEL_LIB"), *ipoptlib = getenv("EMPOWER_IPOPT_LIB"),
               *hsllib = getenv("EMPOWER_HSL_LIB"),
               *form = "polar", *solver = NULL, *device = NULL, *out = "empower-results", *v;
    List args = {0};
    int check_hsl = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(usage, stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            puts("empower " VERSION);
            return 0;
        }
        if (!strcmp(a, "--check-hsl")) { check_hsl = 1; continue; }
        if ((v = flag(a, "--form", &i, argc, argv))) form = v;
        else if ((v = flag(a, "--device", &i, argc, argv))) device = v;
        else if ((v = flag(a, "--solver", &i, argc, argv))) solver = v;
        else if ((v = flag(a, "--options", &i, argc, argv))) parse_options(v);
        else if ((v = flag(a, "--output", &i, argc, argv))) out = v;
        else if ((v = flag(a, "--ipopt-lib", &i, argc, argv))) ipoptlib = v;
        else if ((v = flag(a, "--hsl-lib", &i, argc, argv))) hsllib = v;
        else if ((v = flag(a, "--model-lib", &i, argc, argv))) modellib = v;
        else if (a[0] == '-') { fputs(usage, stderr); die("unknown option %s", a); }
        else push(&args, a);
    }
    if (!args.n && !check_hsl) { fputs(usage, stderr); die("no case given"); }
    Ctx C = {0};
    C.form = form;
    if (!strcmp(form, "polar")) C.model = "acp";
    else if (!strcmp(form, "rect")) C.model = "acr";
    else if (!strcmp(form, "dc")) C.model = "dcp";
    else die("--form must be polar, rect or dc, got %s", form);
    /* The default device: cpu when its backend (Ipopt) is installed, else cuda. */
    if (!device) {
        const char *cudaexe = under_root("cuda/bin/empower-cuda");
        device = (!ipoptlib && !under_root(IPOPT_LIB) && cudaexe && access(cudaexe, X_OK) == 0) ? "cuda" : "cpu";
    }
    if (!strcmp(device, "amdgpu")) die("the amdgpu backend is not available in this release");
    int cuda = !strcmp(device, "cuda");
    if (!cuda && strcmp(device, "cpu")) die("--device must be cpu or cuda, got %s", device);
    if (!solver) solver = cuda ? "madnlp" : "ipopt";
    if (strcmp(solver, cuda ? "madnlp" : "ipopt"))
        die("--solver must be %s with --device=%s, got %s", cuda ? "madnlp" : "ipopt", device, solver);
    if (cuda) return run_cuda(&args, form, out, argc, argv);
    if (!modellib) modellib = under_root(MODEL_LIB);
    if (!modellib) die("no model library: pass --model-lib or set EMPOWER_MODEL_LIB");
    if (!ipoptlib) ipoptlib = under_root(IPOPT_LIB);
    if (!ipoptlib)
        die("the cpu backend (Ipopt) is not installed: rerun the installer with --backends=cpu\n"
            "(or pass --ipopt-lib, or set EMPOWER_IPOPT_LIB)");
    if (hsllib && !*hsllib) hsllib = NULL;
    if (!hsllib) hsllib = under_root(HSL_LIB);
    if (hsllib && access(hsllib, R_OK) != 0) die("HSL library not found: %s", hsllib);
    C.hsllib = hsllib;

    char src[512];
    printf("empower %s, %s form, Ipopt\n", VERSION, form);
    printf("  model    %s\n", modellib);
    printf("  ipopt    %s\n", ipoptlib);
    if (read_source("ipopt", src, sizeof src)) printf("           %s\n", src);
    printf("  hsl      %s\n", hsllib ? hsllib : "(none)");
    if (hsllib && read_source("hsl", src, sizeof src)) printf("           %s\n", src);
    printf("  output   %s\n", out);
    for (int k = 0; k < nopts; k++) printf("  option   %s\n", opts[k]);

    List cases = {0};
    for (int i = 0; i < args.n; i++) {
        if (!strcmp(args.v[i], "all")) add_all(&cases);
        else push(&cases, args.v[i]);
    }

    /* ExaModelsPower's __init__ otherwise creates a data directory next to its
     * source as it was at compile time, a path that only exists on the build
     * machine. */
    if (!getenv("EXA_MODELS_DEPOT")) {
#ifdef _WIN32
        _putenv_s("EXA_MODELS_DEPOT", case_dir());
#else
        setenv("EXA_MODELS_DEPOT", case_dir(), 1);
#endif
    }
#ifdef _WIN32
    /* The Julia runtime loads some of its DLLs by name after startup (libpcre2-8,
     * for one); let Windows find them next to the model library. */
    {
        char dir[8192];
        if (_fullpath(dir, modellib, sizeof dir)) {
            char *sep = last_sep(dir);
            if (sep) *sep = 0;
            SetDllDirectoryA(dir);
            const char *path = getenv("PATH");
            size_t L = strlen(dir) + (path ? strlen(path) : 0) + 2;
            char *np = xmalloc(L);
            snprintf(np, L, "%s;%s", dir, path ? path : "");
            _putenv_s("PATH", np);
        }
    }
#endif

    /* Loading the model library starts its bundled Julia runtime. */
    double t0 = now();
    if (!check_hsl) {
        step("loading model library");
        C.hm = lib_open(modellib);
        if (!C.hm) die("cannot load model library %s: %s", modellib, lib_error());
        step("model library loaded (%.3f s)", now() - t0);
    }

    step("loading Ipopt");
    t0 = now();
    void *hi = lib_open(ipoptlib);
    if (!hi) die("cannot load Ipopt %s: %s", ipoptlib, lib_error());
    C.I.create = sym(hi, "", "CreateIpoptProblem");
    C.I.free = sym(hi, "", "FreeIpoptProblem");
    C.I.solve = sym(hi, "", "IpoptSolve");
    C.I.str_opt = sym(hi, "", "AddIpoptStrOption");
    C.I.num_opt = sym(hi, "", "AddIpoptNumOption");
    C.I.int_opt = sym(hi, "", "AddIpoptIntOption");
    C.I.out_file = sym(hi, "", "OpenIpoptOutputFile");
    C.I.intermediate = lib_sym(hi, "SetIntermediateCallback");
    step("Ipopt loaded (%.3f s)", now() - t0);
    if (check_hsl) {
        /* For the installer: does Ipopt solve with MA27 from this HSL library? */
        int ok = probe_ma27(&C.I, C.hsllib);
        printf("MA27 %s\n", ok ? "works" : "does not work");
        return ok ? 0 : 1;
    }
    /* Whether MA27 works depends on the Ipopt build and on the HSL library, if
     * any, which Ipopt only loads at solve time. So ask Ipopt: solve min x^2
     * with MA27. */
    /* MA27 is the linear solver; another one only if the user asks for it. */
    C.linear_solver = NULL;
    for (int k = 0; k < nopts; k++)
        if (!strncmp(opts[k], "linear_solver=", 14)) C.linear_solver = opts[k] + 14;
    if (!C.linear_solver) {
        if (!probe_ma27(&C.I, C.hsllib)) {
            if (C.hsllib) die("MA27 could not be loaded from %s", C.hsllib);
            die("no HSL library with MA27. Request the free licence at\n  %s\n"
                "then rerun the installer with the download link from the email, or pass\n"
                "--hsl-lib=PATH (or set EMPOWER_HSL_LIB) for an HSL library you already have",
                MA27_URL);
        }
        C.linear_solver = "ma27";
    }
    step("linear solver: %s", C.linear_solver);

    MKDIR(out);
    write_system(out, ipoptlib, C.hsllib, argc, argv);
    char path[8192];
    snprintf(path, sizeof path, "%s/summary.csv", out);
    /* A fresh summary per run: it describes this run only, like system.txt. */
    C.summary = fopen(path, "w");
    if (!C.summary) die("cannot write %s", path);
    fputs("case,form,termination_status,raw_status,objective,iterations,nvar,ncon,"
          "time_parse_and_build,time_solve,time_solve_callbacks,time_solve_ipopt\n",
          C.summary);

    int solved = 0, failed = 0;
    Result *res = xcalloc(cases.n, sizeof *res);
    int *ok = xcalloc(cases.n, sizeof *ok);
    char **names = xcalloc(cases.n, sizeof *names);
    for (int i = 0; i < cases.n; i++) {
        char cf[8192], nm[1024];
        printf("\n[%d/%d] %s\n", i + 1, cases.n, cases.v[i]);
        fflush(stdout);
        int st = resolve_case(cases.v[i], cf, sizeof cf);
        case_name(st == 0 ? cf : cases.v[i], nm, sizeof nm);
        names[i] = strdup(nm);
        if (st == 0 && solve_case(&C, cf, out, &res[i]) == 0) { ok[i] = 1; solved++; }
        else failed++;
    }
    fclose(C.summary);

    /* The runs at a glance; the full table is summary.csv. */
    int w = 4;
    for (int i = 0; i < cases.n; i++)
        if ((int)strlen(names[i]) > w) w = (int)strlen(names[i]);
    printf("\nSummary (%s)\n", path);
    printf("  %-*s  %-14s  %14s  %5s  %8s  %8s\n", w, "case", "status", "objective", "iter",
           "build", "solve");
    for (int i = 0; i < cases.n; i++) {
        Result *r = &res[i];
        if (r->n == 0) {   /* never reached the solver */
            printf("  %-*s  %s\n", w, names[i], "not solved (see the error above)");
            continue;
        }
        printf("  %-*s  %-14s  %14.8g  %5d  %7.3fs  %7.3fs\n", w, names[i],
               termination_status(r->status), r->obj, r->iters, r->t_build, r->t_solve);
    }
    printf("  %d solved, %d not solved, %.1f s in total\n", solved, failed, now() - t_start);
    return failed ? 1 : 0;
}
