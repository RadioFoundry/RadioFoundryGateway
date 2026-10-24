// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The patch RAM's portable part (lr2021_patch.h): reading its info words. */
#include "lr2021_patch.h"

lr2021_patch_info_t lr2021_patch_parse(uint32_t magic, uint32_t info)
{
    lr2021_patch_info_t p = {0};
    if (magic == LR2021_PATCH_MAGIC) {
        p.loaded = true;
        p.type = (uint8_t)(info >> 16);
        p.version = (uint8_t)(info >> 8);
    }
    return p;
}

lr2021_patch_info_t lr2021_patch_image_info(void) { return lr2021_patch_parse(lr2021_patch_image[0], lr2021_patch_image[1]); }

/* ── Our own patch's command, 0x02F0 (pram/README.md) ── */

static size_t put32be(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24), b[1] = (uint8_t)(v >> 16), b[2] = (uint8_t)(v >> 8), b[3] = (uint8_t)v;
    return 4;
}

static uint32_t get32be(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

size_t lr2021_rdof_cmd(uint8_t *b, uint8_t sub)
{
    if (sub == LR2021_RDOF_APPEND || sub > LR2021_RDOF_REPORT)
        return 0;
    b[0] = LR2021_RDOF_CMD >> 8, b[1] = LR2021_RDOF_CMD & 0xFF, b[2] = sub;
    return 3;
}

size_t lr2021_rdof_append(uint8_t *b, const lr2021_rdof_entry_t *e, int n)
{
    if (n < 1 || n > LR2021_RDOF_APPEND_MAX || !e)
        return 0;
    b[0] = LR2021_RDOF_CMD >> 8, b[1] = LR2021_RDOF_CMD & 0xFF, b[2] = LR2021_RDOF_APPEND;
    size_t o = 3;
    for (int i = 0; i < n; i++) {
        o += put32be(b + o, e[i].addr);
        o += put32be(b + o, e[i].mask);
        o += put32be(b + o, e[i].value);
    }
    return o;
}

bool lr2021_rdof_parse_report(const uint8_t *b, size_t n, lr2021_rdof_report_t *out)
{
    if (n < LR2021_RDOF_REPORT_LEN)
        return false;
    out->version = get32be(b);
    out->count = b[4];
    out->enabled = b[5] != 0;
    out->sync_runs = get32be(b + 6);
    out->rf_runs = get32be(b + 10);
    out->samp_runs = get32be(b + 14);
    out->cmds = 0;
    return true;
}

bool lr2021_rdof_parse_state(const uint32_t w[LR2021_RDOF_STATE_WORDS], lr2021_rdof_report_t *out)
{
    if (w[1] != LR2021_RDOF_STATE_MAGIC)
        return false;
    *out = (lr2021_rdof_report_t){.version = w[0], .count = (uint8_t)w[3], .enabled = w[2] != 0, .sync_runs = w[4],
                                  .rf_runs = w[5], .samp_runs = w[6], .cmds = w[7]};
    return true;
}

/* ── DMR 4-FSK reception (R3.40) ── */

uint64_t lr2021_dmr_sync(int family, bool voice)
{
    static const uint64_t s[2][2] = {{0xDFF57D75DF5Dull, 0x755FD7DF75F7ull}, {0xD5D7F77FD757ull, 0x7F7D5DD57DFDull}};
    return family == LR2021_DMR_BS || family == LR2021_DMR_MS ? s[family][voice] : 0;
}

/* The correlator patterns hold one bit a symbol, its sign, the earliest symbol in bit 0 (24 symbols). */
static uint32_t sign_pattern(uint64_t sync)
{
    uint32_t p = 0;
    for (int i = 0; i < 24; i++) {
        unsigned dibit = (unsigned)(sync >> (2 * (23 - i))) & 3; /* DMR: 00 +1, 01 +3, 10 -1, 11 -3 */
        p |= (uint32_t)(dibit >> 1) << i;
    }
    return p;
}

int lr2021_rdof_dmr_rx_table(int family, lr2021_rdof_entry_t out[LR2021_DMR_RX_ENTRIES])
{
    if (family != LR2021_DMR_BS && family != LR2021_DMR_MS)
        return 0;
    const uint64_t data = lr2021_dmr_sync(family, false), voice = lr2021_dmr_sync(family, true);
    const lr2021_rdof_entry_t t[LR2021_DMR_RX_ENTRIES] = {
        {0xF30C18, 0xFFFFFFFF, sign_pattern(data)},  /* the two correlator patterns */
        {0xF30C1C, 0xFFFFFFFF, sign_pattern(voice)},
        {0xF30C20, 0x003F007F, 0x00030057},          /* 24 chips, two patterns, window 3 */
        {0xF30C24, 0xF8FFC003, 0xF8FF4001},          /* dual pattern mode */
        {0xF30C14, 0x0000007F, 0x0000000C},          /* detection threshold */
        {0xF30824, 0xFFFFFFFF, (uint32_t)data},      /* the two sync words' low 32 bits */
        {0xF30820, 0xFFFFFFFF, (uint32_t)voice},
        {0xF30828, 0x0000007F, 32},
    };
    for (int i = 0; i < LR2021_DMR_RX_ENTRIES; i++)
        out[i] = t[i];
    return LR2021_DMR_RX_ENTRIES;
}
