// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Loading the patch RAM on a chip (lr2021.h, lr2021_patch_load / _info): WriteRegMem32 in blocks of 32 words from
 * 0x801000, then EnablePram (0x012D 0x00), as DS 22.3 describes. Our own image (lr2021_patch_load_rdof) loads
 * the same way in place of Semtech's (pram/README.md). Only the driver's public calls. */
#include "lr2021.h"
#include "lr2021_fsk.h"
#include "lr2021_patch.h"
#include "lr2021_radio.h"

lr2021_err_t lr2021_patch_info(lr2021_t *d, lr2021_patch_info_t *info)
{
    uint32_t w[2] = {0};
    lr2021_err_t e = lr2021_read_mem(d, LR2021_PATCH_MAGIC_ADDR, w, 2);
    *info = e == LR2021_OK ? lr2021_patch_parse(w[0], w[1]) : (lr2021_patch_info_t){0};
    return e;
}

static lr2021_err_t write_image(lr2021_t *d, uint32_t addr, const uint32_t *image, size_t words)
{
    uint8_t f[8 + 4 * 32];
    lr2021_err_t e = LR2021_OK;
    for (size_t i = 0; e == LR2021_OK && i < words; i += 32) {
        int n = words - i < 32 ? (int)(words - i) : 32;
        e = lr2021_write(d, f, lr2021_radio_write_regs(f, addr + 4 * (uint32_t)i, image + i, n));
    }
    return e;
}

lr2021_err_t lr2021_patch_load(lr2021_t *d, const uint32_t *image, size_t words)
{
    if (!image) {
        image = lr2021_patch_image;
        words = LR2021_PATCH_WORDS;
    }
    uint8_t f[8 + 4 * 32];
    lr2021_err_t e;
    lr2021_patch_info_t info;
    if ((e = lr2021_patch_info(d, &info)) != LR2021_OK)
        return e;
    if (info.loaded)
        return LR2021_ERR_INVALID_STATE; /* one already runs: only a reset removes it */
    if ((e = lr2021_write(d, f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0))) != LR2021_OK)
        return e;
    if ((e = write_image(d, LR2021_PATCH_ADDR, image, words)) != LR2021_OK)
        return e;
    if ((e = lr2021_write(d, f, lr2021_cmd_u8(f, LR2021_ENABLE_PRAM, 0))) != LR2021_OK)
        return e;
    if ((e = lr2021_patch_info(d, &info)) != LR2021_OK)
        return e;
    return info.loaded ? LR2021_OK : LR2021_ERR_INVALID_RESPONSE; /* enabled, but the magic word isn't there */
}

/* Our image in place of Semtech's: the same load and EnablePram 0, then its type in the marker, an empty table, and
 * the version read straight from its state in bank 2. */
lr2021_err_t lr2021_patch_load_rdof(lr2021_t *d, lr2021_rdof_report_t *rep)
{
    uint8_t f[4];
    lr2021_err_t e;
    if ((e = lr2021_patch_load(d, lr2021_rdof_pram_image, lr2021_rdof_pram_words)) != LR2021_OK)
        return e;
    lr2021_patch_info_t info;
    if ((e = lr2021_patch_info(d, &info)) != LR2021_OK)
        return e;
    if (info.type != LR2021_RDOF_TYPE)
        return LR2021_ERR_INVALID_RESPONSE;
    if ((e = lr2021_write(d, f, lr2021_rdof_cmd(f, LR2021_RDOF_CLEAR))) != LR2021_OK) /* RAM may hold an old table */
        return e;
    lr2021_rdof_report_t r;
    if ((e = lr2021_rdof_state(d, &r)) != LR2021_OK)
        return e;
    if (rep)
        *rep = r;
    return r.version == LR2021_RDOF_VERSION && r.count == 0 ? LR2021_OK : LR2021_ERR_INVALID_RESPONSE;
}

lr2021_err_t lr2021_rdof_state(lr2021_t *d, lr2021_rdof_report_t *rep)
{
    uint32_t w[LR2021_RDOF_STATE_WORDS];
    lr2021_err_t e = lr2021_read_mem(d, LR2021_RDOF_STATE_ADDR, w, LR2021_RDOF_STATE_WORDS);
    if (e != LR2021_OK)
        return e;
    return lr2021_rdof_parse_state(w, rep) ? LR2021_OK : LR2021_ERR_INVALID_RESPONSE;
}

lr2021_err_t lr2021_rdof_report(lr2021_t *d, lr2021_rdof_report_t *rep)
{
    uint8_t f[4], b[LR2021_RDOF_REPORT_LEN];
    lr2021_err_t e = lr2021_read(d, f, lr2021_rdof_cmd(f, LR2021_RDOF_REPORT), b, sizeof b);
    if (e != LR2021_OK)
        return e;
    return lr2021_rdof_parse_report(b, sizeof b, rep) ? LR2021_OK : LR2021_ERR_INVALID_RESPONSE;
}

lr2021_err_t lr2021_rdof_set(lr2021_t *d, const lr2021_rdof_entry_t *entries, int n, bool enable)
{
    if (n < 0 || n > LR2021_RDOF_MAX_ENTRIES || (n && !entries))
        return LR2021_ERR_INVALID_ARG;
    uint8_t f[3 + 12 * LR2021_RDOF_APPEND_MAX];
    lr2021_err_t e = lr2021_write(d, f, lr2021_rdof_cmd(f, LR2021_RDOF_CLEAR));
    for (int i = 0; e == LR2021_OK && i < n; i += LR2021_RDOF_APPEND_MAX) {
        int k = n - i < LR2021_RDOF_APPEND_MAX ? n - i : LR2021_RDOF_APPEND_MAX;
        e = lr2021_write(d, f, lr2021_rdof_append(f, entries + i, k));
    }
    if (e == LR2021_OK && enable)
        e = lr2021_write(d, f, lr2021_rdof_cmd(f, LR2021_RDOF_ENABLE));
    return e;
}

lr2021_err_t lr2021_rdof_enable(lr2021_t *d, bool on)
{
    uint8_t f[4];
    return lr2021_write(d, f, lr2021_rdof_cmd(f, on ? LR2021_RDOF_ENABLE : LR2021_RDOF_DISABLE));
}

lr2021_err_t lr2021_fsk4_dmr_rx(lr2021_t *d, int family)
{
    lr2021_rdof_entry_t tab[LR2021_DMR_RX_ENTRIES];
    if (!lr2021_rdof_dmr_rx_table(family, tab))
        return LR2021_ERR_INVALID_ARG;
    lr2021_patch_info_t info;
    lr2021_err_t e = lr2021_patch_info(d, &info);
    if (e != LR2021_OK)
        return e;
    if (!info.loaded || info.type != LR2021_RDOF_TYPE)
        return LR2021_ERR_INVALID_STATE;
    const lr2021_fsk_packet_t pkt = {.format = LR2021_FSK_FORMAT_FIXED, .payload_len = LR2021_DMR_RX_LEN};
    uint8_t f[16];
    if ((e = lr2021_write(d, f, lr2021_radio_set_packet_type(f, LR2021_PACKET_FSK))) == LR2021_OK &&
        (e = lr2021_write(d, f, lr2021_cmd_fsk_variant(f, 1))) == LR2021_OK &&
        (e = lr2021_write(d, f, lr2021_fsk_modulation(f, 9600, LR2021_FSK_SHAPE_BT_0_5, 0x1E, 1944))) == LR2021_OK &&
        (e = lr2021_write(d, f, lr2021_fsk_packet(f, &pkt))) == LR2021_OK &&
        (e = lr2021_write(d, f, lr2021_fsk_syncword(f, lr2021_dmr_sync(family, false), 48, true))) == LR2021_OK &&
        (e = lr2021_write(d, f, lr2021_cmd_u8(f, LR2021_SET_FSK_DEMOD_OPT, LR2021_DMR_RX_DEMOD_OPT))) == LR2021_OK)
        e = lr2021_rdof_set(d, tab, LR2021_DMR_RX_ENTRIES, true);
    return e;
}
