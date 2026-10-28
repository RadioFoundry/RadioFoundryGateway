// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The parrot (rdof/parrot.h): a voice call, data and a CSBK sent back burst for burst, paced, after the delay;
 * the id filter; a lost call; the other slot while busy; a recording that fills; bit errors regenerated out. */
#include <stdio.h>
#include <string.h>

#include "rdof/ambe.h"
#include "rdof/bits.h"
#include "rdof/ipgw.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/parrot.h"
#include "rdof/pdu.h"

static int checks, failures;

#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        checks++;                                                                                                    \
        if (!(c)) {                                                                                                  \
            failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                      \
        }                                                                                                            \
    } while (0)

#define SENT_MAX 1100
static struct {
    int starts, n;
    uint8_t sid[4];
    int slot;
    uint32_t src, dst;
    bool group;
    rdof_net_burst_t b[SENT_MAX];
    double at[SENT_MAX];
} S;
static double clock_now;
static rdof_parrot_t P;

static void on_start(void *ctx, const uint8_t sid[4], int slot, uint32_t src, uint32_t dst, bool group)
{
    S.starts++;
    memcpy(S.sid, sid, 4);
    S.slot = slot;
    S.src = src;
    S.dst = dst;
    S.group = group;
}

static void on_send(void *ctx, const rdof_net_burst_t *b)
{
    if (S.n < SENT_MAX) {
        S.at[S.n] = clock_now;
        S.b[S.n++] = *b;
    }
}

static void reset(uint32_t id)
{
    memset(&S, 0, sizeof S);
    rdof_parrot_init(&P, id);
    P.start = on_start;
    P.send = on_send;
    clock_now = 100.0;
}

static const uint64_t DATA_SYNC = RDOF_MS_VOICE ^ RDOF_DATA_XOR;

static void lc_burst(int dt, uint32_t src, uint32_t dst, bool group, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t lc[9];
    rdof_group_lc(src, dst, group, 0, lc);
    if (!group)
        lc[0] = RDOF_FLCO_PRIVATE;
    rdof_lc_burst(lc, dt, 1, DATA_SYNC, out);
}

/* hear one burst on a slot, a 60 ms frame after the last, polling as a platform does */
static void hear(int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES])
{
    rdof_parrot_heard(&P, slot, control, burst, clock_now);
    rdof_parrot_poll(&P, clock_now);
    clock_now += 0.06;
}

static void run(double seconds)
{
    for (double end = clock_now + seconds; clock_now < end; clock_now += 0.02)
        rdof_parrot_poll(&P, clock_now);
}

/* a voice call: header, n superframes, terminator; returns the bursts heard */
static int call(int slot, uint32_t src, uint32_t dst, bool group, int superframes, bool terminator)
{
    uint8_t b[RDOF_BURST_BYTES];
    int n = 0;
    lc_burst(RDOF_DT_VOICE_LC_HEADER, src, dst, group, b);
    hear(slot, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b), n++;
    for (int s = 0; s < superframes; s++)
        for (int k = 0; k < 6; k++) {
            for (int i = 0; i < RDOF_BURST_BYTES; i++)
                b[i] = (uint8_t)(s * 31 + k * 7 + i);
            hear(slot, k ? (uint8_t)k : RDOF_SYNC_AUDIO, b), n++;
        }
    if (terminator) {
        lc_burst(RDOF_DT_TERMINATOR_WITH_LC, src, dst, group, b);
        hear(slot, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, b), n++;
    }
    return n;
}

static void test_voice(void)
{
    reset(0);
    const double t0 = clock_now;
    const int n = call(1, 9800001, 91, true, 3, true);
    CHECK(n == 20);
    CHECK(P.state == RDOF_PARROT_WAITING);
    CHECK(S.n == 0);
    run(RDOF_PARROT_DELAY_S - 0.1);
    CHECK(S.starts == 0); /* the delay after the terminator */
    while (!S.starts && clock_now < t0 + RDOF_PARROT_DELAY_S + 3.0) {
        rdof_parrot_poll(&P, clock_now);
        clock_now += 0.02;
    }
    CHECK(S.starts == 1);
    CHECK(S.slot == 1 && S.src == 9800001 && S.dst == 91 && S.group);
    CHECK(S.n == 1 + RDOF_PARROT_AHEAD); /* a few ahead, no more */
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.n == n);
    CHECK(P.state == RDOF_PARROT_IDLE && P.replayed == 1);
    /* the same bursts, in order, one stream, the last one the end */
    CHECK(S.b[0].control == (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER));
    CHECK(S.b[1].control == RDOF_SYNC_AUDIO && S.b[1].burst[0] == 0);
    CHECK(S.b[n - 1].control == (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC));
    int ends = 0;
    for (int i = 0; i < n; i++) {
        CHECK(!memcmp(S.b[i].stream_id, S.sid, 4));
        CHECK(S.b[i].seq == (uint8_t)i);
        ends += S.b[i].terminator;
    }
    CHECK(ends == 1 && S.b[n - 1].terminator);
    /* paced: the last burst goes about n frames after the first, not all at once */
    CHECK(S.at[n - 1] - S.at[0] > (n - 2 - RDOF_PARROT_AHEAD) * 0.06);
    CHECK(S.at[0] - t0 < n * 0.06 + RDOF_PARROT_DELAY_S + 0.1);
    /* a second call is a new stream */
    uint8_t first[4];
    memcpy(first, S.sid, 4);
    call(2, 9800002, 9800001, false, 1, true);
    run(RDOF_PARROT_DELAY_S + 1.0);
    /* a private call comes back from whom it called, to the caller: the ids and the LC swapped */
    CHECK(S.starts == 2 && memcmp(S.sid, first, 4) && S.slot == 2 && !S.group && S.src == 9800001 &&
          S.dst == 9800002);
    rdof_full_lc_t lc;
    CHECK(rdof_decode_voice_lc(S.b[S.n - 1].burst, RDOF_DT_TERMINATOR_WITH_LC, &lc) && lc.src_id == 9800001 &&
          lc.dst_id == 9800002 && S.b[S.n - 1].src == 9800001 && S.b[S.n - 1].dst == 9800002);
}

/* A private call's embedded LC comes back swapped too: a radio that heard the header to it, then an embedded LC to
 * someone else, drops the call at the first superframe. Two superframes: the second sends what the first carried. */
static void test_private_embedded(void)
{
    reset(0);
    uint8_t lc[9], b[RDOF_BURST_BYTES], raw[128];
    rdof_group_lc(9800002, 9800001, false, 0, lc);
    lc[0] = RDOF_FLCO_PRIVATE;
    rdof_embedded_lc_encode(lc, raw);
    rdof_lc_burst(lc, RDOF_DT_VOICE_LC_HEADER, 1, DATA_SYNC, b);
    hear(2, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b);
    for (int s = 0; s < 2; s++)
        for (int n = 0; n < 6; n++) {
            const uint8_t ctl = rdof_silence_burst(n, 1, false, n >= 1 && n <= 4 ? raw + 32 * (n - 1) : NULL, b);
            hear(2, ctl, b);
        }
    rdof_lc_burst(lc, RDOF_DT_TERMINATOR_WITH_LC, 1, DATA_SYNC, b);
    hear(2, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, b);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.n == 14);
    for (int s = 0; s < 2; s++) {
        uint8_t got[128], out[9];
        for (int n = 1; n <= 4; n++)
            for (int i = 0; i < 32; i++)
                got[32 * (n - 1) + i] = (uint8_t)rdof_get_field(S.b[1 + 6 * s + n].burst, 116 + i, 1);
        CHECK(rdof_embedded_lc_decode(got, out));
        CHECK((out[0] & 0x3F) == RDOF_FLCO_PRIVATE && rdof_get_field(out, 24, 24) == 9800002 &&
              rdof_get_field(out, 48, 24) == 9800001);
    }
}

static void test_id(void)
{
    reset(9990);
    call(1, 9800001, 91, true, 2, true);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.starts == 0 && P.ignored == 1 && P.state == RDOF_PARROT_IDLE);
    call(1, 9800001, 9990, false, 2, true);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.starts == 1 && S.src == 9990 && S.dst == 9800001 && S.n == 14);
}

/* the talk path's policy: slot 1 may not reach the parrot */
static bool admit_slot2(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    (void)ctx, (void)src, (void)dst, (void)group;
    return slot == 2;
}

static void test_admit(void)
{
    reset(9990);
    P.admit = admit_slot2;
    call(1, 9800001, 9990, false, 2, true);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.starts == 0 && P.ignored == 1 && P.state == RDOF_PARROT_IDLE);
    call(2, 9800001, 9990, false, 2, true);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.starts == 1 && S.slot == 2);
}

static void test_lost_and_late(void)
{
    reset(0);
    /* late entry: no header, nothing kept */
    uint8_t b[RDOF_BURST_BYTES] = {0};
    for (int i = 0; i < 12; i++)
        hear(1, i % 6 ? (uint8_t)(i % 6) : RDOF_SYNC_AUDIO, b);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.starts == 0 && P.state == RDOF_PARROT_IDLE);
    /* a call with no terminator: lost, then sent back with the last burst marked the end */
    const int n = call(1, 9800001, 91, true, 2, false);
    rdof_parrot_lost(&P, 1, clock_now);
    CHECK(P.state == RDOF_PARROT_WAITING);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.n == n && S.b[n - 1].terminator && S.b[n - 1].control == 5);
    /* nor lost: it ends after the gap */
    reset(0);
    call(1, 9800001, 91, true, 1, false);
    run(RDOF_PARROT_GAP_S - 0.1);
    CHECK(P.state == RDOF_PARROT_RECORDING);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.n == 7 && P.state == RDOF_PARROT_IDLE);
}

static void test_data(void)
{
    reset(0);
    uint8_t b[RDOF_BURST_BYTES], pl[12];
    /* two preamble CSBKs, an unconfirmed data header, three rate 1/2 blocks */
    memset(pl, 0, sizeof pl);
    pl[0] = 0x80 | RDOF_CSBK_PREAMBLE;
    pl[2] = 0x80; /* data follows, individual */
    pl[3] = 4;
    rdof_put_be24(pl + 4, 9800001);
    rdof_put_be24(pl + 7, 9800002);
    rdof_data_burst(pl, RDOF_DT_CSBK, 1, DATA_SYNC, b);
    hear(2, RDOF_SYNC_DATA | RDOF_DT_CSBK, b);
    hear(2, RDOF_SYNC_DATA | RDOF_DT_CSBK, b);
    memset(pl, 0, sizeof pl);
    pl[0] = RDOF_DPF_UNCONFIRMED;
    rdof_put_be24(pl + 2, 9800001);
    rdof_put_be24(pl + 5, 9800002);
    rdof_data_burst(pl, RDOF_DT_DATA_HEADER, 1, DATA_SYNC, b);
    hear(2, RDOF_SYNC_DATA | RDOF_DT_DATA_HEADER, b);
    for (int i = 0; i < 3; i++) {
        memset(b, 0x11 * (i + 1), sizeof b);
        hear(2, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b);
    }
    /* idle bursts are not part of it */
    rdof_data_burst(pl, RDOF_DT_IDLE, 1, DATA_SYNC, b);
    hear(2, RDOF_SYNC_DATA | RDOF_DT_IDLE, b);
    CHECK(P.state == RDOF_PARROT_RECORDING);
    run(RDOF_PARROT_DELAY_S + 2.0);
    CHECK(S.starts == 1 && S.n == 6 && S.slot == 2);
    CHECK(S.dst == 9800001 && S.src == 9800002 && !S.group);
    CHECK(S.b[2].control == (RDOF_SYNC_DATA | RDOF_DT_DATA_HEADER) && S.b[5].terminator);
    CHECK(S.b[5].burst[0] == 0x33);

    /* a lone CSBK */
    reset(9800001);
    memset(pl, 0, sizeof pl);
    pl[0] = 0x80 | 0x04; /* UU_V_Req */
    rdof_put_be24(pl + 4, 9800001);
    rdof_put_be24(pl + 7, 9800002);
    rdof_data_burst(pl, RDOF_DT_CSBK, 1, DATA_SYNC, b);
    hear(1, RDOF_SYNC_DATA | RDOF_DT_CSBK, b);
    run(RDOF_PARROT_DELAY_S + 2.0);
    CHECK(S.starts == 1 && S.n == 1 && S.b[0].terminator && S.dst == 9800001);
}

static void test_busy_and_full(void)
{
    reset(0);
    uint8_t b[RDOF_BURST_BYTES];
    /* a call on slot 1; one on slot 2 at the same time is not kept */
    lc_burst(RDOF_DT_VOICE_LC_HEADER, 9800001, 91, true, b);
    hear(1, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b);
    lc_burst(RDOF_DT_VOICE_LC_HEADER, 9800002, 92, true, b);
    hear(2, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b);
    memset(b, 0, sizeof b);
    hear(1, RDOF_SYNC_AUDIO, b);
    hear(2, RDOF_SYNC_AUDIO, b);
    lc_burst(RDOF_DT_TERMINATOR_WITH_LC, 9800001, 91, true, b);
    hear(1, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, b);
    /* nor is anything heard while waiting or replaying (its own replay, on a simplex radio) */
    call(1, 9800003, 93, true, 1, true);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.starts == 1 && S.n == 3 && S.dst == 91);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.starts == 1);

    /* longer than the recording: cut, the terminator kept */
    reset(0);
    const int sf = RDOF_PARROT_BURSTS / 6 + 2;
    const int n = call(1, 9800001, 91, true, sf, true);
    CHECK(P.n == RDOF_PARROT_BURSTS);
    CHECK(P.cut == (uint32_t)(n - RDOF_PARROT_BURSTS));
    run(RDOF_PARROT_BURSTS * 0.06 + RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.n == RDOF_PARROT_BURSTS);
    CHECK(S.b[S.n - 1].control == (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC) && S.b[S.n - 1].terminator);
}

/* A bit of AMBE frame i (0-2) of a voice burst whose flip its Golay codes correct; -1 none. Returns the burst's bit. */
static int protected_bit(const uint8_t burst[RDOF_BURST_BYTES], int i, int from)
{
    uint8_t bits[264], voice[216], frame[9];
    rdof_bytes_to_bits(burst, RDOF_BURST_BYTES, bits);
    memcpy(voice, bits, 108);
    memcpy(voice + 108, bits + 156, 108);
    for (int k = from; k < 72; k++) {
        voice[72 * i + k] ^= 1;
        rdof_bits_to_bytes(voice + 72 * i, 72, frame);
        const int corrected = rdof_ambe_decode_frame(frame, NULL);
        voice[72 * i + k] ^= 1;
        if (corrected == 1) {
            const int vb = 72 * i + k;
            return vb < 108 ? vb : vb + 48;
        }
    }
    return -1;
}

static void flip(uint8_t burst[RDOF_BURST_BYTES], int bit) { burst[bit / 8] ^= (uint8_t)(0x80 >> (bit % 8)); }

/* bit errors the FEC can take out don't go back on the air: the header, voice and terminator come back clean */
static void test_regen(void)
{
    reset(0);
    uint8_t lc[9], hdr[RDOF_BURST_BYTES], term[RDOF_BURST_BYTES], v[6][RDOF_BURST_BYTES], b[RDOF_BURST_BYTES];
    uint8_t ctl[6];
    rdof_group_lc(9800001, 91, true, 0, lc);
    rdof_lc_burst(lc, RDOF_DT_VOICE_LC_HEADER, 1, DATA_SYNC, hdr);
    rdof_lc_burst(lc, RDOF_DT_TERMINATOR_WITH_LC, 1, DATA_SYNC, term);
    uint8_t raw[128], emb[4][32];
    rdof_embedded_lc_encode(lc, raw);
    for (int n = 0; n < 6; n++)
        ctl[n] = rdof_silence_burst(n, 1, false, n >= 1 && n <= 4 ? raw + 32 * (n - 1) : NULL, v[n]);
    memcpy(emb, raw, sizeof emb);
    /* heard with errors: 3 bits in the header, one in each AMBE frame of A-F, 3 in the terminator */
    memcpy(b, hdr, sizeof b);
    b[0] ^= 0x80, b[5] ^= 0x04, b[30] ^= 0x10;
    hear(1, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b);
    for (int n = 0; n < 6; n++) {
        memcpy(b, v[n], sizeof b);
        for (int i = 0; i < 3; i++) {
            const int bit = protected_bit(v[n], i, 5 * n);
            CHECK(bit >= 0);
            if (bit >= 0)
                flip(b, bit);
        }
        hear(1, ctl[n], b);
    }
    memcpy(b, term, sizeof b);
    b[1] ^= 0x40, b[7] ^= 0x02, b[31] ^= 0x01;
    hear(1, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, b);
    run(RDOF_PARROT_DELAY_S + 1.0);
    CHECK(S.n == 8);
    CHECK(P.corrected >= 18);
    CHECK(!memcmp(S.b[0].burst, hdr, RDOF_BURST_BYTES));
    CHECK(!memcmp(S.b[7].burst, term, RDOF_BURST_BYTES));
    /* the voice: its AMBE frames (the bits outside the 48 in the middle) as sent */
    for (int n = 0; n < 6; n++) {
        CHECK(!memcmp(S.b[1 + n].burst, v[n], 13));
        CHECK(!memcmp(S.b[1 + n].burst + 20, v[n] + 20, 13));
        CHECK((S.b[1 + n].burst[13] & 0xF0) == (v[n][13] & 0xF0));
    }
    (void)emb;
}

/* The reply's message, put back together from its rate 1/2 blocks: the user octets' count, -1 when it's not whole. */
static int reply_message(int from, uint8_t *out)
{
    rdof_data_header_t h;
    rdof_decode_data_header(S.b[from].burst, &h);
    if (!h.crc_ok)
        return -1;
    int len = 0;
    for (int k = 0; k < h.blocks; k++) {
        rdof_data_block_t b;
        if (!rdof_decode_data_block(S.b[from + 1 + k].burst, RDOF_DT_RATE_12, &b))
            return -1;
        memcpy(out + len, b.payload, (size_t)b.len);
        len += b.len;
    }
    return rdof_check_crc32(out, (size_t)len) ? len - 4 - h.pad_octets : -1;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint16_t fold_sum(const uint8_t *d, size_t n, uint32_t s)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        s += (uint32_t)d[i] << 8 | d[i + 1];
    if (n & 1)
        s += (uint32_t)d[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)s;
}

/* Private packet data comes back to its sender: the header's ids, an IPv4 datagram's addresses and UDP ports, a
 * compressed header's address ids and ports swapped; the checksums still right; group data as it was. */
static void test_data_reply(void)
{
    const uint32_t radio = 9800001, parrot = 9990, rip = 0x0C2F4E61, pip = 0x0C000000 | parrot;
    uint8_t dg[128], b[RDOF_BURST_BYTES], m[RDOF_IPGW_MAX_PACKET + 32];
    static rdof_ipgw_dmr_t pk;
    const size_t n = rdof_ipgw_udp_datagram(rip, 4001, pip, 4007, 77, (const uint8_t *)"hello parrot", 12, dg, sizeof dg);
    CHECK(n == 40);
    CHECK(rdof_ipgw_build_dmr(radio, parrot, RDOF_SAP_IP, dg, n, 1, DATA_SYNC, &pk));
    reset(parrot);
    uint8_t pre[2][RDOF_BURST_BYTES];
    rdof_csbk_preambles(radio, parrot, false, 2, 1, pre);
    for (int i = 0; i < 2; i++)
        hear(1, RDOF_SYNC_DATA | RDOF_DT_CSBK, pre[i]);
    for (int i = 0; i < pk.nbursts; i++)
        hear(1, pk.control[i], pk.bursts[i]);
    run(RDOF_PARROT_DELAY_S + 2.0);
    CHECK(P.data_replies == 1 && S.starts == 1 && S.src == parrot && S.dst == radio && !S.group);
    CHECK(S.n == 2 + pk.nbursts);
    rdof_csbk_t c;
    rdof_decode_csbk(S.b[0].burst, &c);
    CHECK(c.crc_ok && c.opcode == RDOF_CSBK_PREAMBLE && c.src_id == parrot && c.dst_id == radio);
    rdof_data_header_t h;
    rdof_decode_data_header(S.b[2].burst, &h);
    CHECK(h.crc_ok && h.src_id == parrot && h.dst_id == radio && h.sap == RDOF_SAP_IP);
    int len = reply_message(2, m);
    CHECK(len == 40 && rdof_be32(m + 12) == pip && rdof_be32(m + 16) == rip);
    CHECK(be16(m + 20) == 4007 && be16(m + 22) == 4001 && !memcmp(m + 28, "hello parrot", 12));
    CHECK(fold_sum(m, 20, 0) == 0xFFFF); /* the IPv4 header's checksum */
    uint8_t ph[12] = {0};
    memcpy(ph, m + 12, 8);
    ph[9] = 17;
    ph[10] = 0, ph[11] = 20;
    CHECK(fold_sum(m + 20, 20, fold_sum(ph, 12, 0)) == 0xFFFF); /* UDP's, over its pseudo-header */
    CHECK(S.b[S.n - 1].terminator);

    /* SAP 3: id, SAID/DAID 0, both ports given, then the payload */
    uint8_t ch[9 + 5] = {0x12, 0x34, 0x00, 0x00, 0x00, 0x0F, 0xA1, 0x0F, 0xA7, 'h', 'e', 'l', 'l', 'o'};
    CHECK(rdof_ipgw_build_dmr(radio, parrot, RDOF_SAP_UDP_COMPRESSED, ch, sizeof ch, 1, DATA_SYNC, &pk));
    reset(parrot);
    for (int i = 0; i < pk.nbursts; i++)
        hear(2, pk.control[i], pk.bursts[i]);
    run(RDOF_PARROT_DELAY_S + 2.0);
    CHECK(P.data_replies == 1 && S.dst == radio && S.slot == 2);
    len = reply_message(0, m);
    CHECK(len == (int)sizeof ch && m[0] == 0x12 && m[1] == 0x34 && m[2] == 0 && m[3] == 0 && m[4] == 0);
    CHECK(be16(m + 5) == 4007 && be16(m + 7) == 4001 && !memcmp(m + 9, "hello", 5));
    /* one port by its id: SPID 0 (given), DPID 5; swapped, the given one is the destination's, in its place */
    uint8_t c1[7 + 2] = {0x12, 0x35, 0x00, 0x00, 0x05, 0x0F, 0xA1, 'h', 'i'};
    CHECK(rdof_ipgw_build_dmr(radio, parrot, RDOF_SAP_UDP_COMPRESSED, c1, sizeof c1, 1, DATA_SYNC, &pk));
    reset(parrot);
    for (int i = 0; i < pk.nbursts; i++)
        hear(2, pk.control[i], pk.bursts[i]);
    run(RDOF_PARROT_DELAY_S + 2.0);
    len = reply_message(0, m);
    CHECK(len == (int)sizeof c1 && m[3] == 0x05 && m[4] == 0x00 && be16(m + 5) == 4001);

    /* group data: as it was heard */
    CHECK(rdof_ipgw_build_dmr(radio, parrot, RDOF_SAP_IP, dg, n, 1, DATA_SYNC, &pk));
    rdof_ipgw_data_header(true, false, false, RDOF_SAP_IP, 0, 91, radio, pk.nbursts - 1, 0, b);
    rdof_data_burst(b, RDOF_DT_DATA_HEADER, 1, DATA_SYNC, pk.bursts[0]);
    reset(0);
    for (int i = 0; i < pk.nbursts; i++)
        hear(1, pk.control[i], pk.bursts[i]);
    run(RDOF_PARROT_DELAY_S + 2.0);
    CHECK(P.data_replies == 0 && S.n == pk.nbursts && S.dst == 91 && S.group);
}

int main(void)
{
    test_voice();
    test_private_embedded();
    test_id();
    test_admit();
    test_lost_and_late();
    test_data();
    test_data_reply();
    test_busy_and_full();
    test_regen();
    printf("parrot: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
