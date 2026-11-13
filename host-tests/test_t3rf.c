// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A trunked site's radio layer (rdof/t3rf.h): control blocks to bursts and back, the CACH with a short LC and the
 * controller's AT, payload channels (warm-up, repeat, the controller's PDU, the network's prebuffer, RF over the
 * network, a lost call), the TSCC's inbound, and a payload carrier on a split TX half (TX_SCHED, and the FIFO). */
#include <stdalign.h>
#include <stdio.h>
#include <string.h>

#include "rdof/fec.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/split.h"
#include "rdof/splitops.h"
#include "rdof/t3.h"
#include "rdof/t3rf.h"

static int fails;

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            fails++;                                                                                               \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                 \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

#define CC 1

#define LAYOUT(t) {#t, sizeof(t), alignof(t)}

/* The Rust's mirrors of the header's structs have the C's size and alignment. */
static void test_layouts(void)
{
    static const rdof_t3rf_layout_t c[] = {
        LAYOUT(rdof_t3rf_burst_t), LAYOUT(rdof_t3rf_ch_t),    LAYOUT(rdof_t3rf_io_t),
        LAYOUT(rdof_t3rf_t),       LAYOUT(rdof_t3rf_split_t),
    };
    const size_t n = sizeof c / sizeof c[0];
    size_t i = 0;
    for (const rdof_t3rf_layout_t *r; (r = rdof_t3rf_layout(i)) && r->name; i++) {
        CHECK(i < n, "rust lists %s, C doesn't", r->name);
        if (i >= n)
            break;
        CHECK(strcmp(r->name, c[i].name) == 0, "%zu: rust %s, C %s", i, r->name, c[i].name);
        CHECK(r->size == c[i].size && r->align == c[i].align, "%s: rust %zu/%zu, C %zu/%zu", c[i].name, r->size,
              r->align, c[i].size, c[i].align);
    }
    CHECK(i == n, "rust lists %zu structs, C %zu", i, n);
}

/* A C_ALOHA-like CSBK body: LB, CSBKO 25, FID 0, then arbitrary fields. The radio layer reads none of them. */
static const uint8_t ALOHA[10] = {0x80 | 25, 0x00, 0x00, 0x2C, 0x00, 0x00, 0xFF, 0xFF, 0xFE, 0x00};

static void test_blocks(void)
{
    static const int dts[4] = {RDOF_DT_CSBK, RDOF_DT_MBC_HEADER, RDOF_DT_MBC_CONTINUATION, RDOF_DT_DATA_HEADER};
    for (int i = 0; i < 4; i++)
        for (int f = 0; f < 2; f++) {
            uint8_t b[RDOF_BURST_BYTES], pdu[10];
            int dt, cc;
            const rdof_family_t fam = f ? RDOF_FAMILY_MS : RDOF_FAMILY_BS;
            CHECK(rdof_t3rf_pdu_burst(dts[i], ALOHA, CC, fam, b), "dt %d builds", dts[i]);
            CHECK(rdof_t3rf_burst_pdu(b, &dt, &cc, pdu) == RDOF_T3RF_PDU_OK && dt == dts[i] && cc == CC &&
                      !memcmp(pdu, ALOHA, 10),
                  "dt %d family %d reads back (dt %d cc %d)", dts[i], f, dt, cc);
            rdof_family_t got;
            bool data;
            int e;
            CHECK(rdof_detect_sync(b, 0, &got, &data, &e) && data && got == fam, "family %d's data sync", f);
        }
    CHECK(!rdof_t3rf_pdu_burst(RDOF_DT_IDLE, ALOHA, CC, RDOF_FAMILY_BS, (uint8_t[33]){0}), "no idle PDU");
    CHECK(rdof_t3rf_mask(RDOF_DT_CSBK) == 0xA5A5 && rdof_t3rf_mask(RDOF_DT_MBC_HEADER) == 0xAAAA &&
              rdof_t3rf_mask(RDOF_DT_DATA_HEADER) == 0xCCCC && rdof_t3rf_mask(RDOF_DT_RATE_12) < 0,
          "masks");

    /* a CSBK sent with the MBC mask (or one bit flipped past BPTC's reach) fails the CSBK's check */
    uint8_t b12[12], b[RDOF_BURST_BYTES], pdu[10];
    int dt, cc;
    memcpy(b12, ALOHA, 10);
    const uint16_t crc = (uint16_t)(rdof_crc16_ccitt(b12, 10) ^ 0xAAAA);
    b12[10] = (uint8_t)(crc >> 8), b12[11] = (uint8_t)crc;
    rdof_data_burst(b12, RDOF_DT_CSBK, CC, rdof_data_sync(RDOF_MS_VOICE), b);
    CHECK(rdof_t3rf_burst_pdu(b, &dt, &cc, pdu) == RDOF_T3RF_PDU_BAD && dt == RDOF_DT_CSBK, "wrong mask: bad");
    rdof_t3rf_pdu_burst(RDOF_DT_CSBK, ALOHA, CC, RDOF_FAMILY_MS, b);
    for (int i = 0; i < 12; i++)
        b[i] ^= 0xFF; /* 96 info bits gone */
    CHECK(rdof_t3rf_burst_pdu(b, &dt, &cc, pdu) == RDOF_T3RF_PDU_BAD, "a collision: bad");

    /* not control blocks: an idle burst, a voice burst */
    rdof_duplex_idle(CC, b);
    CHECK(rdof_t3rf_burst_pdu(b, &dt, &cc, pdu) == RDOF_T3RF_NOT_PDU && dt == RDOF_DT_IDLE, "idle: not a PDU");
    rdof_silence_burst(0, CC, true, NULL, b);
    CHECK(rdof_t3rf_burst_pdu(b, &dt, &cc, pdu) == RDOF_T3RF_NOT_PDU, "voice: not a PDU");

    /* a UDT's appended block (rate 1/2 data, 12 octets as given) and a control block, as blocks */
    uint8_t blk[24], back[24];
    for (int i = 0; i < 24; i++)
        blk[i] = (uint8_t)(0x31 * i + 7);
    CHECK(rdof_t3rf_block_burst(RDOF_DT_RATE_12, blk, CC, RDOF_FAMILY_MS, b), "rate 1/2 block builds");
    CHECK(rdof_t3rf_burst_pdu(b, &dt, &cc, pdu) == RDOF_T3RF_NOT_PDU && dt == RDOF_DT_RATE_12, "not a control block");
    CHECK(rdof_t3rf_burst_block(b, &dt, &cc, back) == RDOF_T3RF_PDU_OK && dt == RDOF_DT_RATE_12 && cc == CC &&
              !memcmp(back, blk, 12),
          "rate 1/2 block reads back");
    memcpy(blk, ALOHA, 10);
    CHECK(rdof_t3rf_block_burst(RDOF_DT_CSBK, blk, CC, RDOF_FAMILY_BS, b) &&
              rdof_t3rf_burst_block(b, &dt, &cc, back) == RDOF_T3RF_PDU_OK && dt == RDOF_DT_CSBK &&
              !memcmp(back, ALOHA, 10),
          "a CSBK as a block");
    CHECK(!rdof_t3rf_block_burst(RDOF_DT_IDLE, blk, CC, RDOF_FAMILY_BS, b), "no idle block");

    /* rate 3/4 (18 octets, the trellis) and rate 1 (24, uncoded) data blocks (361-1 8.2.2, B.2) */
    for (int i = 0; i < 24; i++)
        blk[i] = (uint8_t)(0x5B * i + 3);
    const int rates[2][2] = {{RDOF_DT_RATE_34, 18}, {RDOF_DT_RATE_1, 24}};
    for (int r = 0; r < 2; r++) {
        const int rdt = rates[r][0], len = rates[r][1];
        memset(back, 0, sizeof back);
        CHECK(rdof_t3rf_block_burst(rdt, blk, CC, RDOF_FAMILY_MS, b), "rate %d block builds", rdt);
        CHECK(rdof_t3rf_burst_block(b, &dt, &cc, back) == RDOF_T3RF_PDU_OK && dt == rdt && cc == CC &&
                  !memcmp(back, blk, (size_t)len),
              "rate %d block reads back", rdt);
        uint8_t p18[18], p24[24];
        if (rdt == RDOF_DT_RATE_34) {
            /* the common decoder agrees, with a few bit errors the trellis corrects */
            b[3] ^= 0x10;
            b[29] ^= 0x01;
            CHECK(rdof_trellis34_decode(b, p18) >= 0 && !memcmp(p18, blk, 18), "rate 3/4: errors corrected");
            CHECK(rdof_t3rf_burst_block(b, &dt, &cc, back) == RDOF_T3RF_PDU_OK && !memcmp(back, blk, 18),
                  "rate 3/4 with errors reads back");
        } else {
            rdof_rate1_payload(b, p24);
            CHECK(!memcmp(p24, blk, 24), "rate 1: the common reader agrees");
        }
    }
}

static void test_cach(void)
{
    /* C_SYS_Parms: SLCO 0010b and 24 bits of fields */
    const uint32_t slc = 0x2u << 24 | 0x5A5A5Au;
    const int at[4] = {1, 0, 0, 1};
    uint8_t c[4][3];
    rdof_t3rf_cach4(slc, at, 8, c);
    uint32_t rows[4];
    static const int want_lcss[4] = {1, 3, 3, 2};
    bool ok = true;
    for (int q = 0; q < 4; q++) {
        int a, slot, lcss;
        rdof_t3rf_cach_read(c[q], &a, &slot, &lcss, &rows[q]);
        ok &= a == at[q] && slot == (q & 1) + 1 && lcss == want_lcss[q];
    }
    uint32_t got = 0;
    CHECK(ok, "AT, TC and LCSS read back");
    CHECK(rdof_t3rf_slc_join(rows, &got) && got == slc, "the short LC: %07x", (unsigned)got);
    rows[1] ^= 0x3; /* two bits of one CACH: interleaved into two Hamming rows, both corrected */
    CHECK(rdof_t3rf_slc_join(rows, &got) && got == slc, "two bits of one CACH corrected");
    rows[2] ^= 0x1FFFF; /* a whole CACH lost */
    CHECK(!rdof_t3rf_slc_join(rows, &got), "a lost CACH: refused");
}

/* ── the payload channels and the TSCC, against a recording controller ───── */

static struct {
    int pdus, bads, acts, ups, tsccs;
    int last_ch, last_dt, last_what;
    int64_t last_k;
    uint32_t src, dst;
    bool group, new_stream;
    uint8_t pdu[10];
    int at;
    int32_t slc;
} rec;

static bool r_tscc(void *ctx, int64_t k, int *dt, uint8_t pdu[12])
{
    rec.tsccs++;
    if (k % 8 == 4)
        return false; /* an idle one now and then */
    *dt = RDOF_DT_CSBK;
    memcpy(pdu, ALOHA, 10);
    return true;
}
static void r_cach(void *ctx, int carrier, int64_t k, int *at, int32_t *slc28)
{
    *at = rec.at;
    *slc28 = rec.slc;
}
static void r_pdu(void *ctx, int ch, int64_t k, int dt, const uint8_t *pdu, int rssi)
{
    rec.pdus++, rec.last_ch = ch, rec.last_k = k, rec.last_dt = dt;
    memcpy(rec.pdu, pdu, 10);
}
static void r_bad(void *ctx, int ch, int64_t k) { rec.bads++, rec.last_ch = ch, rec.last_k = k; }
static void r_act(void *ctx, int ch, int64_t k, int what, uint32_t src, uint32_t dst, bool group)
{
    rec.acts++, rec.last_ch = ch, rec.last_what = what, rec.src = src, rec.dst = dst, rec.group = group;
}
static void r_up(void *ctx, int ch, const rdof_frame_t *f)
{
    rec.ups++;
    rec.new_stream |= f->new_stream;
    rec.src = f->src, rec.dst = f->dst;
}

static const rdof_t3rf_io_t IO = {r_tscc, r_cach, r_pdu, r_bad, r_act, r_up, NULL};

static rdof_burst_t inbound(const uint8_t b[33], int dt, int n, int cc)
{
    rdof_burst_t x;
    memset(&x, 0, sizeof x);
    memcpy(x.burst, b, 33);
    x.dt = dt, x.n = n, x.cc = cc, x.family = RDOF_FAMILY_MS;
    return x;
}

/* A radio's call on carrier c's slot from k: header, superframes of voice, terminator; returns the next k. */
static int64_t ms_call(rdof_t3rf_t *t, int c, int64_t k, uint32_t src, uint32_t dst, int superframes, bool header,
                       bool term)
{
    uint8_t lc9[9], b[33];
    rdof_group_lc(src, dst, true, 0, lc9);
    if (header) {
        rdof_lc_burst(lc9, RDOF_DT_VOICE_LC_HEADER, CC, rdof_data_sync(RDOF_MS_VOICE), b);
        rdof_burst_t x = inbound(b, RDOF_DT_VOICE_LC_HEADER, 0, CC);
        rdof_t3rf_rx(t, c, k, &x, -80);
        k += 2;
    }
    for (int i = 0; i < 6 * superframes; i++, k += 2) {
        rdof_silence_burst(i % 6, CC, false, NULL, b);
        rdof_burst_t x = inbound(b, i % 6 ? RDOF_DT_VOICE : RDOF_DT_VOICE_SYNC, i % 6, CC);
        rdof_t3rf_rx(t, c, k, &x, -80);
    }
    if (term) {
        rdof_lc_burst(lc9, RDOF_DT_TERMINATOR_WITH_LC, CC, rdof_data_sync(RDOF_MS_VOICE), b);
        rdof_burst_t x = inbound(b, RDOF_DT_TERMINATOR_WITH_LC, 0, CC);
        rdof_t3rf_rx(t, c, k, &x, -80);
        k += 2;
    }
    return k;
}

static void test_payload(void)
{
    static rdof_t3rf_t t;
    memset(&rec, 0, sizeof rec);
    CHECK(!rdof_t3rf_init(&t, &IO, 0, CC, true) && !rdof_t3rf_init(&t, &IO, 5, CC, true), "carriers 1-4");
    CHECK(rdof_t3rf_init(&t, &IO, 2, CC, true), "init");
    CHECK(!rdof_t3rf_channel(&t, 0) && rdof_t3rf_channel(&t, 1) && rdof_t3rf_channel(&t, 3) &&
              !rdof_t3rf_channel(&t, 4),
          "channel 0 is the TSCC; 1-3 payload; 4 none");
    CHECK(rdof_t3rf_carrier_busy(&t, 0) && !rdof_t3rf_carrier_busy(&t, 1), "the TSCC's carrier always busy");

    /* a grant: channel 2 (carrier 1 TS1); warm-up idles first, then the radio's call repeated */
    const int ch = 2;
    CHECK(rdof_t3rf_open(&t, ch, 3100123, 91, true, RDOF_T3RF_WARMUP), "open");
    CHECK(rdof_t3rf_carrier_busy(&t, 1), "carrier 1 busy once open");
    uint8_t out[33], control;
    int64_t k = 1000;
    CHECK(rdof_t3rf_ch_next(&t, ch, k, &control, out) == RDOF_T3RF_IDLE &&
              rdof_t3rf_ch_next(&t, ch, k + 2, &control, out) == RDOF_T3RF_IDLE,
          "two warm-up idles");
    k = ms_call(&t, 1, 1000, 3100777, 91, 2, true, true);
    CHECK(rec.acts == 4 && rec.last_what == RDOF_T3RF_TERM && rec.src == 3100777 && rec.dst == 91 && rec.group,
          "header, a voice a superframe, terminator reported: %d, src %u", rec.acts, (unsigned)rec.src);
    CHECK(rec.ups == 14, "14 bursts up: %d", rec.ups);
    int traffic = 0, voice_sync = 0, lc_ok = 0;
    rdof_t3rf_out_t o;
    while ((o = rdof_t3rf_ch_next(&t, ch, k, &control, out)) == RDOF_T3RF_TRAFFIC) {
        traffic++;
        rdof_family_t fam;
        bool data;
        int e;
        if (rdof_detect_sync(out, 0, &fam, &data, &e) && fam == RDOF_FAMILY_BS)
            voice_sync += !data;
        if (control == (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER) || control == (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC)) {
            rdof_full_lc_t lc;
            lc_ok += rdof_decode_voice_lc(out, control & 0x0F, &lc) && lc.src_id == 3100777 && lc.dst_id == 91;
        }
    }
    CHECK(traffic == 14 && voice_sync == 2 && lc_ok == 2,
          "repeated with BS syncs: %d bursts, %d voice syncs, %d LCs", traffic, voice_sync, lc_ok);

    /* late entry: voice with no header; the channel's addresses; then lost */
    memset(&rec, 0, sizeof rec);
    k = ms_call(&t, 1, k, 0, 0, 1, false, false);
    CHECK(rec.acts == 1 && rec.last_what == RDOF_T3RF_VOICE && rec.src == 3100123 && rec.dst == 91 && rec.new_stream,
          "late entry with the grant's addresses: acts %d what %d src %u dst %u new %d", rec.acts, rec.last_what,
          (unsigned)rec.src, (unsigned)rec.dst, rec.new_stream);
    rdof_t3rf_tick(&t, k - 2 + RDOF_T3RF_LOST_K); /* the last burst at k - 2 */
    CHECK(rec.acts == 1, "not lost yet");
    rdof_t3rf_tick(&t, k - 2 + RDOF_T3RF_LOST_K + 1);
    CHECK(rec.acts == 2 && rec.last_what == RDOF_T3RF_LOST && t.ch[ch].lost == 1, "lost");

    /* P_CLEAR: ahead of the queued voice, 4 times, then closed and idle */
    const uint8_t clear[10] = {0x80 | 46, 0, 0, 0, 0xFF, 0xFF, 0xD4, 0x00, 0x00, 0x01};
    CHECK(rdof_t3rf_control(&t, ch, RDOF_DT_CSBK, clear, 4, true), "P_CLEAR");
    int ctl = 0;
    for (int i = 0; i < 6; i++) {
        o = rdof_t3rf_ch_next(&t, ch, k + 2 * i, &control, out);
        if (o == RDOF_T3RF_CONTROL) {
            uint8_t pdu[10];
            int dt, cc;
            ctl += rdof_t3rf_burst_pdu(out, &dt, &cc, pdu) == RDOF_T3RF_PDU_OK && dt == RDOF_DT_CSBK &&
                   !memcmp(pdu, clear, 10) && control == (RDOF_SYNC_DATA | RDOF_DT_CSBK);
        } else {
            CHECK(o == RDOF_T3RF_IDLE, "idle after");
        }
    }
    CHECK(ctl == 4 && !t.ch[ch].open && !rdof_t3rf_carrier_busy(&t, 1), "4 P_CLEARs, then closed: %d", ctl);

    /* the network: held for the prebuffer, then plays; RF takes the channel from it */
    CHECK(rdof_t3rf_open(&t, ch, 9990001, 91, true, 0), "open for the network");
    rdof_frame_t f;
    memset(&f, 0, sizeof f);
    uint8_t lc9[9];
    rdof_group_lc(9990001, 91, true, 0, lc9);
    rdof_lc_burst(lc9, RDOF_DT_VOICE_LC_HEADER, 7, rdof_data_sync(RDOF_BS_VOICE), f.burst);
    f.dt = RDOF_DT_VOICE_LC_HEADER;
    CHECK(rdof_t3rf_net(&t, ch, &f), "a network header");
    for (int i = 0; i < 2; i++) {
        f.dt = i ? RDOF_DT_VOICE : RDOF_DT_VOICE_SYNC, f.n = i;
        rdof_silence_burst(i, 7, true, NULL, f.burst);
        CHECK(rdof_t3rf_net(&t, ch, &f), "network voice");
    }
    CHECK(rdof_t3rf_ch_next(&t, ch, k, &control, out) == RDOF_T3RF_IDLE, "held: 3 of 4");
    f.dt = RDOF_DT_VOICE, f.n = 2;
    rdof_silence_burst(2, 7, true, NULL, f.burst);
    rdof_t3rf_net(&t, ch, &f);
    o = rdof_t3rf_ch_next(&t, ch, k, &control, out);
    int dt0, cc0;
    CHECK(o == RDOF_T3RF_TRAFFIC && control == (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER) &&
              rdof_decode_slot_type(out, &cc0, &dt0, NULL) && cc0 == CC,
          "plays at 4, on the site's color code (%d)", cc0);
    memset(&rec, 0, sizeof rec);
    ms_call(&t, 1, k, 3100555, 91, 0, true, false);
    CHECK(t.ch[ch].net_dropped == 1 && !rdof_t3rf_net(&t, ch, &f) && rec.acts == 1,
          "RF's header drops the network's stream, and it stays out");
    o = rdof_t3rf_ch_next(&t, ch, k, &control, out);
    CHECK(o == RDOF_T3RF_TRAFFIC && control == (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER), "RF's header next");
    rdof_t3rf_close(&t, ch);
    CHECK(!rdof_t3rf_net(&t, ch, &f), "nothing on a closed channel");

    /* a P_MAINT heard on the payload inbound goes to the controller, not the air */
    rdof_t3rf_open(&t, ch, 1, 2, false, 0);
    uint8_t b[33];
    const uint8_t maint[10] = {0x80 | 42, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    rdof_t3rf_pdu_burst(RDOF_DT_CSBK, maint, CC, RDOF_FAMILY_MS, b);
    rdof_burst_t x = inbound(b, RDOF_DT_CSBK, 0, CC);
    rdof_t3rf_rx(&t, 1, 2000, &x, -70);
    CHECK(rec.pdus == 1 && rec.last_ch == ch && rec.last_dt == RDOF_DT_CSBK && !memcmp(rec.pdu, maint, 10) &&
              !t.ch[ch].qn,
          "P_MAINT up, nothing repeated");
    x.cc = 5;
    rdof_t3rf_rx(&t, 1, 2000, &x, -70);
    CHECK(rec.pdus == 1 && t.rx_ignored == 1, "another color code ignored");
}

static void test_tscc(void)
{
    static rdof_t3rf_t t;
    memset(&rec, 0, sizeof rec);
    rdof_t3rf_init(&t, &IO, 2, CC, true);
    uint8_t b[33];
    /* inbound: a random access on TS1 of carrier 0; and a collision */
    const uint8_t rand_[10] = {0x80 | 31, 0, 0x10, 0, 0, 91, 0x2F, 0x4D, 0x5B, 0};
    rdof_t3rf_pdu_burst(RDOF_DT_CSBK, rand_, CC, RDOF_FAMILY_MS, b);
    rdof_burst_t x = inbound(b, RDOF_DT_CSBK, 0, CC);
    rdof_t3rf_rx(&t, 0, 4000, &x, -90);
    CHECK(rec.pdus == 1 && rec.last_ch == 0 && rec.last_k == 4000 && !memcmp(rec.pdu, rand_, 10), "C_RAND to ch 0");
    for (int i = 0; i < 12; i++)
        x.burst[i] ^= 0x5A;
    rdof_t3rf_rx(&t, 0, 4002, &x, -90);
    CHECK(rec.bads == 1 && rec.last_k == 4002 && t.rx_tscc_bad == 1, "a collision counted");
    rdof_silence_burst(0, CC, false, NULL, b);
    x = inbound(b, RDOF_DT_VOICE_SYNC, 0, CC);
    rdof_t3rf_rx(&t, 0, 4004, &x, -90);
    CHECK(rec.pdus == 1 && rec.bads == 1, "voice on the TSCC: nothing");
    /* the same burst under an odd slot is carrier 0's payload channel 1 (closed: nothing goes out) */
    rdof_t3rf_pdu_burst(RDOF_DT_CSBK, rand_, CC, RDOF_FAMILY_MS, b);
    x = inbound(b, RDOF_DT_CSBK, 0, CC);
    rdof_t3rf_rx(&t, 0, 4005, &x, -90);
    CHECK(rec.pdus == 2 && rec.last_ch == 1, "TS2 is channel 1");

    /* outbound: the TSCC's blocks in even slots, the controller's CACH */
    rec.at = 1, rec.slc = 0x2123456;
    uint8_t control;
    int at;
    int32_t slc;
    int csbks = 0, idles = 0;
    for (int64_t k = 0; k < 16; k += 2) {
        rdof_t3rf_slot(&t, 0, k, b, &control, &at, &slc);
        uint8_t pdu[10];
        int dt, cc;
        if (rdof_t3rf_burst_pdu(b, &dt, &cc, pdu) == RDOF_T3RF_PDU_OK && !memcmp(pdu, ALOHA, 10) &&
            control == (RDOF_SYNC_DATA | RDOF_DT_CSBK))
            csbks++;
        else if (control == (RDOF_SYNC_DATA | RDOF_DT_IDLE))
            idles++;
        CHECK(at == 1 && slc == (k % 4 ? -1 : 0x2123456), "k %lld: AT %d slc %d", (long long)k, at, (int)slc);
    }
    CHECK(csbks == 6 && idles == 2 && rec.tsccs == 8, "6 blocks and 2 idles: %d %d", csbks, idles);
    rdof_t3rf_slot(&t, 0, 17, b, &control, &at, &slc);
    CHECK(rec.tsccs == 8 && control == (RDOF_SYNC_DATA | RDOF_DT_IDLE), "TS2: channel 1 (closed: idle)");
}

/* The Reverse Channel (361-1 5.1.5): in an idle burst of the alternate slot, in voice burst F (not A-E, not a header
 * or right after a terminator), 360 ms apart, once; in a TSCC slot left without a block. */
static void test_rc(void)
{
    static rdof_t3rf_t t;
    memset(&rec, 0, sizeof rec);
    rdof_t3rf_init(&t, &IO, 2, CC, true);
    uint8_t out[33], control;
    /* channel 3 idle: the RC goes in its next burst, as a dedicated RC (an idle burst's sync replaced) */
    rdof_t3rf_rc(&t, 3, RDOF_T3_RC_SEND | RDOF_T3_RC_CEASE);
    rdof_t3rf_out_t o = rdof_t3rf_ch_next(&t, 3, 101, &control, out);
    CHECK(o == RDOF_T3RF_CONTROL && rdof_t3rf_rc_read(out) == RDOF_T3_RC_CEASE && t.ch[3].rc_sent == 1,
          "a dedicated RC: %d %d", o, rdof_t3rf_rc_read(out));
    int dt, cc;
    CHECK(rdof_decode_slot_type(out, &cc, &dt, NULL) && dt == RDOF_DT_IDLE && cc == CC, "an idle burst's slot type");
    o = rdof_t3rf_ch_next(&t, 3, 103, &control, out);
    CHECK(o == RDOF_T3RF_IDLE && rdof_t3rf_rc_read(out) < 0, "once");
    /* not within 360 ms of the last */
    rdof_t3rf_rc(&t, 3, RDOF_T3_RC_SEND | RDOF_T3_RC_CEASE);
    o = rdof_t3rf_ch_next(&t, 3, 111, &control, out);
    CHECK(o == RDOF_T3RF_IDLE && t.ch[3].rc_sent == 1, "360 ms apart");
    o = rdof_t3rf_ch_next(&t, 3, 113, &control, out);
    CHECK(o == RDOF_T3RF_CONTROL && t.ch[3].rc_sent == 2, "then it goes");
    /* -1 drops one not sent */
    rdof_t3rf_rc(&t, 3, RDOF_T3_RC_SEND | RDOF_T3_RC_CEASE);
    rdof_t3rf_rc(&t, 3, -1);
    o = rdof_t3rf_ch_next(&t, 3, 141, &control, out);
    CHECK(o == RDOF_T3RF_IDLE && t.ch[3].rc_sent == 2, "dropped");

    /* channel 2 carries a call: only voice burst F takes the RC, the voice around it untouched */
    rdof_t3rf_open(&t, 2, 3100123, 91, true, 0);
    int64_t k = ms_call(&t, 1, 200, 3100777, 91, 2, true, true);
    rdof_t3rf_rc(&t, 2, RDOF_T3_RC_SEND | RDOF_T3_RC_CEASE);
    int n = 0, rc_at = -1;
    while (rdof_t3rf_ch_next(&t, 2, k, &control, out) == RDOF_T3RF_TRAFFIC) {
        if (rdof_t3rf_rc_read(out) == RDOF_T3_RC_CEASE)
            rc_at = n;
        n++, k += 2;
    }
    CHECK(n == 14 && rc_at == 6 && t.ch[2].rc_sent == 1, "in the first burst F (6th after the header): %d at %d", n,
          rc_at);
    /* right after a terminator: not in that idle, in the next */
    rdof_t3rf_open(&t, 2, 3100123, 91, true, 0);
    k = ms_call(&t, 1, k + 40, 3100777, 91, 0, true, true);
    rdof_t3rf_ch_next(&t, 2, k, &control, out); /* the header */
    rdof_t3rf_ch_next(&t, 2, k + 2, &control, out); /* the terminator */
    rdof_t3rf_rc(&t, 2, RDOF_T3_RC_SEND | RDOF_T3_RC_CEASE);
    o = rdof_t3rf_ch_next(&t, 2, k + 4, &control, out);
    CHECK(o == RDOF_T3RF_IDLE && rdof_t3rf_rc_read(out) < 0, "not right after a terminator");
    o = rdof_t3rf_ch_next(&t, 2, k + 6, &control, out);
    CHECK(o == RDOF_T3RF_CONTROL && rdof_t3rf_rc_read(out) == RDOF_T3_RC_CEASE, "the next idle");

    /* the TSCC: a slot without a block carries it */
    int at;
    int32_t slc;
    rdof_t3rf_rc(&t, 0, RDOF_T3_RC_SEND | RDOF_T3_RC_CEASE);
    rdof_t3rf_slot(&t, 0, 404, out, &control, &at, &slc); /* r_tscc gives no block at k % 8 == 4 */
    CHECK(rdof_t3rf_rc_read(out) == RDOF_T3_RC_CEASE && t.ch[0].rc_sent == 1, "RC in the TSCC's free slot");
}

/* ── carrier 1 on a split TX half: the frames it writes ──────────────────── */

static struct {
    int starts, stops, shortlcs, data[2], scheds, sched_data;
    int64_t counters[64];
    int ncounters;
    uint32_t slc;
} fr;

static void on_frame(void *ctx, uint8_t cmd, const uint8_t *p, size_t n)
{
    if (cmd == RDOF_MMDVM_DMR_START)
        n && p[0] ? fr.starts++ : fr.stops++;
    else if (cmd == RDOF_MMDVM_DMR_SHORTLC) {
        uint8_t lc5[5];
        fr.shortlcs++;
        if (n >= 9 && rdof_decode_short_lc(p, lc5))
            fr.slc = (uint32_t)lc5[0] << 24 | (uint32_t)lc5[1] << 16 | (uint32_t)lc5[2] << 8 | lc5[3];
    } else if (cmd == RDOF_MMDVM_DMR_DATA1 || cmd == RDOF_MMDVM_DMR_DATA2)
        fr.data[cmd == RDOF_MMDVM_DMR_DATA2]++;
    else if (cmd == RDOF_ODR_SPLIT && n >= 1 + 39 && p[0] == RDOF_OPS_SUB_TX_SCHED) {
        fr.scheds++;
        uint32_t c = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
        if (fr.ncounters < 64)
            fr.counters[fr.ncounters++] = c;
        fr.sched_data += (p[6] & 0x0F) != RDOF_DT_IDLE;
    }
}

static void drain(rdof_split_t *s)
{
    static rdof_mmdvm_reader_t r;
    static bool init;
    if (!init)
        rdof_mmdvm_reader_init(&r), init = true;
    uint8_t buf[2048];
    size_t n;
    while ((n = rdof_split_take(s, RDOF_SPLIT_TX, buf, sizeof buf)))
        rdof_mmdvm_reader_feed(&r, buf, n, on_frame, NULL);
}

static void test_split(bool scheduled)
{
    static rdof_t3rf_t t;
    static rdof_split_t s;
    memset(&rec, 0, sizeof rec);
    memset(&fr, 0, sizeof fr);
    rec.at = -1, rec.slc = 0x3000ABC; /* P_SYS_Parms: SLCO 0011b */
    rdof_t3rf_init(&t, &IO, 2, CC, true);
    rdof_split_params_t p;
    rdof_split_params_default(&p);
    p.stream_mode = scheduled ? RDOF_OPS_STREAM_SCHEDULED : RDOF_OPS_STREAM_FIFO;
    rdof_split_init(&s, 0, &p);
    drain(&s); /* bring-up's GET_VERSION */
    memset(&fr, 0, sizeof fr);
    /* past bring-up: in its role, the slot counter on the UTC grid (the halves' own exchange is test_split's) */
    s.ready = s.tx_role = true;
    if (scheduled)
        s.align_state = RDOF_ALIGN_LOCKED;
    CHECK(rdof_split_sched_ok(&s) == scheduled, "the scheduled stream up: %d", rdof_split_sched_ok(&s));
    rdof_t3rf_split_t o;
    rdof_t3rf_split_init(&o, 1);
    rdof_t3rf_split_pump(&t, &o, &s, 100, 0, 1.0);
    drain(&s);
    CHECK(!fr.starts && !fr.scheds && !fr.data[0] && !o.keyed, "nothing while the carrier is idle");

    /* a call on channel 3 (carrier 1 TS2) */
    rdof_t3rf_open(&t, 3, 3100123, 91, true, RDOF_T3RF_WARMUP);
    ms_call(&t, 1, 101, 3100123, 91, 1, true, true);
    int64_t k = 100;
    for (int i = 0; i < 30; i++, k++) {
        rdof_t3rf_split_pump(&t, &o, &s, k, 0, 1.0 + 0.03 * i);
        drain(&s);
    }
    CHECK(fr.starts == 1 && o.keyed, "keyed once: %d", fr.starts);
    CHECK(fr.shortlcs == 1 && fr.slc == 0x3000ABC, "P_SYS_Parms once: %d, %07x", fr.shortlcs, (unsigned)fr.slc);
    if (scheduled) {
        bool odd = true, rising = true;
        for (int i = 0; i < fr.ncounters; i++) {
            odd &= fr.counters[i] & 1;
            rising &= !i || fr.counters[i] > fr.counters[i - 1];
        }
        CHECK(fr.scheds == 8 && fr.sched_data == 8 && odd && rising && fr.counters[0] == 105,
              "8 TX_SCHED (header, 6 voice, terminator) on TS2's counters from 105 after 2 warm-up idles: %d, first %lld",
              fr.scheds, fr.ncounters ? (long long)fr.counters[0] : -1LL);
        CHECK(!fr.data[0] && !fr.data[1] && o.sched_bursts == 8 && s.sched_sent == 8, "no FIFO frames");
    } else {
        CHECK(fr.data[0] == 15 && fr.data[1] == 15 && !fr.scheds, "every slot in order: %d + %d", fr.data[0],
              fr.data[1]);
    }
    rdof_t3rf_close(&t, 3);
    rdof_t3rf_split_pump(&t, &o, &s, k, 0, 2.0);
    drain(&s);
    CHECK(fr.stops == 1 && !o.keyed, "unkeyed when the carrier goes idle");
    printf("split %s: %d TX_SCHED, %d + %d DMR_DATA\n", scheduled ? "scheduled" : "FIFO", fr.scheds, fr.data[0],
           fr.data[1]);
}

int main(void)
{
    test_layouts();
    test_blocks();
    test_cach();
    test_payload();
    test_tscc();
    test_rc();
    test_split(true);
    test_split(false);
    printf("%s\n", fails ? "trunkrf: FAILED" : "trunkrf: ok");
    return fails ? 1 : 0;
}
