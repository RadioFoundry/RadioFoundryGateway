// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Host tests for a PlutoSDR board's bitstream (plutofpga.h): the board from its facts (the fishball7010's as read on
 * one, 2026-11-04), and the check of a bitstream's head (bootgen's .bin as it begins).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rdof/sdr/plutofpga.h"

static int checks, failures;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                           \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

#define MB (1024ull * 1024ull)

static const char *board(uint32_t idcode, uint32_t ddrc, uint32_t mio16, uint64_t mem, bool lvds)
{
    const rdof_plutofpga_facts_t f = {idcode, ddrc, mio16, mem, lvds};
    const char *b = rdof_plutofpga_board(&f);
    return b ? b : "(none)";
}

static void test_board(void)
{
    /* a fishball7010 clone: 7z010 rev 1, 32-bit DDR, Ethernet on MIO 16, 1 GB, LVDS */
    CHECK(!strcmp(board(0x13722093, 0x81, 0x1202, 1024 * MB, true), "fishball7010"), "%s",
          board(0x13722093, 0x81, 0x1202, 1024 * MB, true));
    CHECK(!strcmp(board(0x13722093, 0x85, 0x1202, 512 * MB, false), "plutoplus"), "16-bit, Ethernet, CMOS");
    CHECK(!strcmp(board(0x03722093, 0x85, 0x0601, 512 * MB, false), "adalm-pluto"), "16-bit, no Ethernet, CMOS");
    CHECK(!strcmp(board(0x13727093, 0x81, 0x1202, 1024 * MB, true), "(none)"), "a 7z020: no board yet");
    CHECK(!strcmp(board(0x13722093, 0x81, 0x1202, 512 * MB, true), "(none)"), "32-bit with 512 MB: unknown");
    CHECK(!strcmp(board(0x13722093, 0x85, 0x1202, 512 * MB, true), "(none)"), "16-bit LVDS: unknown");
    char f[64];
    rdof_plutofpga_file("fishball7010", f, sizeof f);
    CHECK(!strcmp(f, "rfg-pluto-fishball7010.bit.bin"), "%s", f);
}

static void put(uint8_t *b, uint32_t w) { b[0] = (uint8_t)w, b[1] = (uint8_t)(w >> 8), b[2] = (uint8_t)(w >> 16), b[3] = (uint8_t)(w >> 24); }

static void test_check(void)
{
    /* a .bin's head: dummy words, the bus width words, dummies, the sync word, NOOPs, IDCODE's write */
    uint8_t b[256];
    memset(b, 0xFF, sizeof b);
    put(b + 0x20, 0x000000BB), put(b + 0x24, 0x11220044);
    put(b + 0x30, 0xAA995566);
    for (int i = 0x34; i < 0x7C; i += 4)
        put(b + i, 0x20000000);
    put(b + 0x7C, 0x30018001), put(b + 0x80, 0x03722093);
    char err[120] = "";
    CHECK(rdof_plutofpga_check(b, sizeof b, 0x13722093, err, sizeof err) == 0, "7z010 rev 1 takes it: %s", err);
    CHECK(rdof_plutofpga_check(b, sizeof b, 0x13727093, err, sizeof err) == -1 && strstr(err, "another Zynq"),
          "a 7z020 doesn't: %s", err);
    uint8_t c[256];
    memcpy(c, b, sizeof c);
    put(c + 0x30, 0x665599AA); /* the .bit's byte order */
    CHECK(rdof_plutofpga_check(c, sizeof c, 0x13722093, err, sizeof err) == -1 && strstr(err, "sync"), "%s", err);
    CHECK(rdof_plutofpga_check(b, 0x7C, 0x13722093, err, sizeof err) == -1 && strstr(err, "IDCODE"), "cut short: %s",
          err);
    CHECK(rdof_plutofpga_hash((const uint8_t *)"a", 1) == 0xaf63dc4c8601ec8cull, "FNV-1a 64 of \"a\"");
    CHECK(rdof_plutofpga_hash(b, sizeof b) != rdof_plutofpga_hash(c, sizeof c), "the hash tells them apart");
}

int main(void)
{
    test_board();
    test_check();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
