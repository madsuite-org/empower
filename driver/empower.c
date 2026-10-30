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
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
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
#include <signal.h>
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

#define VERSION "0.10.0"
#define MA27_URL "https://www.hsl.rl.ac.uk/download/MA27/1.0.0/a/"
/* Without HSL, Ipopt's linear solver is MUMPS; the same words as the installer's. */
#define NO_HSL_WARNING \
    "warning: no HSL MA27, so Ipopt uses MUMPS. MUMPS is not the most efficient linear\n" \
    "solver for OPF, and its timings are not appropriate for benchmarking solver\n" \
    "performance. Requesting HSL's free MA27 licence takes about 2 minutes:\n" \
    "  " MA27_URL "\n" \
    "then add it with: empower hsl '<download link from HSL's email>'\n"
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
#define MAXJOBS 64   /* --jobs: at most this many worker processes */
/* --quiet: only warnings, errors and the summary (the solver logs still go to their files) */
static int quiet = 0;

/* Styles on a terminal (unless NO_COLOR is set); plain text otherwise. The rule is
 * ASCII on Windows, whose console may not show box-drawing characters. */
static const char *BOLD = "", *DIM = "", *RST = "", *GREEN = "", *RED = "", *YELLOW = "";
#ifdef _WIN32
#define RULE "-"
#define DOT "-"
#else
#define RULE "\u2500"
#define DOT "\u00b7"
#endif
static void init_style(void) {
#ifndef _WIN32
    if (!isatty(1) || getenv("NO_COLOR")) return;
    BOLD = "\033[1m"; DIM = "\033[2m"; RST = "\033[0m";
    GREEN = "\033[32m"; RED = "\033[31m"; YELLOW = "\033[33m";
#endif
}

/* "── [1/3] case ─────": a heading across 78 columns. */
static void heading(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void heading(const char *fmt, ...) {
    if (quiet) return;
    char t[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    printf("\n%s" RULE RULE " %s ", BOLD, t);
    for (int k = (int)strlen(t) + 4; k < 78; k++) fputs(RULE, stdout);
    printf("%s\n", RST);
}

/* "  label    value" */
static void kv(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void kv(const char *label, const char *fmt, ...) {
    if (quiet) return;
    va_list ap;
    printf("  %-9s", label);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static const char *status_color(int ok) { return ok ? GREEN : RED; }

static void step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void step(const char *fmt, ...) {
    if (quiet) return;
    va_list ap;
    printf("  %s%8.3fs%s  ", DIM, now() - t_start, RST);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* In libempower, an error ends the API call, not the process: die() records the message
 * and jumps back to the API function that is running. */
#ifdef EMPOWER_LIB
static char api_error[2048];
static jmp_buf api_jmp;
static int api_active = 0;
#endif

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
    va_list ap;
#ifdef EMPOWER_LIB
    if (api_active) {
        va_start(ap, fmt);
        vsnprintf(api_error, sizeof api_error, fmt, ap);
        va_end(ap);
        longjmp(api_jmp, 1);
    }
#endif
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
#ifdef EMPOWER_LIB
    va_start(ap, fmt);
    vsnprintf(api_error, sizeof api_error, fmt, ap);
    va_end(ap);
    if (api_active && quiet) return -1;
#endif
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
#ifndef _WIN32
/* The child run() waits for. On Ctrl-C (SIGINT), SIGTERM or SIGHUP the child is killed
 * at once: empower-cuda, busy on the GPU, would otherwise keep solving after empower
 * has exited, and print into the terminal. */
static volatile pid_t child_pid = 0;
/* The workers of --jobs, killed the same way. */
static volatile pid_t workers[MAXJOBS];
static void on_signal(int sig) {
    pid_t c = child_pid;
    if (c > 0) kill(c, SIGKILL);
    for (int k = 0; k < MAXJOBS; k++)
        if (workers[k] > 0) kill(workers[k], SIGKILL);
    static const char msg[] = "\ninterrupted\n";
    (void)!write(2, msg, sizeof msg - 1);
    _exit(128 + sig);
}
#endif

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
    struct sigaction sa, old_int, old_term, old_hup;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (posix_spawnp(&pid, args[0], NULL, NULL, args, environ) != 0) return -1;
    int w;
#ifdef EMPOWER_LIB
    /* In a library, the calling program's signal handling stays as it is. */
    (void)sa; (void)old_int; (void)old_term; (void)old_hup;
    while ((w = waitpid(pid, &st, 0)) < 0 && errno == EINTR) {}
    if (w < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#else
    child_pid = pid;
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);
    sigaction(SIGHUP, &sa, &old_hup);
    while ((w = waitpid(pid, &st, 0)) < 0 && errno == EINTR) {}
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGHUP, &old_hup, NULL);
    child_pid = 0;
    if (w < 0) return -1;
    if (WIFSIGNALED(st) && (WTERMSIG(st) == SIGINT || WTERMSIG(st) == SIGTERM)) {
        fprintf(stderr, "\ninterrupted\n");
        exit(128 + WTERMSIG(st));
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
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
#ifdef EMPOWER_LIB
    /* libempower: <install>/lib/libempower.so (bin\\empower.dll on Windows). */
#ifdef _WIN32
    HMODULE self;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void *)find_root, &self)) return;
    DWORD k = GetModuleFileNameA(self, exe, sizeof exe);
    if (k == 0 || k >= sizeof exe) return;
    if (!_fullpath(real, exe, sizeof real)) return;
#else
    Dl_info info;
    if (!dladdr((void *)find_root, &info) || !info.dli_fname) return;
    snprintf(exe, sizeof exe, "%s", info.dli_fname);
    if (!realpath(exe, real)) return;
#endif
#elif defined(_WIN32)
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
    /* Its violations writer (ExaModelsPower 0.4.4 and newer; NULL in older libraries):
     * the largest violation of each group of constraints and bounds, as CSV. */
    int32_t (*write_violations)(int32_t, const char *, const double *, const double *,
                                const double *, const double *, double, const char *);
    int32_t id;
    long nfev, ngev, nconsev, njev, nhev, iters;
    double tfev, tgev, tconsev, tjev, thev;
    double t0, t_init;   /* the solve's start; the time to its first iteration (-1 until then) */
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
    char wv[256];
    snprintf(wv, sizeof wv, "%s_write_violations", p);
    M->write_violations = lib_sym(h, wv);
}

/* The groups ExaModelsPower's violations writer reports, in its order, and the short
 * names the summary uses for them. */
#define NGROUPS 6
static const char *group_key[NGROUPS] = {"power_balance", "power_flow", "branch_flow",
                                         "angle_difference", "voltage", "generation"};
static const char *group_head[NGROUPS] = {"balance", "flow eqs", "branch lim", "angle",
                                          "voltage", "gen"};

/* Read the writer's CSV (a header, then group,violation rows) into v. Returns 0 on success. */
static int read_violations(const char *path, double v[NGROUPS]) {
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, ',');
        if (!c) continue;
        *c = 0;
        for (int k = 0; k < NGROUPS; k++)
            if (!strcmp(line, group_key[k])) { v[k] = strtod(c + 1, NULL); found++; }
    }
    fclose(f);
    return found == NGROUPS ? 0 : 1;
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
    Model *M = u;
    M->iters = iter;
    if (iter == 0 && M->t_init < 0) M->t_init = now() - M->t0;
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
static int no_hsl = 0;   /* MUMPS because no HSL library is installed */
static int hsl_system = 0;   /* the HSL library was found on this machine, not installed */

/* An HSL library in a standard location (HSL's libHSL, Coin-HSL), for when none is
 * installed or given: the first that exists. */
static const char *system_hsl(void) {
#if defined(_WIN32)
    return NULL;
#else
#ifdef __APPLE__
    static const char *dirs[] = {"/usr/local/lib", "/opt/homebrew/lib", NULL};
    const char *ext = "dylib";
#else
    static const char *dirs[] = {"/usr/local/lib", "/usr/local/lib64", "/usr/lib", "/usr/lib64",
                                 "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", NULL};
    const char *ext = "so";
#endif
    static char path[1024];
    const char *names[] = {"libhsl", "libcoinhsl"};
    for (int k = 0; k < 2; k++)
        for (int d = 0; dirs[d]; d++) {
            snprintf(path, sizeof path, "%s/%s.%s", dirs[d], names[k], ext);
            if (access(path, R_OK) == 0) return path;
        }
    return NULL;
#endif
}
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

/* The whole PGLib-OPF library in <cases>/pglib-opf-<version>, downloaded once: the
 * typical operating conditions at the top level, api/ and sad/ below. Its directory. */
static const char *fetch_pglib(void) {
    static char dir[8300];
    const char *cd = case_dir();
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
    return dir;
}

/* The PGLib-OPF cases (pglib_opf_*.m) in a directory, smallest first. */
static void list_cases(const char *dir, List *found) {
    DIR *d = opendir(dir);
    if (!d) die("cannot read %s", dir);
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (!strncmp(e->d_name, "pglib_opf_", 10) && L > 2 && !strcmp(e->d_name + L - 2, ".m")) {
            char p[8600];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            push(found, p);
        }
    }
    closedir(d);
    qsort(found->v, found->n, sizeof(char *), cmpcase);
}

/* empower download: the whole PGLib-OPF library, for use offline or with paths. */
static int download_pglib(void) {
    const char *dir = fetch_pglib();
    const char *sets[3][2] = {{"", "typical operating conditions"},
                              {"/api", "active power increase (api)"},
                              {"/sad", "small angle difference (sad)"}};
    for (int k = 0; k < 3; k++) {
        char sub[8400];
        snprintf(sub, sizeof sub, "%s%s", dir, sets[k][0]);
        List found = {0};
        list_cases(sub, &found);
        printf("  %3d cases, %s: %s\n", found.n, sets[k][1], sub);
    }
    printf("empower all solves the typical operating conditions; a path solves any case\n");
    return 0;
}

/* "all": every case at the top level of PGLib-OPF (typical operating conditions). */
static void add_all(List *cases) {
    List found = {0};
    list_cases(fetch_pglib(), &found);
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
    double t_init, t_la;   /* in the solve: to the first iteration, and in the linear solver */
    double viol;   /* max absolute constraint violation, max(gl - g, g - gu, 0), at the solution */
    double vg[NGROUPS];   /* the largest violation of each group (violations writer) */
    int has_groups;
    const char *st;   /* termination status, as in summary.csv */
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

/* ------------------------------------------------------------ MATPOWER tables */

/* The bus, gen and branch tables of a MATPOWER case (row by row, as numbers), its base
 * MVA, and the objective and status of a solution written by ExaModelsPower. */
typedef struct {
    int n, cap, *len;
    double **row;
} MpTable;
typedef struct {
    double base, objective;
    char status[64];
    MpTable bus, gen, branch;
} MpCase;

static void mp_push(MpTable *t, double *v, int k) {
    if (t->n == t->cap) {
        t->cap = t->cap ? 2 * t->cap : 64;
        t->row = xrealloc(t->row, t->cap * sizeof *t->row);
        t->len = xrealloc(t->len, t->cap * sizeof *t->len);
    }
    t->row[t->n] = xmalloc((k ? k : 1) * sizeof(double));
    memcpy(t->row[t->n], v, k * sizeof(double));
    t->len[t->n++] = k;
}

/* The numbers of one table row ("1 3 0 0 ...", separated by spaces, tabs or commas). */
static int mp_numbers(const char *seg, double *v, int cap) {
    int k = 0;
    const char *q = seg;
    while (*q && k < cap) {
        while (*q == ' ' || *q == '\t' || *q == ',' || *q == '\r' || *q == '\n') q++;
        if (!*q) break;
        char *end;
        double x = strtod(q, &end);
        if (end == q) break;
        v[k++] = x;
        q = end;
    }
    return k;
}

/* Which table a line opens (mpc.bus = [ ...): 1 bus, 2 gen, 3 branch, -1 another one, 0 none;
 * *rest points after the '['. */
static int mp_opens(const char *code, const char **rest) {
    const char *m = strstr(code, "mpc.");
    if (!m) return 0;
    const char *b = strchr(m, '[');
    if (!b) return 0;
    *rest = b + 1;
    if (!strncmp(m + 4, "bus", 3) && (m[7] == ' ' || m[7] == '=' || m[7] == '\t')) return 1;
    if (!strncmp(m + 4, "gen", 3) && (m[7] == ' ' || m[7] == '=' || m[7] == '\t')) return 2;
    if (!strncmp(m + 4, "branch", 6)) return 3;
    return -1;
}

static int read_matpower(const char *path, MpCase *mc) {
    memset(mc, 0, sizeof *mc);
    mc->base = 100;
    mc->objective = NAN;
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    char line[16384];
    int table = 0;
    double v[64];
    while (fgets(line, sizeof line, f)) {
        const char *h = strstr(line, "Solved by ExaModelsPower: objective = ");
        if (h) {
            mc->objective = strtod(h + 38, NULL);
            const char *st = strstr(h, "status = ");
            if (st) {
                snprintf(mc->status, sizeof mc->status, "%s", st + 9);
                mc->status[strcspn(mc->status, " \r\n")] = 0;
            }
        }
        char *pct = strchr(line, '%');
        if (pct) *pct = 0;
        const char *code = line;
        if (!table) {
            const char *bm = strstr(code, "mpc.baseMVA");
            if (bm && strchr(bm, '=')) mc->base = strtod(strchr(bm, '=') + 1, NULL);
            const char *rest;
            int t = mp_opens(code, &rest);
            if (!t) continue;
            table = t;
            code = rest;
        }
        /* rows end at ';', the table at ']' */
        char *close = strchr((char *)code, ']');
        if (close) *close = 0;
        char *seg = (char *)code;
        for (;;) {
            char *semi = strchr(seg, ';');
            if (semi) *semi = 0;
            int k = mp_numbers(seg, v, 64);
            if (k > 0 && table > 0)
                mp_push(table == 1 ? &mc->bus : table == 2 ? &mc->gen : &mc->branch, v, k);
            if (!semi) break;
            seg = semi + 1;
        }
        if (close) table = 0;
    }
    fclose(f);
    return mc->bus.n ? 0 : 1;
}

static double mp_at(MpTable *t, int i, int col) { return col <= t->len[i] ? t->row[i][col - 1] : 0; }

/* <case>/solution.json from the solved case solution.m, in PowerModels' result layout:
 * per-unit powers, radians, buses keyed by number, generators and branches by their
 * position in the case (only those in service). */
static int write_solution_json(const char *solution_m, const char *json) {
    MpCase mc;
    if (read_matpower(solution_m, &mc) != 0) return 1;
    FILE *f = fopen(json, "w");
    if (!f) return 1;
    double b = mc.base, deg = M_PI / 180;
    fprintf(f, "{\n  \"termination_status\": \"%s\",\n  \"objective\": %.17g,\n", mc.status,
            mc.objective);
    fprintf(f, "  \"solution\": {\n    \"baseMVA\": %.17g,\n    \"per_unit\": true,\n", b);
    fprintf(f, "    \"bus\": {");
    int first = 1;
    for (int i = 0; i < mc.bus.n; i++) {
        if (mp_at(&mc.bus, i, 2) == 4) continue;   /* isolated */
        fprintf(f, "%s\n      \"%.0f\": {\"vm\": %.17g, \"va\": %.17g}", first ? "" : ",",
                mp_at(&mc.bus, i, 1), mp_at(&mc.bus, i, 8), mp_at(&mc.bus, i, 9) * deg);
        first = 0;
    }
    fprintf(f, "\n    },\n    \"gen\": {");
    first = 1;
    for (int i = 0; i < mc.gen.n; i++) {
        if (mp_at(&mc.gen, i, 8) <= 0) continue;
        fprintf(f, "%s\n      \"%d\": {\"pg\": %.17g, \"qg\": %.17g}", first ? "" : ",", i + 1,
                mp_at(&mc.gen, i, 2) / b, mp_at(&mc.gen, i, 3) / b);
        first = 0;
    }
    fprintf(f, "\n    },\n    \"branch\": {");
    first = 1;
    for (int i = 0; i < mc.branch.n; i++) {
        if (mp_at(&mc.branch, i, 11) <= 0) continue;
        fprintf(f, "%s\n      \"%d\": {\"pf\": %.17g, \"qf\": %.17g, \"pt\": %.17g, \"qt\": %.17g}",
                first ? "" : ",", i + 1, mp_at(&mc.branch, i, 14) / b, mp_at(&mc.branch, i, 15) / b,
                mp_at(&mc.branch, i, 16) / b, mp_at(&mc.branch, i, 17) / b);
        first = 0;
    }
    fprintf(f, "\n    }\n  }\n}\n");
    fclose(f);
    return 0;
}

/* The wall-clock time Ipopt spent in its linear solver, from the timing statistics at the
 * end of its log (print_timing_statistics): scaling, symbolic and numeric factorization,
 * and back solves. NAN if the log has none. */
static double linear_algebra_time(const char *log) {
    FILE *f = fopen(log, "r");
    if (!f) return NAN;
    if (fseek(f, -65536, SEEK_END) != 0) rewind(f);
    static const char *timers[] = {"LinearSystemScaling.", "LinearSystemSymbolicFactorization.",
                                   "LinearSystemFactorization.", "LinearSystemBackSolve."};
    char line[512];
    double t = 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        const char *p = line + strspn(line, " ");
        for (size_t k = 0; k < sizeof timers / sizeof *timers; k++) {
            if (strncmp(p, timers[k], strlen(timers[k]))) continue;
            const char *w = strstr(p, "wall:");
            if (w) { t += strtod(w + 5, NULL); found++; }
        }
    }
    fclose(f);
    return found ? t : NAN;
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
    /* --quiet: nothing from Ipopt on the console (ipopt.log keeps everything); an explicit
     * print_level in --options still wins, being set after. */
    if (quiet) { I->int_opt(P, "print_level", 0); I->str_opt(P, "sb", "yes"); }
    /* Ipopt's timers, at the end of ipopt.log: the linear algebra time is read from there. */
    I->str_opt(P, "print_timing_statistics", "yes");
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
    if (!quiet) printf("\n");
    fflush(stdout);
    t0 = now();
    M.t0 = t0;
    M.t_init = -1;
    r->status = I->solve(P, x, g, &r->obj, y, zl, zu, &M);
    r->t_solve = now() - t0;
    r->t_init = M.t_init >= 0 ? M.t_init : r->t_solve;
    I->free(P);   /* closes ipopt.log */
    P = NULL;
    snprintf(path, sizeof path, "%s/ipopt.log", out);
    r->t_la = linear_algebra_time(path);
    fflush(stdout);
    if (!quiet) printf("\n");
    r->iters = M.iters;
    r->st = termination_status(r->status);
    /* Ipopt returns the constraint values at the solution in g, unscaled. */
    r->viol = 0;
    for (int i = 0; i < m; i++) {
        double v = gl[i] - g[i] > g[i] - gu[i] ? gl[i] - g[i] : g[i] - gu[i];
        if (v > r->viol) r->viol = v;
    }
    r->t_cb = M.tfev + M.tgev + M.tconsev + M.tjev + M.thev;
    step("%s%s%s%s: objective %.10g, %d iterations, %.3f s (%.3f s in model evaluations)", BOLD,
         status_color(r->status == 0 || r->status == 1), termination_status(r->status), RST, r->obj,
         r->iters, r->t_solve, r->t_cb);

    snprintf(path, sizeof path, "%s/solution.m", out);
    if (M.write_matpower(M.id, path, x, y, zl, zu, r->obj, termination_status(r->status)) != 0)
        case_error("could not write %s", path);
    else {
        char json[8400];
        snprintf(json, sizeof json, "%s/solution.json", out);
        if (write_solution_json(path, json) != 0) case_error("could not write %s", json);
        step("wrote %s/{solution.m, solution.json, ipopt.log}", out);
    }
    if (M.write_violations) {
        snprintf(path, sizeof path, "%s/violations.csv", out);
        r->has_groups = M.write_violations(M.id, path, x, y, zl, zu, r->obj,
                                           termination_status(r->status)) == 0 &&
                        read_violations(path, r->vg) == 0;
    }
    r->t_total = now() - t_case;

    if (C->summary) {
        csv_field(C->summary, name);
        fprintf(C->summary, ",%s,%s,%s,%.17g,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.3e", C->form,
                termination_status(r->status), status_name(r->status), r->obj, r->iters, r->n,
                r->m, r->t_build, r->t_solve, r->t_cb, r->t_solve - r->t_cb, r->t_init, r->t_la, r->viol);
        for (int k = 0; k < NGROUPS; k++)
            r->has_groups ? fprintf(C->summary, ",%.3e", r->vg[k]) : fprintf(C->summary, ",");
        fprintf(C->summary, "\n");
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
static void load_ipopt(Ipopt *I, const char *ipoptlib) {
    void *hi = lib_open(ipoptlib);
    if (!hi) die("cannot load Ipopt %s: %s", ipoptlib, lib_error());
    I->create = sym(hi, "", "CreateIpoptProblem");
    I->free = sym(hi, "", "FreeIpoptProblem");
    I->solve = sym(hi, "", "IpoptSolve");
    I->str_opt = sym(hi, "", "AddIpoptStrOption");
    I->num_opt = sym(hi, "", "AddIpoptNumOption");
    I->int_opt = sym(hi, "", "AddIpoptIntOption");
    I->out_file = sym(hi, "", "OpenIpoptOutputFile");
    I->intermediate = lib_sym(hi, "SetIntermediateCallback");
}

/* MA27 if Ipopt can load it (from the HSL library, if any, which Ipopt only loads at
 * solve time: so solve min (x - 1)^2 with it), else MUMPS, with a warning. */
static void choose_linear_solver(Ctx *C) {
    if (probe_ma27(&C->I, C->hsllib)) { C->linear_solver = "ma27"; return; }
    if (C->hsllib && !hsl_system) die("MA27 could not be loaded from %s", C->hsllib);
    if (hsl_system)
        fprintf(stderr, "\n%s%snote:%s found %s, but Ipopt cannot load MA27 from it as it is\n"
                "(it may need libraries such as Julia's libblastrampoline); `empower hsl %s`\n"
                "installs an adapted copy.\n", BOLD, YELLOW, RST, C->hsllib, C->hsllib);
    C->hsllib = NULL;
    fprintf(stderr, "\n%s%swarning:%s %s\n", BOLD, YELLOW, RST, NO_HSL_WARNING + 9);
    C->linear_solver = "mumps";
    no_hsl = 1;
}

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

/* ------------------------------------------------------------ memory limit */

#ifdef __linux__
/* The memory this process may use under its control group (a Slurm job's --mem, a
 * container's limit): the smallest limit on the path from its cgroup to the root, or 0
 * when there is none. */
static unsigned long long cgroup_memory_limit(void) {
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f) return 0;
    char line[4096], path[4096] = "", file[8300];
    int v2 = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "0::", 3)) { snprintf(path, sizeof path, "%s", line + 3); v2 = 1; }
        else {
            char *m = strstr(line, ":memory:");
            if (m && !v2) snprintf(path, sizeof path, "%s", m + 8);
        }
    }
    fclose(f);
    unsigned long long best = 0;
    for (;;) {
        snprintf(file, sizeof file,
                 v2 ? "/sys/fs/cgroup%s/memory.max" : "/sys/fs/cgroup/memory%s/memory.limit_in_bytes", path);
        FILE *g = fopen(file, "r");
        if (g) {
            char b[64];
            if (fgets(b, sizeof b, g) && strncmp(b, "max", 3)) {
                unsigned long long v = strtoull(b, NULL, 10);
                /* cgroup v1 reports "no limit" as a huge number */
                if (v > 0 && v < (1ULL << 60) && (best == 0 || v < best)) best = v;
            }
            fclose(g);
        }
        char *slash = strrchr(path, '/');
        if (!slash) break;
        if (slash == path && !path[1]) break;   /* at the root */
        slash[slash == path ? 1 : 0] = 0;       /* parent, keeping "/" for the root */
    }
    return best;
}
#endif

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
#ifdef __linux__
    /* Below about 8 GB the system keeps evicting empower-cuda's 2 GB executable and its
     * NVIDIA libraries and reading them again: on a Slurm job with the default 1 GB a run
     * took 30 to 60 s instead of 2 s. */
    unsigned long long lim = cgroup_memory_limit();
    if (lim && lim < (8ULL << 30))
        fprintf(stderr,
                "\n%s%swarning:%s this process may use only %.1f GB of memory (its cgroup limit, such as\n"
                "a Slurm job's --mem). The cuda backend needs about 8 GB: with less, the system\n"
                "keeps re-reading its files, and runs are many times slower. Request more memory,\n"
                "for example salloc/sbatch --mem=32G.\n\n",
                BOLD, YELLOW, RST, lim / 1e9);
#endif
    /* The bundle's own depot, then the NVIDIA libraries the installer downloaded. */
    char depot[16400];
    snprintf(depot, sizeof depot, "%s/cuda/share/julia:%s/cuda-depot", root, root);
    if (!quiet)
        printf("%sempower %s%s  " DOT "  %s form  " DOT "  MadNLP with cuDSS on the GPU\n", BOLD, VERSION, RST, form);
    kv("solver", "%s", exe);
    kv("output", "%s", out);
    for (int k = 0; k < nopts; k++) kv("option", "%s", opts[k]);
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
    char **cmd = xcalloc(5 + 2 * nopts + files.n, sizeof *cmd);
    int n = 0;
    cmd[n++] = (char *)exe;
    cmd[n++] = formarg;
    cmd[n++] = outarg;
    if (quiet) cmd[n++] = "--quiet";
    for (int k = 0; k < nopts; k++) { cmd[n++] = "--option"; cmd[n++] = opts[k]; }
    for (int i = 0; i < files.n; i++) cmd[n++] = files.v[i];
    cmd[n] = NULL;
    /* The bundle finds its Julia libraries and artifacts in its own depot. */
    setenv("JULIA_DEPOT_PATH", depot, 1);
    /* As for the cpu model library: ExaModelsPower's __init__ otherwise creates a data
     * directory next to its source as it was on the build machine. */
    if (!getenv("EXA_MODELS_DEPOT")) setenv("EXA_MODELS_DEPOT", case_dir(), 1);
    /* empower-cuda's timestamps continue this run's. */
    char el[64];
    snprintf(el, sizeof el, "%.6f", now() - t_start);
    setenv("EMPOWER_ELAPSED", el, 1);
    unsetenv("JULIA_PROJECT");
    unsetenv("JULIA_LOAD_PATH");
    fflush(stdout);
    int rc = run(cmd);
    if (rc < 0) die("could not run %s", exe);
    /* solution.json next to each solution.m, as the cpu backend writes it. */
    for (int i = 0; i < files.n; i++) {
        char nm[1024], sm[8400], js[8400];
        case_name(files.v[i], nm, sizeof nm);
        snprintf(sm, sizeof sm, "%s/%s/solution.m", out, nm);
        snprintf(js, sizeof js, "%s/%s/solution.json", out, nm);
        if (access(sm, R_OK) == 0 && write_solution_json(sm, js) != 0) case_error("could not write %s", js);
    }
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

/* ------------------------------------------------------------------ table */

/* A table with a ruled header: align[k] is 'l' or 'r'. Box-drawing characters, or ASCII
 * on Windows, whose console may not show them. */
static void print_table(int ncol, const char **head, const char *align, int nrow, char ***cells) {
#ifdef _WIN32
    const char *H = "-", *V = "|", *TL = "+", *TM = "+", *TR = "+", *ML = "+", *MM = "+", *MR = "+",
               *BL = "+", *BM = "+", *BR = "+";
#else
    const char *H = "\u2500", *V = "\u2502", *TL = "\u250c", *TM = "\u252c", *TR = "\u2510",
               *ML = "\u251c", *MM = "\u253c", *MR = "\u2524", *BL = "\u2514", *BM = "\u2534",
               *BR = "\u2518";
#endif
    int w[32];
    for (int k = 0; k < ncol; k++) {
        w[k] = (int)strlen(head[k]);
        for (int i = 0; i < nrow; i++)
            if ((int)strlen(cells[i][k]) > w[k]) w[k] = (int)strlen(cells[i][k]);
    }
    const char *edges[3][3] = {{TL, TM, TR}, {ML, MM, MR}, {BL, BM, BR}};
    for (int pass = 0; pass < nrow + 4; pass++) {
        int rule = pass == 0 ? 0 : pass == 2 ? 1 : pass == nrow + 3 ? 2 : -1;
        printf("  ");
        if (rule >= 0) {
            for (int k = 0; k < ncol; k++) {
                fputs(edges[rule][k == 0 ? 0 : 1], stdout);
                for (int c = 0; c < w[k] + 2; c++) fputs(H, stdout);
            }
            printf("%s\n", edges[rule][2]);
        } else {
            const char **row = pass == 1 ? head : (const char **)cells[pass - 3];
            for (int k = 0; k < ncol; k++)
                printf("%s %*s ", V, align[k] == 'l' ? -w[k] : w[k], row[k]);
            printf("%s\n", V);
        }
    }
}

/* -------------------------------------------------------------------- hsl */

/* ------------------------------------------------------------------ check */

/* Run empower itself on a case, quietly (output to log). Returns its exit status. */
static int run_quiet(char *const args[], const char *log) {
#ifdef _WIN32
    (void)log;
    return run(args);
#else
    pid_t pid;
    int st;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    int rc = posix_spawn(&pid, args[0], &fa, NULL, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

/* empower check: solve case14 with each installed backend. */
static int check_install(const char *self) {
    if (!*root) die("cannot find the empower install directory");
    char tmp[8300], log[8400], outarg[8400];
    snprintf(tmp, sizeof tmp, "%s/.check", root);
    MKDIR(tmp);
    snprintf(log, sizeof log, "%s/check.log", tmp);
    snprintf(outarg, sizeof outarg, "--output=%s/out", tmp);
    int failed = 0;
    if (under_root(IPOPT_LIB)) {
        char *cmd[] = {(char *)self, "case14_ieee", "--device=cpu", outarg, NULL};
        double t0 = now();
        int rc = run_quiet(cmd, log);
        if (rc == 0) printf("  cpu:  case14 solved (%.1f s)\n", now() - t0);
        else { printf("  cpu:  case14 not solved; see %s\n", log); failed = 1; }
        fflush(stdout);
    }
#ifdef __linux__
    if (under_root("cuda/bin/empower-cuda")) {
        char *cmd[] = {(char *)self, "case14_ieee", "--device=cuda", outarg, NULL};
        snprintf(log, sizeof log, "%s/check-cuda.log", tmp);
        double t0 = now();
        if (run_quiet(cmd, log) == 0) printf("  cuda: case14 solved (%.1f s)\n", now() - t0);
        else printf("  cuda: case14 not solved (no usable GPU here?); see %s\n", log);
        fflush(stdout);
    }
#endif
    return failed;
}

/* empower hsl [LINK|FILE|LIB]: add HSL MA27 to this install, by running the installer for
 * the installed release (which downloads nothing it already has) with the HSL download
 * link, package, or library; without an argument the installer asks for it. */
static int add_hsl(const char *arg) {
    if (!*root) die("cannot find the empower install directory");
    char tag[256] = "";
    const char *rel = under_root("RELEASE");
    FILE *f = rel ? fopen(rel, "r") : NULL;
    if (f) {
        if (fgets(tag, sizeof tag, f)) tag[strcspn(tag, "\r\n")] = 0;
        fclose(f);
    }
    int cuda = under_root("cuda/bin/empower-cuda") != NULL;
    size_t n = arg ? strlen(arg) : 0;
    int is_lib = arg && ((n > 3 && !strcmp(arg + n - 3, ".so")) || strstr(arg, ".so.") ||
                         (n > 6 && !strcmp(arg + n - 6, ".dylib")) || (n > 4 && !strcmp(arg + n - 4, ".dll")));
    printf("adding HSL MA27 to empower %s in %s\n", VERSION, root);
    fflush(stdout);
#ifdef _WIN32
    _putenv_s("EMPOWER_DIR", root);
    _putenv_s("EMPOWER_YES", "1");
    _putenv_s("EMPOWER_ADD_TO_PATH", "no");
    if (*tag) _putenv_s("EMPOWER_VERSION", tag);
    if (arg) _putenv_s(is_lib ? "EMPOWER_HSL_LIB" : "EMPOWER_HSL", arg);
    (void)cuda;
    char *cmd[] = {"powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                   "irm https://raw.githubusercontent.com/madsuite-org/empower/main/install.ps1 | iex", NULL};
#else
    if (strchr(root, '\'') || strchr(tag, '\'') || (arg && strchr(arg, '\'')))
        die("cannot pass a path or link with a single quote to the installer");
    char opt[8400] = "";
    if (arg) snprintf(opt, sizeof opt, " %s='%s'", is_lib ? "--hsl-lib" : "--hsl", arg);
    char script[16400];
    snprintf(script, sizeof script,
             "curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/madsuite-org/empower/main/install.sh"
             " | EMPOWER_VERSION='%s' sh -s -- -y --path '%s' --add-to-path=no --backends=%s%s",
             *tag ? tag : "latest", root, cuda ? "cpu,cuda" : "cpu", opt);
    char *cmd[] = {"sh", "-c", script, NULL};
#endif
    int rc = run(cmd);
    if (rc != 0) die("adding HSL failed");
    return 0;
}

/* ------------------------------------------------------------------- main */

static const char *usage =
    "usage: empower up           update empower to the latest release (same backends)\n"
    "       empower download     download the whole PGLib-OPF v" PGLIB_VERSION " library (typical, api, sad)\n"
    "       empower check        solve case14 with each installed backend\n"
    "       empower json SOLUTION.m [OUT.json]\n"
    "                            a solved case as JSON (as each run writes solution.json)\n"
    "       empower hsl [LINK|FILE|LIB]\n"
    "                            add HSL MA27 to the cpu backend: the download link from\n"
    "                            HSL's email (" MA27_URL "),\n"
    "                            the package, or an HSL library; asks when not given\n"
    "       empower <case>... [--form=polar|rect|dc] [--device=cpu|cuda] [--solver=ipopt] [--options=\"NAME=VALUE ...\"]\n"
    "                [--output=DIR] [--time-limit=SECONDS] [--jobs=N] [--quiet]\n"
    "                [--hsl-lib=PATH] [--ipopt-lib=PATH] [--model-lib=PATH]\n"
    "\n"
    "Solves the AC (or DC) optimal power flow problem for one or more MATPOWER cases.\n"
    "\n"
    "  <case>       a MATPOWER file, or a PGLib-OPF case name (case118_ieee,\n"
    "               pglib_opf_case118_ieee.m), downloaded on first use; \"all\" solves\n"
    "               every PGLib-OPF v" PGLIB_VERSION " case for typical operating conditions\n"
    "  --form       formulation: polar (default) or rect (AC), or dc\n"
    "  --device     cpu (Ipopt with HSL MA27, or MUMPS without HSL) or cuda (MadNLP with cuDSS on an NVIDIA\n"
    "               GPU; Linux x86_64); the installer's --backends selects which are\n"
    "               installed. Default: cpu if installed, else cuda\n"
    "  --solver     ipopt on cpu, madnlp on cuda (the defaults, and the only ones)\n"
    "  --options    solver options separated by spaces or commas,\n"
    "               --options=\"tol=1e-8 max_iter=500\"; --options=@FILE reads one\n"
    "               \"NAME VALUE\" per line; may be repeated\n"
    "  --output     output directory (default ./empower-results)\n"
    "  --time-limit wall-clock limit per case, in seconds (the solver's max_wall_time);\n"
    "               a case that reaches it ends with status TIME_LIMIT\n"
    "  -j, --jobs   solve up to N cases at a time, each in its own process (cpu only);\n"
    "               a case's console output goes to <output>/<case>/console.log\n"
    "  -q, --quiet  print only warnings, errors, one line per case, and the summary\n"
    "  --hsl-lib    HSL library providing MA27 (and MA57, MA86, MA97 if it has them),\n"
    "               passed to Ipopt's hsllib option; default <install>/hsl, or\n"
    "               $EMPOWER_HSL_LIB, or libhsl/libcoinhsl in /usr/local/lib and other\n"
    "               standard library directories. MA27 is the linear solver unless --options sets\n"
    "               linear_solver; without HSL it is MUMPS, which is slower for OPF\n"
    "               and not for benchmarking (empower hsl adds MA27)\n"
    "  --ipopt-lib  Ipopt shared library, 3.14 or newer (default <install>/ipopt, or\n"
    "               $EMPOWER_IPOPT_LIB)\n"
    "  --model-lib  compiled ExaModelsPower library (default <install>/emp, from the release)\n"
    "\n"
    "For each case, writes <output>/<case>/solution.m (the solved case, in MATPOWER's\n"
    "savecase format), solution.json (PowerModels' result layout: per unit, radians),\n"
    "violations.csv and the solver log, and a row of <output>/summary.csv\n"
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

/* ------------------------------------------------------------------ --jobs */

/* One line per finished case, for --quiet and --jobs. */
static void progress(int k, int n, const char *name, const Result *r) {
    int ok = r->st && (!strcmp(r->st, "LOCALLY_SOLVED") || !strcmp(r->st, "ALMOST_LOCALLY_SOLVED"));
    if (!r->st) printf("  [%d/%d] %-32s %snot solved%s\n", k, n, name, RED, RST);
    else
        printf("  [%d/%d] %-32s %s%-22s%s objective %-14.8g %8.3f s\n", k, n, name, ok ? GREEN : RED,
               r->st, RST, r->obj, r->t_build + r->t_solve);
    fflush(stdout);
}

/* The data row of a worker's summary file, into r. Returns 0 when there is one. */
static int read_worker_row(const char *path, char *row, size_t cap, Result *r) {
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    char head[4096];
    int got = fgets(head, sizeof head, f) && fgets(row, (int)cap, f);
    fclose(f);
    if (!got) return 1;
    /* Fields after the case name, which may be quoted. */
    const char *p = row;
    if (*p == '"') {
        for (p++; *p && !(*p == '"' && p[1] != '"'); p += (*p == '"') ? 2 : 1) {}
        if (*p) p++;
    } else
        while (*p && *p != ',') p++;
    char *fields[32];
    int nf = 0;
    static char copy[8192];
    snprintf(copy, sizeof copy, "%s", *p == ',' ? p + 1 : p);
    copy[strcspn(copy, "\r\n")] = 0;
    for (char *s = copy; nf < 32;) {
        fields[nf++] = s;
        char *c = strchr(s, ',');
        if (!c) break;
        *c = 0;
        s = c + 1;
    }
    if (nf < 14) return 1;
    /* form,termination_status,raw_status,objective,iterations,nvar,ncon,build,solve,
     * callbacks,ipopt,init,linear_algebra,violation[,6 groups] */
    r->st = strdup(fields[1]);
    r->obj = atof(fields[3]);
    r->iters = atoi(fields[4]);
    r->n = atoi(fields[5]);
    r->m = atoi(fields[6]);
    r->t_build = atof(fields[7]);
    r->t_solve = atof(fields[8]);
    r->t_cb = atof(fields[9]);
    r->t_init = atof(fields[11]);
    r->t_la = *fields[12] && strcmp(fields[12], "nan") ? atof(fields[12]) : NAN;
    r->viol = atof(fields[13]);
    r->has_groups = nf >= 14 + NGROUPS && *fields[14];
    for (int k = 0; k < NGROUPS && r->has_groups; k++) r->vg[k] = atof(fields[14 + k]);
    return 0;
}

#ifdef _WIN32
typedef intptr_t Proc;
#else
typedef pid_t Proc;
#endif

/* Start a program without waiting; returns 0 when it could not be started. */
static Proc spawn_worker(char *const args[]) {
#ifdef _WIN32
    int n = 0;
    while (args[n]) n++;
    char **q = xmalloc((n + 1) * sizeof *q);
    for (int i = 0; i < n; i++) {
        size_t L = strlen(args[i]) + 3;
        q[i] = xmalloc(L);
        snprintf(q[i], L, "\"%s\"", args[i]);
    }
    q[n] = NULL;
    intptr_t h = _spawnv(_P_NOWAIT, args[0], (const char *const *)q);
    for (int i = 0; i < n; i++) free(q[i]);
    free(q);
    return h == -1 ? 0 : h;
#else
    pid_t pid;
    return posix_spawnp(&pid, args[0], NULL, NULL, args, environ) == 0 ? pid : 0;
#endif
}

/* Wait for one of the running workers to exit; returns its index in procs. */
static int wait_worker(Proc *procs, int n) {
#ifdef _WIN32
    HANDLE h[MAXJOBS];
    int idx[MAXJOBS], m = 0;
    for (int k = 0; k < n; k++)
        if (procs[k]) { h[m] = (HANDLE)procs[k]; idx[m++] = k; }
    DWORD w = WaitForMultipleObjects(m, h, FALSE, INFINITE);
    if (w >= WAIT_OBJECT_0 + (DWORD)m) die("waiting for the workers failed");
    int k = idx[w - WAIT_OBJECT_0];
    CloseHandle(h[w - WAIT_OBJECT_0]);
    return k;
#else
    for (;;) {
        int st;
        pid_t p = waitpid(-1, &st, 0);
        if (p < 0) {
            if (errno == EINTR) continue;
            die("waiting for the workers failed");
        }
        for (int k = 0; k < n; k++)
            if (procs[k] == p) return k;
    }
#endif
}


/* -------------------------------------------------------------- libempower */

/* libempower runs the empower executable of its install for each solve, as a child
 * process, and reads the solution back. The Julia runtime and the libraries it brings
 * (OpenSSL among them) then never meet the ones the calling program has loaded: in the
 * same process, a system OpenSSL loaded first makes the runtime abort. */
#ifdef EMPOWER_LIB
#include "empower.h"

struct empower {
    char exe[4200];
};

typedef struct {
    empower_result r;   /* first: the pointer handed out is to this */
    MpCase mc;
    double *cols;
    char *case_name, *form, *status, *dir;
} Owned;

#define API_BEGIN(fail)                     \
    api_error[0] = 0;                       \
    if (setjmp(api_jmp)) { api_active = 0; return fail; } \
    api_active = 1
#define API_END(v) do { api_active = 0; return v; } while (0)

EMPOWER_API const char *empower_error(void) { return api_error; }
EMPOWER_API const char *empower_version(void) { return VERSION; }

EMPOWER_API empower *empower_open(const char *install) {
    API_BEGIN(NULL);
    if (install) snprintf(root, sizeof root, "%s", install);
    else find_root();
    if (!*root) die("cannot find the empower install directory");
#ifdef _WIN32
    const char *exe = under_root("bin/empower.exe");
#else
    const char *exe = under_root("bin/empower");
#endif
    if (!exe) die("no empower executable in %s/bin", root);
    empower *e = xcalloc(1, sizeof *e);
    snprintf(e->exe, sizeof e->exe, "%s", exe);
    API_END(e);
}

/* Remove dir/<case>, its files, and dir (or what of them exists). */
static void remove_run_dir(const char *dir, const char *name) {
    static const char *files[] = {"solution.m", "solution.json", "ipopt.log", "madnlp.log",
                                  "violations.csv", "console.log"};
    char p[8600];
    for (size_t k = 0; k < sizeof files / sizeof *files; k++) {
        if (name) {
            snprintf(p, sizeof p, "%s/%s/%s", dir, name, files[k]);
            unlink(p);
        }
    }
    if (name) {
        snprintf(p, sizeof p, "%s/%s", dir, name);
        rmdir(p);
    }
    static const char *top[] = {"summary.csv", "system.txt", "console.log", "violations.csv"};
    for (size_t k = 0; k < sizeof top / sizeof *top; k++) {
        snprintf(p, sizeof p, "%s/%s", dir, top[k]);
        unlink(p);
    }
    rmdir(dir);
}

static char *temp_dir(void) {
#ifdef _WIN32
    char base[MAX_PATH], path[MAX_PATH + 64];
    if (!GetTempPathA(sizeof base, base)) die("no temporary directory");
    srand((unsigned)GetTickCount());
    for (int k = 0; k < 100; k++) {
        snprintf(path, sizeof path, "%sempower-%lu-%d", base, (unsigned long)GetCurrentProcessId(), rand());
        if (_mkdir(path) == 0) return strdup(path);
    }
    die("cannot create a temporary directory");
#else
    const char *t = getenv("TMPDIR");
    char path[4096];
    snprintf(path, sizeof path, "%s/empower-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(path)) die("cannot create a temporary directory in %s", t && *t ? t : "/tmp");
    return strdup(path);
#endif
}

/* Column col (1-based) of table t, scaled; zeros where a row is shorter. */
static const double *column(Owned *o, MpTable *t, int col, double scale, int *used) {
    double *c = o->cols + *used;
    *used += t->n;
    for (int i = 0; i < t->n; i++) c[i] = mp_at(t, i, col) * scale;
    return c;
}

/* The errors the run printed ("error: ..." lines of its console log), into api_error. */
static void run_errors(const char *log) {
    FILE *f = fopen(log, "r");
    api_error[0] = 0;
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "error: ", 7)) continue;
        size_t L = strlen(api_error);
        snprintf(api_error + L, sizeof api_error - L, "%s", line + 7);
    }
    fclose(f);
    size_t L = strlen(api_error);
    while (L && (api_error[L - 1] == '\n' || api_error[L - 1] == '\r')) api_error[--L] = 0;
}

EMPOWER_API empower_result *empower_solve(empower *e, const char *casefile, const char *form_,
                                          const char *device_, const char *options, const char *output) {
    API_BEGIN(NULL);
    if (!e) die("empower_solve: no empower (did empower_open fail?)");
    if (!casefile || !*casefile) die("empower_solve: no case");
    const char *form = form_ ? form_ : "polar", *device = device_ ? device_ : "cpu";
    char *dir = output ? strdup(output) : temp_dir();
    MKDIR(dir);
    char a_form[64], a_dev[64], a_out[8300], a_log[8400], a_csv[8400], *a_opt = NULL;
    snprintf(a_form, sizeof a_form, "--form=%s", form);
    snprintf(a_dev, sizeof a_dev, "--device=%s", device);
    snprintf(a_out, sizeof a_out, "--output=%s", dir);
    snprintf(a_log, sizeof a_log, "--log=%s/console.log", dir);
    snprintf(a_csv, sizeof a_csv, "%s/summary.csv", dir);
    char *cmd[10];
    int n = 0;
    cmd[n++] = e->exe;
    cmd[n++] = (char *)casefile;
    cmd[n++] = a_form;
    cmd[n++] = a_dev;
    cmd[n++] = a_out;
    cmd[n++] = a_log;
    cmd[n++] = "--quiet";
    if (options && *options) {
        size_t L = strlen(options) + 16;
        a_opt = xmalloc(L);
        snprintf(a_opt, L, "--options=%s", options);
        cmd[n++] = a_opt;
    }
    cmd[n] = NULL;
    unlink(a_csv);
    int rc = run(cmd);
    free(a_opt);
    if (rc < 0) {
        free(dir);
        die("could not run %s", e->exe);
    }
    Result res = {0};
    char row[8192];
    snprintf(a_log, sizeof a_log, "%s/console.log", dir);
    if (read_worker_row(a_csv, row, sizeof row, &res) != 0) {
        run_errors(a_log);
        if (!*api_error) snprintf(api_error, sizeof api_error, "%s could not be solved (empower exited with %d)", casefile, rc);
        if (!output) remove_run_dir(dir, NULL);
        free(dir);
        API_END(NULL);
    }
    /* The case name: the first field of the row. */
    char name[8192];
    snprintf(name, sizeof name, "%s", row[0] == '"' ? row + 1 : row);
    name[strcspn(name, row[0] == '"' ? "\"" : ",")] = 0;
    char sm[8600];
    snprintf(sm, sizeof sm, "%s/%s/solution.m", dir, name);
    Owned *o = xcalloc(1, sizeof *o);
    if (read_matpower(sm, &o->mc) != 0) {
        free(o);
        if (!output) remove_run_dir(dir, name);
        free(dir);
        die("could not read the solution %s", sm);
    }
    MpCase *mc = &o->mc;
    o->case_name = strdup(name);
    o->form = strdup(form);
    o->status = (char *)res.st;   /* strdup'ed by read_worker_row */
    empower_result *r = &o->r;
    r->case_name = o->case_name;
    r->form = o->form;
    r->status = o->status;
    r->iterations = res.iters;
    r->nvar = res.n;
    r->ncon = res.m;
    r->objective = res.obj;
    r->constraint_violation = res.viol;
    r->time_build = res.t_build;
    r->time_solve = res.t_solve;
    r->time_evals = res.t_cb;
    r->time_init = res.t_init;
    r->time_linear_algebra = res.t_la;
    r->base_mva = mc->base;
    r->nbus = mc->bus.n;
    r->ngen = mc->gen.n;
    r->nbranch = mc->branch.n;
    o->cols = xcalloc(5 * mc->bus.n + 3 * mc->gen.n + 6 * mc->branch.n + 1, sizeof(double));
    int u = 0;
    double b = 1 / mc->base, deg = M_PI / 180;
    r->bus_i = column(o, &mc->bus, 1, 1, &u);
    r->vm = column(o, &mc->bus, 8, 1, &u);
    r->va = column(o, &mc->bus, 9, deg, &u);
    r->lam_p = column(o, &mc->bus, 14, 1, &u);
    r->lam_q = column(o, &mc->bus, 15, 1, &u);
    r->gen_bus = column(o, &mc->gen, 1, 1, &u);
    r->pg = column(o, &mc->gen, 2, b, &u);
    r->qg = column(o, &mc->gen, 3, b, &u);
    r->f_bus = column(o, &mc->branch, 1, 1, &u);
    r->t_bus = column(o, &mc->branch, 2, 1, &u);
    r->pf = column(o, &mc->branch, 14, b, &u);
    r->qf = column(o, &mc->branch, 15, b, &u);
    r->pt = column(o, &mc->branch, 16, b, &u);
    r->qt = column(o, &mc->branch, 17, b, &u);
    if (output) {
        char d[8600];
        snprintf(d, sizeof d, "%s/%s", dir, name);
        o->dir = strdup(d);
        r->dir = o->dir;
    } else
        remove_run_dir(dir, name);
    free(dir);
    API_END(r);
}

static void free_table(MpTable *t) {
    for (int i = 0; i < t->n; i++) free(t->row[i]);
    free(t->row);
    free(t->len);
}

EMPOWER_API void empower_free_result(empower_result *r) {
    if (!r) return;
    Owned *o = (Owned *)r;
    free_table(&o->mc.bus);
    free_table(&o->mc.gen);
    free_table(&o->mc.branch);
    free(o->cols);
    free(o->case_name);
    free(o->form);
    free(o->status);
    free(o->dir);
    free(o);
}

EMPOWER_API void empower_close(empower *e) { free(e); }
#endif /* EMPOWER_LIB */

#ifndef EMPOWER_LIB
int main(int argc, char **argv) {
    t_start = now();
    init_style();
    find_root();
    if (argc == 2 && (!strcmp(argv[1], "up") || !strcmp(argv[1], "update"))) return self_update();
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "hsl")) return add_hsl(argc == 3 ? argv[2] : NULL);
    if (argc == 2 && !strcmp(argv[1], "download")) return download_pglib();
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "json")) {
        char out[8300];
        if (argc == 4) snprintf(out, sizeof out, "%s", argv[3]);
        else {
            snprintf(out, sizeof out, "%s", argv[2]);
            char *dot = strrchr(out, '.');
            if (dot && !strchr(dot, '/')) *dot = 0;
            strncat(out, ".json", sizeof out - strlen(out) - 1);
        }
        if (write_solution_json(argv[2], out) != 0) die("cannot read %s as a solved MATPOWER case", argv[2]);
        printf("wrote %s\n", out);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "check")) {
#ifdef _WIN32
        const char *self = under_root("bin/empower.exe");
#else
        const char *self = under_root("bin/empower");
#endif
        return check_install(self ? self : argv[0]);
    }
    const char *modellib = getenv("EMPOWER_MODEL_LIB"), *ipoptlib = getenv("EMPOWER_IPOPT_LIB"),
               *hsllib = getenv("EMPOWER_HSL_LIB"),
               *form = "polar", *solver = NULL, *device = NULL, *out = "empower-results", *v;
    List args = {0};
    int check_hsl = 0, jobs = 1;
    const char *summary_csv = NULL, *log = NULL;
    List pass = {0};   /* the options a --jobs worker gets as they are */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int i0 = i;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(usage, stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            puts("empower " VERSION);
            return 0;
        }
        if (!strcmp(a, "--check-hsl")) { check_hsl = 1; continue; }
        if (!strcmp(a, "--quiet") || !strcmp(a, "-q")) { quiet = 1; continue; }
        if ((v = flag(a, "--form", &i, argc, argv))) form = v;
        else if ((v = flag(a, "--device", &i, argc, argv))) device = v;
        else if ((v = flag(a, "--solver", &i, argc, argv))) solver = v;
        else if ((v = flag(a, "--options", &i, argc, argv))) parse_options(v);
        else if ((v = flag(a, "--output", &i, argc, argv))) out = v;
        else if ((v = flag(a, "--ipopt-lib", &i, argc, argv))) ipoptlib = v;
        else if ((v = flag(a, "--hsl-lib", &i, argc, argv))) hsllib = v;
        else if ((v = flag(a, "--model-lib", &i, argc, argv))) modellib = v;
        else if ((v = flag(a, "--time-limit", &i, argc, argv))) {
            char *e;
            double t = strtod(v, &e);
            if (*e || t <= 0) die("--time-limit needs a number of seconds, got %s", v);
            /* A real number for Ipopt, which checks the option's type ("60.0", not "60"). */
            char tl[64];
            snprintf(tl, sizeof tl, "%.17g", t);
            if (!strpbrk(tl, ".en")) strcat(tl, ".0");
            add_opt("max_wall_time", tl);
        }
        else if ((v = flag(a, "--jobs", &i, argc, argv)) || (v = flag(a, "-j", &i, argc, argv))) {
            jobs = atoi(v);
            if (jobs < 1) die("--jobs needs a positive number, got %s", v);
        }
        /* For --jobs workers: where to write the summary row, and the console output. */
        else if ((v = flag(a, "--summary-csv", &i, argc, argv))) { summary_csv = v; continue; }
        else if ((v = flag(a, "--log", &i, argc, argv))) { log = v; continue; }
        else if (a[0] == '-') { fputs(usage, stderr); die("unknown option %s", a); }
        else { push(&args, a); continue; }
        if (strcmp(a, "-j") && strncmp(a, "--jobs", 6) && strncmp(a, "--output", 8))
            for (int k = i0; k <= i; k++) push(&pass, argv[k]);
    }
    if (log) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) die("cannot write %s", log);
        fflush(stdout);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
#ifdef _WIN32
        /* The Julia runtime writes to the Windows handles, not the C library's. */
        SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(1));
        SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(2));
#endif
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
    if (cuda) {
        if (jobs > 1) die("--jobs is for --device=cpu; the cuda backend solves one case at a time on the GPU");
        return run_cuda(&args, form, out, argc, argv);
    }
    if (!modellib) modellib = under_root(MODEL_LIB);
    if (!modellib) die("no model library: pass --model-lib or set EMPOWER_MODEL_LIB");
    if (!ipoptlib) ipoptlib = under_root(IPOPT_LIB);
    if (!ipoptlib)
        die("the cpu backend (Ipopt) is not installed: rerun the installer with --backends=cpu\n"
            "(or pass --ipopt-lib, or set EMPOWER_IPOPT_LIB)");
    if (hsllib && !*hsllib) hsllib = NULL;
    if (!hsllib) hsllib = under_root(HSL_LIB);
    if (!hsllib && (hsllib = system_hsl()) != NULL) hsl_system = 1;
    if (hsllib && access(hsllib, R_OK) != 0) die("HSL library not found: %s", hsllib);
    C.hsllib = hsllib;

    char src[512];
    if (!quiet) printf("%sempower %s%s  " DOT "  %s form  " DOT "  Ipopt\n", BOLD, VERSION, RST, form);
    kv("model", "%s", modellib);
    kv("ipopt", "%s", ipoptlib);
    if (read_source("ipopt", src, sizeof src)) kv("", "%s", src);
    kv("hsl", "%s%s", hsllib ? hsllib : "(none)", hsl_system ? " (found on this machine)" : "");
    if (hsllib && read_source("hsl", src, sizeof src)) kv("", "%s", src);
    kv("output", "%s", out);
    for (int k = 0; k < nopts; k++) kv("option", "%s", opts[k]);

    List cases = {0};
    for (int i = 0; i < args.n; i++) {
        if (!strcmp(args.v[i], "all")) add_all(&cases);
        else push(&cases, args.v[i]);
    }
    if (jobs > cases.n) jobs = cases.n;
    if (jobs > MAXJOBS) jobs = MAXJOBS;
    if (jobs > 1) kv("jobs", "%d cases at a time, each in its own process (console output in <case>/console.log)", jobs);

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
    if (!check_hsl && jobs <= 1) {   /* --jobs: the workers load it */
        step("loading model library");
        C.hm = lib_open(modellib);
        if (!C.hm) die("cannot load model library %s: %s", modellib, lib_error());
        step("model library loaded (%.3f s)", now() - t0);
    }

    step("loading Ipopt");
    t0 = now();
    load_ipopt(&C.I, ipoptlib);
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
    if (!C.linear_solver) choose_linear_solver(&C);
    step("linear solver: %s", C.linear_solver);

    MKDIR(out);
    if (!summary_csv) write_system(out, ipoptlib, C.hsllib, argc, argv);
    char path[8192];
    snprintf(path, sizeof path, "%s", summary_csv ? summary_csv : "");
    if (!summary_csv) snprintf(path, sizeof path, "%s/summary.csv", out);
    /* A fresh summary per run: it describes this run only, like system.txt. */
    C.summary = fopen(path, "w");
    if (!C.summary) die("cannot write %s", path);
    fputs("case,form,termination_status,raw_status,objective,iterations,nvar,ncon,"
          "time_parse_and_build,time_solve,time_solve_callbacks,time_solve_ipopt,time_solve_init,"
          "time_solve_linear_algebra,constraint_violation,"
          "violation_power_balance,violation_power_flow,violation_branch_flow,"
          "violation_angle_difference,violation_voltage,violation_generation\n",
          C.summary);

    int solved = 0, failed = 0;
    Result *res = xcalloc(cases.n, sizeof *res);
    int *ok = xcalloc(cases.n, sizeof *ok);
    char **names = xcalloc(cases.n, sizeof *names);
    if (jobs <= 1)
        for (int i = 0; i < cases.n; i++) {
            char cf[8192], nm[1024];
            heading("[%d/%d] %s", i + 1, cases.n, cases.v[i]);
            fflush(stdout);
            int st = resolve_case(cases.v[i], cf, sizeof cf);
            case_name(st == 0 ? cf : cases.v[i], nm, sizeof nm);
            names[i] = strdup(nm);
            if (st == 0 && solve_case(&C, cf, out, &res[i]) == 0) { ok[i] = 1; solved++; }
            else failed++;
            if (quiet && !summary_csv) progress(i + 1, cases.n, nm, &res[i]);
        }
    else {
        /* --jobs: each case in a worker, `empower <case> --quiet` with this run's options,
         * at most jobs at a time; the parent collects their summary rows. */
        char jd[8400];
        snprintf(jd, sizeof jd, "%s/.jobs", out);
        MKDIR(jd);
#ifdef _WIN32
        const char *self = under_root("bin/empower.exe");
#else
        const char *self = under_root("bin/empower");
#endif
        if (!self) self = argv[0];
        char lsopt[600];
        int user_ls = 0;
        for (int k = 0; k < nopts; k++) user_ls |= !strncmp(opts[k], "linear_solver=", 14);
        /* The linear solver chosen here; the workers need not probe for MA27 again. */
        snprintf(lsopt, sizeof lsopt, "--options=linear_solver=%s", C.linear_solver);
        /* One thread per worker, for the BLAS that MUMPS uses: N workers on N cores. */
#ifdef _WIN32
        if (!getenv("OPENBLAS_NUM_THREADS")) _putenv_s("OPENBLAS_NUM_THREADS", "1");
        if (!getenv("OMP_NUM_THREADS")) _putenv_s("OMP_NUM_THREADS", "1");
#else
        if (!getenv("OPENBLAS_NUM_THREADS")) setenv("OPENBLAS_NUM_THREADS", "1", 1);
        if (!getenv("OMP_NUM_THREADS")) setenv("OMP_NUM_THREADS", "1", 1);
#endif
        Proc procs[MAXJOBS] = {0};
        int slot_case[MAXJOBS], next = 0, running = 0, done = 0;
        char **csvs = xcalloc(cases.n, sizeof *csvs);
        while (next < cases.n || running) {
            while (running < jobs && next < cases.n) {
                int i = next++;
                char cf[8192], nm[1024], buf[8600];
                int st = resolve_case(cases.v[i], cf, sizeof cf);
                case_name(st == 0 ? cf : cases.v[i], nm, sizeof nm);
                names[i] = strdup(nm);
                if (st != 0) { failed++; progress(++done, cases.n, nm, &res[i]); continue; }
                snprintf(buf, sizeof buf, "%s/%s", out, nm);
                MKDIR(buf);
                char **cmd = xcalloc(pass.n + 8, sizeof *cmd);
                int n = 0;
                cmd[n++] = (char *)self;
                cmd[n++] = strdup(cf);
                snprintf(buf, sizeof buf, "--output=%s", out);
                cmd[n++] = strdup(buf);
                snprintf(buf, sizeof buf, "%s/%d.csv", jd, i);
                csvs[i] = strdup(buf);
                unlink(buf);
                snprintf(buf, sizeof buf, "--summary-csv=%s", csvs[i]);
                cmd[n++] = strdup(buf);
                snprintf(buf, sizeof buf, "--log=%s/%s/console.log", out, nm);
                cmd[n++] = strdup(buf);
                cmd[n++] = "--quiet";
                if (!user_ls) cmd[n++] = lsopt;
                for (int k = 0; k < pass.n; k++) cmd[n++] = pass.v[k];
                cmd[n] = NULL;
                int slot = 0;
                while (procs[slot]) slot++;
                fflush(stdout);
                procs[slot] = spawn_worker(cmd);
                if (!procs[slot]) die("could not run %s", self);
#ifndef _WIN32
                workers[slot] = procs[slot];
                if (running == 0) {   /* Ctrl-C kills the workers too */
                    struct sigaction sa;
                    memset(&sa, 0, sizeof sa);
                    sa.sa_handler = on_signal;
                    sigemptyset(&sa.sa_mask);
                    sigaction(SIGINT, &sa, NULL);
                    sigaction(SIGTERM, &sa, NULL);
                    sigaction(SIGHUP, &sa, NULL);
                }
#endif
                slot_case[slot] = i;
                running++;
            }
            if (!running) break;
            int slot = wait_worker(procs, jobs);
            int i = slot_case[slot];
            procs[slot] = 0;
#ifndef _WIN32
            workers[slot] = 0;
#endif
            running--;
            char row[8192];
            if (read_worker_row(csvs[i], row, sizeof row, &res[i]) == 0) {
                fputs(row, C.summary);
                if (!strchr(row, '\n')) fputc('\n', C.summary);
                if (!strcmp(res[i].st, "LOCALLY_SOLVED") || !strcmp(res[i].st, "ALMOST_LOCALLY_SOLVED")) {
                    ok[i] = 1;
                    solved++;
                } else failed++;
                unlink(csvs[i]);
            } else {
                memset(&res[i], 0, sizeof res[i]);
                failed++;
            }
            progress(++done, cases.n, names[i], &res[i]);
        }
        rmdir(jd);
    }
    fclose(C.summary);
    if (summary_csv) return failed ? 1 : 0;

    /* The runs at a glance, as a table; the full record is summary.csv. */
    enum { NCOL = 11 };
    const char *head[NCOL] = {"case", "status", "objective", "max viol", "iter",
                              "build", "init", "solve", "evals", "lin alg", "other"};
    char ***cells = xmalloc(cases.n * sizeof *cells);
    for (int i = 0; i < cases.n; i++) {
        Result *r = &res[i];
        char v[NCOL][64];
        if (r->n == 0) {   /* never reached the solver */
            snprintf(v[1], 64, "not solved");
            for (int k = 2; k < NCOL; k++) snprintf(v[k], 64, "-");
        } else {
            snprintf(v[1], 64, "%s", r->st);
            snprintf(v[2], 64, "%.8g", r->obj);
            snprintf(v[3], 64, "%.2e", r->viol);
            snprintf(v[4], 64, "%d", r->iters);
            snprintf(v[5], 64, "%.3f", r->t_build);
            snprintf(v[6], 64, "%.3f", r->t_init);
            snprintf(v[7], 64, "%.3f", r->t_solve);
            snprintf(v[8], 64, "%.3f", r->t_cb);
            if (isnan(r->t_la)) {
                snprintf(v[9], 64, "-");
                snprintf(v[10], 64, "%.3f", r->t_solve - r->t_cb);
            } else {
                snprintf(v[9], 64, "%.3f", r->t_la);
                snprintf(v[10], 64, "%.3f", r->t_solve - r->t_cb - r->t_la);
            }
        }
        cells[i] = xmalloc(NCOL * sizeof(char *));
        cells[i][0] = names[i];
        for (int k = 1; k < NCOL; k++) cells[i][k] = strdup(v[k]);
    }
    heading("Summary");
    kv("written", "%s", path);
    print_table(NCOL, head, "llrrrrrrrrr", cases.n, cells);
    /* The violations by group, when the model library reports them. */
    int ng = 0;
    for (int i = 0; i < cases.n; i++) ng += res[i].has_groups;
    if (ng) {
        const char *gh[NGROUPS + 1] = {"case"};
        for (int k = 0; k < NGROUPS; k++) gh[k + 1] = group_head[k];
        char ***gc = xmalloc(cases.n * sizeof *gc);
        for (int i = 0; i < cases.n; i++) {
            gc[i] = xmalloc((NGROUPS + 1) * sizeof(char *));
            gc[i][0] = names[i];
            for (int k = 0; k < NGROUPS; k++) {
                char b[32];
                if (res[i].has_groups) snprintf(b, sizeof b, "%.2e", res[i].vg[k]);
                else snprintf(b, sizeof b, "-");
                gc[i][k + 1] = strdup(b);
            }
        }
        printf("  violations by group, max(l - v, v - u, 0) over each group's constraints and bounds:\n");
        print_table(NGROUPS + 1, gh, "lrrrrrr", cases.n, gc);
    }
    printf("  %s%d solved%s, %s%d not solved%s, %.1f s in total\n", solved ? GREEN : "", solved, RST,
           failed ? RED : "", failed, RST, now() - t_start);
    printf("  %sIpopt with %s. max viol: max(cl - c(x), c(x) - cu) at the solution. Times in seconds:\n"
           "  solve = evals (model functions and derivatives) + lin alg (%s) + other (the rest of\n"
           "  Ipopt); init, the time to the first iteration, is part of solve.%s\n",
           DIM, C.linear_solver, C.linear_solver, RST);
    if (no_hsl)
        printf("  %sMUMPS, without HSL: not for benchmarking; `empower hsl` adds MA27 (see the warning above)%s\n",
               YELLOW, RST);
    return failed ? 1 : 0;
}
#endif /* EMPOWER_LIB */
