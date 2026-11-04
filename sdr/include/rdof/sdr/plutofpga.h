// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A PlutoSDR-family board's FPGA bitstream (the channelizer and the TX interpolator, built in rfg-pluto): which
 * board this is, the bitstream's file name for it, and whether a file is a bitstream for this board's Zynq. Portable
 * C: the caller reads the facts (rdofradio_fpga.c on the board, a table in the host tests).
 *
 * Clones carry ADI's model string, so the board is told from what its FSBL set up and its device tree:
 *   IDCODE    0xF8000530 bits 27..12: 0x3722 7z010, 0x3727 7z020
 *   DDRC      0xF8006000 bits 3..2: 0 a 32-bit bus, 1 16-bit
 *   MIO_PIN16 0xF8000740 bits 7..1: 1 Ethernet on MIO 16-27
 *   the DDR's size (/proc/device-tree/memory/reg), and the AD936x's bus (adi,lvds-mode-enable on the ad9361-phy)
 */
#ifndef RDOF_SDR_PLUTOFPGA_H
#define RDOF_SDR_PLUTOFPGA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_PLUTOFPGA_IDCODE 0xF8000530u
#define RDOF_PLUTOFPGA_DDRC 0xF8006000u
#define RDOF_PLUTOFPGA_MIO16 0xF8000740u

typedef struct {
    uint32_t idcode, ddrc, mio16; /* the registers above */
    uint64_t mem;                 /* DDR bytes */
    bool lvds;                    /* the AD936x on LVDS */
} rdof_plutofpga_facts_t;

/* The board's name as rfg-pluto's hdl/boards has it ("adalm-pluto", "plutoplus", "fishball7010"), or NULL: none
 * of them (no bitstream is loaded there). */
const char *rdof_plutofpga_board(const rdof_plutofpga_facts_t *f);
/* Its bitstream's file name: "rfg-pluto-<board>.bit.bin" (bootgen's .bin, as the FPGA manager takes it). */
void rdof_plutofpga_file(const char *board, char *out, size_t cap);
/* 0 if b is a Zynq bitstream (.bin: the sync word, byte-swapped words) for the part with IDCODE idcode (the
 * revision, bits 31..28, aside), else -1 with why in err. */
int rdof_plutofpga_check(const uint8_t *b, size_t n, uint32_t idcode, char *err, size_t cap);
/* FNV-1a, 64 bits: which bitstream is in (rdofradio's note of the one it loaded) */
uint64_t rdof_plutofpga_hash(const uint8_t *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif
