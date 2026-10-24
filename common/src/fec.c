// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * DMR FEC and CRCs (ETSI TS 102 361-1 annex B): the rdof_* API over RadioFoundry's
 * MIT code in src/radiofoundry/: BPTC(196,96) and its Hamming codes (dmrFec.c),
 * RS(12,9), CRC-CCITT and the embedded LC (dmr_lc.c), the rate 3/4 trellis and
 * rate 1 (dmrTrellis.c), CRC-32 and CRC-9 (dmrDataCrc.c). Here: the conversions
 * between RadioFoundry's packed
 * words and bytes and the 0/1 bit arrays the rest of common uses, and the burst
 * layout helpers (ETSI TS 102 361-1 figure 9.1 / 9.2).
 */
#include "rdof/fec.h"

#include <string.h>

#include "radiofoundry/dmrDataCrc.h"
#include "radiofoundry/dmrFec.h"
#include "radiofoundry/dmrTrellis.h"
#include "radiofoundry/dmr_lc.h"
#include "rdof/bits.h"
#include "rdof/features.h"

/* ── Hamming codes ─────────────────────────────────────────────────────────── */

/* A codeword is n 0/1 bytes, the k data bits first. (15,11,3) and (13,9,3) are RadioFoundry's
 * (B.3.5, B.3.4); (16,11,4) is (15,11,3) plus an even parity bit over the other 15
 * (B.3.4's extended code, as dmr_lc.c's embedded LC rows). */
struct rdof_hamming {
    int n, k;
};

static const struct rdof_hamming H_15_11 = {15, 11};
static const struct rdof_hamming H_13_9 = {13, 9};
static const struct rdof_hamming H_16_11 = {16, 11};

const rdof_hamming_t *const RDOF_H_15_11 = &H_15_11;
const rdof_hamming_t *const RDOF_H_13_9 = &H_13_9;
const rdof_hamming_t *const RDOF_H_16_11 = &H_16_11;

static uint16_t ham_encode(const rdof_hamming_t *h, uint16_t data)
{
    return h->k == 9 ? dmrFecHamming1393Encode(data) : dmrFecHamming15113Encode(data);
}

static bool ham_correct(const rdof_hamming_t *h, uint16_t *cw)
{
    return h->k == 9 ? dmrFecHamming1393Correct(cw) : dmrFecHamming15113Correct(cw);
}

static int parity16(uint16_t v)
{
    return rdof_popcount64(v) & 1;
}

int rdof_hamming_syndrome(const rdof_hamming_t *h, const uint8_t *w)
{
    if (h->n == 16) {
        uint16_t cw = (uint16_t)rdof_bits_to_u64(w, 15);
        int s = (ham_encode(h, cw >> 4) ^ cw) & 0x0F;
        return s | ((parity16(cw) ^ w[15]) << 4);
    }
    uint16_t cw = (uint16_t)rdof_bits_to_u64(w, h->n);
    return (ham_encode(h, cw >> 4) ^ cw) & 0x0F;
}

void rdof_hamming_encode(const rdof_hamming_t *h, uint8_t *w)
{
    uint16_t cw = ham_encode(h, (uint16_t)rdof_bits_to_u64(w, h->k));
    if (h->n == 16) {
        rdof_u64_to_bits(cw, 15, w);
        w[15] = (uint8_t)parity16(cw);
    } else {
        rdof_u64_to_bits(cw, h->n, w);
    }
}

bool rdof_hamming_decode(const rdof_hamming_t *h, uint8_t *w, bool *corrected)
{
    if (corrected)
        *corrected = false;
    if (h->n == 16) {
        uint16_t cw = (uint16_t)rdof_bits_to_u64(w, 15), fixed = cw;
        bool syndrome = ham_encode(h, cw >> 4) != cw;
        if (parity16(cw) == w[15]) /* even weight: no error, or two */
            return !syndrome;
        if (syndrome) { /* one error in the first 15 bits */
            if (!ham_correct(h, &fixed))
                return false;
            rdof_u64_to_bits(fixed, 15, w);
        } else { /* one error, in the parity bit */
            w[15] ^= 1;
        }
        if (corrected)
            *corrected = true;
        return true;
    }
    uint16_t cw = (uint16_t)rdof_bits_to_u64(w, h->n), fixed = cw;
    if (!ham_correct(h, &fixed))
        return false;
    if (fixed != cw) {
        rdof_u64_to_bits(fixed, h->n, w);
        if (corrected)
            *corrected = true;
    }
    return true;
}

/* ── burst layout ──────────────────────────────────────────────────────────── */

void rdof_info_bits(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out196[196])
{
    uint8_t b[264];
    rdof_bytes_to_bits(burst, RDOF_BURST_BYTES, b);
    memcpy(out196, b, 98);
    memcpy(out196 + 98, b + 166, 98);
}

void rdof_build_burst(const uint8_t info196[196], const uint8_t *middle48, const uint8_t *slot_type20,
                    uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t bits[264];
    memcpy(bits, info196, 98);
    if (slot_type20) {
        memcpy(bits + 98, slot_type20, 10);
        memcpy(bits + 156, slot_type20 + 10, 10);
    } else {
        memset(bits + 98, 0, 10);
        memset(bits + 156, 0, 10);
    }
    if (middle48)
        memcpy(bits + 108, middle48, 48);
    else
        memset(bits + 108, 0, 48);
    memcpy(bits + 166, info196 + 98, 98);
    rdof_bits_to_bytes(bits, 264, out);
}

/* ── BPTC(196,96) ──────────────────────────────────────────────────────────── */

bool rdof_bptc196_decode(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out12[12])
{
    return dmrFecBptcDecode(burst, out12) >= 0;
}

void rdof_bptc196_encode(const uint8_t payload12[12], uint8_t info196[196])
{
    uint8_t burst[RDOF_BURST_BYTES] = {0};
    dmrFecBptcEncode(payload12, burst);
    rdof_info_bits(burst, info196);
}

/* ── slot type / EMB (raw) ─────────────────────────────────────────────────── */

void rdof_slot_type_raw(const uint8_t burst[RDOF_BURST_BYTES], int *cc, int *dt)
{
    *cc = (int)rdof_get_field(burst, 98, 4);
    *dt = (int)rdof_get_field(burst, 102, 4);
}

void rdof_emb_raw(const uint8_t burst[RDOF_BURST_BYTES], int *cc, int *pi, int *lcss)
{
    *cc = (int)rdof_get_field(burst, 108, 4);
    *pi = (int)rdof_get_field(burst, 112, 1);
    *lcss = (int)rdof_get_field(burst, 113, 2);
}

void rdof_embedded_signalling(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out32[32])
{
    for (int i = 0; i < 32; i++)
        out32[i] = (uint8_t)rdof_get_field(burst, 116 + i, 1);
}

void rdof_ambe_frames(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out[3][9])
{
    uint8_t b[264], voice[216];
    rdof_bytes_to_bits(burst, RDOF_BURST_BYTES, b);
    memcpy(voice, b, 108);
    memcpy(voice + 108, b + 156, 108);
    for (int f = 0; f < 3; f++)
        rdof_bits_to_bytes(voice + 72 * f, 72, out[f]);
}

/* ── embedded LC ───────────────────────────────────────────────────────────── */

int rdof_five_bit_checksum(const uint8_t lc9[9])
{
    return dmr_emb_lc_checksum(lc9);
}

bool rdof_embedded_lc_decode(const uint8_t raw128[128], uint8_t lc9[9])
{
    uint8_t packed[16];
    rdof_bits_to_bytes(raw128, 128, packed);
    return dmr_emb_lc_decode_bits(packed, lc9);
}

void rdof_embedded_lc_encode(const uint8_t lc9[9], uint8_t raw128[128])
{
    uint8_t packed[16];
    dmr_emb_lc_encode_bits(lc9, packed);
    rdof_bytes_to_bits(packed, 16, raw128);
}

/* ── rate 3/4 trellis, rate 1 ──────────────────────────────────────────────── */

#if RDOF_FEATURE_DATA_BLOCKS

int rdof_trellis34_decode(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out18[18])
{
    return dmrTrellisDecode(burst, out18);
}

void rdof_trellis34_encode(const uint8_t payload18[18], uint8_t info196[196])
{
    uint8_t burst[RDOF_BURST_BYTES] = {0};
    dmrTrellisEncode(payload18, burst);
    rdof_info_bits(burst, info196);
}

void rdof_rate1_payload(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out24[24])
{
    dmrRate1Decode(burst, out24);
}

#endif /* RDOF_FEATURE_DATA_BLOCKS */

/* ── CRCs ──────────────────────────────────────────────────────────────────── */

uint16_t rdof_crc16_ccitt(const uint8_t *data, size_t len)
{
    if (len <= 255)
        return dmr_crc_ccitt(data, (uint8_t)len);
    /* Longer than RadioFoundry's API takes (no caller here does that): the same B.3.8 CRC, bit by bit. */
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++)
        for (int b = 7; b >= 0; b--) {
            int fb = ((crc >> 15) ^ (data[i] >> b)) & 1;
            crc = (uint16_t)(crc << 1);
            if (fb)
                crc ^= 0x1021;
        }
    return (uint16_t)~crc;
}

bool rdof_check_crc16(const uint8_t block12[12], uint16_t mask)
{
    uint16_t stored = (uint16_t)(((block12[10] << 8) | block12[11]) ^ mask);
    return dmr_crc_ccitt(block12, 10) == stored;
}

#if RDOF_FEATURE_DATA_BLOCKS

uint32_t rdof_crc32_dmr(const uint8_t *data, size_t len)
{
    return dmrDataCRC32(data, (int)len);
}

bool rdof_check_crc32(const uint8_t *p, size_t len)
{
    if (len < 4)
        return false;
    uint32_t stored = (uint32_t)p[len - 4] | ((uint32_t)p[len - 3] << 8) | ((uint32_t)p[len - 2] << 16) |
                      ((uint32_t)p[len - 1] << 24);
    return dmrDataCRC32(p, (int)(len - 4)) == stored;
}

uint16_t rdof_crc9_dmr(const uint8_t *bits, size_t nbits)
{
    if (nbits >= 7 && (nbits - 7) % 8 == 0 && nbits - 7 <= 8 * 64) {
        /* The B.3.10 shape: data octets, then the 7-bit serial number. */
        uint8_t data[64];
        size_t n = (nbits - 7) / 8;
        rdof_bits_to_bytes(bits, nbits - 7, data);
        return dmrDataCRC9(data, (int)n, (uint8_t)rdof_bits_to_u64(bits + nbits - 7, 7));
    }
    /* Any other length: the same CRC (x^9 + x^6 + x^4 + x^3 + 1, inverted), bit by bit. */
    uint16_t crc = 0;
    for (size_t i = 0; i < nbits; i++) {
        int fb = ((crc >> 8) ^ bits[i]) & 1;
        crc = (uint16_t)((crc << 1) & 0x1FF);
        if (fb)
            crc ^= 0x059;
    }
    return crc ^ 0x1FF;
}

static int reverse9(int v)
{
    int r = 0;
    for (int i = 0; i < 9; i++)
        r |= ((v >> i) & 1) << (8 - i);
    return r;
}

bool rdof_check_crc9(const uint8_t *block_data, size_t len, int dbsn, int stored, int mask)
{
    /* Readers disagree on the bit order of the stored CRC on air; both are taken. */
    int crc = dmrDataCRC9(block_data, (int)len, (uint8_t)(dbsn & 0x7F)) ^ mask;
    return crc == stored || crc == reverse9(stored);
}

#endif /* RDOF_FEATURE_DATA_BLOCKS */

/* ── Reed-Solomon (12,9) ───────────────────────────────────────────────────── */

void rdof_rs129_parity(const uint8_t msg9[9], uint8_t out3[3])
{
    dmr_rs129_parity(msg9, out3);
}

bool rdof_check_rs129(const uint8_t lc12[12], uint32_t mask)
{
    uint8_t par[3];
    dmr_rs129_parity(lc12, par);
    return par[0] == (lc12[9] ^ (uint8_t)(mask >> 16)) && par[1] == (lc12[10] ^ (uint8_t)(mask >> 8)) &&
           par[2] == (lc12[11] ^ (uint8_t)mask);
}
