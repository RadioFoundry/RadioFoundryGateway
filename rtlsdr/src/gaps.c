// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rtlsdr/gaps.h"

#include <string.h>

void rtl_gaps_reset(rtl_gaps_t *g, uint32_t max_fill)
{
    memset(g, 0, sizeof *g);
    g->max_fill = max_fill;
}

void rtl_gaps_note(rtl_gaps_t *g, uint64_t at, uint32_t len)
{
    g->lost += len;
    if (g->count) {
        int last = (g->head + g->count - 1) % RTL_GAPS_MAX;
        /* nothing written since the last gap (or no room for another): one longer gap */
        if (g->at[last] == at || g->count == RTL_GAPS_MAX) {
            uint32_t room = g->max_fill - g->len[last];
            g->len[last] += len < room ? len : room;
            return;
        }
    }
    int i = (g->head + g->count) % RTL_GAPS_MAX;
    g->at[i] = at;
    g->len[i] = len < g->max_fill ? len : g->max_fill;
    g->count++;
}

size_t rtl_gaps_next(rtl_gaps_t *g, uint64_t pos, size_t want, bool *fill)
{
    while (g->count && !g->len[g->head]) { /* a gap filled to the cap (or of nothing): done */
        g->head = (g->head + 1) % RTL_GAPS_MAX;
        g->count--;
    }
    if (!g->count) {
        *fill = false;
        return want;
    }
    uint64_t at = g->at[g->head];
    if (pos >= at) {
        *fill = true;
        return g->len[g->head] < want ? g->len[g->head] : want;
    }
    *fill = false;
    return at - pos < want ? (size_t)(at - pos) : want;
}

void rtl_gaps_took(rtl_gaps_t *g, size_t n)
{
    if (!g->count)
        return;
    uint32_t *len = &g->len[g->head];
    *len -= n < *len ? (uint32_t)n : *len;
    g->filled += n;
    if (!*len) {
        g->head = (g->head + 1) % RTL_GAPS_MAX;
        g->count--;
    }
}
