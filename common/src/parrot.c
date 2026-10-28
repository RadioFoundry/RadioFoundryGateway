// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/parrot.h"

#if RDOF_FEATURE_TX

#include <string.h>

#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/ipgw.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"

#define FRAME_S 0.06 /* one slot's bursts come a 60 ms frame apart */

void rdof_parrot_init(rdof_parrot_t *p, uint32_t id)
{
    memset(p, 0, sizeof *p);
    p->id = id;
    p->delay_s = RDOF_PARROT_DELAY_S;
}

const char *rdof_parrot_state_name(const rdof_parrot_t *p)
{
    static const char *names[] = {"idle", "recording", "waiting", "replaying"};
    return names[p->state];
}

/* A burst that starts a transmission, and whom it's from and to: a voice LC header, a CSBK (a data preamble
 * among them), an MBC header or a data header. */
static bool starts(uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], bool *voice, uint32_t *src,
                   uint32_t *dst, bool *group)
{
    if (!(control & RDOF_SYNC_DATA))
        return false;
    const int dt = control & 0x0F;
    if (dt != RDOF_DT_VOICE_LC_HEADER && dt != RDOF_DT_CSBK && dt != RDOF_DT_MBC_HEADER && dt != RDOF_DT_DATA_HEADER)
        return false;
    uint8_t b[12];
    if (!rdof_bptc196_decode(burst, b))
        return false;
    *voice = dt == RDOF_DT_VOICE_LC_HEADER;
    if (dt == RDOF_DT_VOICE_LC_HEADER) { /* full LC: FLCO, FID, service options, dst, src */
        *group = (b[0] & 0x3F) == RDOF_FLCO_GROUP;
        *dst = rdof_be24(b + 3);
        *src = rdof_be24(b + 6);
    } else if (dt == RDOF_DT_DATA_HEADER) { /* G/I, ..., dst, src */
        *group = b[0] & 0x80;
        *dst = rdof_be24(b + 2);
        *src = rdof_be24(b + 5);
    } else { /* a CSBK or MBC header: opcode, FID, two bytes, dst, src (the preamble's group bit in the third) */
        *group = (b[0] & 0x3F) == RDOF_CSBK_PREAMBLE && (b[2] & 0x40);
        *dst = rdof_be24(b + 4);
        *src = rdof_be24(b + 7);
    }
    return true;
}

#if RDOF_FEATURE_DATA_BLOCKS
static void swap8(uint8_t *a, uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

/* Private packet data, whole and its CRC good: the recording replaced by the reply (the header comment says how).
 * Anything else stays as it was heard. */
static void data_reply(rdof_parrot_t *p)
{
    int h = -1;
    for (int i = 0; i < p->n && h < 0; i++)
        if ((p->bursts[i].control & RDOF_SYNC_DATA) && (p->bursts[i].control & 0x0F) == RDOF_DT_DATA_HEADER)
            h = i;
    if (h < 0)
        return;
    rdof_data_header_t hd;
    rdof_decode_data_header(p->bursts[h].burst, &hd);
    if (!hd.crc_ok || hd.group || (hd.dpf != RDOF_DPF_UNCONFIRMED && hd.dpf != RDOF_DPF_CONFIRMED) || hd.blocks <= 0)
        return;
    int len = 0, got = 0;
    for (int i = h + 1; i < p->n && got < hd.blocks; i++) {
        const int dt = p->bursts[i].control & 0x0F;
        if (!(p->bursts[i].control & RDOF_SYNC_DATA))
            return;
        if (dt == RDOF_DT_DATA_HEADER) { /* a second header (proprietary): one of the blocks */
            got++;
            continue;
        }
        if (dt != RDOF_DT_RATE_12 && dt != RDOF_DT_RATE_34 && dt != RDOF_DT_RATE_1)
            return;
        rdof_data_block_t b;
        if (!rdof_decode_data_block(p->bursts[i].burst, dt, &b) || !b.fec_ok)
            return;
        uint8_t body[24];
        int n = b.len;
        const uint8_t *src = b.payload;
        if (hd.dpf == RDOF_DPF_CONFIRMED) {
            int dbsn;
            if (!rdof_split_confirmed(&b, &dbsn, body, &n))
                return;
            src = body;
        }
        if (len + n > (int)sizeof p->data)
            return;
        memcpy(p->data + len, src, (size_t)n);
        len += n;
        got++;
    }
    const int user = len - 4 - hd.pad_octets;
    if (got < hd.blocks || user < 0 || !rdof_check_crc32(p->data, (size_t)len))
        return;
    uint8_t *d = p->data;
    if (hd.sap == RDOF_SAP_IP && user >= 20 && d[0] >> 4 == 4) {
        const int ihl = (d[0] & 0x0F) * 4;
        swap8(d + 12, d + 16, 4);
        if (d[9] == 17 && user >= ihl + 8)
            swap8(d + ihl, d + ihl + 2, 2);
    } else if (hd.sap == RDOF_SAP_UDP_COMPRESSED && user >= 5) {
        if ((d[2] & 0x0F) == 2)
            return; /* to a talkgroup's address */
        d[2] = (uint8_t)(d[2] << 4 | d[2] >> 4);                     /* SAID and DAID */
        const uint8_t s3 = d[3], s4 = d[4];                          /* SPID and DPID, the opcode bits stay */
        d[3] = (uint8_t)((s3 & 0x80) | (s4 & 0x7F));
        d[4] = (uint8_t)((s4 & 0x80) | (s3 & 0x7F));
        if (!(s3 & 0x7F) && !(s4 & 0x7F) && user >= 9)
            swap8(d + 5, d + 7, 2); /* both ports there; with one, it stays in its place */
    }
    rdof_ipgw_dmr_t *r = &p->reply;
    if (!rdof_ipgw_build_dmr(hd.dst_id, hd.src_id, hd.sap, d, (size_t)user, p->cc, rdof_data_sync(RDOF_BS_VOICE), r))
        return;
    const int pre = h < RDOF_PARROT_BURSTS - r->nbursts ? h : RDOF_PARROT_BURSTS - r->nbursts;
    uint8_t (*pb)[RDOF_BURST_BYTES] = (uint8_t (*)[RDOF_BURST_BYTES])p->data; /* the message is in r now */
    const int npre = pre * RDOF_BURST_BYTES <= (int)sizeof p->data ? pre : (int)sizeof p->data / RDOF_BURST_BYTES;
    if (npre)
        rdof_csbk_preambles(hd.dst_id, hd.src_id, false, npre, p->cc, pb);
    for (int i = 0; i < npre; i++) {
        p->bursts[i].control = RDOF_SYNC_DATA | RDOF_DT_CSBK;
        memcpy(p->bursts[i].burst, pb[i], RDOF_BURST_BYTES);
    }
    for (int i = 0; i < r->nbursts; i++) {
        p->bursts[npre + i].control = r->control[i];
        memcpy(p->bursts[npre + i].burst, r->bursts[i], RDOF_BURST_BYTES);
    }
    p->n = npre + r->nbursts;
    p->src = hd.dst_id;
    p->dst = hd.src_id;
    p->data_replies++;
}
#endif

static void end(rdof_parrot_t *p, double due)
{
    if (!p->n) {
        p->state = RDOF_PARROT_IDLE;
        return;
    }
#if RDOF_FEATURE_DATA_BLOCKS
    if (!p->voice)
        data_reply(p);
#endif
    p->state = RDOF_PARROT_WAITING;
    p->due = due;
}

/* A burst as it's kept: regenerated (the header comment says how) */
static void clean(rdof_parrot_t *p, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], uint8_t out[RDOF_BURST_BYTES])
{
    if (!(control & RDOF_SYNC_DATA)) {
        if (p->voice) {
            const long before = p->regen.errors;
            rdof_voice_regen_burst(&p->regen, control, burst, out);
            p->corrected += p->regen.errors - before;
        } else {
            memcpy(out, burst, RDOF_BURST_BYTES);
        }
        return;
    }
    const int dt = control & 0x0F;
    if (p->voice && (dt == RDOF_DT_VOICE_LC_HEADER || dt == RDOF_DT_TERMINATOR_WITH_LC)) {
        rdof_lc_burst(p->regen.lc9, dt, p->cc, rdof_data_sync(RDOF_MS_VOICE), out);
        return;
    }
    if (!rdof_duplex_data(control, burst, p->cc, out)) /* a CSBK or data header whose CRC fails: kept as heard */
        memcpy(out, burst, RDOF_BURST_BYTES);
}

void rdof_parrot_heard(rdof_parrot_t *p, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], double now)
{
    if (p->state == RDOF_PARROT_WAITING || p->state == RDOF_PARROT_REPLAYING)
        return;
    if ((control & RDOF_SYNC_DATA) && (control & 0x0F) == RDOF_DT_IDLE)
        return;
    if (p->state == RDOF_PARROT_IDLE) {
        bool voice, group;
        uint32_t src, dst;
        if (!starts(control, burst, &voice, &src, &dst, &group))
            return; /* late entry, or the rest of a transmission not kept */
        if ((p->id && dst != p->id) || (p->admit && !p->admit(p->ctx, slot, src, dst, group))) {
            p->ignored++;
            return;
        }
        p->state = RDOF_PARROT_RECORDING;
        p->slot = slot;
        p->voice = voice;
        p->src = src;
        p->dst = dst;
        p->group = group;
        p->n = 0;
        p->corrected = 0;
        int cc, dt, errors;
        p->cc = rdof_decode_slot_type(burst, &cc, &dt, &errors) ? cc : 1;
        if (voice) {
            uint8_t lc[12];
            rdof_bptc196_decode(burst, lc);
            if (!group) { /* a private call comes back from whom it called to the caller, as a reply would */
                uint8_t t[3];
                memcpy(t, lc + 3, 3);
                memcpy(lc + 3, lc + 6, 3);
                memcpy(lc + 6, t, 3);
                p->src = dst;
                p->dst = src;
            }
            rdof_voice_regen_init(&p->regen, lc, p->cc);
            p->regen.swap = !group; /* the embedded LC too, or the radio drops the call at the first superframe */
        }
    } else if (slot != p->slot) {
        return;
    } else if (!p->voice && (control & RDOF_SYNC_DATA) && (control & 0x0F) == RDOF_DT_VOICE_LC_HEADER) {
        /* data or a CSBK, then a call right after it: the call is its own transmission */
        bool voice, group;
        uint32_t src, dst;
        if (starts(control, burst, &voice, &src, &dst, &group)) {
            end(p, now + p->delay_s);
            return;
        }
    }
    const bool terminator = (control & RDOF_SYNC_DATA) && (control & 0x0F) == RDOF_DT_TERMINATOR_WITH_LC;
    if (p->n < RDOF_PARROT_BURSTS - 1 || (terminator && p->n < RDOF_PARROT_BURSTS)) {
        p->bursts[p->n].control = control;
        clean(p, control, burst, p->bursts[p->n].burst);
        p->n++;
    } else {
        p->cut++; /* full: the last place is kept for the terminator */
    }
    p->last = now;
    if (p->voice && terminator)
        end(p, now + p->delay_s);
}

void rdof_parrot_lost(rdof_parrot_t *p, int slot, double now)
{
    if (p->state == RDOF_PARROT_RECORDING && slot == p->slot)
        end(p, now + p->delay_s);
}

static void send(rdof_parrot_t *p, int i)
{
    rdof_net_burst_t b;
    memset(&b, 0, sizeof b);
    rdof_put_be32(b.stream_id, RDOF_PARROT_STREAM | (p->streams & 0xFFFF));
    b.control = p->bursts[i].control;
    memcpy(b.burst, p->bursts[i].burst, RDOF_BURST_BYTES);
    b.terminator = i == p->n - 1; /* the end even when the call was cut or lost */
    b.seq = (uint8_t)i;
    b.src = p->src;
    b.dst = p->dst;
    b.group = p->group;
    b.slot = p->slot;
    if (p->send)
        p->send(p->ctx, &b);
}

void rdof_parrot_poll(rdof_parrot_t *p, double now)
{
    if (p->state == RDOF_PARROT_RECORDING && now - p->last >= RDOF_PARROT_GAP_S) {
        const double due = p->last + p->delay_s;
        end(p, due > now ? due : now);
    }
    if (p->state == RDOF_PARROT_WAITING && now >= p->due) {
        p->streams++;
        p->sent = 0;
        p->state = RDOF_PARROT_REPLAYING;
        if (p->start) {
            uint8_t sid[4];
            rdof_put_be32(sid, RDOF_PARROT_STREAM | (p->streams & 0xFFFF));
            p->start(p->ctx, sid, p->slot, p->src, p->dst, p->group);
        }
    }
    if (p->state == RDOF_PARROT_REPLAYING) {
        const int upto = (int)((now - p->due) / FRAME_S) + 1 + RDOF_PARROT_AHEAD;
        while (p->sent < p->n && p->sent < upto)
            send(p, p->sent++);
        if (p->sent == p->n) {
            p->replayed++;
            p->state = RDOF_PARROT_IDLE;
        }
    }
}

#endif /* RDOF_FEATURE_TX */
