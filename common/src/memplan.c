// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/memplan.h"

#include <stdio.h>
#include <string.h>

#define KB(x) ((long long)(((x) + 1023) / 1024))

void rdof_memplan_init(rdof_memplan_t *p, uint32_t margin, bool strict)
{
    memset(p, 0, sizeof *p);
    p->margin = margin;
    p->strict = strict;
    p->ok = true;
}

static void set_error(rdof_memplan_t *p, const char *msg)
{
    p->ok = false;
    if (!p->error[0])
        snprintf(p->error, sizeof p->error, "%s", msg);
}

int rdof_memplan_role(const rdof_memplan_t *p, const char *role)
{
    for (int r = 0; r < p->nroles; r++)
        if (!strcmp(p->role[r].name, role))
            return r;
    return -1;
}

int rdof_memplan_find(const rdof_memplan_t *p, const char *role, const char *name)
{
    const int r = rdof_memplan_role(p, role);
    for (int i = 0; r >= 0 && i < p->n; i++)
        if (p->c[i].role == r && !strcmp(p->c[i].name, name))
            return i;
    return -1;
}

int rdof_memplan_add(rdof_memplan_t *p, const char *role, const char *name, uint32_t size, uint16_t align,
                     uint8_t caps, rdof_mem_how_t how)
{
    char e[96];
    if (!align)
        align = 4;
    if (!size || (align & (align - 1)) || align > 4096 || !(caps & (RDOF_MEM_INTERNAL | RDOF_MEM_DMA | RDOF_MEM_PSRAM)) ||
        ((caps & RDOF_MEM_FALLBACK) && !(caps & RDOF_MEM_INTERNAL)) || how > RDOF_MEM_BUDGET ||
        strlen(role) >= sizeof p->role[0].name || strlen(name) >= sizeof p->c[0].name) {
        snprintf(e, sizeof e, "%.11s %.15s: bad claim", role, name);
        set_error(p, e);
        return -1;
    }
    if (rdof_memplan_find(p, role, name) >= 0) {
        snprintf(e, sizeof e, "%s %s: claimed twice", role, name);
        set_error(p, e);
        return -1;
    }
    int r = rdof_memplan_role(p, role);
    if (r < 0) {
        if (p->nroles >= RDOF_MEM_MAX_ROLES) {
            set_error(p, "too many roles in the memory plan");
            return -1;
        }
        r = p->nroles++;
        memset(&p->role[r], 0, sizeof p->role[r]);
        snprintf(p->role[r].name, sizeof p->role[r].name, "%s", role);
    }
    if (p->n >= RDOF_MEM_MAX_CLAIMS) {
        set_error(p, "too many claims in the memory plan");
        return -1;
    }
    rdof_mem_claim_t *c = &p->c[p->n];
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    c->role = r;
    c->size = c->got = size;
    c->align = align < 4 ? 4 : align;
    c->count = 1;
    c->caps = caps;
    c->how = (uint8_t)how;
    c->region = -1;
    return p->n++;
}

void rdof_memplan_count(rdof_memplan_t *p, int i, int count)
{
    if (i >= 0 && i < p->n)
        p->c[i].count = (uint16_t)(count < 1 ? 1 : count > RDOF_MEM_MAX_COUNT ? RDOF_MEM_MAX_COUNT : count);
}

void rdof_memplan_elastic(rdof_memplan_t *p, int i, uint32_t max, uint32_t unit)
{
    if (i < 0 || i >= p->n)
        return;
    p->c[i].max = max > p->c[i].size ? max : 0;
    p->c[i].unit = unit ? unit : 1;
}

/* What a claim costs: its blocks, and what aligning one past the heap's 4 bytes can waste. */
static int64_t cost(const rdof_mem_claim_t *c)
{
    return (int64_t)c->got * c->count + (c->align > 4 ? c->align - 4 : 0);
}

/* Where a claim goes before the spare is shared out (FALLBACK claims: nowhere yet). */
static int first_where(const rdof_mem_claim_t *c, bool psram)
{
    if (c->caps & RDOF_MEM_DMA)
        return RDOF_MEM_IN_DMA;
    if (c->caps & RDOF_MEM_FALLBACK)
        return RDOF_MEM_NOWHERE;
    if (c->caps & RDOF_MEM_INTERNAL)
        return RDOF_MEM_IN_INTERNAL;
    return psram ? RDOF_MEM_IN_PSRAM : RDOF_MEM_IN_INTERNAL;
}

static void refuse(rdof_memplan_t *p, int r, const char *why)
{
    p->role[r].state = RDOF_ROLE_REFUSED;
    snprintf(p->role[r].why, sizeof p->role[r].why, "%s", why);
    for (int i = 0; i < p->n; i++)
        if (p->c[i].role == r) {
            p->c[i].where = RDOF_MEM_NOWHERE;
            p->c[i].region = -1;
        }
    set_error(p, why);
}

void rdof_memplan_refuse(rdof_memplan_t *p, int r, const char *why)
{
    if (r >= 0 && r < p->nroles)
        refuse(p, r, why);
}

static bool planned(const rdof_memplan_t *p, int r)
{
    return r >= 0 && r < p->nroles && p->role[r].state == RDOF_ROLE_PLANNED;
}

bool rdof_memplan_role_ok(const rdof_memplan_t *p, const char *role)
{
    return planned(p, rdof_memplan_role(p, role));
}

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

static void layout(rdof_memplan_t *p)
{
    p->nr = 0;
    for (int r = 0; r < p->nroles; r++) {
        if (!planned(p, r))
            continue;
        for (int w = RDOF_MEM_IN_INTERNAL; w <= RDOF_MEM_IN_PSRAM; w++) {
            int k = -1;
            uint32_t off = 0;
            uint16_t al = 4;
            for (int i = 0; i < p->n; i++) {
                rdof_mem_claim_t *c = &p->c[i];
                if (c->role != r || c->how != RDOF_MEM_ALLOC || c->where != w)
                    continue;
                if (k < 0) {
                    if (p->nr >= RDOF_MEM_MAX_REGIONS) {
                        refuse(p, r, "too many regions in the memory plan");
                        break;
                    }
                    k = p->nr++;
                    memset(&p->r[k], 0, sizeof p->r[k]);
                    p->r[k].role = r;
                    p->r[k].where = (uint8_t)w;
                }
                off = align_up(off, c->align);
                c->region = k;
                c->offset = off;
                off += c->got * c->count;
                if (c->align > al)
                    al = c->align;
            }
            if (k >= 0 && planned(p, r)) {
                p->r[k].bytes = off;
                p->r[k].align = al;
            }
        }
    }
}

bool rdof_memplan_solve(rdof_memplan_t *p, const rdof_mem_avail_t *a)
{
    char why[160];
    p->at_plan = *a;
    p->solved = true;
    const bool psram = a->psram_free > 0;
    int64_t left = (int64_t)a->internal_free - p->margin, dma_left = (int64_t)a->dma_free - p->margin;
    int64_t ps_left = a->psram_free;
    for (int r = 0; r < p->nroles; r++) {
        p->role[r].state = RDOF_ROLE_PLANNED;
        p->role[r].why[0] = 0;
    }
    for (int r = 0; r < p->nroles; r++) {
        int64_t need = 0, dneed = 0, pneed = 0;
        for (int i = 0; i < p->n; i++) {
            rdof_mem_claim_t *c = &p->c[i];
            if (c->role != r)
                continue;
            c->got = c->size;
            c->where = RDOF_MEM_NOWHERE;
            c->region = -1;
            const int w = first_where(c, psram);
            if (w == RDOF_MEM_IN_PSRAM)
                pneed += cost(c);
            else if (w != RDOF_MEM_NOWHERE) {
                need += cost(c);
                if (w == RDOF_MEM_IN_DMA)
                    dneed += cost(c);
            }
        }
        if (pneed > ps_left) {
            snprintf(why, sizeof why, "%s needs %lld KB of PSRAM, %lld KB is free", p->role[r].name, KB(pneed),
                     KB(ps_left));
            refuse(p, r, why);
            continue;
        }
        if (need > left || dneed > dma_left) {
            const bool dma = need <= left;
            snprintf(why, sizeof why, "%s needs %lld KB of %s RAM, %lld KB is free above the %lld KB margin",
                     p->role[r].name, KB(dma ? dneed : need), dma ? "DMA" : "internal",
                     (long long)((dma ? dma_left : left) > 0 ? (dma ? dma_left : left) / 1024 : 0), KB(p->margin));
            if (p->strict) {
                refuse(p, r, why);
                continue;
            }
            set_error(p, why);
            snprintf(p->role[r].why, sizeof p->role[r].why, "over the margin: %.90s", why);
        }
        for (int i = 0; i < p->n; i++)
            if (p->c[i].role == r)
                p->c[i].where = (uint8_t)first_where(&p->c[i], psram);
        left -= need;
        dma_left -= dneed;
        ps_left -= pneed;
    }
    /* internal where the spare holds it, else PSRAM */
    for (int i = 0; i < p->n; i++) {
        rdof_mem_claim_t *c = &p->c[i];
        if (!(c->caps & RDOF_MEM_FALLBACK) || !planned(p, c->role))
            continue;
        const int64_t b = cost(c);
        if (b <= left || !psram || b > ps_left) {
            c->where = RDOF_MEM_IN_INTERNAL;
            left -= b;
            if (left < 0) {
                snprintf(why, sizeof why, "%s %s: %lld KB short of internal RAM above the margin",
                         p->role[c->role].name, c->name, KB(-left));
                set_error(p, why);
            }
        } else {
            c->where = RDOF_MEM_IN_PSRAM;
            ps_left -= b;
        }
    }
    /* elastic claims share what's spare, in order */
    for (int i = 0; i < p->n; i++) {
        rdof_mem_claim_t *c = &p->c[i];
        if (!c->max || !planned(p, c->role) || c->where == RDOF_MEM_NOWHERE)
            continue;
        int64_t *pool = c->where == RDOF_MEM_IN_PSRAM ? &ps_left : &left;
        if (*pool <= 0)
            continue;
        int64_t extra = *pool / c->count;
        if (extra > (int64_t)(c->max - c->size))
            extra = c->max - c->size;
        extra -= extra % c->unit;
        c->got = c->size + (uint32_t)extra;
        *pool -= extra * c->count;
        if (c->where == RDOF_MEM_IN_DMA)
            dma_left -= extra * c->count;
    }
    p->spare = left;
    layout(p);
    return p->ok;
}

bool rdof_memplan_hand_over(rdof_memplan_t *p, int i)
{
    if (i < 0 || i >= p->n || p->c[i].how != RDOF_MEM_HOLD || p->c[i].where == RDOF_MEM_NOWHERE ||
        p->c[i].handed >= p->c[i].count)
        return false;
    p->c[i].handed++;
    return true;
}

uint32_t rdof_memplan_role_bytes(const rdof_memplan_t *p, int r, int where)
{
    uint32_t n = 0;
    for (int i = 0; i < p->n; i++) {
        const rdof_mem_claim_t *c = &p->c[i];
        if (c->role != r || c->where != where || c->how == RDOF_MEM_BUDGET)
            continue;
        n += c->got * (uint32_t)(c->how == RDOF_MEM_HOLD ? c->count - c->handed : c->count);
    }
    return n;
}

uint32_t rdof_memplan_role_budget(const rdof_memplan_t *p, int r)
{
    uint32_t n = 0;
    for (int i = 0; i < p->n; i++)
        if (p->c[i].role == r && p->c[i].how == RDOF_MEM_BUDGET && p->c[i].where != RDOF_MEM_NOWHERE)
            n += p->c[i].got * p->c[i].count;
    return n;
}

uint32_t rdof_memplan_internal_held(const rdof_memplan_t *p)
{
    uint32_t n = 0;
    for (int r = 0; r < p->nroles; r++)
        n += rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_INTERNAL) + rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_DMA);
    return n;
}

uint32_t rdof_memplan_release(rdof_memplan_t *p, const char *role)
{
    const int r = rdof_memplan_role(p, role);
    if (r < 0 || p->role[r].state == RDOF_ROLE_RELEASED)
        return 0;
    const uint32_t n = rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_INTERNAL) + rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_DMA);
    for (int k = 0; k < p->nr; k++)
        if (p->r[k].role == r) {
            p->r[k].released = true;
            p->r[k].base = NULL;
        }
    for (int i = 0; i < p->n; i++)
        if (p->c[i].role == r) {
            p->c[i].where = RDOF_MEM_NOWHERE;
            p->c[i].taken = false;
            p->c[i].region = -1;
        }
    p->role[r].state = RDOF_ROLE_RELEASED;
    snprintf(p->role[r].why, sizeof p->role[r].why, "released: %u KB of internal RAM given back", (unsigned)KB(n));
    return n;
}

int rdof_memplan_add_now(rdof_memplan_t *p, const char *role, const char *name, uint32_t size, uint16_t align,
                         uint8_t caps, const rdof_mem_avail_t *now, char *why, size_t why_cap)
{
    const int n0 = p->n, roles0 = p->nroles;
    char e[sizeof p->error];
    snprintf(e, sizeof e, "%s", p->error);
    const bool ok0 = p->ok;
    int i = rdof_memplan_find(p, role, name);
    if (i >= 0 && p->c[i].where == RDOF_MEM_NOWHERE && (align & (align - 1)) == 0 &&
        (caps & (RDOF_MEM_INTERNAL | RDOF_MEM_DMA | RDOF_MEM_PSRAM)) && size) { /* a released claim, again */
        rdof_mem_claim_t *c = &p->c[i];
        c->size = c->got = size;
        c->max = 0;
        c->align = align < 4 ? 4 : align;
        c->count = 1;
        c->caps = caps;
        c->how = RDOF_MEM_ALLOC;
        c->handed = 0;
        c->taken = false;
    } else
        i = rdof_memplan_add(p, role, name, size, align, caps, RDOF_MEM_ALLOC);
    const bool reused = i >= 0 && i < n0;
    if (i < 0) {
        if (why_cap)
            snprintf(why, why_cap, "%s", p->error[0] && strcmp(p->error, e) ? p->error : "bad claim");
        p->ok = ok0;
        snprintf(p->error, sizeof p->error, "%s", e);
        return -1;
    }
    rdof_mem_claim_t *c = &p->c[i];
    const bool psram = now->psram_free > 0;
    int w = first_where(c, psram);
    const int64_t b = cost(c), left = (int64_t)now->internal_free - p->margin;
    if (w == RDOF_MEM_NOWHERE) /* FALLBACK */
        w = b <= left || !psram ? RDOF_MEM_IN_INTERNAL : RDOF_MEM_IN_PSRAM;
    char no[160] = "";
    if (w == RDOF_MEM_IN_PSRAM ? b > (int64_t)now->psram_largest
                               : b > left || b > (int64_t)now->internal_largest ||
                                     (w == RDOF_MEM_IN_DMA && b > (int64_t)now->dma_free - p->margin))
        snprintf(no, sizeof no, "%s %s: %lld KB doesn't fit (%s: %lld KB free above the %lld KB margin)", role, name,
                 KB(b), rdof_mem_where_name(w),
                 (long long)(w == RDOF_MEM_IN_PSRAM ? now->psram_largest / 1024 : left > 0 ? left / 1024 : 0),
                 KB(w == RDOF_MEM_IN_PSRAM ? 0 : p->margin));
    int k = 0;
    while (k < p->nr && !p->r[k].released) /* a released region's slot, else a new one */
        k++;
    if (!no[0] && k >= RDOF_MEM_MAX_REGIONS)
        snprintf(no, sizeof no, "too many regions in the memory plan");
    if (why_cap)
        snprintf(why, why_cap, "%s", no);
    if (no[0]) {
        if (!reused) {
            p->n = n0;
            p->nroles = roles0;
        }
        return -1;
    }
    if (k == p->nr)
        p->nr++;
    memset(&p->r[k], 0, sizeof p->r[k]);
    p->r[k].role = c->role;
    p->r[k].where = (uint8_t)w;
    p->r[k].align = c->align;
    p->r[k].bytes = size;
    c->where = (uint8_t)w;
    c->region = k;
    c->offset = 0;
    rdof_mem_role_t *r = &p->role[c->role];
    if (r->state != RDOF_ROLE_PLANNED) {
        r->state = RDOF_ROLE_PLANNED;
        r->why[0] = 0;
    }
    return i;
}

const char *rdof_mem_where_name(int where)
{
    switch (where) {
    case RDOF_MEM_IN_INTERNAL: return "internal";
    case RDOF_MEM_IN_DMA: return "dma";
    case RDOF_MEM_IN_PSRAM: return "psram";
    default: return "none";
    }
}

const char *rdof_mem_how_name(int how)
{
    return how == RDOF_MEM_ALLOC ? "alloc" : how == RDOF_MEM_HOLD ? "hold" : "budget";
}

static const char *state_name(int s)
{
    return s == RDOF_ROLE_PLANNED ? "planned" : s == RDOF_ROLE_REFUSED ? "refused" : "released";
}

void rdof_memplan_json(const rdof_memplan_t *p, const rdof_mem_avail_t *now, rdof_json_t *j)
{
    uint32_t planned_in = 0, budget = 0, planned_ps = 0;
    for (int r = 0; r < p->nroles; r++) {
        planned_ps += rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_PSRAM);
        budget += rdof_memplan_role_budget(p, r);
    }
    planned_in = rdof_memplan_internal_held(p);
    rdof_json_open(j, "memory");
    rdof_json_bool(j, "planned", p->solved);
    rdof_json_bool(j, "ok", p->ok);
    rdof_json_str(j, "error", p->error);
    rdof_json_int(j, "margin", p->margin);
    rdof_json_bool(j, "strict", p->strict);
    rdof_json_int(j, "spare", p->spare);
    rdof_json_open(j, "internal");
    rdof_json_int(j, "free", now->internal_free);
    rdof_json_int(j, "largest", now->internal_largest);
    rdof_json_int(j, "at_plan", p->at_plan.internal_free);
    rdof_json_int(j, "planned", planned_in);
    rdof_json_int(j, "budget", budget);
    /* taken since the plan by everything else: the budgets' owners, Wi-Fi, lwIP, what isn't planned */
    rdof_json_int(j, "outside_plan", (long long)p->at_plan.internal_free - now->internal_free - planned_in);
    rdof_json_close(j);
    rdof_json_open(j, "dma");
    rdof_json_int(j, "free", now->dma_free);
    rdof_json_int(j, "largest", now->dma_largest);
    rdof_json_close(j);
    rdof_json_open(j, "psram");
    rdof_json_int(j, "free", now->psram_free);
    rdof_json_int(j, "largest", now->psram_largest);
    rdof_json_int(j, "planned", planned_ps);
    rdof_json_close(j);
    rdof_json_open(j, "roles");
    for (int r = 0; r < p->nroles; r++) {
        rdof_json_open(j, p->role[r].name);
        rdof_json_str(j, "state", state_name(p->role[r].state));
        rdof_json_str(j, "why", p->role[r].why);
        rdof_json_int(j, "internal", rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_INTERNAL));
        rdof_json_int(j, "dma", rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_DMA));
        rdof_json_int(j, "psram", rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_PSRAM));
        rdof_json_int(j, "budget", rdof_memplan_role_budget(p, r));
        rdof_json_open(j, "claims");
        for (int i = 0; i < p->n; i++) {
            const rdof_mem_claim_t *c = &p->c[i];
            if (c->role != r)
                continue;
            rdof_json_open(j, c->name);
            rdof_json_int(j, "bytes", (long long)c->got * c->count);
            rdof_json_str(j, "in", rdof_mem_where_name(c->where));
            rdof_json_str(j, "how", rdof_mem_how_name(c->how));
            if (c->align > 4)
                rdof_json_int(j, "align", c->align);
            if (c->how == RDOF_MEM_HOLD)
                rdof_json_int(j, "handed", c->handed);
            else if (c->how == RDOF_MEM_ALLOC)
                rdof_json_bool(j, "taken", c->taken);
            rdof_json_close(j);
        }
        rdof_json_close(j);
        rdof_json_close(j);
    }
    rdof_json_close(j);
    rdof_json_close(j);
}

size_t rdof_memplan_describe(const rdof_memplan_t *p, char *out, size_t cap)
{
    size_t n = 0;
    if (cap)
        out[0] = 0;
    for (int r = 0; r < p->nroles && n + 1 < cap; r++) {
        int k = snprintf(out + n, cap - n, "%s %s: %lld KB internal (%lld DMA), %lld KB PSRAM, %lld KB budgeted%s%s\n",
                         p->role[r].name, state_name(p->role[r].state),
                         KB(rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_INTERNAL) +
                            rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_DMA)),
                         KB(rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_DMA)),
                         KB(rdof_memplan_role_bytes(p, r, RDOF_MEM_IN_PSRAM)), KB(rdof_memplan_role_budget(p, r)),
                         p->role[r].why[0] ? ": " : "", p->role[r].why);
        if (k < 0)
            break;
        n += (size_t)k < cap - n ? (size_t)k : cap - n - 1;
    }
    return n;
}
