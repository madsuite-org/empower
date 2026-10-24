/*
 * opfsolve: solve AC optimal power flow cases with a compiled ExaModelsPower
 * model library (cnlp ABI v0.1) and any Ipopt shared library (>= 3.13).
 *
 * Both libraries are loaded at run time, so the same driver works with the
 * conda-forge Ipopt, with or without the user's own HSL (hsllib), or a local one.
 */
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

#define VERSION "0.4.0"
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
    char *q[16];
    int n = 0;
    for (; args[n] && n < 15; n++) {
        size_t L = strlen(args[n]) + 3;
        q[n] = malloc(L);
        snprintf(q[n], L, "\"%s\"", args[n]);
    }
    q[n] = NULL;
    intptr_t rc = _spawnvp(_P_WAIT, args[0], (const char *const *)q);
    for (int i = 0; i < n; i++) free(q[i]);
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
    /* -s without -S: a failed download gets opfsolve's own message, not curl's. */
    char *c[] = {"curl", "-fsL", "-o", (char *)path, (char *)url, NULL};
    int rc = run(c);
    if (rc == -1) die("curl is needed to download %s", url);
    return rc;
}

/* Release layout: <root>/bin/opfsolve and the model library MODEL_LIB; placed by
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
    const char *env = getenv("OPFSOLVE_CASES");
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
    int32_t (*nblocks)(int32_t);
    int32_t (*block)(int32_t, int32_t, int32_t *);
    int32_t (*block_name)(int32_t, int32_t, uint8_t *, int32_t);
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
    M->nblocks = sym(h, p, "nblocks");
    M->block = sym(h, p, "block");
    M->block_name = sym(h, p, "block_name");
}

/* Offset and length of the named variable (kind 0) or constraint (kind 1) block. */
static int find_block(Model *M, int kind, const char *want, int *off, int *len) {
    int32_t info[64];
    int nb = M->nblocks(M->id);
    for (int k = 0; k < nb; k++) {
        if (M->block(M->id, k, info) != 0 || info[0] != kind) continue;
        char name[128];
        int n = M->block_name(M->id, k, (uint8_t *)name, sizeof name - 1);
        if (n < 0) continue;
        name[n < (int)sizeof name - 1 ? n : (int)sizeof name - 1] = 0;
        if (!strcmp(name, want)) {
            *off = info[1];
            *len = info[2];
            return 1;
        }
    }
    return 0;
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
static int verbose = 0;

/* Unless --verbose, opfsolve prints its own compact iteration table; Ipopt's full
 * output goes to ipopt.log. */
static Bool on_iter(int alg, int iter, double f, double pr, double du, double mu, double d,
                    double reg, double adu, double apr, int ls, void *u) {
    (void)d; (void)reg; (void)ls;
    ((Model *)u)->iters = iter;
    if (!verbose) {
        if (iter == 0)
            printf("              %5s  %15s  %9s  %9s  %6s  %9s  %9s\n", "iter", "objective",
                   "inf_pr", "inf_du", "lg(mu)", "alpha_du", "alpha_pr");
        printf("              %4d%c  %15.8e  %9.2e  %9.2e  %6.1f  %9.2e  %9.2e\n", iter,
               alg ? 'r' : ' ', f, pr, du, mu > 0 ? log10(mu) : -INFINITY, adu, apr);
        fflush(stdout);
    }
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

static const char *primal_status(int s) {
    switch (s) {
    case 0: case 6: return "FEASIBLE_POINT";
    case 1: return "NEARLY_FEASIBLE_POINT";
    case 2: return "INFEASIBLE_POINT";
    default: return "UNKNOWN_RESULT_STATUS";
    }
}

/* ------------------------------------------------------- MATPOWER tables */

/* The rows of mpc.<name>, as parsed numbers; enough to label the solution. */
typedef struct {
    int rows, cols;
    double *v;
} Table;

static double baseMVA_of(const char *text) {
    const char *p = strstr(text, "mpc.baseMVA");
    if (!p) return 100.0;
    p = strchr(p, '=');
    return p ? strtod(p + 1, NULL) : 100.0;
}

static Table read_table(const char *text, const char *name) {
    Table t = {0, 0, NULL};
    char key[64];
    snprintf(key, sizeof key, "mpc.%s", name);
    const char *p = text;
    while ((p = strstr(p, key))) {
        const char *q = p + strlen(key);
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '=') break;
        p = q;
    }
    if (!p) return t;
    p = strchr(p, '[');
    if (!p) return t;
    p++;
    int cap = 1024;
    t.v = xmalloc(cap * sizeof(double));
    int n = 0, col = 0;
    while (*p && *p != ']') {
        if (*p == '%') {
            while (*p && *p != '\n') p++;
        } else if (*p == ';' || *p == '\n') {
            if (col) {
                if (!t.cols) t.cols = col;
                if (col < t.cols) /* pad a short row */
                    for (; col < t.cols; col++) {
                        if (n == cap) t.v = realloc(t.v, (cap *= 2) * sizeof(double));
                        t.v[n++] = 0;
                    }
                t.rows++;
                col = 0;
            }
            p++;
        } else if (isspace((unsigned char)*p) || *p == ',') {
            p++;
        } else {
            char *end;
            double x = strtod(p, &end);
            if (end == p) { p++; continue; }
            p = end;
            if (t.cols && col >= t.cols) continue; /* ignore extra columns */
            if (n == cap) t.v = realloc(t.v, (cap *= 2) * sizeof(double));
            t.v[n++] = x;
            col++;
        }
    }
    return t;
}

#define AT(t, r, c) ((t).v[(size_t)(r) * (t).cols + (c)])

/* -------------------------------------------------------------------- JSON */

static void json_num(FILE *f, double v) {
    if (isfinite(v)) fprintf(f, "%.17g", v);
    else fputs("null", f);
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void json_key(FILE *f, int *first, const char *k) {
    fprintf(f, "%s", *first ? "" : ",");
    *first = 0;
    json_str(f, k);
    fputs(":", f);
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
    if (l->n == l->cap) l->v = realloc(l->v, (l->cap = l->cap ? 2 * l->cap : 16) * sizeof(char *));
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

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = xmalloc(n + 1);
    s[fread(s, 1, n, f)] = 0;
    fclose(f);
    return s;
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
    double obj, t_parse, t_build, t_solve, t_cb, t_total;
} Result;

static void write_solution(const char *path, const char *casefile, Ctx *C, Model *M,
                           const Result *r, const double *x, const double *y);

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
    if (M.meta(M.id, x, xl, xu, gl, gu) != 0) return case_error("could not read model bounds");
    for (int i = 0; i < n; i++) {
        if (xl[i] < -1e20) xl[i] = -1e20;
        if (xu[i] > 1e20) xu[i] = 1e20;
    }
    for (int i = 0; i < m; i++) {
        if (gl[i] < -1e20) gl[i] = -1e20;
        if (gu[i] > 1e20) gu[i] = 1e20;
    }

    Ipopt *I = &C->I;
    void *P = I->create(n, xl, xu, m, gl, gu, nnzj, nnzh, 1, eval_f, eval_g, eval_grad, eval_jac,
                        eval_h);
    if (!P) return case_error("CreateIpoptProblem failed");
    if (I->intermediate) I->intermediate(P, on_iter);
    snprintf(path, sizeof path, "%s/ipopt.log", out);
    I->out_file(P, path, 5);
    if (C->hsllib) I->str_opt(P, "hsllib", C->hsllib);
    I->str_opt(P, "linear_solver", C->linear_solver);
    if (!verbose) {
        I->int_opt(P, "print_level", 0);   /* the console; ipopt.log keeps level 5 */
        I->str_opt(P, "sb", "yes");
    }
    for (int k = 0; k < nopts; k++) {
        char key[512];
        const char *eq = strchr(opts[k], '=');
        snprintf(key, sizeof key, "%.*s", (int)(eq - opts[k]), opts[k]);
        const char *v = eq + 1;
        char *end;
        long iv = strtol(v, &end, 10);
        Bool ok;
        if (*v && !*end) ok = I->int_opt(P, key, (int)iv);
        else {
            double dv = strtod(v, &end);
            ok = (*v && !*end) ? I->num_opt(P, key, dv) : I->str_opt(P, key, v);
        }
        if (!ok) die("Ipopt rejected option %s", opts[k]);
    }

    step("solving with Ipopt, %s", C->linear_solver);
    if (verbose) printf("\n");
    fflush(stdout);
    t0 = now();
    r->status = I->solve(P, x, g, &r->obj, y, zl, zu, &M);
    r->t_solve = now() - t0;
    fflush(stdout);
    if (verbose) printf("\n");
    r->iters = M.iters;
    r->t_cb = M.tfev + M.tgev + M.tconsev + M.tjev + M.thev;
    step("%s: objective %.10g, %d iterations, %.3f s (%.3f s in model callbacks)",
         termination_status(r->status), r->obj, r->iters, r->t_solve, r->t_cb);

    snprintf(path, sizeof path, "%s/solution.json", out);
    step("wrote %s/{solution.json, ipopt.log}", out);
    r->t_total = now() - t_case;
    write_solution(path, casefile, C, &M, r, x, y);

    if (C->summary) {
        fprintf(C->summary, "%s,%s,%s,%s,%.17g,%d,%d,%d,%.6f,%.6f,%.6f,%.6f\n", name, C->form,
                termination_status(r->status), status_name(r->status), r->obj, r->iters, r->n,
                r->m, r->t_build, r->t_solve, r->t_cb, r->t_solve - r->t_cb);
        fflush(C->summary);
    }

    I->free(P);
    free(x); free(xl); free(xu); free(gl); free(gu); free(g); free(y); free(zl); free(zu);
    return (r->status == 0 || r->status == 1) ? 0 : 1;
}

/*
 * PowerModels-style result: solution in per unit, keyed by MATPOWER bus number
 * and by generator and branch row (1-based, out-of-service rows omitted, which
 * is also the order ExaPowerIO builds the model in).
 */
static void write_solution(const char *path, const char *casefile, Ctx *C, Model *M,
                           const Result *r, const double *x, const double *y) {
    FILE *f = fopen(path, "w");
    if (!f) { case_error("cannot write %s", path); return; }
    char *text = slurp(casefile);
    Table bus = read_table(text ? text : "", "bus");
    Table gen = read_table(text ? text : "", "gen");
    Table br = read_table(text ? text : "", "branch");
    double baseMVA = baseMVA_of(text ? text : "");
    int polar = !strcmp(C->form, "polar");
    int ova, nva, ovm, nvm, opg, npg, oqg, nqg, op, np, oq, nq;
    int ok = polar ? find_block(M, 0, "va", &ova, &nva) && find_block(M, 0, "vm", &ovm, &nvm)
                   : find_block(M, 0, "vr", &ova, &nva) && find_block(M, 0, "vim", &ovm, &nvm);
    ok = ok && find_block(M, 0, "pg", &opg, &npg) && find_block(M, 0, "qg", &oqg, &nqg) &&
         find_block(M, 0, "p", &op, &np) && find_block(M, 0, "q", &oq, &nq);

    fputs("{\n", f);
    fputs("  \"case\": ", f); json_str(f, casefile);
    fputs(",\n  \"form\": ", f); json_str(f, C->form);
    fputs(",\n  \"solver\": \"ipopt\"", f);
    fputs(",\n  \"opfsolve_version\": \"" VERSION "\"", f);
    fprintf(f, ",\n  \"termination_status\": \"%s\"", termination_status(r->status));
    fprintf(f, ",\n  \"primal_status\": \"%s\"", primal_status(r->status));
    fprintf(f, ",\n  \"raw_status\": \"%s\"", status_name(r->status));
    fputs(",\n  \"objective\": ", f); json_num(f, r->obj);
    fputs(",\n  \"solve_time\": ", f); json_num(f, r->t_solve);
    fprintf(f, ",\n  \"iterations\": %d", r->iters);
    fprintf(f, ",\n  \"stats\": {\"nvar\": %d, \"ncon\": %d, \"time_parse_and_build\": %.6f, "
               "\"time_solve\": %.6f, \"time_solve_callbacks\": %.6f, \"time_solve_ipopt\": %.6f, "
               "\"time_total\": %.6f}",
            r->n, r->m, r->t_build, r->t_solve, r->t_cb, r->t_solve - r->t_cb, r->t_total);
    fputs(",\n  \"solution\": {\n    \"per_unit\": true,\n    \"baseMVA\": ", f);
    json_num(f, baseMVA);

    /* Active rows of each table, in file order. */
    int kb = 0, kg = 0, kl = 0, first;
    char key[64];
    if (ok && bus.cols >= 2) {
        fputs(",\n    \"bus\": {", f);
        first = 1;
        for (int i = 0; i < bus.rows && kb < nva; i++) {
            if ((int)AT(bus, i, 1) == 4) continue;
            snprintf(key, sizeof key, "%d", (int)AT(bus, i, 0));
            fputs("\n      ", f);
            json_key(f, &first, key);
            double a = x[ova + kb], b = x[ovm + kb];
            double vm = polar ? b : hypot(a, b), va = polar ? a : atan2(b, a);
            fputs(" {\"vm\": ", f); json_num(f, vm);
            fputs(", \"va\": ", f); json_num(f, va);
            if (!polar) {
                fputs(", \"vr\": ", f); json_num(f, a);
                fputs(", \"vi\": ", f); json_num(f, b);
            }
            fputs("}", f);
            kb++;
        }
        fputs("\n    }", f);
    }
    if (ok && gen.cols >= 8) {
        fputs(",\n    \"gen\": {", f);
        first = 1;
        for (int i = 0; i < gen.rows && kg < npg; i++) {
            if (AT(gen, i, 7) == 0) continue;
            snprintf(key, sizeof key, "%d", i + 1);
            fputs("\n      ", f);
            json_key(f, &first, key);
            fputs(" {\"pg\": ", f); json_num(f, x[opg + kg]);
            fputs(", \"qg\": ", f); json_num(f, x[oqg + kg]);
            fputs("}", f);
            kg++;
        }
        fputs("\n    }", f);
    }
    if (ok && br.cols >= 11) {
        int nbr = np / 2;
        fputs(",\n    \"branch\": {", f);
        first = 1;
        for (int i = 0; i < br.rows && kl < nbr; i++) {
            if (AT(br, i, 10) == 0) continue;
            snprintf(key, sizeof key, "%d", i + 1);
            fputs("\n      ", f);
            json_key(f, &first, key);
            fputs(" {\"pf\": ", f); json_num(f, x[op + kl]);
            fputs(", \"qf\": ", f); json_num(f, x[oq + kl]);
            fputs(", \"pt\": ", f); json_num(f, x[op + nbr + kl]);
            fputs(", \"qt\": ", f); json_num(f, x[oq + nbr + kl]);
            fputs("}", f);
            kl++;
        }
        fputs("\n    }", f);
    }
    fputs("\n  }", f);

    /* Ipopt's constraint multipliers by model block, for anyone who wants the duals. */
    fputs(",\n  \"constraint_multipliers\": {", f);
    first = 1;
    int32_t info[64];
    int nb = M->nblocks(M->id);
    for (int k = 0; k < nb; k++) {
        if (M->block(M->id, k, info) != 0 || info[0] != 1) continue;
        char bname[128];
        int len = M->block_name(M->id, k, (uint8_t *)bname, sizeof bname - 1);
        if (len < 0) continue;
        bname[len < (int)sizeof bname - 1 ? len : (int)sizeof bname - 1] = 0;
        fputs("\n    ", f);
        json_key(f, &first, bname);
        fputs(" [", f);
        for (int i = 0; i < info[2]; i++) {
            if (i) fputc(',', f);
            json_num(f, y[info[1] + i]);
        }
        fputc(']', f);
    }
    fputs("\n  }\n}\n", f);
    fclose(f);
    if (!ok)
        case_error("%s: variable blocks not found; solution written without the per-bus table",
                   casefile);
    else if (kb != nva || kg != npg || 2 * kl != np)
        case_error("%s: case tables (%d buses, %d gens, %d branches) do not match the model "
                   "(%d, %d, %d)",
                   casefile, kb, kg, kl, nva, npg, np / 2);
    free(bus.v); free(gen.v); free(br.v); free(text);
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
    int saved = dup(1), devnull = open(NULL_DEVICE, O_WRONLY);
    if (devnull >= 0) dup2(devnull, 1);
    int status = I->solve(P, &x, NULL, &obj, NULL, &zl, &zu, NULL);
    fflush(stdout);
    if (devnull >= 0) { dup2(saved, 1); close(devnull); }
    close(saved);
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
    fprintf(f, "date: %s\nopfsolve: %s\n", date, VERSION);
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

/* ------------------------------------------------------------------- main */

static const char *usage =
    "usage: opfsolve <case>... [--form=polar|rect] [--solver=ipopt] [--options=\"NAME=VALUE ...\"]\n"
    "                [--output=DIR] [--hsl-lib=PATH] [--ipopt-lib=PATH] [--model-lib=PATH]\n"
    "\n"
    "Solves the AC optimal power flow problem for one or more MATPOWER cases.\n"
    "\n"
    "  <case>       a MATPOWER file, or a PGLib-OPF case name (case118_ieee,\n"
    "               pglib_opf_case118_ieee.m), downloaded on first use; \"all\" solves\n"
    "               every PGLib-OPF v" PGLIB_VERSION " case for typical operating conditions\n"
    "  --form       voltage formulation: polar (default) or rect\n"
    "  --solver     ipopt (default; the only solver in this release)\n"
    "  --options    solver options separated by spaces or commas,\n"
    "               --options=\"tol=1e-8 max_iter=500\"; --options=@FILE reads one\n"
    "               \"NAME VALUE\" per line; may be repeated\n"
    "  --output     output directory (default ./opfsolve-results)\n"
    "  --hsl-lib    HSL library providing MA27 (and MA57, MA86, MA97 if it has them),\n"
    "               passed to Ipopt's hsllib option; default <install>/hsl, or\n"
    "               $OPFSOLVE_HSL_LIB. MA27 is the linear solver unless --options sets\n"
    "               linear_solver\n"
    "  --ipopt-lib  Ipopt shared library, 3.14 or newer (default <install>/ipopt, or\n"
    "               $OPFSOLVE_IPOPT_LIB)\n"
    "  --model-lib  compiled ExaModelsPower library (default <install>/emp/lib)\n"
    "  --verbose    print Ipopt's full output (it always goes to <case>/ipopt.log)\n"
    "\n"
    "For each case, writes <output>/<case>/solution.json (PowerModels-style result)\n"
    "and <output>/<case>/ipopt.log, and appends a row to <output>/summary.csv;\n"
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
    const char *modellib = getenv("OPFSOLVE_MODEL_LIB"), *ipoptlib = getenv("OPFSOLVE_IPOPT_LIB"),
               *hsllib = getenv("OPFSOLVE_HSL_LIB"),
               *form = "polar", *solver = "ipopt", *out = "opfsolve-results", *v;
    List args = {0};
    int check_hsl = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(usage, stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            puts("opfsolve " VERSION);
            return 0;
        }
        if (!strcmp(a, "--check-hsl")) { check_hsl = 1; continue; }
        if (!strcmp(a, "--verbose")) { verbose = 1; continue; }
        if ((v = flag(a, "--form", &i, argc, argv))) form = v;
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
    else die("--form must be polar or rect, got %s", form);
    if (strcmp(solver, "ipopt")) die("--solver must be ipopt, got %s", solver);
    if (!modellib) modellib = under_root(MODEL_LIB);
    if (!modellib) die("no model library: pass --model-lib or set OPFSOLVE_MODEL_LIB");
    if (!ipoptlib) ipoptlib = under_root(IPOPT_LIB);
    if (!ipoptlib) die("no Ipopt library: pass --ipopt-lib or set OPFSOLVE_IPOPT_LIB");
    if (hsllib && !*hsllib) hsllib = NULL;
    if (!hsllib) hsllib = under_root(HSL_LIB);
    if (hsllib && access(hsllib, R_OK) != 0) die("HSL library not found: %s", hsllib);
    C.hsllib = hsllib;

    char src[512];
    printf("opfsolve %s, %s form, Ipopt\n", VERSION, form);
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
                "--hsl-lib=PATH (or set OPFSOLVE_HSL_LIB) for an HSL library you already have",
                MA27_URL);
        }
        C.linear_solver = "ma27";
    }
    step("linear solver: %s", C.linear_solver);

    MKDIR(out);
    write_system(out, ipoptlib, C.hsllib, argc, argv);
    char path[8192];
    snprintf(path, sizeof path, "%s/summary.csv", out);
    int fresh = access(path, F_OK) != 0;
    C.summary = fopen(path, "a");
    if (!C.summary) die("cannot write %s", path);
    if (fresh)
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
