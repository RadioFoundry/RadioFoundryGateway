// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A PlutoSDR-family board's FPGA bitstream: the board, its file, a check (plutofpga.h) */
#include "rdof/sdr/plutofpga.h"

#include <stdio.h>

#define ZYNQ_7Z010 0x3722u
#define ZYNQ_7Z020 0x3727u
#define MB (1024ull * 1024ull)

const char *rdof_plutofpga_board(const rdof_plutofpga_facts_t *f)
{
    const unsigned dev = f->idcode >> 12 & 0xFFFFu;
    const bool ddr32 = (f->ddrc >> 2 & 3u) == 0, eth = (f->mio16 >> 1 & 0x7Fu) == 1;
    if (dev != ZYNQ_7Z010)
        return NULL; /* no 7020 board's bitstream yet */
    if (ddr32 && f->mem == 1024 * MB && eth && f->lvds)
        return "fishball7010";
    if (!ddr32 && f->mem == 512 * MB && eth && !f->lvds)
        return "plutoplus";
    if (!ddr32 && f->mem == 512 * MB && !eth && !f->lvds)
        return "adalm-pluto";
    return NULL;
}

void rdof_plutofpga_file(const char *board, char *out, size_t cap) { snprintf(out, cap, "rfg-pluto-%s.bit.bin", board); }

static uint32_t word(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

int rdof_plutofpga_check(const uint8_t *b, size_t n, uint32_t idcode, char *err, size_t cap)
{
    /* bootgen's .bin: 32-bit words, each byte-swapped from the .bit's; the sync word in the first few, then a type 1
       write of IDCODE (0x30018001) and the part's IDCODE */
    size_t i = 0;
    while (i + 4 <= n && i < 256 && word(b + i) != 0xAA995566u)
        i += 4;
    if (i + 4 > n || i >= 256) {
        snprintf(err, cap, "not a Zynq bitstream (.bin): no sync word");
        return -1;
    }
    for (i += 4; i + 8 <= n && i < 1024; i += 4)
        if (word(b + i) == 0x30018001u) {
            const uint32_t id = word(b + i + 4);
            if ((id & 0x0FFFFFFFu) != (idcode & 0x0FFFFFFFu)) {
                snprintf(err, cap, "for another Zynq (IDCODE 0x%08x, this one's 0x%08x)", id, idcode);
                return -1;
            }
            return 0;
        }
    snprintf(err, cap, "not a Zynq bitstream (.bin): no IDCODE");
    return -1;
}

uint64_t rdof_plutofpga_hash(const uint8_t *b, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
