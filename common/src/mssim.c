// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The timed confirmed-data responder (rdof/mssim.h). */
#include "rdof/mssim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/jsonp.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"

#define BURST48 1320  /* a burst on air: 132 symbols, 27.5 ms */
#define FRAME48 2880  /* one sender's bursts, two slots apart */
#define IDLE48 (4 * FRAME48) /* a packet with nothing heard for this long is over */

const char *rdof_mssim_kind_name(rdof_mssim_kind_t k)
{
    static const char *const n[] = {"ack", "nack", "sack", "none", "ack2"};
    return (unsigned)k < sizeof n / sizeof n[0] ? n[k] : "?";
}

/* ── the answer bursts ─────────────────────────────────────────────────────── */

void rdof_mssim_response(uint32_t src, uint32_t dst, int sap, int cls, int type, int status, int blocks, int cc,
                         uint64_t sync48, uint8_t out[33])
{
    uint8_t d[12] = {0};
    d[0] = RDOF_DPF_RESPONSE;
    d[1] = (uint8_t)((sap & 0x0F) << 4);
    rdof_put_be24(d + 2, dst);
    rdof_put_be24(d + 5, src);
    d[8] = (uint8_t)(blocks & 0x7F);
    d[9] = (uint8_t)((cls & 3) << 6 | (type & 7) << 3 | (status & 7));
    uint16_t crc = rdof_crc16_ccitt(d, 10) ^ RDOF_MASK_DATA_HEADER;
    d[10] = (uint8_t)(crc >> 8);
    d[11] = (uint8_t)crc;
    rdof_data_burst(d, RDOF_DT_DATA_HEADER, cc, sync48, out);
}

void rdof_mssim_sack_block(uint64_t received, int cc, uint64_t sync48, uint8_t out[33])
{
    uint8_t d[12];
    for (int i = 0; i < 8; i++)
        d[i] = (uint8_t)(received >> (8 * i)); /* block n: octet n/8, bit n%8 from the LSB */
    uint32_t crc = rdof_crc32_dmr(d, 8);
    for (int i = 0; i < 4; i++)
        d[8 + i] = (uint8_t)(crc >> (8 * i));
    rdof_data_burst(d, RDOF_DT_RATE_12, cc, sync48, out);
}

/* ── the profile ───────────────────────────────────────────────────────────── */

static bool parse_step(const rdof_jdoc_t *doc, int o, rdof_mssim_step_t *st, char *err, size_t errcap)
{
    *st = (rdof_mssim_step_t){.kind = RDOF_MSSIM_ACK, .slots = 1, .nack_type = 1, .sack_type = 0};
    const rdof_jnode_t *n = rdof_jnode(doc, rdof_jget(doc, o, "answer"));
    if (n) {
        if (n->type != RDOF_JSTR)
            goto bad_answer;
        int k = -1;
        for (int i = 0; i <= RDOF_MSSIM_ACK_TWICE; i++)
            if (!strcmp(n->str, rdof_mssim_kind_name((rdof_mssim_kind_t)i)))
                k = i;
        if (k < 0)
            goto bad_answer;
        st->kind = (rdof_mssim_kind_t)k;
    }
    if ((n = rdof_jnode(doc, rdof_jget(doc, o, "slots")))) {
        if (n->type != RDOF_JNUM || !n->integer || n->num < 1 || n->num > 200) {
            snprintf(err, errcap, "slots is a whole number 1-200");
            return false;
        }
        st->slots = (int)n->num;
    }
    if ((n = rdof_jnode(doc, rdof_jget(doc, o, "nack_type"))))
        st->nack_type = n->type == RDOF_JNUM ? (int)n->num & 7 : 1;
    if ((n = rdof_jnode(doc, rdof_jget(doc, o, "sack_type"))))
        st->sack_type = n->type == RDOF_JNUM ? (int)n->num & 7 : 0;
    int a = rdof_jget(doc, o, "resend");
    if ((n = rdof_jnode(doc, a))) {
        if (n->type != RDOF_JARR) {
            snprintf(err, errcap, "resend is a list of block numbers");
            return false;
        }
        for (int i = 0; i < n->count; i++) {
            const rdof_jnode_t *b = rdof_jnode(doc, rdof_jat(doc, a, i));
            if (!b || b->type != RDOF_JNUM || b->num < 0 || b->num > 63) {
                snprintf(err, errcap, "resend: block numbers are 0-63");
                return false;
            }
            st->resend |= 1ULL << (int)b->num;
        }
    }
    return true;
bad_answer:
    snprintf(err, errcap, "answer is ack, nack, sack, none or ack2");
    return false;
}

bool rdof_mssim_parse(const char *json, size_t len, rdof_mssim_profile_t *out, char *err, size_t errcap)
{
    rdof_jnode_t nodes[128];
    char buf[1024];
    rdof_jdoc_t doc = {.nodes = nodes, .max = 128, .buf = buf, .cap = sizeof buf};
    memset(out, 0, sizeof *out);
    int root = rdof_jparse(&doc, json, len);
    const rdof_jnode_t *r = rdof_jnode(&doc, root);
    if (!r || r->type != RDOF_JOBJ) {
        snprintf(err, errcap, "the profile isn't a JSON object: %s", doc.error);
        return false;
    }
    int a = rdof_jget(&doc, root, "answers");
    const rdof_jnode_t *n = rdof_jnode(&doc, a);
    if (n) {
        if (n->type != RDOF_JARR || n->count < 1 || n->count > RDOF_MSSIM_STEPS) {
            snprintf(err, errcap, "answers is a list of 1-%d steps", RDOF_MSSIM_STEPS);
            return false;
        }
        for (int i = 0; i < n->count; i++) {
            int o = rdof_jat(&doc, a, i);
            const rdof_jnode_t *s = rdof_jnode(&doc, o);
            if (!s || s->type != RDOF_JOBJ) {
                snprintf(err, errcap, "answers: each step is an object");
                return false;
            }
            if (!parse_step(&doc, o, &out->step[i], err, errcap))
                return false;
        }
        out->nsteps = n->count;
    } else {
        if (!parse_step(&doc, root, &out->step[0], err, errcap))
            return false;
        out->nsteps = 1;
    }
    if ((n = rdof_jnode(&doc, rdof_jget(&doc, root, "repeat"))))
        out->repeat = n->type == RDOF_JBOOL && n->b;
    if ((n = rdof_jnode(&doc, rdof_jget(&doc, root, "end")))) {
        if (n->type == RDOF_JSTR && !strcmp(n->str, "block"))
            out->end = RDOF_MSSIM_END_BLOCK;
        else if (n->type == RDOF_JSTR && !strcmp(n->str, "terminator"))
            out->end = RDOF_MSSIM_END_TERMINATOR;
        else if (n->type == RDOF_JSTR && !strcmp(n->str, "learn"))
            out->end = RDOF_MSSIM_END_LEARN;
        else {
            snprintf(err, errcap, "end is block, terminator or learn");
            return false;
        }
    }
    if ((n = rdof_jnode(&doc, rdof_jget(&doc, root, "to"))) && n->type == RDOF_JNUM)
        out->to = (uint32_t)n->num;
    if ((n = rdof_jnode(&doc, rdof_jget(&doc, root, "from"))) && n->type == RDOF_JNUM)
        out->from = (uint32_t)n->num;
    if ((n = rdof_jnode(&doc, rdof_jget(&doc, root, "count"))) && n->type == RDOF_JNUM)
        out->count = n->num > 0 ? (int)n->num : 0;
    return true;
}

/* ── the responder ─────────────────────────────────────────────────────────── */

void rdof_mssim_init(rdof_mssim_t *s, rdof_mssim_send_fn send, void *ctx)
{
    memset(s, 0, sizeof *s);
    s->send = send;
    s->ctx = ctx;
}

void rdof_mssim_arm(rdof_mssim_t *s, const rdof_mssim_profile_t *p)
{
    s->prof = *p;
    if (s->prof.nsteps < 1) {
        s->prof.nsteps = 1;
        s->prof.step[0] = (rdof_mssim_step_t){.kind = RDOF_MSSIM_ACK, .slots = 1, .nack_type = 1};
    }
    s->armed = true;
    s->answered = 0;
    s->active = false;
    s->nq = s->nsent = 0;
}

void rdof_mssim_disarm(rdof_mssim_t *s)
{
    s->armed = false;
    s->active = false;
    s->nq = s->nsent = 0;
}

/* The sync our answer carries: a radio answers in its own family; in TDMA direct mode it answers in the other
   timeslot when it starts an odd number of slots later. */
static uint64_t answer_sync(rdof_family_t heard, int slots)
{
    rdof_family_t f = heard;
    if (f == RDOF_FAMILY_BS)
        f = RDOF_FAMILY_MS;
    else if ((f == RDOF_FAMILY_TS1 || f == RDOF_FAMILY_TS2) && (slots & 1))
        f = f == RDOF_FAMILY_TS1 ? RDOF_FAMILY_TS2 : RDOF_FAMILY_TS1;
    return rdof_data_sync(rdof_family_voice_sync(f));
}

static rdof_mssim_entry_t *cur(rdof_mssim_t *s) { return &s->log[s->cur]; }

static bool wants_terminator(const rdof_mssim_t *s, uint32_t src)
{
    if (s->prof.end == RDOF_MSSIM_END_TERMINATOR)
        return true;
    return s->prof.end == RDOF_MSSIM_END_LEARN && s->learned_src == src && s->learned_term;
}

/* (Re)build the queue from the entry's end; nothing once a burst of it went. */
static void schedule(rdof_mssim_t *s)
{
    rdof_mssim_entry_t *e = cur(s);
    if (s->nsent)
        return;
    s->nq = 0;
    if (e->kind == RDOF_MSSIM_NONE)
        return;
    const rdof_mssim_step_t *st = &s->prof.step[e->step];
    int64_t at = e->end48 + (int64_t)st->slots * RDOF_MSSIM_SLOT48;
    uint64_t sync = answer_sync(s->family, st->slots);
    uint32_t me = e->dst, them = e->src;
    switch (e->kind) {
    case RDOF_MSSIM_ACK:
    case RDOF_MSSIM_ACK_TWICE:
        rdof_mssim_response(me, them, e->sap, 0, 1, e->ns, 0, s->cc, sync, s->q[0].burst);
        s->q[0].at48 = at;
        s->nq = 1;
        if (e->kind == RDOF_MSSIM_ACK_TWICE) {
            memcpy(s->q[1].burst, s->q[0].burst, 33);
            s->q[1].at48 = at + FRAME48;
            s->nq = 2;
        }
        break;
    case RDOF_MSSIM_NACK:
        rdof_mssim_response(me, them, e->sap, 1, st->nack_type, e->ns, 0, s->cc, sync, s->q[0].burst);
        s->q[0].at48 = at;
        s->nq = 1;
        break;
    case RDOF_MSSIM_SACK: {
        uint64_t got = ~st->resend;
        rdof_mssim_response(me, them, e->sap, 2, st->sack_type, e->ns, 1, s->cc, sync, s->q[0].burst);
        rdof_mssim_sack_block(got, s->cc, sync, s->q[1].burst);
        s->q[0].at48 = at;
        s->q[1].at48 = at + FRAME48;
        s->nq = 2;
        break;
    }
    default:
        break;
    }
    for (int i = 0; i < s->nq; i++)
        s->q[i].first = i == 0;
    e->asked48 = at;
}

static void close_packet(rdof_mssim_t *s)
{
    if (!s->active)
        return;
    rdof_mssim_entry_t *e = cur(s);
    s->learned_src = e->src;
    s->learned_term = e->terminator;
    s->active = false;
    if (s->nsent < s->nq) {
        if (s->nsent == 0 && e->kind != RDOF_MSSIM_NONE)
            e->failed = true; /* another packet or the end came before the answer went */
        s->nq = s->nsent = 0;
    }
}

static void start_packet(rdof_mssim_t *s, const rdof_burst_t *b, const rdof_data_header_t *h, int64_t pos48)
{
    close_packet(s);
    s->cur = s->logpos;
    s->logpos = (s->logpos + 1) % RDOF_MSSIM_LOG;
    if (s->nlog < RDOF_MSSIM_LOG)
        s->nlog++;
    rdof_mssim_entry_t *e = cur(s);
    memset(e, 0, sizeof *e);
    e->seq = ++s->seq;
    e->src = h->src_id;
    e->dst = h->dst_id;
    e->sap = h->sap;
    e->ns = h->ns;
    e->fsn = h->fsn;
    e->blocks = h->blocks;
    e->rate_dt = -1;
    e->header48 = pos48;
    e->placed48 = e->air48 = -1;
    int n = s->prof.nsteps;
    e->step = s->prof.repeat ? s->answered % n : (s->answered < n ? s->answered : n - 1);
    e->kind = s->prof.step[e->step].kind;
    e->slots = s->prof.step[e->step].slots;
    bool term = wants_terminator(s, e->src);
    e->end_used = term ? RDOF_MSSIM_END_TERMINATOR : RDOF_MSSIM_END_BLOCK;
    e->end48 = pos48 + (int64_t)(h->blocks + (term ? 1 : 0)) * FRAME48;
    s->family = b->family;
    s->cc = b->cc >= 0 ? b->cc : 0;
    s->last48 = pos48;
    s->active = true;
    s->nq = s->nsent = 0;
    s->answered++;
    schedule(s);
}

void rdof_mssim_heard(rdof_mssim_t *s, const rdof_burst_t *b, int64_t pos48)
{
    if (!s->armed)
        return;
    if (b->dt == RDOF_DT_DATA_HEADER) {
        rdof_data_header_t h;
        rdof_decode_data_header(b->burst, &h);
        if (!h.crc_ok || h.dpf != RDOF_DPF_CONFIRMED || !h.response_requested || h.group || h.blocks < 1)
            return;
        if ((s->prof.to && h.dst_id != s->prof.to) || (s->prof.from && h.src_id != s->prof.from))
            return;
        if (s->prof.count && s->answered >= s->prof.count)
            return;
        start_packet(s, b, &h, pos48);
        return;
    }
    if (!s->active)
        return;
    rdof_mssim_entry_t *e = cur(s);
    if (b->dt == RDOF_DT_RATE_12 || b->dt == RDOF_DT_RATE_34 || b->dt == RDOF_DT_RATE_1) {
        if (e->heard >= e->blocks || pos48 > e->header48 + (int64_t)(e->blocks + 1) * FRAME48)
            return;
        e->heard++;
        e->rate_dt = b->dt;
        s->last48 = pos48;
        rdof_data_block_t blk;
        int dbsn, blen;
        uint8_t body[24];
        if (rdof_decode_data_block(b->burst, b->dt, &blk) && rdof_split_confirmed(&blk, &dbsn, body, &blen))
            e->dbsn |= 1ULL << (dbsn & 63);
        if (e->heard == e->blocks && e->end_used == RDOF_MSSIM_END_BLOCK) {
            e->end48 = pos48;
            e->end_heard = true;
            schedule(s);
        }
    } else if (b->dt == RDOF_DT_TERMINATOR_WITH_LC) {
        if (pos48 > s->last48 + FRAME48 + RDOF_MSSIM_SLOT48)
            return;
        e->terminator = true;
        s->last48 = pos48;
        if (e->end_used == RDOF_MSSIM_END_TERMINATOR) {
            e->end48 = pos48;
            e->end_heard = true;
            schedule(s);
        }
    }
}

void rdof_mssim_echo(rdof_mssim_t *s, int64_t pos48)
{
    /* the nearest answer not yet heard back, within 4 slots (a front end's delay past what's assumed) */
    rdof_mssim_entry_t *best = NULL;
    for (int i = 0; i < s->nlog; i++) {
        rdof_mssim_entry_t *e = &s->log[i];
        if (e->sent && e->air48 < 0 && llabs(pos48 - e->placed48) < 4 * RDOF_MSSIM_SLOT48 &&
            (!best || llabs(pos48 - e->placed48) < llabs(pos48 - best->placed48)))
            best = e;
    }
    if (best)
        best->air48 = pos48;
}

void rdof_mssim_poll(rdof_mssim_t *s, int64_t now48, int64_t lead48)
{
    if (!s->active)
        return;
    rdof_mssim_entry_t *e = cur(s);
    while (s->nsent < s->nq && s->q[s->nsent].at48 - lead48 <= now48) {
        int64_t placed = -1;
        bool ok = s->send && s->send(s->ctx, s->q[s->nsent].at48, s->q[s->nsent].burst, &placed);
        if (s->q[s->nsent].first) {
            e->sent = ok;
            e->failed = !ok;
            e->placed48 = ok ? placed : -1;
        }
        s->nsent++;
    }
    int64_t done = s->nq ? s->q[s->nq - 1].at48 : e->end48;
    if (s->nsent == s->nq && now48 > s->last48 + IDLE48 && now48 > done + FRAME48) {
        close_packet(s);
        if (s->prof.count && s->answered >= s->prof.count)
            s->armed = false;
    }
}

/* ── the log ───────────────────────────────────────────────────────────────── */

static long long us(int64_t d48) { return (long long)(d48 * 1000000 / 48000); }

size_t rdof_mssim_log_json(const rdof_mssim_t *s, uint32_t since, char *out, size_t cap, uint32_t *next)
{
    if (next)
        *next = since;
    if (cap < 3) {
        if (cap)
            out[0] = 0;
        return 0;
    }
    size_t n = 0;
    int w;
    const size_t room = cap - 1; /* the closing bracket's */
#define PUT(...)                                                                                                       \
    do {                                                                                                               \
        w = snprintf(out + n, n < room ? room - n : 0, __VA_ARGS__);                                                   \
        if (w < 0 || (n += (size_t)w) >= room)                                                                         \
            goto full;                                                                                                 \
    } while (0)
    out[n++] = '[';
    bool first = true;
    size_t keep = n;
    for (int k = 0; k < s->nlog; k++) {
        const rdof_mssim_entry_t *e = &s->log[(s->logpos - s->nlog + k + RDOF_MSSIM_LOG) % RDOF_MSSIM_LOG];
        if (e->seq <= since)
            continue;
        PUT("%s{\"seq\":%u,\"src\":%u,\"dst\":%u,\"sap\":%d,\"ns\":%d,\"fsn\":%d,\"blocks\":%d,\"heard\":%d,"
            "\"dbsn\":\"%016llx\",\"rate_dt\":%d,\"terminator\":%s,\"end\":\"%s\",\"end_heard\":%s,\"step\":%d,"
            "\"answer\":\"%s\",\"slots\":%d,\"header48\":%lld,\"end48\":%lld,\"asked48\":%lld,\"placed48\":%lld,"
            "\"air48\":%lld,\"sent\":%s,\"failed\":%s",
            first ? "" : ",", e->seq, e->src, e->dst, e->sap, e->ns, e->fsn, e->blocks, e->heard,
            (unsigned long long)e->dbsn, e->rate_dt, e->terminator ? "true" : "false",
            e->end_used == RDOF_MSSIM_END_TERMINATOR ? "terminator" : "block", e->end_heard ? "true" : "false",
            e->step, rdof_mssim_kind_name(e->kind), e->slots, (long long)e->header48, (long long)e->end48,
            (long long)e->asked48, (long long)e->placed48, (long long)e->air48, e->sent ? "true" : "false",
            e->failed ? "true" : "false");
        if (e->sent) /* start to start, and the gap after the end burst's 27.5 ms */
            PUT(",\"turnaround_us\":%lld,\"gap_us\":%lld", us(e->placed48 - e->end48),
                us(e->placed48 - e->end48 - BURST48));
        if (e->air48 >= 0)
            PUT(",\"air_turnaround_us\":%lld,\"air_gap_us\":%lld,\"air_late_us\":%lld", us(e->air48 - e->end48),
                us(e->air48 - e->end48 - BURST48), us(e->air48 - e->placed48));
        PUT("}");
        first = false;
        keep = n;
        if (next)
            *next = e->seq;
    }
full:
#undef PUT
    n = keep; /* an entry that didn't fit waits for the next call */
    out[n++] = ']';
    out[n] = 0;
    return n;
}
