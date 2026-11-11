// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The call router (rdof/router.h). */
#include "rdof/router.h"

#include <stdio.h>
#include <string.h>

void rdof_router_init(rdof_router_t *r, const rdof_site_t *site)
{
    memset(r, 0, sizeof *r);
    r->site = site;
    for (int k = 0; k < RDOF_ROUTER_HEARD; k++)
        r->heard[k].path = -1;
}

static void set_heard(rdof_router_t *r, uint32_t id, int path, double now)
{
    int slot = 0;
    for (int k = 0; k < RDOF_ROUTER_HEARD; k++) {
        if (r->heard[k].id == id) {
            slot = k;
            break;
        }
        if (r->heard[k].at < r->heard[slot].at) /* else the oldest goes */
            slot = k;
    }
    r->heard[slot].id = id;
    r->heard[slot].path = (int16_t)path;
    r->heard[slot].at = now;
}

int rdof_router_heard(const rdof_router_t *r, uint32_t id, double now)
{
    for (int k = 0; k < RDOF_ROUTER_HEARD; k++)
        if (r->heard[k].id == id && r->heard[k].path >= 0 && now - r->heard[k].at <= r->site->pc_hold_s)
            return r->heard[k].path;
    return -1;
}

static bool ptt_held(const rdof_router_t *r, int path, uint32_t tg, double now)
{
    for (int k = 0; k < RDOF_ROUTER_PTT; k++)
        if (r->ptt[path][k].tg == tg && r->ptt[path][k].until > now)
            return true;
    return false;
}

static void ptt_hold(rdof_router_t *r, int path, uint32_t tg, double now)
{
    const double until = now + r->site->path[path].pol.ptt_hold_s;
    int slot = 0;
    for (int k = 0; k < RDOF_ROUTER_PTT; k++) {
        if (r->ptt[path][k].tg == tg) {
            slot = k;
            break;
        }
        if (r->ptt[path][k].until < r->ptt[path][slot].until)
            slot = k;
    }
    r->ptt[path][slot].tg = tg;
    r->ptt[path][slot].until = until;
}

int rdof_router_carries(const rdof_router_t *r, int path, uint32_t tg, double now)
{
    const rdof_policy_t *pol = &r->site->path[path].pol;
    if (rdof_idset_listed(&pol->tg, tg) || (ptt_held(r, path, tg, now) && rdof_idset_has(&pol->ptt, tg)))
        return RDOF_RANK_LISTED;
    if (rdof_idset_has(&pol->tg, tg))
        return RDOF_RANK_STAR;
    return 0;
}

/* The app that claims a call's ID and that path may reach, or -1. */
static int app_for(const rdof_site_t *s, int path, const rdof_rcall_t *c)
{
    for (int a = 0; a < s->napp; a++) {
        const rdof_app3_t *ap = &s->app[a];
        if (ap->type == RDOF_APP_REPEATER || !(s->path[path].pol.apps >> a & 1))
            continue;
        if (rdof_idset_has(c->group ? &ap->tg : &ap->pc, c->dst))
            return a;
    }
    return -1;
}

rdof_route_t rdof_router_rf(rdof_router_t *r, int path, const rdof_rcall_t *c, double now)
{
    rdof_route_t out = {.kind = RDOF_ROUTE_NONE, .app = -1, .net = -1};
    const rdof_site_t *s = r->site;
    if (path < 0 || path >= s->npath) {
        out.why = "not a talk path";
        r->not_routed++;
        return out;
    }
    const rdof_path_t *p = &s->path[path];
    const rdof_policy_t *pol = &p->pol;
    if (c->src)
        set_heard(r, c->src, path, now);
    int a = app_for(s, path, c);
    if (a >= 0) {
        out.kind = RDOF_ROUTE_APP;
        out.app = a;
        r->to_app++;
        return out;
    }
    bool carried;
    if (c->group) {
        carried = rdof_idset_has(&pol->tg, c->dst);
        if (!carried && rdof_idset_has(&pol->ptt, c->dst)) {
            ptt_hold(r, path, c->dst, now);
            carried = true;
        } else if (carried && rdof_idset_has(&pol->ptt, c->dst)) {
            ptt_hold(r, path, c->dst, now);
        }
        if (!carried)
            out.why = "talkgroup not carried here";
    } else {
        carried = rdof_idset_has(&pol->pc, c->dst);
        if (!carried)
            out.why = "private call not carried here";
    }
    if (carried && !(pol->dir & RDOF_FLOW_RF_NET)) {
        carried = false;
        out.why = "dir: net>rf";
    }
    if (carried && pol->net < 0) {
        carried = false;
        out.why = "no network";
    }
    if (!carried) {
        r->not_routed++;
        return out;
    }
    out.kind = RDOF_ROUTE_NET;
    out.net = pol->net;
    r->to_net++;
    return out;
}

static void expire(rdof_router_t *r, double now)
{
    for (int k = 0; k < r->site->npath; k++)
        if (r->busy[k].stream && now - r->busy[k].last > RDOF_ROUTER_IDLE_S)
            r->busy[k].stream = 0;
}

static void unplaced(rdof_router_t *r, uint32_t stream)
{
    r->unplaced[r->unplaced_next] = stream;
    r->unplaced_next = (r->unplaced_next + 1) % 8;
}

/* A path that may take calls from net. */
static bool sends(const rdof_site_t *s, int k, int net)
{
    const rdof_path_t *p = &s->path[k];
    return p->tx_freq >= 0 && p->pol.net == net && (p->pol.dir & RDOF_FLOW_NET_RF);
}

uint64_t rdof_router_net(rdof_router_t *r, int net, uint64_t only, uint32_t stream, const rdof_rcall_t *c, bool end,
                         double now, uint64_t *preempt)
{
    const rdof_site_t *s = r->site;
    uint64_t mask = 0;
    if (preempt)
        *preempt = 0;
    expire(r, now);
    /* a stream already placed */
    for (int k = 0; k < s->npath; k++)
        if (r->busy[k].stream == stream && stream) {
            mask |= 1ull << k;
            r->busy[k].last = now;
            if (end)
                r->busy[k].stream = 0;
        }
    if (mask || !stream)
        return mask;
    for (int k = 0; k < 8; k++)
        if (r->unplaced[k] == stream) {
            if (end)
                r->unplaced[k] = 0;
            return 0;
        }
    /* a new one: which paths want it, at what rank */
    uint8_t rank[RDOF_SITE_MAX_PATHS] = {0};
    const int boost = c->emergency ? RDOF_RANK_EMERGENCY : 0;
    if (c->group) {
        for (int k = 0; k < s->npath; k++) {
            if (!sends(s, k, net) || !(only >> k & 1))
                continue;
            int rk = rdof_router_carries(r, k, c->dst, now);
            if (rk)
                rank[k] = (uint8_t)(boost ? boost : rk);
        }
    } else {
        int k = rdof_router_heard(r, c->dst, now);
        /* sent to one peer's paths (identity per_path): the master chose the path; heard here or not */
        if (only != ~0ull && (k < 0 || !(only >> k & 1)))
            for (k = 0; k < s->npath && !(only >> k & 1 && sends(s, k, net)); k++)
                ;
        if (k >= s->npath)
            k = -1;
        if (k < 0) {
            r->pc_dropped++;
            if (!end)
                unplaced(r, stream);
            return 0;
        }
        if (!sends(s, k, net) || !(only >> k & 1) || !rdof_idset_has(&s->path[k].pol.pc, c->src)) {
            r->no_path++;
            if (!end)
                unplaced(r, stream);
            return 0;
        }
        rank[k] = (uint8_t)(boost ? boost : RDOF_RANK_LISTED);
    }
    bool wanted = false;
    for (int k = 0; k < s->npath; k++) {
        if (!rank[k])
            continue;
        wanted = true;
        if (r->busy[k].stream) {
            if (rank[k] <= r->busy[k].rank) {
                r->refused++;
                continue;
            }
            if (preempt)
                *preempt |= 1ull << k;
        }
        if (!end) {
            r->busy[k].stream = stream;
            r->busy[k].rank = rank[k];
            r->busy[k].last = now;
        }
        mask |= 1ull << k;
    }
    if (!wanted)
        r->no_path++;
    else if (mask)
        r->from_net++;
    if (!mask && !end)
        unplaced(r, stream);
    return mask;
}

void rdof_router_drop(rdof_router_t *r, int path, uint32_t stream)
{
    if (path >= 0 && path < r->site->npath && r->busy[path].stream == stream)
        r->busy[path].stream = 0;
}

size_t rdof_router_apps_json(const rdof_router_t *r, double now, rdof_apps_extra_fn extra, void *ctx, char *out,
                             size_t cap)
{
    const rdof_site_t *s = r->site;
    size_t n = 0;
    bool fit = cap >= 3;
    if (fit)
        out[n++] = '[';
    for (int a = 0; fit && s && a < s->napp; a++) {
        const rdof_app3_t *ap = &s->app[a];
        char obj[768];
        rdof_json_t j;
        rdof_json_init(&j, obj, sizeof obj);
        rdof_json_open(&j, NULL);
        rdof_json_str(&j, "name", ap->name);
        if (ap->type == RDOF_APP_REPEATER) {
            rdof_json_str(&j, "type", "repeater");
            if (ap->in >= 0)
                rdof_json_int(&j, "in", s->freq[ap->in].hz);
            if (ap->out >= 0) {
                rdof_json_int(&j, "out", s->freq[ap->out].hz);
                if (s->freq[ap->out].cc >= 0)
                    rdof_json_int(&j, "cc", s->freq[ap->out].cc);
            }
            char slots[320];
            size_t m = 0;
            int net = -1;
            slots[m++] = '[';
            for (int i = 0; i < 2; i++) {
                const int k = ap->path[i];
                if (k < 0 || k >= s->npath)
                    continue;
                const rdof_path_t *p = &s->path[k];
                net = p->pol.net;
                const bool own = net >= 0 && s->net[net].type == RDOF_NET_HBLINK && s->net[net].per_path;
                char peer[32] = "";
                if (own)
                    snprintf(peer, sizeof peer, ",\"peer\":%lu", (unsigned long)p->peer_id);
                const int w = snprintf(slots + m, sizeof slots - m, "%s{\"slot\":%d,\"path\":\"%s\"%s,\"sending\":%s}",
                                       m > 1 ? "," : "", p->slot ? p->slot : i + 1, p->name, peer,
                                       r->busy[k].stream && now - r->busy[k].last < 2.0 ? "true" : "false");
                if (w < 0 || (size_t)w >= sizeof slots - m - 2)
                    break;
                m += (size_t)w;
            }
            slots[m++] = ']';
            slots[m] = 0;
            rdof_json_str(&j, "net", net >= 0 ? s->net[net].name : "");
            rdof_json_raw(&j, "slots", slots);
        } else {
            rdof_json_str(&j, "type", ap->type == RDOF_APP_PARROT ? "parrot" : "ipgw");
            const uint32_t tg = rdof_idset_single(&ap->tg), pc = rdof_idset_single(&ap->pc);
            if (tg || pc) {
                rdof_json_int(&j, "id", tg ? tg : pc);
                rdof_json_bool(&j, "group", tg != 0);
            }
        }
        if (extra)
            extra(ctx, a, &j);
        rdof_json_close(&j);
        if (!rdof_json_done(&j) || n + j.len + 3 > cap) {
            fit = false;
            break;
        }
        if (a)
            out[n++] = ',';
        memcpy(out + n, obj, j.len);
        n += j.len;
    }
    if (!fit) {
        n = 0;
        if (cap >= 3)
            out[n++] = '[';
    }
    if (cap >= 3)
        out[n++] = ']';
    if (cap)
        out[n < cap ? n : cap - 1] = 0;
    return n;
}
