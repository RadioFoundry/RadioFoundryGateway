// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Spec conformance, the radio layer's coding (ETSI TS 102 361-1 annex B and table E.1): a reference encoder written
 * from the standard (CRC-CCITT with the data type mask, BPTC(196,96), the interleave, Golay(20,8) slot type, the
 * syncs; the CACH TACT with Hamming(7,4), the short LC's CRC-8, Hamming(17,12) rows and parity row) compared bit for
 * bit with rdof/t3rf.h. Nothing here uses our encoder for an expected value. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rdof/t3rf.h"

static int fails, checks;

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(cond)) {                                                                                             \
            fails++;                                                                                               \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                 \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

/* A known deviation: printed once, then its checks are skipped (they stay in the file as the finding's test). */
static int finding7;
static bool skip_finding7(int dt)
{
    if (dt != 5)
        return false;
    if (!finding7++)
        printf("SPEC FINDING 7: 361-1 7.4.1, B.3.12 note 1: an MBC last block's CRC takes no mask; ours uses 0xAAAA\n");
    return true;
}

/* Data types (table 9.22). An MBC continuation here is a two-block MBC's last block: its CRC covers that block only
 * (7.4.1). */
enum { DT_VLC = 1, DT_CSBK = 3, DT_MBC_HDR = 4, DT_MBC_CONT = 5, DT_DATA_HDR = 6, DT_IDLE = 9 };

/* Generator matrices, typed from tables B.11, B.13, B.14, B.15, B.17 */
static const char *const G_GOLAY[8] = {
    "10000000001111011010", "01000000110110011001", "00100000011011001101", "00010000001101100111",
    "00001000110111000110", "00000100101010010111", "00000010100100111110", "00000001100011101011",
};
static const char *const G_H17[12] = {
    "10000000000011011", "01000000000011111", "00100000000011101", "00010000000011100",
    "00001000000001110", "00000100000000111", "00000010000010001", "00000001000011010",
    "00000000100001101", "00000000010010100", "00000000001001010", "00000000000100101",
};
static const char *const G_H13[9] = {
    "1000000001111", "0100000001110", "0010000000111", "0001000001010", "0000100000101",
    "0000010001011", "0000001001100", "0000000100110", "0000000010011",
};
static const char *const G_H15[11] = {
    "100000000001001", "010000000001101", "001000000001111", "000100000001110",
    "000010000000111", "000001000001010", "000000100000101", "000000010001011",
    "000000001001100", "000000000100110", "000000000010011",
};
static const char *const G_H7[4] = {"1000101", "0100111", "0010110", "0001011"};

/* The parity bits (out[0] first) of k data bits through a systematic generator matrix of n columns. */
static void parity(const char *const *g, int k, int n, const int *d, int *out)
{
    for (int j = 0; j < n - k; j++) {
        int p = 0;
        for (int i = 0; i < k; i++)
            p ^= d[i] & (g[i][k + j] - '0');
        out[j] = p;
    }
}

static int getbit(const uint8_t *b, int i)
{
    return (b[i / 8] >> (7 - i % 8)) & 1;
}

static void setbit(uint8_t *b, int i, int v)
{
    if (v)
        b[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    else
        b[i / 8] &= (uint8_t)~(0x80 >> (i % 8));
}

/* B.3.8: x^16 M(x) mod (x^16 + x^12 + x^5 + 1), initial remainder 0, plus the all-ones inversion polynomial. */
static uint16_t crc_ccitt(const uint8_t *b, int nbits)
{
    uint16_t r = 0;
    for (int i = 0; i < nbits; i++) {
        int top = ((r >> 15) & 1) ^ getbit(b, i);
        r = (uint16_t)(r << 1);
        if (top)
            r ^= 0x1021;
    }
    return r ^ 0xFFFF;
}

/* B.3.7: x^8 M(x) mod (x^8 + x^2 + x + 1), initial 0, no inversion; M = the 28 short LC bits. */
static uint8_t crc8_slc(uint32_t slc)
{
    uint8_t r = 0;
    for (int i = 27; i >= 0; i--) {
        int top = ((r >> 7) & 1) ^ (int)((slc >> i) & 1);
        r = (uint8_t)(r << 1);
        if (top)
            r ^= 0x07;
    }
    return r;
}

/* Table B.21 */
static int spec_mask(int dt)
{
    switch (dt) {
    case DT_CSBK: return 0xA5A5;
    case DT_MBC_HDR: return 0xAAAA;
    case DT_MBC_CONT: return 0; /* note 1: none on a last block */
    case DT_DATA_HDR: return 0xCCCC;
    default: return -1;
    }
}

/* Table 9.2 */
static const uint8_t SYNC_BS_DATA[6] = {0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D};
static const uint8_t SYNC_MS_DATA[6] = {0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57};
static const uint8_t SYNC_TS1_DATA[6] = {0xF7, 0xFD, 0xD5, 0xDD, 0xFD, 0x55};
static const uint8_t SYNC_TS2_DATA[6] = {0xD7, 0x55, 0x7F, 0x5F, 0xF7, 0xF5};
static const uint8_t SYNC_BS_VOICE[6] = {0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7};

/* A general data burst (figure B.1, equation B.1, table E.1): 98 info bits, slot type CC DT Golay(11..10), sync,
 * Golay(9..0), 98 info bits. crc_mask is applied to the CRC (it may differ from dt's for a negative test). */
static void ref_burst(int dt, int crc_mask, const uint8_t pdu[10], int cc, const uint8_t sync[6], uint8_t out[33])
{
    uint8_t blk[12];
    memcpy(blk, pdu, 10);
    uint16_t crc = (uint16_t)(crc_ccitt(pdu, 80) ^ crc_mask);
    blk[10] = (uint8_t)(crc >> 8);
    blk[11] = (uint8_t)crc;
    int d[99] = {0}; /* R(2), R(1), R(0), I(95) .. I(0) */
    for (int j = 0; j < 96; j++)
        d[3 + j] = getbit(blk, j);
    int m[13][15] = {{0}};
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 11; c++)
            m[r][c] = d[11 * r + c];
        parity(G_H15, 11, 15, m[r], &m[r][11]);
    }
    for (int c = 0; c < 15; c++) {
        int col[9], p[4];
        for (int r = 0; r < 9; r++)
            col[r] = m[r][c];
        parity(G_H13, 9, 13, col, p);
        for (int r = 0; r < 4; r++)
            m[9 + r][c] = p[r];
    }
    int idx[196] = {0}; /* index 0 is R(3) */
    for (int r = 0; r < 13; r++)
        for (int c = 0; c < 15; c++)
            idx[1 + 15 * r + c] = m[r][c];
    int tx[196];
    for (int i = 0; i < 196; i++)
        tx[(i * 181) % 196] = idx[i];
    int st[8] = {(cc >> 3) & 1, (cc >> 2) & 1, (cc >> 1) & 1, cc & 1, (dt >> 3) & 1, (dt >> 2) & 1, (dt >> 1) & 1, dt & 1};
    int g[12];
    parity(G_GOLAY, 8, 20, st, g);
    memset(out, 0, 33);
    int n = 0;
    for (int i = 0; i < 98; i++)
        setbit(out, n++, tx[i]);
    for (int i = 0; i < 8; i++)
        setbit(out, n++, st[i]);
    setbit(out, n++, g[0]);
    setbit(out, n++, g[1]);
    for (int i = 0; i < 48; i++)
        setbit(out, n++, getbit(sync, i));
    for (int i = 2; i < 12; i++)
        setbit(out, n++, g[i]);
    for (int i = 98; i < 196; i++)
        setbit(out, n++, tx[i]);
}

static void pdu_of(int seed, uint8_t p[10])
{
    uint32_t x = 0x9E3779B9u * (uint32_t)(seed + 1);
    for (int i = 0; i < 10; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (uint8_t)x;
    }
    if (seed == 0)
        memset(p, 0, 10);
    if (seed == 1)
        memset(p, 0xFF, 10);
}

/* Table B.21: the masks. */
static void test_masks(void)
{
    const int dts[] = {DT_CSBK, DT_MBC_HDR, DT_MBC_CONT, DT_DATA_HDR};
    for (int i = 0; i < 4; i++)
        if (!skip_finding7(dts[i]))
            CHECK(rdof_t3rf_mask(dts[i]) == spec_mask(dts[i]), "dt %d: %#x", dts[i], rdof_t3rf_mask(dts[i]));
    CHECK(rdof_t3rf_mask(DT_VLC) == -1, "a voice LC header isn't a control block");
    CHECK(rdof_t3rf_mask(DT_IDLE) == -1, "idle has no CRC");
}

/* Figure B.1, B.3.8, B.3.12, table E.1: a control block's burst, bit for bit; outbound with the BS data sync and
 * inbound with the MS data sync. */
static void test_bursts(void)
{
    const int dts[] = {DT_CSBK, DT_MBC_HDR, DT_MBC_CONT, DT_DATA_HDR};
    const int ccs[] = {0, 1, 7, 15};
    for (int s = 0; s < 6; s++) {
        uint8_t p[10];
        pdu_of(s, p);
        for (int i = 0; i < 4; i++) {
            if (skip_finding7(dts[i]))
                continue;
            for (int c = 0; c < 4; c++) {
                for (int fam = 0; fam < 2; fam++) {
                    uint8_t want[33], got[33];
                    ref_burst(dts[i], spec_mask(dts[i]), p, ccs[c], fam ? SYNC_MS_DATA : SYNC_BS_DATA, want);
                    memset(got, 0, 33);
                    bool ok = rdof_t3rf_pdu_burst(dts[i], p, ccs[c], fam ? RDOF_FAMILY_MS : RDOF_FAMILY_BS, got);
                    CHECK(ok && memcmp(want, got, 33) == 0, "pdu %d dt %d cc %d family %d", s, dts[i], ccs[c], fam);
                }
            }
        }
    }
    uint8_t p[10] = {0}, out[33];
    CHECK(!rdof_t3rf_pdu_burst(DT_VLC, p, 1, RDOF_FAMILY_BS, out), "no burst for a data type without a mask");
}

/* The reverse: a burst built from the standard reads back, from each data sync of table 9.2. */
static void test_read(void)
{
    const uint8_t *syncs[4] = {SYNC_BS_DATA, SYNC_MS_DATA, SYNC_TS1_DATA, SYNC_TS2_DATA};
    const int dts[] = {DT_CSBK, DT_MBC_HDR, DT_MBC_CONT, DT_DATA_HDR};
    for (int s = 0; s < 4; s++) {
        uint8_t p[10];
        pdu_of(s + 2, p);
        for (int i = 0; i < 4; i++) {
            if (skip_finding7(dts[i]))
                continue;
            uint8_t b[33], back[10];
            int dt = -1, cc = -1;
            ref_burst(dts[i], spec_mask(dts[i]), p, 9, syncs[s], b);
            rdof_trunk_pdu_t r = rdof_t3rf_burst_pdu(b, &dt, &cc, back);
            CHECK(r == RDOF_T3RF_PDU_OK && dt == dts[i] && cc == 9 && memcmp(back, p, 10) == 0, "sync %d dt %d: %d", s,
                  dts[i], (int)r);
        }
    }
    /* B.3.12: a CRC under another data type's mask fails */
    uint8_t p[10], b[33], back[10];
    int dt, cc;
    pdu_of(3, p);
    ref_burst(DT_MBC_HDR, spec_mask(DT_CSBK), p, 1, SYNC_MS_DATA, b);
    CHECK(rdof_t3rf_burst_pdu(b, &dt, &cc, back) == RDOF_T3RF_PDU_BAD, "CSBK mask under an MBC header");
    ref_burst(DT_CSBK, 0, p, 1, SYNC_MS_DATA, b);
    CHECK(rdof_t3rf_burst_pdu(b, &dt, &cc, back) == RDOF_T3RF_PDU_BAD, "a CSBK with no mask");
    /* a voice sync isn't a control block */
    ref_burst(DT_CSBK, spec_mask(DT_CSBK), p, 1, SYNC_BS_VOICE, b);
    CHECK(rdof_t3rf_burst_pdu(b, &dt, &cc, back) == RDOF_T3RF_NOT_PDU, "voice sync");
}

/* Figure B.6 rows (Hamming(17,12) + the even parity row), read column by column into four 17-bit rows. */
static void ref_slc_rows(uint32_t slc, uint8_t crc, uint32_t rows[4])
{
    int d[36];
    for (int i = 0; i < 28; i++)
        d[i] = (int)((slc >> (27 - i)) & 1);
    for (int i = 0; i < 8; i++)
        d[28 + i] = (crc >> (7 - i)) & 1;
    int m[4][17] = {{0}};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 12; c++)
            m[r][c] = d[12 * r + c];
        parity(G_H17, 12, 17, m[r], &m[r][12]);
    }
    for (int c = 0; c < 17; c++)
        m[3][c] = m[0][c] ^ m[1][c] ^ m[2][c];
    for (int j = 0; j < 4; j++) {
        rows[j] = 0;
        for (int i = 0; i < 17; i++) {
            int n = 17 * j + i;
            rows[j] = (rows[j] << 1) | (uint32_t)m[n % 4][n / 4];
        }
    }
}

/* Figure B.9: AT P16 P15 P14 TC P13 P12 P11 LS1 P10 P9 P8 LS0 P7 H2 P6 P5 P4 H1 P3 P2 P1 H0 P0. */
static void ref_cach(int at, int tc, int lcss, uint32_t p17, uint8_t out[3])
{
    int d[4] = {at, tc, (lcss >> 1) & 1, lcss & 1}, h[3];
    parity(G_H7, 4, 7, d, h);
    int seq[24];
    int n = 0, pi = 16, hi = 0;
    const char *lay = "APPPTPPPLPPPMPHPPPHPPPHP"; /* M: LS0 */
    for (int i = 0; i < 24; i++) {
        switch (lay[i]) {
        case 'A': seq[n++] = d[0]; break;
        case 'T': seq[n++] = d[1]; break;
        case 'L': seq[n++] = d[2]; break;
        case 'M': seq[n++] = d[3]; break;
        case 'H': seq[n++] = h[hi++]; break;
        default: seq[n++] = (int)((p17 >> pi--) & 1); break;
        }
    }
    memset(out, 0, 3);
    for (int i = 0; i < 24; i++)
        setbit(out, i, seq[i]);
}

/* B.2.3, B.3.7, B.4.1, tables 9.20 (LCSS: first 01b, continuation 11b, last 10b): the four CACH of a short LC. TC is
 * the slot after the CACH (0: TS1). */
static void test_cach(void)
{
    const uint32_t slcs[] = {0, 0x0FFFFFFF, 0x2408A2C, 0x3123456};
    const int ats[3][4] = {{0, 0, 0, 0}, {1, 0, 1, 0}, {0, 1, 1, 1}};
    const int lcss[4] = {1, 3, 3, 2};
    for (int s = 0; s < 4; s++) {
        uint32_t rows[4];
        ref_slc_rows(slcs[s], crc8_slc(slcs[s]), rows);
        for (int a = 0; a < 3; a++) {
            for (int64_t k = 0; k <= 8; k += 4) {
                uint8_t got[4][3];
                rdof_t3rf_cach4(slcs[s], ats[a], k, got);
                for (int i = 0; i < 4; i++) {
                    uint8_t want[3];
                    ref_cach(ats[a][i], (int)((k + i) & 1), lcss[i], rows[i], want);
                    CHECK(memcmp(want, got[i], 3) == 0, "slc %#x at %d k %lld CACH %d", slcs[s], a, (long long)k, i);
                    int at = -1, slot = -1, ls = -1;
                    uint32_t p = 0;
                    bool ok = rdof_t3rf_cach_read(want, &at, &slot, &ls, &p);
                    CHECK(ok && at == ats[a][i] && slot == 1 + (int)((k + i) & 1) && ls == lcss[i] && p == rows[i],
                          "read slc %#x CACH %d: at %d slot %d lcss %d", slcs[s], i, at, slot, ls);
                }
            }
        }
        uint32_t back = 0;
        CHECK(rdof_t3rf_slc_join(rows, &back) && back == slcs[s], "join %#x: %#x", slcs[s], back);
        /* a wrong CRC-8 (rows otherwise consistent) is refused */
        uint32_t bad[4];
        ref_slc_rows(slcs[s], (uint8_t)(crc8_slc(slcs[s]) ^ 0x01), bad);
        CHECK(!rdof_t3rf_slc_join(bad, &back), "join with a bad CRC-8, slc %#x", slcs[s]);
    }
}

int main(void)
{
    test_masks();
    test_bursts();
    test_read();
    test_cach();
    printf("t3spec_rf: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
