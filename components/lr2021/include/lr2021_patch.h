// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Semtech's patch RAM (PRAM) for the LR2021 (DS 22.3): a program the ROM runs from 0x801000 once enabled, lost on
 * reset. Semtech's drivers load it at start. It fixes DCDC clocking for sub-GHz RX, RTToF, BLE and FLRC details, and
 * adds Z-Wave, O-QPSK and WM-Bus N 4GFSK extras; on ROM 1.24 FSK packet RX works only with a patch running
 * (docs/spec/lr2021-iq.md R1.4, R3.35). The firmware keeps the raw I/Q paths for sessions without one (R1.2, a host
 * rule: the chip doesn't refuse them). Only a reset removes it.
 */
#ifndef LR2021_PATCH_H
#define LR2021_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LR2021_PATCH_WORDS 560
#define LR2021_PATCH_ADDR 0x00801000u       /* where it loads */
#define LR2021_PATCH_MAGIC_ADDR 0x00800FF8u /* the patch writes its magic here once running */
#define LR2021_PATCH_MAGIC 0x600DB002u
#define LR2021_PATCH_INFO_ADDR 0x00800FFCu  /* its type (bits 23:16) and version (15:8) */
#define LR2021_ENABLE_PRAM 0x012D           /* argument 0: run the loaded patch */

/* The image (src/lr2021_patch_image.c): Semtech's LR2021 patch, type 3, version 0x13 (USP 2.0.2). */
extern const uint32_t lr2021_patch_image[LR2021_PATCH_WORDS];

typedef struct {
    bool loaded;     /* the magic word is there */
    uint8_t type;    /* 3 for the LR2021 image */
    uint8_t version;
} lr2021_patch_info_t;

/* From the words at LR2021_PATCH_MAGIC_ADDR and LR2021_PATCH_INFO_ADDR. */
lr2021_patch_info_t lr2021_patch_parse(uint32_t magic, uint32_t info);
/* The image's own type and version (its words 0 and 1 are the magic and the info word). */
lr2021_patch_info_t lr2021_patch_image_info(void);

/* ── Our own patch image (pram/rdof_pram.S, pram/README.md) ──
 * In place of Semtech's: loaded at LR2021_PATCH_ADDR and started by EnablePram 0 the same way. It keeps a table of
 * register overrides it applies after the chip programs its FSK sync detector, after every frequency change and
 * after the ROM's sample-rate helper (JLI 0x01, 0x31 and 0x00), set through command 0x02F0. Its state is plain
 * data in SRAM bank 2, which the host reads directly. */
#define LR2021_RDOF_TYPE 0x52             /* the info word's type ('R'); Semtech's is 3 */
#define LR2021_RDOF_VERSION 0x52440200u   /* "RD", v2.0: the state's and the report's version word */
#define LR2021_RDOF_STATE_ADDR 0x00802000u /* version, magic, enabled, count, 3 run counts, commands; then the table */
#define LR2021_RDOF_STATE_WORDS 8
#define LR2021_RDOF_STATE_MAGIC 0x52445354u
#define LR2021_RDOF_CMD 0x02F0
#define LR2021_RDOF_MAX_ENTRIES 32        /* the table's size */
#define LR2021_RDOF_APPEND_MAX 10         /* entries per append frame (the chip takes 21; 10 keeps a frame to 123 bytes) */
#define LR2021_RDOF_REPORT_LEN 18

/* Sub-commands, the payload's first byte. */
enum {
    LR2021_RDOF_CLEAR = 0x00,  /* empty the table and disable it (the run counts stay: EnablePram zeroes them) */
    LR2021_RDOF_APPEND = 0x01, /* add entries (all or none: a full table or a bad address fails the command) */
    LR2021_RDOF_ENABLE = 0x02, /* apply the table from now on, and once at once */
    LR2021_RDOF_DISABLE = 0x03, /* stop applying it; what was written stays until the chip rewrites it */
    LR2021_RDOF_REPORT = 0x04,
};

/* One override: mask 0 writes value as it is (for a port register written several times in a row); otherwise the
 * masked bits of the register at addr take value's. addr: a word in RAM 0x800000-0x80FFFF or registers
 * 0xF00000-0xF4FFFF. */
typedef struct {
    uint32_t addr, mask, value;
} lr2021_rdof_entry_t;

typedef struct {
    uint32_t version;   /* LR2021_RDOF_VERSION when ours runs */
    uint8_t count;      /* entries in the table */
    bool enabled;
    uint32_t sync_runs; /* times the chip set up its FSK sync detector since EnablePram */
    uint32_t rf_runs;   /* times it changed frequency since EnablePram */
    uint32_t samp_runs; /* times JLI 0x00 ran (0xF30C30, 0xF30C24 [25:24]) since EnablePram */
    uint32_t cmds;      /* 0x02F0 commands handled (read from the state only; the report leaves it 0) */
} lr2021_rdof_report_t;

/* 4-FSK reception of DMR through our patch (docs/spec/lr2021-iq.md R3.40). The chip's own setup for the 4-FSK
 * variant leaves a correlator built for a 2-FSK sync word; these entries replace it with DMR's two sync patterns of
 * a family (data and voice, which also covers both polarities) and the dual 32-bit sync layout. Each received
 * packet is the LR2021_DMR_RX_LEN bytes after a sync, in the chip's TX coding: lr2021_fsk4_from_dmr (lr2021_tx.h,
 * XOR 0xAA) turns them into DMR dibits. BS was checked on the bench; MS is the same construction, not yet tried. */
enum { LR2021_DMR_BS = 0, LR2021_DMR_MS = 1 };
#define LR2021_DMR_RX_ENTRIES 8
#define LR2021_DMR_RX_LEN 13 /* the 52 symbols after the sync (of 54 in the burst's second half) */
#define LR2021_DMR_RX_DEMOD_OPT 0x05 /* 0x022F's byte: bit 0 cuts the symbol errors about 4x (R3.42) */
/* DMR's 48-bit sync word: a family's data or voice pattern. */
uint64_t lr2021_dmr_sync(int family, bool voice);
/* The override table for a family into out; returns LR2021_DMR_RX_ENTRIES, 0 for a bad family. */
int lr2021_rdof_dmr_rx_table(int family, lr2021_rdof_entry_t out[LR2021_DMR_RX_ENTRIES]);

/* The image (src/lr2021_rdof_pram_image.c, generated by pram/build.sh). */
extern const uint32_t lr2021_rdof_pram_image[];
extern const size_t lr2021_rdof_pram_words;

/* Frame builders: the whole frame into buf; returns its length, 0 for bad arguments. */
size_t lr2021_rdof_cmd(uint8_t *buf, uint8_t sub); /* clear, enable, disable, report; buf of 3 bytes */
/* 1 to LR2021_RDOF_APPEND_MAX entries; buf of 3 + 12 n bytes. */
size_t lr2021_rdof_append(uint8_t *buf, const lr2021_rdof_entry_t *entries, int n);
/* The report's answer (LR2021_RDOF_REPORT_LEN bytes after the status). */
bool lr2021_rdof_parse_report(const uint8_t *b, size_t n, lr2021_rdof_report_t *out);
/* The state words read at LR2021_RDOF_STATE_ADDR; false if the magic isn't there. */
bool lr2021_rdof_parse_state(const uint32_t w[LR2021_RDOF_STATE_WORDS], lr2021_rdof_report_t *out);

#ifdef __cplusplus
}
#endif

#endif
