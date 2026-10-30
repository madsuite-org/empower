/* The C API against an install: case14 and case118 in each form, an unknown case, and a
 * kept output directory. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "empower.h"

int main(int argc, char **argv) {
    const char *device = argc > 1 ? argv[1] : "cpu";
    const char *options = argc > 2 ? argv[2] : NULL;
    empower *e = empower_open(NULL);
    if (!e) { fprintf(stderr, "empower_open: %s\n", empower_error()); return 1; }
    int bad = 0;
    const char *forms[] = {"polar", "rect", "dc"};
    double ref[3] = {97214, 97214, 93101};
    for (int f = 0; f < 3; f++) {
        empower_result *r = empower_solve(e, "case118_ieee", forms[f], device, options, NULL);
        if (!r) { fprintf(stderr, "empower_solve: %s\n", empower_error()); return 1; }
        printf("%s %s %s objective %.8g, %d iterations, %d buses, vm[0] %.6f, pg[0] %.6f, pf[0] %.6f\n",
               r->case_name, r->form, r->status, r->objective, r->iterations, r->nbus, r->vm[0],
               r->pg[0], r->pf[0]);
        if (strcmp(r->status, "LOCALLY_SOLVED") || fabs(r->objective - ref[f]) > 1e-4 * ref[f] ||
            r->nbus != 118 || r->ngen != 54 || r->nbranch != 186 || r->dir) bad = 1;
        empower_free_result(r);
    }
    empower_result *r = empower_solve(e, "no_such_case_xyz", NULL, device, options, NULL);
    printf("unknown case: %s\n", empower_error());
    if (r || !*empower_error()) bad = 1;
    r = empower_solve(e, "case14_ieee", NULL, device, options, "api-out");
    if (!r || !r->dir) bad = 1;
    else printf("kept %s\n", r->dir);
    empower_free_result(r);
    empower_close(e);
    puts(bad ? "C API: FAILED" : "C API: ok");
    return bad;
}
