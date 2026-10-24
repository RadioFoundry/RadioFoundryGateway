// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Bursts as a repeater sends them, and the CACH short LC (docs/spec/repeater.md, sections 5 and 9). */
#include "rdof/duplex.h"

#if RDOF_FEATURE_REPEATER

#include <string.h>

#include "radiofoundry/dmrCach.h"
#include "rdof/ambe.h"
#include "rdof/bits.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"

/* The idle message's info field (ETSI TS 102 361-1 annex D.2, table D.2), as
 * RadioFoundry's core/dmr/bsOutbound.c BS_IDLE_INFO; BPTC(196,96) coded, with
 * the slot type for this color code and BS data sync. */
static const uint8_t IDLE_INFO[12] = {0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91};

void rdof_duplex_idle(int cc, uint8_t out[RDOF_BURST_BYTES])
{
    rdof_data_burst(IDLE_INFO, RDOF_DT_IDLE, cc, rdof_data_sync(RDOF_BS_VOICE), out);
}

void rdof_duplex_lc_burst(const uint8_t lc9[9], int dt, int cc, uint8_t out[RDOF_BURST_BYTES])
{
    rdof_lc_burst(lc9, dt, cc, rdof_data_sync(RDOF_BS_VOICE), out);
}

bool rdof_duplex_data(uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int cc, uint8_t out[RDOF_BURST_BYTES])
{
    int dt = control & 0x0F;
    if (dt == RDOF_DT_CSBK || dt == RDOF_DT_DATA_HEADER || dt == RDOF_DT_PI_HEADER || dt == RDOF_DT_RATE_12) {
        uint8_t payload[12];
        bool ok = rdof_bptc196_decode(burst, payload);
        if (dt == RDOF_DT_CSBK && !rdof_check_crc16(payload, RDOF_MASK_CSBK))
            return false;
        if (dt == RDOF_DT_DATA_HEADER && !rdof_check_crc16(payload, RDOF_MASK_DATA_HEADER))
            return false;
        if (ok || dt != RDOF_DT_RATE_12) {
            rdof_data_burst(payload, dt, cc, rdof_data_sync(RDOF_BS_VOICE), out);
            return true;
        }
    }
    rdof_regenerate(control, burst, cc, true, out);
    return true;
}

int rdof_regen_ambe(uint8_t burst[RDOF_BURST_BYTES])
{
    uint8_t bits[264], voice[216], frame[9], params[49], fbits[72];
    int errors = 0;
    rdof_bytes_to_bits(burst, RDOF_BURST_BYTES, bits);
    memcpy(voice, bits, 108);
    memcpy(voice + 108, bits + 156, 108);
    for (int i = 0; i < 3; i++) {
        rdof_bits_to_bytes(voice + 72 * i, 72, frame);
        errors += rdof_ambe_decode_frame(frame, params);
        rdof_ambe_encode_frame(params, frame);
        rdof_bytes_to_bits(frame, 9, fbits);
        memcpy(voice + 72 * i, fbits, 72);
    }
    memcpy(bits, voice, 108);
    memcpy(bits + 156, voice + 108, 108);
    rdof_bits_to_bytes(bits, 264, burst);
    return errors;
}

uint8_t rdof_duplex_silence(int n, int cc, uint8_t out[RDOF_BURST_BYTES])
{
    return rdof_silence_burst(n, cc, true, NULL, out); /* R5.10 */
}

void rdof_voice_regen_init(rdof_voice_regen_t *v, const uint8_t lc9[9], int cc)
{
    memset(v, 0, sizeof *v);
    memcpy(v->lc9, lc9, 9);
    v->cc = cc;
    v->regen_ambe = lc9[1] == 0x00 || lc9[1] == 0x10; /* ETSI and Motorola FIDs (R5.7) */
    rdof_embedded_lc_encode(v->lc9, v->lc_raw);
    memcpy(v->sending, v->lc_raw, 128);
}

void rdof_voice_regen_burst(rdof_voice_regen_t *v, uint8_t control, const uint8_t in[RDOF_BURST_BYTES],
                          uint8_t out[RDOF_BURST_BYTES])
{
    static const int lcss_of[5] = {0, 1, 3, 3, 2};
    memcpy(out, in, RDOF_BURST_BYTES);
    if (v->regen_ambe)
        v->errors += rdof_regen_ambe(out);
    if (control & RDOF_SYNC_AUDIO) {
        /* A new superframe: send what the last one carried, if it decodes; else the call's LC. */
        uint8_t raw[128], lc[9];
        bool decoded = false;
        if (v->have == 0x1E) {
            for (int n = 1; n <= 4; n++)
                memcpy(raw + 32 * (n - 1), v->heard[n], 32);
            decoded = rdof_embedded_lc_decode(raw, lc);
        }
        if (decoded)
            rdof_embedded_lc_encode(lc, v->sending);
        else
            memcpy(v->sending, v->lc_raw, 128);
        v->have = 0;
        rdof_set_field(out, 108, 48, RDOF_BS_VOICE);
        return;
    }
    int n = control & 0x0F, cc, pi, lcss;
    if (!rdof_decode_emb(out, &cc, &pi, &lcss, NULL)) {
        pi = 0;
        lcss = 0;
    }
    if (n >= 1 && n <= 4) {
        for (int i = 0; i < 32; i++) {
            v->heard[n][i] = (uint8_t)rdof_get_field(out, 116 + i, 1);
            rdof_set_field(out, 116 + i, 1, v->sending[(n - 1) * 32 + i]);
        }
        v->have |= (uint8_t)(1 << n);
        lcss = lcss_of[n];
    }
    uint8_t emb[16];
    rdof_emb_bits(v->cc, lcss, pi, emb);
    rdof_set_field(out, 108, 8, rdof_bits_to_u64(emb, 8));
    rdof_set_field(out, 148, 8, rdof_bits_to_u64(emb + 8, 8));
}

/* ── the CACH short LC ─────────────────────────────────────────────────────────
 * RadioFoundry's src/radiofoundry/dmrCach.c (ETSI TS 102 361-1 7.1.4, B.2.3, B.3.3,
 * B.3.7): the 28-bit short LC and its CRC-8 in three Hamming(17,12,3) rows and a
 * column parity row, sent as the four 17-bit CACH payloads. Here: the
 * 5-byte form (4 zero bits, the 28 bits, the CRC-8) and the 9 bytes DMR_SHORTLC
 * takes. */

static uint32_t slc_of(const uint8_t lc5[5])
{
    return rdof_be32(lc5) & 0x0FFFFFFFu;
}

void rdof_short_lc(const uint8_t lc5[5], uint8_t out9[9])
{
    uint32_t rows[4];
    dmrShortLcEncode(slc_of(lc5), rows); /* the CRC-8 is computed again, not taken from lc5[4] */
    dmrShortLcPack(rows, out9);
}

bool rdof_decode_short_lc(const uint8_t sl9[9], uint8_t lc5[5])
{
    uint32_t rows[4], slc;
    dmrShortLcUnpack(sl9, rows);
    if (!dmrShortLcDecode(rows, &slc))
        return false;
    rdof_put_be32(lc5, slc);
    lc5[4] = dmrShortLcCrc8(slc);
    return true;
}

void rdof_short_lc_rows(const uint8_t sl9[9], uint32_t rows[4]) { dmrShortLcUnpack(sl9, rows); }

void rdof_short_lc_join(const uint32_t rows[4], uint8_t sl9[9]) { dmrShortLcPack(rows, sl9); }

bool rdof_short_lc_accept(const uint8_t in9[9], uint8_t out9[9])
{
    uint8_t lc5[5];
    if (rdof_decode_short_lc(in9, lc5)) {
        memcpy(out9, in9, 9);
        return true;
    }
    if (dmrShortLcCrc8(slc_of(in9)) != in9[4])
        return false;
    rdof_short_lc(in9, out9);
    return true;
}

static int activity_code(const rdof_activity_t *a)
{
    switch (a->kind) {
    case RDOF_ACT_VOICE: return a->group ? 0x8 : 0x9;
    case RDOF_ACT_DATA: return a->group ? 0xB : 0xA;
    case RDOF_ACT_CSBK: return a->group ? 0x2 : 0x3;
    default: return 0;
    }
}

void rdof_activity_lc(const rdof_activity_t *slot1, const rdof_activity_t *slot2, bool ts1_low, uint8_t out9[9])
{
    uint32_t rows[4];
    bool idle1 = !slot1 || slot1->kind == RDOF_ACT_NONE, idle2 = !slot2 || slot2->kind == RDOF_ACT_NONE;
    if (idle1 && idle2) { /* R9.4: the Null short LC */
        dmrShortLcEncode(dmrShortLcNull(), rows);
        dmrShortLcPack(rows, out9);
        return;
    }
    if (ts1_low) { /* the interop option short_lc_ts1_low (R9.6): the slots the other way round */
        const rdof_activity_t *t = slot1;
        bool ti = idle1;
        slot1 = slot2;
        idle1 = idle2;
        slot2 = t;
        idle2 = ti;
    }
    /* Act_Updt (ETSI TS 102 361-2 7.1.3.2, table 7.10): slot 1's activity, slot 2's, then each
     * slot's hashed destination (its CRC-8); an idle slot is 0 in both. */
    uint32_t slc = dmrShortLcActivity((uint8_t)(idle1 ? 0 : activity_code(slot1)),
                                      (uint8_t)(idle2 ? 0 : activity_code(slot2)),
                                      idle1 ? 0 : dmrShortLcHash(slot1->dst), idle2 ? 0 : dmrShortLcHash(slot2->dst));
    dmrShortLcEncode(slc, rows);
    dmrShortLcPack(rows, out9);
}

#endif /* RDOF_FEATURE_REPEATER */
