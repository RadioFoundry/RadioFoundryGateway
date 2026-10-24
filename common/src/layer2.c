// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* DMR layer 2: sync patterns, the short block codes and building bursts (rdof/layer2.h). */
#include "rdof/layer2.h"

#include <string.h>

#include "radiofoundry/dmrCach.h"
#include "radiofoundry/dmrFec.h"
#include "rdof/bits.h"

static const uint64_t VOICE_SYNC[RDOF_FAMILY_COUNT] = {RDOF_BS_VOICE, RDOF_MS_VOICE, RDOF_TS1_VOICE, RDOF_TS2_VOICE};
static const char *const FAMILY_NAMES[RDOF_FAMILY_COUNT] = {"bs", "ms", "ts1", "ts2"};

uint64_t rdof_family_voice_sync(rdof_family_t f)
{
    return f < RDOF_FAMILY_COUNT ? VOICE_SYNC[f] : RDOF_MS_VOICE;
}

const char *rdof_family_name(rdof_family_t f)
{
    return f < RDOF_FAMILY_COUNT ? FAMILY_NAMES[f] : "?";
}

uint64_t rdof_middle_field(const uint8_t burst[RDOF_BURST_BYTES])
{
    return rdof_get_field(burst, 108, 48);
}

int rdof_sync_errors(const uint8_t burst[RDOF_BURST_BYTES], uint64_t pattern48)
{
    return rdof_popcount64(rdof_middle_field(burst) ^ pattern48);
}

bool rdof_detect_sync(const uint8_t burst[RDOF_BURST_BYTES], int max_errors, rdof_family_t *family, bool *data,
                    int *errors)
{
    uint64_t mid = rdof_middle_field(burst);
    int best = 49, bf = 0;
    bool bd = false;
    for (int f = 0; f < RDOF_FAMILY_COUNT; f++)
        for (int d = 0; d < 2; d++) {
            uint64_t p = d ? rdof_data_sync(VOICE_SYNC[f]) : VOICE_SYNC[f];
            int e = rdof_popcount64(mid ^ p);
            if (e < best) {
                best = e;
                bf = f;
                bd = d;
            }
        }
    if (errors)
        *errors = best;
    if (best > max_errors)
        return false;
    if (family)
        *family = (rdof_family_t)bf;
    if (data)
        *data = bd;
    return true;
}

/* ── block codes ─────────────────────────────────────────────────────────────
 * Golay(20,8), QR(16,7) and the CACH (TACT Hamming(7,4) and interleave) are
 * RadioFoundry's (src/radiofoundry/dmrFec.c, src/radiofoundry/dmrCach.c; ETSI TS 102 361-1
 * B.3.1, B.3.2, B.3.5, B.4.1). Here: the rdof_* int API and error counts. */

uint32_t rdof_golay2087_encode(int value8)
{
    return dmrFecGolay2087Encode((uint8_t)value8);
}

uint32_t rdof_qr1676_encode(int value7)
{
    return dmrFecQr1676Encode((uint8_t)(value7 & 0x7F));
}

uint32_t rdof_hamming74_encode(int value4)
{
    return dmrCachTactEncode((uint8_t)((value4 >> 3) & 1), (uint8_t)((value4 >> 2) & 1), (uint8_t)(value4 & 3));
}

/* RadioFoundry's result (bits corrected, or -1) as rdof's: the value, or -1 with errors = radius + 1. */
static int block_result(int r, int value, int radius, int *errors)
{
    if (errors)
        *errors = r < 0 ? radius + 1 : r;
    return r < 0 ? -1 : value;
}

/* RadioFoundry's decoders try every codeword, encoding each as they go (about 45k cycles a Golay word on the S3, which
 * every data burst and every voice burst's EMB pays). Here: the codewords once, in tables; a word that already
 * is one (the usual case) is a lookup; else the first codeword within the radius, which is the only one there
 * (Golay(20,8) has distance 7, QR(16,7) 6), so the result is RadioFoundry's. */
static uint32_t golay_cw[256];
static uint16_t qr_cw[128];
static volatile bool cw_ready;

static void cw_init(void)
{
    if (cw_ready)
        return;
    for (int d = 0; d < 256; d++)
        golay_cw[d] = dmrFecGolay2087Encode((uint8_t)d);
    for (int d = 0; d < 128; d++)
        qr_cw[d] = dmrFecQr1676Encode((uint8_t)d);
    cw_ready = true; /* two cores may both fill the tables: with the same values */
}

int rdof_golay2087_decode(uint32_t word20, int *errors)
{
    cw_init();
    word20 &= 0xFFFFF;
    int d = (int)(word20 >> 12), r = 0;
    if (golay_cw[d] != word20) {
        r = -1;
        for (d = 0; d < 256; d++) {
            const int e = __builtin_popcount(golay_cw[d] ^ word20);
            if (e <= 3) {
                r = e;
                break;
            }
        }
    }
    return block_result(r, d, 3, errors);
}

int rdof_qr1676_decode(uint32_t word16, int *errors)
{
    cw_init();
    word16 &= 0xFFFF;
    int d = (int)(word16 >> 9), r = 0;
    if (qr_cw[d] != word16) {
        r = -1;
        for (d = 0; d < 128; d++) {
            const int e = __builtin_popcount((uint32_t)qr_cw[d] ^ word16);
            if (e <= 2) {
                r = e;
                break;
            }
        }
    }
    return block_result(r, d, 2, errors);
}

int rdof_hamming74_decode(uint32_t word7, int *errors)
{
    uint8_t at, tc, lcss;
    int r = dmrCachTactDecode((uint8_t)(word7 & 0x7F), &at, &tc, &lcss);
    return block_result(r, (at << 3) | (tc << 2) | lcss, 1, errors);
}

/* ── field decoders ────────────────────────────────────────────────────────── */

bool rdof_decode_slot_type(const uint8_t burst[RDOF_BURST_BYTES], int *cc, int *dt, int *errors)
{
    uint32_t w = (uint32_t)((rdof_get_field(burst, 98, 10) << 10) | rdof_get_field(burst, 156, 10));
    int v = rdof_golay2087_decode(w, errors);
    if (v < 0)
        return false;
    if (cc)
        *cc = v >> 4;
    if (dt)
        *dt = v & 0x0F;
    return true;
}

bool rdof_decode_emb(const uint8_t burst[RDOF_BURST_BYTES], int *cc, int *pi, int *lcss, int *errors)
{
    uint32_t w = (uint32_t)((rdof_get_field(burst, 108, 8) << 8) | rdof_get_field(burst, 148, 8));
    int v = rdof_qr1676_decode(w, errors);
    if (v < 0) {
        if (pi)
            *pi = 0;
        if (lcss)
            *lcss = 0;
        return false;
    }
    if (cc)
        *cc = v >> 3;
    if (pi)
        *pi = (v >> 2) & 1;
    if (lcss)
        *lcss = v & 3;
    return true;
}

int rdof_decode_cach_slot(const uint8_t cach24[24])
{
    uint8_t tact, at, tc, lcss;
    uint32_t payload;
    dmrCachDeinterleave((uint32_t)rdof_bits_to_u64(cach24, 24), &tact, &payload);
    dmrCachTactDecode(tact, &at, &tc, &lcss); /* Hamming(7,4) always decodes */
    return tc ? 2 : 1;
}

void rdof_encode_cach(int slot, int at, int lcss, uint8_t out24[24])
{
    rdof_encode_cach_lc(slot, at, lcss, 0, out24);
}

void rdof_encode_cach_lc(int slot, int at, int lcss, uint32_t payload17, uint8_t out24[24])
{
    rdof_u64_to_bits(dmrCachBuild((uint8_t)at, (uint8_t)(slot - 1), (uint8_t)lcss, payload17 & 0x1FFFFu), 24, out24);
}

uint32_t rdof_decode_cach_payload(const uint8_t cach24[24], int *lcss)
{
    uint8_t tact, at, tc, ls;
    uint32_t payload;
    dmrCachDeinterleave((uint32_t)rdof_bits_to_u64(cach24, 24), &tact, &payload);
    dmrCachTactDecode(tact, &at, &tc, &ls);
    if (lcss)
        *lcss = ls;
    return payload;
}

/* ── burst builders ────────────────────────────────────────────────────────── */

void rdof_slot_type_bits(int cc, int dt, uint8_t out20[20])
{
    rdof_u64_to_bits(rdof_golay2087_encode(((cc & 0xF) << 4) | (dt & 0xF)), 20, out20);
}

void rdof_emb_bits(int cc, int lcss, int pi, uint8_t out16[16])
{
    rdof_u64_to_bits(rdof_qr1676_encode(((cc & 0xF) << 3) | ((pi & 1) << 2) | (lcss & 3)), 16, out16);
}

void rdof_info_burst(const uint8_t info196[196], int dt, int cc, uint64_t sync48, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t mid[48], st[20];
    rdof_u64_to_bits(sync48, 48, mid);
    rdof_slot_type_bits(cc, dt, st);
    rdof_build_burst(info196, mid, st, out);
}

void rdof_data_burst(const uint8_t payload12[12], int dt, int cc, uint64_t sync48, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t info[196];
    rdof_bptc196_encode(payload12, info);
    rdof_info_burst(info, dt, cc, sync48, out);
}

void rdof_voice_lc12(const uint8_t lc9[9], int data_type, uint8_t out12[12])
{
    uint32_t mask = data_type == 1 ? RDOF_MASK_VOICE_LC_HEADER : RDOF_MASK_TERMINATOR_LC;
    uint8_t par[3];
    rdof_rs129_parity(lc9, par);
    memcpy(out12, lc9, 9);
    out12[9] = par[0] ^ (uint8_t)(mask >> 16);
    out12[10] = par[1] ^ (uint8_t)(mask >> 8);
    out12[11] = par[2] ^ (uint8_t)mask;
}

void rdof_group_lc(uint32_t src, uint32_t dst, bool group, int service_options, uint8_t out9[9])
{
    out9[0] = group ? 0x00 : 0x03;
    out9[1] = 0;
    out9[2] = (uint8_t)service_options;
    rdof_put_be24(out9 + 3, dst);
    rdof_put_be24(out9 + 6, src);
}

void rdof_lc_burst(const uint8_t lc9[9], int data_type, int cc, uint64_t sync48, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t lc12[12];
    rdof_voice_lc12(lc9, data_type, lc12);
    rdof_data_burst(lc12, data_type, cc, sync48, out);
}

void rdof_voice_burst(const uint8_t voice216[216], const uint8_t middle48[48], uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t bits[264];
    memcpy(bits, voice216, 108);
    memcpy(bits + 108, middle48, 48);
    memcpy(bits + 156, voice216 + 108, 108);
    rdof_bits_to_bytes(bits, 264, out);
}

void rdof_voice_middle(int cc, int lcss, const uint8_t frag32[32], int pi, uint8_t out48[48])
{
    uint8_t e[16];
    rdof_emb_bits(cc, lcss, pi, e);
    memcpy(out48, e, 8);
    memcpy(out48 + 8, frag32, 32);
    memcpy(out48 + 40, e + 8, 8);
}
