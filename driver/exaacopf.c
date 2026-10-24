/*
 * exaacopf: solve AC optimal power flow cases with a compiled ExaModelsPower
 * model library (cnlp ABI v0.1) and any Ipopt shared library (>= 3.13).
 *
 * Both libraries are loaded at run time, so the same driver works with the
 * IDAES Ipopt build, a conda Ipopt, or a local one.
 */
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#define LIBEXT "dylib"
#else
#define LIBEXT "so"
#endif

#define VERSION "0.1.0"
#define PGLIB_RAW "https://raw.githubusercontent.com/power-grid-lib/pglib-opf/master"
#define PGLIB_TGZ "https://github.com/power-grid-lib/pglib-opf/archive/refs/heads/master.tar.gz"

extern char **environ;

/* ---------------------------------------------------------------- utilities */

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

static double t_start;

static void step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void step(const char *fmt, ...) {
    va_list ap;
    printf("[%9.3f s] ", now() - t_start);
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
    void *p = dlsym(h, buf);
    if (!p) die("symbol %s not found: %s", buf, dlerror());
    return p;
}

static int run(char *const args[]) {
    pid_t pid;
    int st;
    if (posix_spawnp(&pid, args[0], NULL, NULL, args, environ) != 0) return -1;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Release layout: <root>/bin/exaacopf, <root>/emp/lib/libemp.<ext>,
 * <root>/ipopt/libipopt.<ext> (placed by install.sh), <root>/cases/. */
static char root[4096];

static void find_root(void) {
    char exe[4096], real[4096];
    uint32_t size = sizeof exe;
#ifdef __APPLE__
    if (_NSGetExecutablePath(exe, &size) != 0) return;
#else
    ssize_t k = readlink("/proc/self/exe", exe, size - 1);
    if (k <= 0) return;
    exe[k] = 0;
#endif
    if (!realpath(exe, real)) return;
    for (int up = 0; up < 2; up++) {
        char *slash = strrchr(real, '/');
        if (!slash) return;
        *slash = 0;
    }
    snprintf(root, sizeof root, "%s", real);
}

static const char *under_root(const char *rel) {
    static char buf[2][8192];
    static int which = 0;
    if (!*root) return NULL;
    char *b = buf[which ^= 1];
    snprintf(b, sizeof buf[0], "%s/%s", root, rel);
    return access(b, R_OK) == 0 ? b : NULL;
}

static const char *case_dir(void) {
    static char dir[8192];
    const char *env = getenv("EXAACOPF_CASES");
    if (env) snprintf(dir, sizeof dir, "%s", env);
    else if (*root) snprintf(dir, sizeof dir, "%s/cases", root);
    else snprintf(dir, sizeof dir, "cases");
    mkdir(dir, 0755);
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
    snprintf(dir, sizeof dir, "%s/pglib-opf-master", cd);
    if (access(dir, R_OK) != 0) {
        step("downloading the PGLib-OPF library: %s", PGLIB_TGZ);
        char cmd[9000];
        snprintf(cmd, sizeof cmd, "curl -fsSL '%s' | tar xz -C '%s'", PGLIB_TGZ, cd);
        char *args[] = {"sh", "-c", cmd, NULL};
        if (run(args) != 0) die("could not download PGLib-OPF");
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
    if (strchr(c, '/')) return case_error("case file not found: %s", c);
    char name[1024];
    snprintf(name, sizeof name, "%s%s", strncmp(c, "pglib_opf_", 10) ? "pglib_opf_" : "", c);
    size_t L = strlen(name);
    if (L > 2 && !strcmp(name + L - 2, ".m")) name[L - 2] = 0;
    const char *cd = case_dir();
    snprintf(path, cap, "%s/pglib-opf-master/%s.m", cd, name);
    if (access(path, R_OK) == 0) return 0;
    snprintf(path, cap, "%s/%s.m", cd, name);
    if (access(path, R_OK) == 0) {
        step("using cached case %s", path);
        return 0;
    }
    char url[2048], tmp[8300];
    snprintf(url, sizeof url, "%s/%s.m", PGLIB_RAW, name);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    step("downloading %s", url);
    char *args[] = {"curl", "-fsSL", "-o", tmp, url, NULL};
    if (run(args) != 0) {
        unlink(tmp);
        return case_error("%s is not a file, and no PGLib-OPF case %s could be downloaded", c, name);
    }
    rename(tmp, path);
    step("saved %s", path);
    return 0;
}

static void case_name(const char *path, char *out, size_t cap) {
    const char *b = strrchr(path, '/');
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
    const char *form, *model;
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
    mkdir(out, 0755);

    Model M = {0};
    load_model(&M, C->hm, C->model);

    step("parsing %s and building the %s-form model", casefile, C->form);
    double t0 = now();
    M.id = M.new_str(casefile);
    r->t_build = now() - t0;
    if (M.id <= 0) return case_error("the model library could not build a model from %s", casefile);
    int n = r->n = M.nvar(M.id), m = r->m = M.ncon(M.id);
    int nnzj = M.nnzj(M.id), nnzh = M.nnzh(M.id);
    step("model built (%.3f s): %d variables, %d constraints, %d Jacobian and %d Hessian "
         "nonzeros",
         r->t_build, n, m, nnzj, nnzh);

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

    step("setting up Ipopt");
    Ipopt *I = &C->I;
    void *P = I->create(n, xl, xu, m, gl, gu, nnzj, nnzh, 1, eval_f, eval_g, eval_grad, eval_jac,
                        eval_h);
    if (!P) return case_error("CreateIpoptProblem failed");
    if (I->intermediate) I->intermediate(P, on_iter);
    snprintf(path, sizeof path, "%s/ipopt.log", out);
    I->out_file(P, path, 5);
    I->str_opt(P, "linear_solver", "ma27");
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
        step("Ipopt option %s = %s", key, v);
    }

    step("solving with Ipopt (log also written to %s)", path);
    printf("\n");
    fflush(stdout);
    t0 = now();
    r->status = I->solve(P, x, g, &r->obj, y, zl, zu, &M);
    r->t_solve = now() - t0;
    fflush(stdout);
    printf("\n");
    r->iters = M.iters;
    r->t_cb = M.tfev + M.tgev + M.tconsev + M.tjev + M.thev;
    step("Ipopt finished (%.3f s, %d iterations): %s, objective %.10g", r->t_solve, r->iters,
         status_name(r->status), r->obj);
    step("time in model callbacks %.3f s, in Ipopt %.3f s", r->t_cb, r->t_solve - r->t_cb);

    snprintf(path, sizeof path, "%s/solution.json", out);
    step("writing %s", path);
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
    fputs(",\n  \"exaacopf_version\": \"" VERSION "\"", f);
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

/* ------------------------------------------------------------------- main */

static const char *usage =
    "usage: exaacopf <case>... [--form=polar|rect] [--solver=ipopt] [--options=\"NAME=VALUE ...\"]\n"
    "                [--output=DIR] [--ipopt-lib=PATH] [--model-lib=PATH]\n"
    "\n"
    "Solves the AC optimal power flow problem for one or more MATPOWER cases.\n"
    "\n"
    "  <case>       a MATPOWER file, or a PGLib-OPF case name (case118_ieee,\n"
    "               pglib_opf_case118_ieee.m), downloaded on first use; \"all\" solves\n"
    "               every PGLib-OPF case for typical operating conditions\n"
    "  --form       voltage formulation: polar (default) or rect\n"
    "  --solver     ipopt (default; the only solver in this release)\n"
    "  --options    solver options separated by spaces or commas,\n"
    "               --options=\"tol=1e-8 max_iter=500\"; --options=@FILE reads one\n"
    "               \"NAME VALUE\" per line; may be repeated\n"
    "  --output     output directory (default ./exaacopf-results)\n"
    "  --ipopt-lib  Ipopt shared library (default <install>/ipopt, or $EXAACOPF_IPOPT_LIB)\n"
    "  --model-lib  compiled ExaModelsPower library (default <install>/emp/lib)\n"
    "\n"
    "For each case, writes <output>/<case>/solution.json (PowerModels-style result)\n"
    "and <output>/<case>/ipopt.log, and appends a row to <output>/summary.csv.\n";

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
    const char *modellib = getenv("EXAACOPF_MODEL_LIB"), *ipoptlib = getenv("EXAACOPF_IPOPT_LIB"),
               *form = "polar", *solver = "ipopt", *out = "exaacopf-results", *v;
    List args = {0};

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(usage, stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            puts("exaacopf " VERSION);
            return 0;
        }
        if ((v = flag(a, "--form", &i, argc, argv))) form = v;
        else if ((v = flag(a, "--solver", &i, argc, argv))) solver = v;
        else if ((v = flag(a, "--options", &i, argc, argv))) parse_options(v);
        else if ((v = flag(a, "--output", &i, argc, argv))) out = v;
        else if ((v = flag(a, "--ipopt-lib", &i, argc, argv))) ipoptlib = v;
        else if ((v = flag(a, "--model-lib", &i, argc, argv))) modellib = v;
        else if (a[0] == '-') { fputs(usage, stderr); die("unknown option %s", a); }
        else push(&args, a);
    }
    if (!args.n) { fputs(usage, stderr); die("no case given"); }
    Ctx C = {0};
    C.form = form;
    if (!strcmp(form, "polar")) C.model = "acp";
    else if (!strcmp(form, "rect")) C.model = "acr";
    else die("--form must be polar or rect, got %s", form);
    if (strcmp(solver, "ipopt")) die("--solver must be ipopt, got %s", solver);
    if (!modellib) modellib = under_root("emp/lib/libemp." LIBEXT);
    if (!modellib) die("no model library: pass --model-lib or set EXAACOPF_MODEL_LIB");
    if (!ipoptlib) ipoptlib = under_root("ipopt/libipopt." LIBEXT);
    if (!ipoptlib) die("no Ipopt library: pass --ipopt-lib or set EXAACOPF_IPOPT_LIB");

    printf("exaacopf %s\n  form:          %s\n  solver:        %s\n  model library: %s\n"
           "  Ipopt library: %s\n  output:        %s\n",
           VERSION, form, solver, modellib, ipoptlib, out);
    for (int k = 0; k < nopts; k++) printf("  option:        %s\n", opts[k]);

    List cases = {0};
    for (int i = 0; i < args.n; i++) {
        if (!strcmp(args.v[i], "all")) add_all(&cases);
        else push(&cases, args.v[i]);
    }

    /* Loading the model library starts its bundled Julia runtime. */
    step("loading model library");
    double t0 = now();
    C.hm = dlopen(modellib, RTLD_NOW | RTLD_LOCAL);
    if (!C.hm) die("cannot load model library: %s", dlerror());
    step("model library loaded (%.3f s)", now() - t0);

    step("loading Ipopt");
    t0 = now();
    void *hi = dlopen(ipoptlib, RTLD_NOW | RTLD_LOCAL);
    if (!hi) die("cannot load Ipopt: %s", dlerror());
    C.I.create = sym(hi, "", "CreateIpoptProblem");
    C.I.free = sym(hi, "", "FreeIpoptProblem");
    C.I.solve = sym(hi, "", "IpoptSolve");
    C.I.str_opt = sym(hi, "", "AddIpoptStrOption");
    C.I.num_opt = sym(hi, "", "AddIpoptNumOption");
    C.I.int_opt = sym(hi, "", "AddIpoptIntOption");
    C.I.out_file = sym(hi, "", "OpenIpoptOutputFile");
    C.I.intermediate = dlsym(hi, "SetIntermediateCallback");
    step("Ipopt loaded (%.3f s)", now() - t0);

    mkdir(out, 0755);
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
    for (int i = 0; i < cases.n; i++) {
        printf("\n");
        step("case %d/%d: %s", i + 1, cases.n, cases.v[i]);
        char cf[8192];
        Result r = {0};
        if (resolve_case(cases.v[i], cf, sizeof cf) != 0 || solve_case(&C, cf, out, &r) != 0)
            failed++;
        else
            solved++;
    }
    fclose(C.summary);
    printf("\n");
    step("done: %d solved, %d not solved; summary in %s", solved, failed, path);
    return failed ? 1 : 0;
}
