// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "netclock_select.h"

#include <stdlib.h>

typedef struct {
    double v;
    int type;  // -1 = interval start, +1 = end (starts sort first on ties: touching intervals overlap)
} edge_t;

static int cmp_edge(const void *a, const void *b)
{
    const edge_t *x = a, *y = b;
    if (x->v != y->v) {
        return x->v < y->v ? -1 : 1;
    }
    return x->type - y->type;
}

int nc_select(const nc_cand_t *c, int n, char *mark, double *off_us)
{
    edge_t e[2 * NC_SELECT_MAX];
    int ne = 0, nok = 0;
    if (n > NC_SELECT_MAX) {
        n = NC_SELECT_MAX;
    }
    for (int i = 0; i < n; i++) {
        mark[i] = NC_MARK_NONE;
        if (!c[i].ok) {
            continue;
        }
        e[ne++] = (edge_t){c[i].off_us - c[i].lambda_us, -1};
        e[ne++] = (edge_t){c[i].off_us + c[i].lambda_us, +1};
        nok++;
    }
    if (nok == 0) {
        return -1;
    }
    qsort(e, ne, sizeof(edge_t), cmp_edge);
    // Sweep: the first stretch where the most intervals overlap.
    int cnt = 0, best = 0;
    double lo = 0, hi = 0;
    for (int k = 0; k < ne; k++) {
        if (e[k].type < 0) {
            if (++cnt > best) {
                best = cnt;
                lo = e[k].v;
                hi = e[k + 1].v;  // an end always follows somewhere; the next edge bounds this stretch
            }
        } else {
            cnt--;
        }
    }
    bool majority = 2 * best > nok;
    if (!majority && nok >= 3) {
        for (int i = 0; i < n; i++) {
            if (c[i].ok) {
                mark[i] = NC_MARK_FALSE;
            }
        }
        return -1;
    }
    // One or two sources that don't agree: trust the tighter one alone.
    int bi = -1;
    if (!majority) {
        for (int i = 0; i < n; i++) {
            if (c[i].ok && (bi < 0 || c[i].lambda_us < c[bi].lambda_us)) {
                bi = i;
            }
        }
        lo = c[bi].off_us - c[bi].lambda_us;
        hi = c[bi].off_us + c[bi].lambda_us;
    }
    double sw = 0, so = 0;
    bi = -1;
    for (int i = 0; i < n; i++) {
        if (!c[i].ok) {
            continue;
        }
        bool in = c[i].off_us - c[i].lambda_us <= hi && c[i].off_us + c[i].lambda_us >= lo;
        mark[i] = in ? NC_MARK_COMBINED : NC_MARK_FALSE;
        if (in) {
            double w = 1.0 / (c[i].lambda_us * c[i].lambda_us);
            sw += w;
            so += w * c[i].off_us;
            if (bi < 0 || c[i].lambda_us < c[bi].lambda_us) {
                bi = i;
            }
        }
    }
    mark[bi] = NC_MARK_BEST;
    *off_us = so / sw;
    return bi;
}
