// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* ID sets (rdof/idset.h). */
#include "rdof/idset.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rdof_idset_clear(rdof_idset_t *s) { memset(s, 0, sizeof *s); }

static bool err_(char *err, size_t cap, const char *what, const char *item)
{
    if (err && cap)
        snprintf(err, cap, "%s: %s", item, what);
    return false;
}

static int cmp(const void *a, const void *b)
{
    const rdof_idrange_t *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* Sorts and merges n ranges in place (touching ones too); returns how many are left. */
static int merge(rdof_idrange_t *r, int n)
{
    if (n < 2)
        return n;
    qsort(r, (size_t)n, sizeof *r, cmp);
    int m = 0;
    for (int k = 1; k < n; k++) {
        if (r[k].lo <= r[m].hi + 1) {
            if (r[k].hi > r[m].hi)
                r[m].hi = r[k].hi;
        } else {
            r[++m] = r[k];
        }
    }
    return m + 1;
}

static void tidy(rdof_idset_t *s)
{
    rdof_idrange_t in[RDOF_IDSET_MAX], out[RDOF_IDSET_MAX];
    memcpy(in, s->r, s->nin * sizeof *in);
    memcpy(out, s->r + s->nin, s->nout * sizeof *out);
    int ni = merge(in, s->nin), no = merge(out, s->nout);
    memcpy(s->r, in, (size_t)ni * sizeof *in);
    memcpy(s->r + ni, out, (size_t)no * sizeof *out);
    s->nin = (uint8_t)ni, s->nout = (uint8_t)no;
}

static bool put(rdof_idset_t *s, bool neg, uint32_t lo, uint32_t hi, char *err, size_t cap, const char *item)
{
    if (s->nin + s->nout >= RDOF_IDSET_MAX)
        tidy(s);
    if (s->nin + s->nout >= RDOF_IDSET_MAX)
        return err_(err, cap, "too many ranges (12 at most, after merging)", item);
    rdof_idrange_t x = {lo, hi};
    if (neg) {
        s->r[s->nin + s->nout++] = x;
    } else {
        memmove(s->r + s->nin + 1, s->r + s->nin, s->nout * sizeof x);
        s->r[s->nin++] = x;
    }
    return true;
}

static bool id_of(const char *p, const char *end, uint32_t *out)
{
    if (p == end)
        return false;
    uint64_t v = 0;
    for (; p < end; p++) {
        if (!isdigit((unsigned char)*p))
            return false;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > RDOF_ID_MAX)
            return false;
    }
    if (!v)
        return false;
    *out = (uint32_t)v;
    return true;
}

static bool one(rdof_idset_t *s, const char *p, const char *end, char *err, size_t cap)
{
    while (p < end && isspace((unsigned char)*p))
        p++;
    while (end > p && isspace((unsigned char)end[-1]))
        end--;
    char item[40];
    snprintf(item, sizeof item, "\"%.*s\"", (int)(end - p < 30 ? end - p : 30), p);
    bool neg = p < end && *p == '!';
    if (neg)
        p++;
    if (end - p == 1 && *p == '*') {
        if (neg)
            return err_(err, cap, "\"!*\" is the empty set: write false", item);
        s->star = true;
        return true;
    }
    const char *dash = memchr(p, '-', (size_t)(end - p));
    uint32_t lo, hi;
    if (dash) {
        if (!id_of(p, dash, &lo) || !id_of(dash + 1, end, &hi))
            return err_(err, cap, "a range is two IDs from 1 to 16777215, as \"3100-3199\"", item);
        if (hi < lo)
            return err_(err, cap, "a range's first ID is the lower", item);
    } else {
        if (!id_of(p, end, &lo))
            return err_(err, cap, "an ID from 1 to 16777215, a range \"A-B\", \"*\", or one of those after \"!\"",
                        item);
        hi = lo;
    }
    return put(s, neg, lo, hi, err, cap, item);
}

bool rdof_idset_add(rdof_idset_t *s, const char *items, char *err, size_t cap)
{
    const char *p = items;
    if (!*p)
        return err_(err, cap, "an empty item (false is the empty set)", "\"\"");
    for (;;) {
        const char *comma = strchr(p, ',');
        const char *end = comma ? comma : p + strlen(p);
        if (!one(s, p, end, err, cap))
            return false;
        if (!comma)
            return true;
        p = comma + 1;
    }
}

bool rdof_idset_add_id(rdof_idset_t *s, uint32_t id, char *err, size_t cap)
{
    char item[16];
    snprintf(item, sizeof item, "%lu", (unsigned long)id);
    if (!id || id > RDOF_ID_MAX)
        return err_(err, cap, "an ID from 1 to 16777215", item);
    return put(s, false, id, id, err, cap, item);
}

void rdof_idset_finish(rdof_idset_t *s)
{
    if (!s->nin && s->nout)
        s->star = true;
    tidy(s);
}

static bool in(const rdof_idrange_t *r, int n, uint32_t id)
{
    for (int k = 0; k < n; k++)
        if (id >= r[k].lo && id <= r[k].hi)
            return true;
    return false;
}

bool rdof_idset_has(const rdof_idset_t *s, uint32_t id)
{
    if (!id || id > RDOF_ID_MAX || in(s->r + s->nin, s->nout, id))
        return false;
    return s->star || in(s->r, s->nin, id);
}

bool rdof_idset_listed(const rdof_idset_t *s, uint32_t id)
{
    return in(s->r, s->nin, id) && !in(s->r + s->nin, s->nout, id);
}

int rdof_idset_ranges(const rdof_idset_t *s, rdof_idrange_t *out, int max)
{
    rdof_idrange_t base[RDOF_IDSET_MAX];
    int nb;
    if (s->star) {
        base[0] = (rdof_idrange_t){1, RDOF_ID_MAX};
        nb = 1;
    } else {
        memcpy(base, s->r, s->nin * sizeof *base);
        nb = s->nin;
    }
    int n = 0;
    for (int k = 0; k < nb; k++) {
        uint32_t lo = base[k].lo;
        const uint32_t hi = base[k].hi;
        /* the "!" ranges are sorted: cut each out of [lo, hi] in turn */
        for (int x = 0; x < s->nout && lo <= hi; x++) {
            const rdof_idrange_t *o = &s->r[s->nin + x];
            if (o->hi < lo || o->lo > hi)
                continue;
            if (o->lo > lo) {
                if (n < max)
                    out[n] = (rdof_idrange_t){lo, o->lo - 1};
                n++;
            }
            if (o->hi >= hi) {
                lo = hi + 1;
                break;
            }
            lo = o->hi + 1;
        }
        if (lo <= hi && lo) {
            if (n < max)
                out[n] = (rdof_idrange_t){lo, hi};
            n++;
        }
    }
    return n;
}

bool rdof_idset_empty(const rdof_idset_t *s) { return rdof_idset_ranges(s, NULL, 0) == 0; }

uint32_t rdof_idset_single(const rdof_idset_t *s)
{
    rdof_idrange_t r[2];
    return rdof_idset_ranges(s, r, 2) == 1 && r[0].lo == r[0].hi ? r[0].lo : 0;
}

size_t rdof_idset_format(const rdof_idset_t *s, char *out, size_t cap)
{
    size_t o = 0;
#define PUT(...) o += (size_t)snprintf(out + (o < cap ? o : cap), o < cap ? cap - o : 0, __VA_ARGS__)
    if (cap)
        out[0] = 0;
    const bool any = rdof_idset_empty(s) ? false : true;
    if (!any)
        return 0;
    bool first = true;
    if (s->star) {
        PUT("*");
        first = false;
    }
    for (int k = 0; k < s->nin; k++) {
        /* under "*" a plain item still says "listed"; one wholly excluded says nothing */
        rdof_idset_t one = {.nin = 1};
        one.r[0] = s->r[k];
        if (s->nout)
            memcpy(one.r + 1, s->r + s->nin, s->nout * sizeof *one.r), one.nout = s->nout;
        rdof_idrange_t parts[RDOF_IDSET_MAX];
        int np = rdof_idset_ranges(&one, parts, RDOF_IDSET_MAX);
        for (int p = 0; p < np && p < RDOF_IDSET_MAX; p++) {
            if (parts[p].lo == parts[p].hi)
                PUT("%s%lu", first ? "" : ",", (unsigned long)parts[p].lo);
            else
                PUT("%s%lu-%lu", first ? "" : ",", (unsigned long)parts[p].lo, (unsigned long)parts[p].hi);
            first = false;
        }
    }
    for (int k = 0; s->star && k < s->nout; k++) {
        const rdof_idrange_t *x = &s->r[s->nin + k];
        if (x->lo == x->hi)
            PUT(",!%lu", (unsigned long)x->lo);
        else
            PUT(",!%lu-%lu", (unsigned long)x->lo, (unsigned long)x->hi);
    }
#undef PUT
    return o < cap ? o : (cap ? cap - 1 : 0);
}
