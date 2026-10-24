// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The portable core's less-traveled paths: error and validation branches and helpers the scenarios don't reach.
 * Expected values come from the reference (the FEC, the OLED and the radio) where there is one. */
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/ambe.h"
#include "rdof/benchcfg.h"
#include "rdof/bits.h"
#include "rdof/config.h"
#include "rdof/features.h"
#include "rdof/fec.h"
#include "rdof/hblink.h"
#include "rdof/json.h"
#include "rdof/jsonp.h"
#include "rdof/layer2.h"
#include "rdof/netcalls.h"
#include "rdof/mmdvm.h"
#include "rdof/modem.h"
#include "rdof/oled.h"
#include "rdof/oled_sdr.h"
#include "rdof/pdu.h"
#include "rdof/rawburst.h"
#include "rdof/slotgrid.h"
#include "rdof/status.h"
#include "rdof/tracker.h"

extern int checks, failures;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                          \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

/* ── FEC and CRCs ─────────────────────────────────────────────────────────── */

static void t_crcs(void)
{
    /* fec.crc16_ccitt(b"123456789") */
    CHECK(rdof_crc16_ccitt((const uint8_t *)"123456789", 9) == 0xCE3C, "crc16 %04x",
          rdof_crc16_ccitt((const uint8_t *)"123456789", 9));
    /* fec.five_bit_checksum(bytes([0, 0, 0x10, 0, 0, 1, 0, 0, 2])) */
    const uint8_t lc[9] = {0, 0, 0x10, 0, 0, 1, 0, 0, 2};
    CHECK(rdof_five_bit_checksum(lc) == 19, "5-bit checksum %d", rdof_five_bit_checksum(lc));
#if RDOF_FEATURE_DATA_BLOCKS
    /* fec.crc32_dmr(bytes(range(12))), stored little endian after the data */
    uint8_t pkt[16];
    for (int i = 0; i < 12; i++)
        pkt[i] = (uint8_t)i;
    const uint32_t crc = rdof_crc32_dmr(pkt, 12);
    CHECK(crc == 0x65609AE6, "crc32 %08x", crc);
    for (int i = 0; i < 4; i++)
        pkt[12 + i] = (uint8_t)(crc >> (8 * i));
    CHECK(rdof_check_crc32(pkt, 16), "crc32 checks");
    pkt[3] ^= 0x01;
    CHECK(!rdof_check_crc32(pkt, 16), "a flipped bit fails");
    CHECK(!rdof_check_crc32(pkt, 3), "shorter than a CRC");
    /* fec.crc9_dmr([(i * 7) % 3 % 2 for i in range(80)]) */
    uint8_t bits[80];
    for (int i = 0; i < 80; i++)
        bits[i] = (uint8_t)((i * 7) % 3 % 2);
    CHECK(rdof_crc9_dmr(bits, 80) == 0x0FE, "crc9 %03x", rdof_crc9_dmr(bits, 80));
#endif
}

static void t_hamming_code(const rdof_hamming_t *h, int n, int k, const char *name)
{
    uint8_t w[16], c[16];
    for (int v = 0; v < (1 << k); v += 37) {
        rdof_u64_to_bits((uint64_t)v, k, w);
        rdof_hamming_encode(h, w);
        CHECK(rdof_hamming_syndrome(h, w) == 0, "%s %d: clean syndrome", name, v);
        bool corrected = true;
        memcpy(c, w, (size_t)n);
        CHECK(rdof_hamming_decode(h, c, &corrected) && !corrected && !memcmp(c, w, (size_t)n), "%s %d: clean", name, v);
        for (int b = 0; b < n; b++) { /* every single error is found and fixed */
            memcpy(c, w, (size_t)n);
            c[b] ^= 1;
            CHECK(rdof_hamming_syndrome(h, c) != 0, "%s %d bit %d: syndrome", name, v, b);
            CHECK(rdof_hamming_decode(h, c, &corrected) && corrected && !memcmp(c, w, (size_t)n), "%s %d bit %d: fixed",
                  name, v, b);
        }
    }
    CHECK(rdof_hamming_decode(h, w, NULL), "%s: no corrected flag wanted", name);
}

static void t_hamming(void)
{
    t_hamming_code(RDOF_H_15_11, 15, 11, "(15,11)");
    t_hamming_code(RDOF_H_13_9, 13, 9, "(13,9)");
    t_hamming_code(RDOF_H_16_11, 16, 11, "(16,11,4)");
    /* (16,11,4): any two errors are detected, never "fixed" into a wrong word */
    uint8_t w[16], c[16];
    rdof_u64_to_bits(0x5A3, 11, w);
    rdof_hamming_encode(RDOF_H_16_11, w);
    int wrong = 0;
    for (int a = 0; a < 16; a++)
        for (int b = a + 1; b < 16; b++) {
            memcpy(c, w, 16);
            c[a] ^= 1;
            c[b] ^= 1;
            wrong += rdof_hamming_decode(RDOF_H_16_11, c, NULL);
        }
    CHECK(wrong == 0, "(16,11,4): %d double errors passed", wrong);
    /* (13,9) is shortened: some double errors give a syndrome no single bit has, and are refused */
    rdof_u64_to_bits(0x0A5, 9, w);
    rdof_hamming_encode(RDOF_H_13_9, w);
    int refused = 0;
    for (int a = 0; a < 13; a++)
        for (int b = a + 1; b < 13; b++) {
            memcpy(c, w, 13);
            c[a] ^= 1;
            c[b] ^= 1;
            refused += !rdof_hamming_decode(RDOF_H_13_9, c, NULL);
        }
    CHECK(refused > 0, "(13,9): no double error refused");
    /* (15,11) is perfect: every syndrome is one bit, so ham_correct can't fail there (fec.c's (16,11,4) branch
     * "if (!ham_correct) return false" is unreachable for that reason). */
}

/* rdof/bits.h at its edges: 0 and 64 bit fields, fields across byte boundaries, the bits either side kept. */
static void t_bits(void)
{
    uint8_t b[10];
    memset(b, 0xFF, sizeof b);
    rdof_set_field(b, 3, 64, 0x0123456789ABCDEFull);
    CHECK(rdof_get_field(b, 3, 64) == 0x0123456789ABCDEFull, "64 bit field %llx",
          (unsigned long long)rdof_get_field(b, 3, 64));
    CHECK(rdof_get_field(b, 0, 3) == 7 && rdof_get_field(b, 67, 13) == 0x1FFF, "neighbors kept");
    CHECK(b[0] == 0xE0 && b[7] == 0xBD && b[8] == 0xFF, "packed %02x %02x", b[0], b[7]); /* 111 00000; ...EF<<5 */
    rdof_set_field(b, 5, 0, 0);
    CHECK(rdof_get_field(b, 5, 0) == 0 && b[0] == 0xE0, "0 bit field");
    rdof_set_field(b, 13, 7, 0x55);
    CHECK(rdof_get_field(b, 13, 7) == 0x55 && rdof_get_field(b, 3, 10) == 0x004, "7 bits across a byte %llx",
          (unsigned long long)rdof_get_field(b, 3, 10));

    /* every start and length to 72 bits against a bit at a time (past 64, the first bits fall off the top) */
    uint8_t r[19];
    uint32_t x = 7;
    for (size_t i = 0; i < sizeof r; i++)
        r[i] = (uint8_t)((x = x * 1664525u + 1013904223u) >> 24);
    int bad = 0;
    for (int start = 0; start < 80; start++)
        for (int n = 0; n <= 72; n++) {
            uint64_t want = 0;
            for (int i = start; i < start + n; i++)
                want = (want << 1) | ((r[i >> 3] >> (7 - (i & 7))) & 1);
            bad += rdof_get_field(r, start, n) != want;
        }
    CHECK(bad == 0, "get_field at every start and length: %d wrong", bad);

    uint8_t bits[80], back[10];
    rdof_bytes_to_bits(b, 10, bits);
    rdof_bits_to_bytes(bits, 80, back);
    CHECK(!memcmp(b, back, 10), "bytes -> bits -> bytes");
    CHECK(rdof_bits_to_u64(bits + 3, 64) == rdof_get_field(b, 3, 64), "bits_to_u64 64");
    bits[0] = 2; /* any nonzero is a 1 when packing; bits_to_u64 takes the low bit */
    rdof_bits_to_bytes(bits, 11, back);
    CHECK(back[0] == 0xE0 && back[1] == (b[1] & 0xE0), "11 bits zero padded %02x %02x", back[0], back[1]);
    CHECK(rdof_bits_to_u64(bits, 3) == 3, "low bit only");
    memset(bits, 9, sizeof bits);
    rdof_u64_to_bits(0x8000000000000001ull, 64, bits);
    CHECK(bits[0] == 1 && bits[1] == 0 && bits[63] == 1 && bits[64] == 9, "u64_to_bits 64");
    rdof_u64_to_bits(5, 0, bits);
    CHECK(bits[0] == 1, "0 bits written");

    CHECK(rdof_popcount64(0) == 0 && rdof_popcount64(~0ull) == 64 && rdof_popcount64(0x8000000000000101ull) == 3,
          "popcount");
    uint8_t p[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    rdof_put_be24(p + 1, 0xFF123456);
    CHECK(p[0] == 0xAA && p[1] == 0x12 && p[3] == 0x56 && p[4] == 0xAA, "put_be24");
    CHECK(rdof_be24(p + 1) == 0x123456, "be24");
    rdof_put_be32(p + 1, 0xDEADBEEF);
    CHECK(rdof_be32(p + 1) == 0xDEADBEEF && p[0] == 0xAA && p[1] == 0xDE, "be32 unaligned");
}

static void t_burst_fields(void)
{
    uint8_t burst[RDOF_BURST_BYTES] = {0};
    rdof_set_field(burst, 98, 4, 0xA);  /* slot type: color code */
    rdof_set_field(burst, 102, 4, 0x3); /* data type */
    int cc, dt;
    rdof_slot_type_raw(burst, &cc, &dt);
    CHECK(cc == 10 && dt == 3, "slot type %d %d", cc, dt);
    memset(burst, 0, sizeof burst);
    rdof_set_field(burst, 108, 4, 0x7); /* EMB: color code, PI, LCSS */
    rdof_set_field(burst, 112, 1, 1);
    rdof_set_field(burst, 113, 2, 2);
    int pi, lcss;
    rdof_emb_raw(burst, &cc, &pi, &lcss);
    CHECK(cc == 7 && pi == 1 && lcss == 2, "emb %d %d %d", cc, pi, lcss);
    /* no slot type and no middle field: zeros there, the info bits either side */
    uint8_t info[196], back[196];
    for (int i = 0; i < 196; i++)
        info[i] = (uint8_t)((i * 5 + 1) % 3 == 0);
    memset(burst, 0xFF, sizeof burst);
    rdof_build_burst(info, NULL, NULL, burst);
    rdof_info_bits(burst, back);
    CHECK(!memcmp(info, back, 196), "info bits round trip");
    CHECK(rdof_get_field(burst, 98, 10) == 0 && rdof_get_field(burst, 108, 48) == 0 && rdof_get_field(burst, 156, 10) == 0,
          "slot type and middle zeroed");
}

/* ── JSON reader ──────────────────────────────────────────────────────────── */

typedef struct {
    rdof_jnode_t nodes[32];
    char buf[256];
    rdof_jdoc_t doc;
} jdoc_t;

static int jparse(jdoc_t *j, const char *text, size_t len, int max_nodes, size_t cap)
{
    j->doc = (rdof_jdoc_t){.nodes = j->nodes, .max = max_nodes, .buf = j->buf, .cap = cap};
    return rdof_jparse(&j->doc, text, len);
}

static void t_json_errors(const char *text, const char *why)
{
    jdoc_t j;
    int r = jparse(&j, text, strlen(text), 32, sizeof j.buf);
    CHECK(r < 0 && strstr(j.doc.error, why), "%s: want \"%s\", got %d \"%s\"", text, why, r, j.doc.error);
}

static void t_json(void)
{
    jdoc_t j;
    /* every escape, \u to UTF-8 at each length, a surrogate pair */
    const char *s = "{\"a\": \"x\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0041\\u00e9\\u20AC\\ud83d\\ude00\"}";
    int root = jparse(&j, s, strlen(s), 32, sizeof j.buf);
    const rdof_jnode_t *a = rdof_jnode(&j.doc, rdof_jget(&j.doc, root, "a"));
    const char want[] = "x\"\\/\b\f\n\r\tA\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    CHECK(a && a->type == RDOF_JSTR && !strcmp(a->str, want), "escapes: %s", a ? a->str : "(none)");
    /* a high surrogate without its pair: kept as the code point */
    s = "\"\\ud83d!\"";
    root = jparse(&j, s, strlen(s), 32, sizeof j.buf);
    CHECK(root >= 0 && !strcmp(rdof_jnode(&j.doc, root)->str, "\xED\xA0\xBD!"), "lone surrogate");
    /* numbers, booleans, null, nesting, duplicate keys (the last wins) */
    s = "{\"n\": -1.5e2, \"i\": 42, \"t\": true, \"f\": false, \"z\": null, \"l\": [1, [2]], \"n\": 7}";
    root = jparse(&j, s, strlen(s), 32, sizeof j.buf);
    CHECK(root >= 0, "parses: %s", j.doc.error);
    const rdof_jnode_t *n = rdof_jnode(&j.doc, rdof_jget(&j.doc, root, "n"));
    CHECK(n && n->num == 7 && n->integer, "the last n wins");
    CHECK(rdof_jnode(&j.doc, rdof_jget(&j.doc, root, "t"))->b && !rdof_jnode(&j.doc, rdof_jget(&j.doc, root, "f"))->b, "bools");
    CHECK(rdof_jnode(&j.doc, rdof_jget(&j.doc, root, "z"))->type == RDOF_JNULL, "null");
    int l = rdof_jget(&j.doc, root, "l");
    CHECK(rdof_jnode(&j.doc, rdof_jat(&j.doc, rdof_jat(&j.doc, l, 1), 0))->num == 2, "nested");
    CHECK(rdof_jat(&j.doc, l, 5) < 0 && rdof_jat(&j.doc, -1, 0) < 0 && rdof_jget(&j.doc, l, "x") < 0 && !rdof_jnode(&j.doc, -1),
          "missing things are -1 / NULL");
    /* the errors */
    t_json_errors("", "unexpected end");
    t_json_errors("{\"a\" 1}", "expected :");
    t_json_errors("{\"a\": 1 \"b\": 2}", "expected , or }");
    t_json_errors("[1 2]", "expected , or ]");
    t_json_errors("{1: 2}", "bad key");
    t_json_errors("\"\\q\"", "bad string");
    t_json_errors("\"\\u12\"", "bad string");
    t_json_errors("\"\\u12G4\"", "bad string"); /* four hex digits, all of them */
    t_json_errors("\"abc", "bad string");
    t_json_errors("\"a\x01\"", "bad string");
    t_json_errors("\"a\\", "bad string");
    t_json_errors("-", "bad number");
    t_json_errors("1.2.3", "bad number");
    t_json_errors("@", "unexpected character");
    t_json_errors("1 2", "extra text after the value");
    char deep[80];
    memset(deep, '[', 40);
    memset(deep + 40, ']', 40);
    jdoc_t big;
    CHECK(jparse(&big, deep, 80, 32, sizeof big.buf) < 0 && strstr(big.doc.error, "too many values"), "node limit");
    rdof_jnode_t many[64];
    char sbuf[16];
    rdof_jdoc_t d = {.nodes = many, .max = 64, .buf = sbuf, .cap = sizeof sbuf};
    CHECK(rdof_jparse(&d, deep, 80) < 0 && strstr(d.error, "nested too deep"), "depth limit: %s", d.error);
    /* each kind of value with no node left for it */
    const char *one[] = {"[\"s\"]", "[true]", "[null]", "[1]", "[[]]"};
    for (int i = 0; i < 5; i++)
        CHECK(jparse(&j, one[i], strlen(one[i]), 1, sizeof j.buf) < 0 && strstr(j.doc.error, "too many values"),
              "%s with one node: %s", one[i], j.doc.error);
    /* strings that don't fit the buffer */
    CHECK(jparse(&j, "\"abcdef\"", 8, 32, 4) < 0, "string buffer full");
    CHECK(jparse(&j, "\"ab\\u20ac\"", 10, 32, 3) < 0, "escape into a full buffer");
    /* the text isn't NUL terminated: a literal cut by the end must not be read past it (ASan sees a read over) */
    char cut[4];
    memcpy(cut, "[tru", 4);
    CHECK(jparse(&j, cut, 4, 32, sizeof j.buf) < 0, "true cut short");
    memcpy(cut, "[nul", 4);
    CHECK(jparse(&j, cut, 4, 32, sizeof j.buf) < 0, "null cut short");
    char t3[3];
    memcpy(t3, "[fa", 3);
    CHECK(jparse(&j, t3, 3, 32, sizeof j.buf) < 0, "false cut short");
    /* a NUL byte inside the text isn't part of a number */
    const char nul[] = "[1\0]";
    CHECK(jparse(&j, nul, 4, 32, sizeof j.buf) < 0, "NUL after a number");
}

/* FNV-1a over what a parse left: the result, the error, every node. A digest of many parses pins their output to
   the C's (rdof_tests_rust runs the same digests against the Rust). */
static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ ((const uint8_t *)p)[i]) * 16777619u;
    return h;
}

static uint32_t fnv_str(uint32_t h, const char *s)
{
    return s ? fnv(h, s, strlen(s) + 1) : fnv(h, "-", 1);
}

static uint32_t jdigest(uint32_t h, const rdof_jdoc_t *d, int root)
{
    int head[3] = {root, d->n, (int)d->used};
    h = fnv(fnv(h, head, sizeof head), d->error, strlen(d->error));
    for (int i = 0; i < d->n; i++) {
        const rdof_jnode_t *n = &d->nodes[i];
        int v[6] = {(int)n->type, n->b, n->integer, n->child, n->next, n->count};
        h = fnv_str(fnv_str(fnv(h, v, sizeof v), n->key), n->type == RDOF_JSTR ? n->str : NULL);
        if (n->type == RDOF_JNUM)
            h = fnv(h, &n->num, sizeof n->num);
    }
    return h;
}

/* The text parsed from an exact-size copy: ASan sees any read past its end. */
static int jparse_exact(jdoc_t *j, const char *text, size_t len, int max_nodes, size_t cap)
{
    char *copy = malloc(len ? len : 1);
    memcpy(copy, text, len);
    int r = jparse(j, copy, len, max_nodes, cap);
    free(copy);
    return r;
}

static void t_json_error_is(const char *text, size_t len, const char *error)
{
    jdoc_t j;
    int r = jparse_exact(&j, text, len, 32, sizeof j.buf);
    CHECK(r < 0 && !strcmp(j.doc.error, error), "%.*s: want \"%s\", got %d \"%s\"", (int)len, text, error, r,
          j.doc.error);
}

static const rdof_jnode_t *jone(jdoc_t *j, const char *text)
{
    int r = jparse_exact(j, text, strlen(text), 32, sizeof j->buf);
    CHECK(r >= 0, "%s: %s", text, j->doc.error);
    return rdof_jnode(&j->doc, r);
}

#define JSON_PREFIX_DIGEST 0xe85cff30u
#define JSONC_PREFIX_DIGEST 0xe46c1a80u

static void t_json_edges(void)
{
    jdoc_t j;
    /* the error and its offset, exactly */
    t_json_error_is("[1,", 3, "unexpected end at offset 3");
    t_json_error_is("[1", 2, "expected , or ] at offset 2");
    t_json_error_is("{\"a\":1", 6, "expected , or } at offset 6");
    t_json_error_is("{\"a\"", 4, "expected : at offset 4");
    t_json_error_is("{", 1, "bad key at offset 1");
    t_json_error_is("[1.5e]", 6, "bad number at offset 5");
    t_json_error_is("1e+", 3, "bad number at offset 3");
    t_json_error_is("--1", 3, "bad number at offset 3");
    t_json_error_is("+1", 2, "unexpected character at offset 0");
    t_json_error_is("[tru]", 5, "unexpected character at offset 1");
    t_json_error_is("[nul]", 5, "unexpected character at offset 1");
    t_json_error_is(" \"a\\x\"", 6, "bad string at offset 5");
    t_json_error_is("\"\\u", 3, "bad string at offset 3");
    t_json_error_is("\"a\nb\"", 5, "bad string at offset 3");
    t_json_error_is("[] ]", 4, "extra text after the value at offset 3");
    t_json_error_is("\t\r\n ", 4, "unexpected end at offset 4");
    /* hex digits: each side of each range */
    const char *nothex[] = {"\"\\u/000\"", "\"\\u:000\"", "\"\\u@000\"", "\"\\uG000\"", "\"\\u`000\"", "\"\\ug000\""};
    for (int i = 0; i < 6; i++)
        t_json_error_is(nothex[i], 8, "bad string at offset 3");
    const rdof_jnode_t *n = jone(&j, "\"\\u09aF\\u0A0f\"");
    CHECK(n && !strcmp(n->str, "\xE0\xA6\xAF\xE0\xA8\x8F"), "hex digits in each case");
    /* a high surrogate and what follows it: only a \u low surrogate makes a pair */
    static const struct {
        const char *in, *want;
    } sur[] = {
        {"\"\\ud83d\"", "\xED\xA0\xBD"},                    /* the string ends */
        {"\"\\ud83dxxxxxx\"", "\xED\xA0\xBDxxxxxx"},        /* no backslash */
        {"\"\\ud83d\\n\\u0041\"", "\xED\xA0\xBD\nA"},       /* another escape */
        {"\"\\ud83d\\u0041\"", "\xED\xA0\xBD" "A"},         /* not a low surrogate */
        {"\"\\ud83d\\ue000\"", "\xED\xA0\xBD\xEE\x80\x80"}, /* past the low surrogates */
        {"\"\\ud83d\\udbff\"", "\xED\xA0\xBD\xED\xAF\xBF"}, /* two high ones */
        {"\"\\udc00\\ud83d\"", "\xED\xB0\x80\xED\xA0\xBD"}, /* low before high */
        {"\"\\udbff\\udfff\"", "\xF4\x8F\xBF\xBF"},         /* U+10FFFF */
        {"\"\\u0000\"", ""},                                /* NUL: in the buffer, ends the C string */
        {"\"\\u07ff\\u0800\\uffff\"", "\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF"},
    };
    for (size_t i = 0; i < sizeof sur / sizeof sur[0]; i++) {
        n = jone(&j, sur[i].in);
        CHECK(n && n->type == RDOF_JSTR && !strcmp(n->str, sur[i].want), "%s", sur[i].in);
    }
    t_json_error_is("\"\\ud83d\\u12\"", 12, "bad string at offset 9"); /* no pair; then a bad \u */
    t_json_error_is("\"\\ud83d\\uZZZZ\"", 14, "bad string at offset 9");
    /* bytes past ASCII are kept as they are, not checked as UTF-8 */
    n = jone(&j, "\"\xFF\xC3\"");
    CHECK(n && !strcmp(n->str, "\xFF\xC3"), "raw bytes");
    /* numbers: strtod's, from at most 47 characters */
    n = jone(&j, "1e400");
    CHECK(n && isinf(n->num) && n->num > 0 && !n->integer, "1e400 is inf");
    n = jone(&j, "-1e400");
    CHECK(n && isinf(n->num) && n->num < 0, "-1e400");
    n = jone(&j, "1e-400");
    CHECK(n && n->num == 0, "1e-400");
    n = jone(&j, "4.9406564584124654e-324");
    CHECK(n && n->num == 4.9406564584124654e-324, "the least subnormal");
    n = jone(&j, "-0");
    CHECK(n && n->num == 0 && signbit(n->num) && n->integer, "-0");
    n = jone(&j, "007");
    CHECK(n && n->num == 7 && n->integer, "leading zeros (strtod's, not JSON's)");
    n = jone(&j, "-.5");
    CHECK(n && n->num == -0.5, "-.5");
    n = jone(&j, "1.");
    CHECK(n && n->num == 1 && !n->integer, "1.");
    n = jone(&j, "123456789012345678901234567890");
    CHECK(n && n->num == 123456789012345678901234567890.0 && n->integer, "30 digits");
    char digits47[48];
    memset(digits47, '9', 47);
    digits47[47] = 0;
    n = jone(&j, digits47);
    CHECK(n && n->num == 1e47, "47 digits");
    t_json_error_is("0.1000000000000000055511151231257827021181583404541015625", 57,
                    "extra text after the value at offset 47");
    t_json_error_is("[11111111111111111111111111111111111111111111111111]", 52, "expected , or ] at offset 48");
    t_json_error_is("[1e5e5]", 7, "bad number at offset 6");
    /* containers: empty with space inside, keys with escapes, counts */
    n = jone(&j, "{ \"a\\u0062\" : [ ] , \"c\":{ } }");
    CHECK(n && n->count == 2 && rdof_jget(&j.doc, 0, "ab") == 1 && rdof_jnode(&j.doc, 1)->count == 0 &&
              rdof_jnode(&j.doc, rdof_jget(&j.doc, 0, "c"))->type == RDOF_JOBJ,
          "spaces, an escaped key");
    /* lookups on the wrong kind, with odd indices, past the parsed nodes */
    jone(&j, "[10, 20]");
    CHECK(rdof_jget(&j.doc, 0, "a") < 0 && rdof_jget(&j.doc, 1, "a") < 0, "jget on an array, a number");
    CHECK(rdof_jat(&j.doc, 0, -1) == 1 && rdof_jat(&j.doc, 0, INT_MIN) == 1 && rdof_jat(&j.doc, 0, 1) == 2 &&
              rdof_jat(&j.doc, 0, INT_MAX) < 0 && rdof_jat(&j.doc, 1, 0) < 0,
          "jat: i <= 0 is the first child, past the end -1, a number has none");
    CHECK(rdof_jnode(&j.doc, 3) == NULL && rdof_jat(&j.doc, 3, 0) < 0 && rdof_jat(&j.doc, 31, 0) < 0 &&
              rdof_jget(&j.doc, 3, "a") < 0 && rdof_jnode(&j.doc, 2)->num == 20,
          "a node past the parsed ones is none");
    rdof_jdoc_t none = {.nodes = j.nodes, .max = 32};
    CHECK(rdof_jnode(&none, 0) == NULL, "nothing parsed");

    /* depth: 32 containers is the most */
    char deep[66];
    for (int d = 31; d <= 33; d++) {
        memset(deep, '[', (size_t)d);
        memset(deep + d, ']', (size_t)d);
        rdof_jnode_t many[40];
        rdof_jdoc_t dd = {.nodes = many, .max = 40, .buf = j.buf, .cap = sizeof j.buf};
        int r = rdof_jparse(&dd, deep, (size_t)(2 * d));
        CHECK(d <= 32 ? r == 0 && dd.n == d : r < 0 && !strcmp(dd.error, "nested too deep at offset 32"),
              "depth %d: %s", d, dd.error);
    }
    /* nodes and buffer: exactly enough, and one short */
    const char *doc = "{\"k\": [\"\\u20ac\", 1, true], \"z\": null}";
    size_t len = strlen(doc);
    CHECK(jparse_exact(&j, doc, len, 6, 8) == 0 && j.doc.n == 6 && j.doc.used == 8, "6 nodes, 8 bytes: %s",
          j.doc.error);
    CHECK(jparse_exact(&j, doc, len, 5, 8) < 0 && !strcmp(j.doc.error, "too many values at offset 32"), "5 nodes: %s",
          j.doc.error);
    for (size_t cap = 0; cap < 8; cap++) /* cut in a key, in an escape, at a NUL */
        CHECK(jparse_exact(&j, doc, len, 6, cap) < 0 && strstr(j.doc.error, "bad "), "%zu bytes: %s", cap,
              j.doc.error);
    CHECK(jparse_exact(&j, "[1]", 3, -1, 0) < 0, "a negative node count is none");

    /* every prefix of a few documents, with room and short of nodes and buffer: as the C did */
    static const char *const docs[] = {
        "{\"name\": \"a\\u00e9\\ud83d\\ude00\\n\", \"n\": [1, -2.5e3, true, false, null], \"o\": {\"x\": {}}, \"e\": []}",
        " [ \"\\\\\\\"\" , 0.125 , [ [ ] ] , { \"\" : \"\" } ] ",
        "{\"a\":{\"b\":{\"c\":[1e5,\"\\uDBFF\\uDFFF\"]}},\"a\":-0.0}",
    };
    uint32_t h = 2166136261u;
    for (int d = 0; d < 3; d++) {
        size_t dl = strlen(docs[d]);
        for (size_t cut = 0; cut <= dl; cut++) {
            int r = jparse_exact(&j, docs[d], cut, 32, sizeof j.buf);
            CHECK(r < 0 || cut >= dl - 1, "doc %d cut at %zu parses", d, cut);
            h = jdigest(h, &j.doc, r);
            h = jdigest(h, &j.doc, jparse_exact(&j, docs[d], cut, 7, 40));
        }
    }
    CHECK(h == JSON_PREFIX_DIGEST, "prefixes: digest %08x", h);
}

/* The JSONC helpers on cut text: strip, add and drop on every prefix of a settings text. */
static void t_jsonc_edges(void)
{
    static const char text[] =
        "{\n"
        "  // a comment, \"wifi\": not this one\n"
        "  \"name\": \"x\", /* \"ntp\": nor this */\n"
        "  \"wifi\": {\"ssid\": \"a,}\\\"b\", \"password\": \"p\"}, \"x\": [1, /* ] */ 2,],\n"
        "  \"cc\": [1, {\"wifi\": 2}], \"ntp\" /* c */ : // d\n true\n"
        "}\n";
    char out[512], t[512];
    uint32_t h = 2166136261u;
    for (size_t cut = 0; cut <= sizeof text - 1; cut++) {
        char *copy = malloc(cut ? cut : 1); /* exact size: ASan sees a read past it */
        memcpy(copy, text, cut);
        size_t n = rdof_jsonc_strip(copy, cut, out);
        CHECK(n <= cut, "strip grows %zu to %zu", cut, n);
        h = fnv(fnv(h, &n, sizeof n), out, n);
        size_t m = rdof_jsonc_add(copy, cut, "k", "1", out, sizeof out);
        h = fnv(fnv(h, &m, sizeof m), out, m);
        free(copy);
        memcpy(t, text, cut);
        t[cut] = 'Z';
        n = rdof_jsonc_drop(t, cut, "wifi");
        n = rdof_jsonc_drop(t, n, "ntp");
        CHECK(n == cut || t[n] == 0, "drop at %zu: %zu", cut, n);
        h = fnv(fnv(h, &n, sizeof n), t, n);
    }
    CHECK(h == JSONC_PREFIX_DIGEST, "jsonc prefixes: digest %08x", h);

    /* strip: a string cut after a backslash, a block comment never closed, a line comment at the end */
    size_t n = rdof_jsonc_strip("[\"a\\", 4, out);
    CHECK(n == 4 && !memcmp(out, "[\"a\\", 4), "a cut string is kept");
    n = rdof_jsonc_strip("[1 /* x", 7, out);
    CHECK(n == 4 && !memcmp(out, "[1  ", 4), "open comment: %.*s", (int)n, out);
    n = rdof_jsonc_strip("[1,\t\r\n] // x", 12, out);
    CHECK(n == 8 && !memcmp(out, "[1 \t\r\n] ", 8), "trailing comma, line comment: %.*s", (int)n, out);
    n = rdof_jsonc_strip("}", 1, out);
    CHECK(n == 1 && out[0] == '}', "a close at the start");
    n = rdof_jsonc_strip("[1 /x /* a * b */]", 18, out);
    CHECK(n == 8 && !memcmp(out, "[1 /x  ]", 8), "a slash alone, a star in a comment: %.*s", (int)n, out);
    /* drop: not an object, every top-level copy, no colon, a line to itself, a value cut short */
    strcpy(t, " [\"a\", 1]");
    CHECK(rdof_jsonc_drop(t, 9, "a") == 9, "not an object");
    strcpy(t, "{\"a\": 1, \"b\": [2, {\"a\": 3}], \"a\": x}");
    rdof_jsonc_drop(t, strlen(t), "a");
    CHECK(!strcmp(t, "{ \"b\": [2, {\"a\": 3}], }"), "every top-level copy: %s", t);
    strcpy(t, "{\"ab\": 1, \"a\" 2}");
    CHECK(rdof_jsonc_drop(t, strlen(t), "a") == 16, "no colon: stops");
    strcpy(t, "{\n\t\"a\": 1 , \n}");
    rdof_jsonc_drop(t, strlen(t), "a");
    CHECK(!strcmp(t, "{\n}"), "its own line, with tabs: %s", t);
    strcpy(t, "{\"a\": {\"x\": \"}\"");
    rdof_jsonc_drop(t, strlen(t), "a");
    CHECK(!strcmp(t, "{"), "a value cut short: %s", t);
    strcpy(t, "{1}");
    CHECK(rdof_jsonc_drop(t, 3, "a") == 3, "a member that isn't a string");
    strcpy(t, "{\"a\": 1 /* * */, \"b\": 2}");
    rdof_jsonc_drop(t, strlen(t), "b");
    CHECK(!strcmp(t, "{\"a\": 1 /* * */, }"), "past a comment with a star: %s", t);
    strcpy(t, "{\"a\": 1 /x, \"b\": 2}");
    CHECK(rdof_jsonc_drop(t, strlen(t), "b") == 19, "a slash that isn't a comment stops it");
    /* add: what it refuses, and the room it needs exactly */
    CHECK(!rdof_jsonc_add("{\"a\": 1", 7, "b", "2", out, sizeof out), "no closing brace");
    CHECK(!rdof_jsonc_add("{\"a\" 1}", 7, "b", "2", out, sizeof out), "no colon");
    CHECK(!rdof_jsonc_add("{1}", 3, "b", "2", out, sizeof out), "a member that isn't a string");
    CHECK(!rdof_jsonc_add("", 0, "b", "2", out, sizeof out), "empty");
    size_t m = rdof_jsonc_add("{\"a\": 1,}", 9, "b", "2", out, sizeof out);
    CHECK(m == 18 && !strcmp(out, "{\"a\": 1,\n  \"b\": 2}"), "after a trailing comma: %s", out);
    CHECK(rdof_jsonc_add("{\"a\": 1,}", 9, "b", "2", out, 19) == 18 &&
              rdof_jsonc_add("{\"a\": 1,}", 9, "b", "2", out, 18) == 0 && out[0] == 0,
          "fits with its NUL, not without");
    CHECK(rdof_jsonc_add("{}", 2, "b", "2", out, 0) == 0, "no room at all");
    /* a NUL in the text is copied (%.*s stopped at it) */
    m = rdof_jsonc_add("{\"a\": \"x\0y\"}", 12, "b", "2", out, sizeof out);
    CHECK(m == 22 && !memcmp(out, "{\"a\": \"x\0y\",\n  \"b\": 2}", 23), "a NUL inside: %zu", m);
}

/* ── the modem client against a scripted port ─────────────────────────────── */

enum {
    P_ACK, P_NAK, P_SILENT, P_WRITE_FAILS, P_UNPLUG_WAITING, P_EMPTY_ACK, P_OTHER_ACK, P_VERSION, P_SHORT,
    P_UNPLUG_WRITING, /* the write unplugs the port and fails */
    P_LATE,           /* the reply comes as wait_reply times out */
    P_BARE_NAK,       /* a NAK with its command only */
};

typedef struct {
    rdof_modem_t *m;
    int mode, writes, waits, sleeps, replies, rx, lost, nak_cmd, nak_err, rssi;
} port_t;

static void feed_frame(port_t *p, uint8_t cmd, const uint8_t *pl, size_t n)
{
    uint8_t f[300];
    size_t len = rdof_mmdvm_frame(cmd, pl, n, f, sizeof f);
    rdof_modem_feed(p->m, f, len);
}

static bool p_write(void *ctx, const uint8_t *d, size_t n)
{
    port_t *p = ctx;
    p->writes++;
    const uint8_t cmd = d[2];
    switch (p->mode) {
    case P_WRITE_FAILS: return false;
    case P_UNPLUG_WRITING: rdof_modem_fail(p->m, "unplugged"); return false;
    case P_BARE_NAK: feed_frame(p, RDOF_MMDVM_NAK, &cmd, 1); break;
    case P_ACK: feed_frame(p, RDOF_MMDVM_ACK, &cmd, 1); break;
    case P_NAK: {
        const uint8_t nak[2] = {cmd, 4};
        feed_frame(p, RDOF_MMDVM_NAK, nak, 2);
        break;
    }
    case P_EMPTY_ACK: feed_frame(p, RDOF_MMDVM_ACK, NULL, 0); break;
    case P_OTHER_ACK: {
        const uint8_t other = (uint8_t)(cmd ^ 0x01);
        feed_frame(p, RDOF_MMDVM_ACK, &other, 1);
        break;
    }
    case P_VERSION: {
        const uint8_t v[] = {1, 'M', 'M', 'D', 'V', 'M', ' ', 'T', 'E', 'S', 'T'};
        feed_frame(p, cmd, v, sizeof v);
        break;
    }
    case P_SHORT: {
        const uint8_t s[2] = {0, 0};
        feed_frame(p, cmd, s, 2);
        break;
    }
    default: break;
    }
    return true;
}

static bool p_wait(void *ctx, int ms)
{
    port_t *p = ctx;
    p->waits++;
    if (p->mode == P_UNPLUG_WAITING)
        rdof_modem_fail(p->m, "unplugged");
    if (p->mode == P_LATE) {
        const uint8_t a = (uint8_t)p->m->waiting_for;
        feed_frame(p, RDOF_MMDVM_ACK, &a, 1);
    }
    return false;
}

static void p_on_reply(void *ctx) { ((port_t *)ctx)->replies++; }
static double p_clock(void *ctx) { return 12.5; }
static void p_sleep(void *ctx, int ms) { ((port_t *)ctx)->sleeps += ms; }

static void p_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    port_t *p = ctx;
    p->rx = slot;
    p->rssi = rssi;
}
static void p_lost(void *ctx, int slot) { ((port_t *)ctx)->lost = slot; }
static void p_nak(void *ctx, uint8_t cmd, int err)
{
    port_t *p = ctx;
    p->nak_cmd = cmd;
    p->nak_err = err;
}

static void modem_on(rdof_modem_t *m, port_t *p, int mode)
{
    memset(p, 0, sizeof *p);
    p->m = m;
    p->mode = mode;
    const rdof_modem_port_t port = {p_write, p_wait, p_on_reply, p_clock, p_sleep, p};
    rdof_modem_init(m, &port);
    m->on_rx = p_rx;
    m->on_lost = p_lost;
    m->on_nak = p_nak;
    m->cb_ctx = p;
}

static void t_modem(void)
{
    rdof_modem_t m;
    port_t p;
    modem_on(&m, &p, P_VERSION);
    CHECK(rdof_modem_start(&m, 3) && m.protocol == 1 && strstr(m.version, "MMDVM TEST") && p.replies == 1,
          "version: %d %s", m.protocol, m.version);
    modem_on(&m, &p, P_SILENT);
    CHECK(!rdof_modem_start(&m, 3) && p.writes == 3 && m.no_reply == 3 && strstr(m.error, "no reply"), "three tries: %s",
          m.error);
    modem_on(&m, &p, P_WRITE_FAILS);
    CHECK(!rdof_modem_start(&m, 3) && p.writes == 1 && !strcmp(m.failed, "port write failed"),
          "a dead port stops the tries: %d %s", p.writes, m.failed);
    CHECK(!rdof_modem_set_mode(&m, RDOF_MODE_DMR) && p.writes == 1 && !strcmp(m.error, "port write failed"),
          "failed: no more writes");
    modem_on(&m, &p, P_UNPLUG_WAITING);
    CHECK(!rdof_modem_set_mode(&m, RDOF_MODE_DMR) && !strcmp(m.error, "unplugged") && p.replies == 1,
          "unplugged while waiting: %s", m.error);
    modem_on(&m, &p, P_NAK);
    CHECK(!rdof_modem_set_mode(&m, RDOF_MODE_DMR) && m.refused == 1 && strstr(m.error, "refused command 0x03 (error 4)"),
          "refused: %s", m.error);
    modem_on(&m, &p, P_EMPTY_ACK);
    CHECK(!rdof_modem_set_mode(&m, RDOF_MODE_DMR) && m.no_reply == 1, "an ACK without its command isn't the reply");
    modem_on(&m, &p, P_OTHER_ACK);
    CHECK(!rdof_modem_set_mode(&m, RDOF_MODE_DMR) && m.no_reply == 1, "an ACK for another command isn't the reply");
    modem_on(&m, &p, P_SHORT);
    rdof_mmdvm_status_t st;
    CHECK(!rdof_modem_status(&m, &st) && !strcmp(m.error, "short status reply"), "short status: %s", m.error);
    modem_on(&m, &p, P_SILENT);
    CHECK(!rdof_modem_status(&m, &st), "status: no reply");
    /* what the modem hears, unasked */
    modem_on(&m, &p, P_SILENT);
    uint8_t d[36] = {0x42};
    feed_frame(&p, RDOF_MMDVM_DMR_DATA1, d, 34);
    CHECK(p.rx == 1 && p.rssi == -1 && m.last_rx == 12.5, "data on slot 1, no RSSI");
    d[34] = 0x01;
    d[35] = 0x23;
    feed_frame(&p, RDOF_MMDVM_DMR_DATA2, d, 36);
    CHECK(p.rx == 2 && p.rssi == 0x123, "data on slot 2 with RSSI %d", p.rssi);
    p.rx = 0;
    feed_frame(&p, RDOF_MMDVM_DMR_DATA1, d, 20);
    CHECK(p.rx == 0, "a short burst isn't passed on");
    feed_frame(&p, RDOF_MMDVM_DMR_LOST2, NULL, 0);
    CHECK(p.lost == 2, "lost on slot 2");
    const uint8_t nak[2] = {RDOF_MMDVM_DMR_DATA1, 5};
    feed_frame(&p, RDOF_MMDVM_NAK, nak, 2);
    CHECK(p.nak_cmd == RDOF_MMDVM_DMR_DATA1 && p.nak_err == 5, "unanswered command NAKed");
    p.nak_cmd = 0;
    feed_frame(&p, RDOF_MMDVM_ACK, nak, 1);
    CHECK(p.nak_cmd == 0, "an ACK of it isn't a NAK");
    m.on_rx = NULL;
    m.on_lost = NULL;
    m.on_nak = NULL;
    feed_frame(&p, RDOF_MMDVM_DMR_DATA1, d, 34);
    feed_frame(&p, RDOF_MMDVM_DMR_LOST1, NULL, 0);
    feed_frame(&p, RDOF_MMDVM_NAK, nak, 2);
    CHECK(1, "no callbacks: nothing breaks");
}

/* What one call wrote: the last frame. */
typedef struct {
    port_t p;
    uint8_t last[300];
    size_t len;
} capture_t;

static bool cap_write(void *ctx, const uint8_t *d, size_t n)
{
    capture_t *c = ctx;
    memcpy(c->last, d, n);
    c->len = n;
    return p_write(&c->p, d, n);
}

static void cap_on(rdof_modem_t *m, capture_t *c, int mode)
{
    modem_on(m, &c->p, mode);
    m->port.write = cap_write;
    m->port.ctx = c;
    c->len = 0;
}

static void t_modem_edges(void)
{
    rdof_modem_t m;
    capture_t c;
    cap_on(&m, &c, P_ACK);
    CHECK(m.protocol == 1 && m.timeout_ms == 1000 && m.waiting_for == -1 && m.last_rx == -INFINITY && !m.failed[0],
          "init");
    /* each command's frame */
    CHECK(rdof_modem_set_mode(&m, RDOF_MODE_DMR) && c.len == 4 && !memcmp(c.last, "\xE0\x04\x03\x02", 4), "SET_MODE");
    CHECK(rdof_modem_set_freq(&m, 461137500, 466137500, 200) && c.len == 17 && c.last[2] == RDOF_MMDVM_SET_FREQ &&
              c.last[3] == 0 && c.last[4] == 0x5C && c.last[12] == 200 && m.requests == 2,
          "SET_FREQ");
    CHECK(rdof_modem_send_cwid(&m, "w1aw/r\xc3\xa9") && c.len == 3 + 6 && !memcmp(c.last + 3, "W1AW/R", 6),
          "CW ID: upper case, ASCII only");
    char cw[60];
    memset(cw, 'a', sizeof cw - 1);
    cw[sizeof cw - 1] = 0;
    CHECK(rdof_modem_send_cwid(&m, cw) && c.len == 3 + 40 && c.last[42] == 'A', "CW ID: at most 40");
    CHECK(rdof_modem_send_cwid(&m, "") && c.len == 3, "CW ID: empty");
    uint8_t burst[RDOF_BURST_BYTES];
    for (int i = 0; i < RDOF_BURST_BYTES; i++)
        burst[i] = (uint8_t)i;
    const int w = c.p.writes;
    CHECK(rdof_modem_write_dmr(&m, 0x41, burst, 1) && c.len == 37 && c.last[2] == RDOF_MMDVM_DMR_DATA1 && c.last[3] == 0x41 &&
              !memcmp(c.last + 4, burst, RDOF_BURST_BYTES),
          "DMR data on slot 1");
    CHECK(rdof_modem_write_dmr(&m, 0x41, burst, 2) && c.last[2] == RDOF_MMDVM_DMR_DATA2, "slot 2");
    CHECK(rdof_modem_write_dmr(&m, 0x41, burst, 0) && c.last[2] == RDOF_MMDVM_DMR_DATA2, "slot 0: not 1, so 2");
    CHECK(rdof_modem_dmr_start(&m, true) && c.len == 4 && c.last[2] == RDOF_MMDVM_DMR_START && c.last[3] == 1, "start on");
    CHECK(rdof_modem_dmr_start(&m, false) && c.last[3] == 0, "start off");
    CHECK(rdof_modem_dmr_abort(&m, 2) && c.last[2] == RDOF_MMDVM_DMR_ABORT && c.last[3] == 2, "abort");
    const uint8_t lc[9] = {9, 8, 7, 6, 5, 4, 3, 2, 1};
    CHECK(rdof_modem_dmr_short_lc(&m, lc) && c.len == 12 && c.last[2] == RDOF_MMDVM_DMR_SHORTLC && !memcmp(c.last + 3, lc, 9),
          "short LC");
    CHECK(c.p.writes == w + 7 && m.requests == 5, "unanswered: no request counted");
    /* config by protocol */
    rdof_modem_config_t cfg;
    rdof_modem_config_default(&cfg);
    CHECK(rdof_modem_configure(&m, &cfg, 7, false) && c.len == 3 + 23 && c.last[3 + 6] == 7, "config v1: %zu", c.len);
    m.protocol = 2;
    CHECK(rdof_modem_configure(&m, &cfg, 7, true) && c.len == 3 + 37 && c.last[3 + 26] == 7, "config v2: %zu", c.len);
    /* a protocol 2 version reply */
    cap_on(&m, &c, P_SILENT);
    m.port.wait_reply = NULL;
    CHECK(!rdof_modem_start(&m, 2) && c.p.writes == 2 && m.no_reply == 2 && !strcmp(m.error, "no reply to command 0x00"),
          "no wait_reply: no reply, %s", m.error);
    uint8_t v2[30] = {2};
    memcpy(v2 + 20, " MMDVM_HS ", 10);
    m.waiting_for = RDOF_MMDVM_GET_VERSION;
    feed_frame(&c.p, RDOF_MMDVM_GET_VERSION, v2, sizeof v2);
    CHECK(m.reply_ready && m.reply_len == 30 && m.reply_ok, "a reply while waiting");
    m.waiting_for = -1;
    /* status */
    cap_on(&m, &c, P_SILENT);
    m.waiting_for = RDOF_MMDVM_GET_STATUS;
    const uint8_t st[6] = {0, 0, 0x41, 0, 3, 9};
    feed_frame(&c.p, RDOF_MMDVM_GET_STATUS, st, 6);
    rdof_mmdvm_status_t s;
    /* reply_ready stays set: request() clears it first, so this one is gone */
    CHECK(!rdof_modem_status(&m, &s) && m.no_reply == 1, "a reply before the request is dropped");
    /* NAKs */
    cap_on(&m, &c, P_SILENT);
    m.waiting_for = RDOF_MMDVM_SET_MODE;
    const uint8_t nak1 = RDOF_MMDVM_SET_MODE;
    feed_frame(&c.p, RDOF_MMDVM_NAK, &nak1, 1);
    CHECK(m.reply_ready && !m.reply_ok && m.reply_len == 0, "a NAK without its error");
    m.waiting_for = -1;
    const uint8_t dnak = RDOF_MMDVM_DMR_START;
    feed_frame(&c.p, RDOF_MMDVM_NAK, &dnak, 1);
    CHECK(c.p.nak_cmd == RDOF_MMDVM_DMR_START && c.p.nak_err == 0, "an unanswered NAK without its error: 0");
    c.p.nak_cmd = 0;
    feed_frame(&c.p, RDOF_MMDVM_NAK, NULL, 0);
    CHECK(c.p.nak_cmd == 0, "an empty NAK");
    /* fail */
    cap_on(&m, &c, P_ACK);
    rdof_modem_fail(&m, NULL);
    CHECK(!m.failed[0] && c.p.replies == 0, "fail(NULL): empty, and nobody waiting: no reply");
    char why[100];
    memset(why, 'x', sizeof why - 1);
    why[sizeof why - 1] = 0;
    rdof_modem_fail(&m, why);
    CHECK(strlen(m.failed) == 63 && !rdof_modem_set_mode(&m, 2) && strlen(m.error) == 63 && c.p.writes == 0,
          "a long reason, cut");
    CHECK(!rdof_modem_write_dmr(&m, 0, burst, 1) && !rdof_modem_dmr_start(&m, true) && c.p.writes == 0, "failed: no writes");
    /* no write function: the port failed */
    cap_on(&m, &c, P_ACK);
    m.port.write = NULL;
    CHECK(!rdof_modem_set_mode(&m, 2) && !strcmp(m.failed, "port write failed") && m.waiting_for == -1, "no write");
    /* no clock */
    cap_on(&m, &c, P_SILENT);
    m.port.clock = NULL;
    feed_frame(&c.p, RDOF_MMDVM_DMR_LOST1, NULL, 0);
    CHECK(m.last_rx == 0 && c.p.lost == 1, "no clock: 0");
    /* on_reply optional */
    cap_on(&m, &c, P_ACK);
    m.port.on_reply = NULL;
    CHECK(rdof_modem_set_mode(&m, 2), "no on_reply");
    m.waiting_for = 5;
    rdof_modem_fail(&m, "gone");
    CHECK(m.reply_ready && !m.reply_ok, "fail while waiting, no on_reply");
    /* the request's ends */
    cap_on(&m, &c, P_ACK);
    CHECK(rdof_modem_set_mode(&m, 2) && c.p.waits == 0 && m.waiting_for == -1, "replied during the write: no wait");
    cap_on(&m, &c, P_LATE);
    CHECK(rdof_modem_set_mode(&m, 2) && c.p.waits == 1 && m.no_reply == 0, "a reply as the wait times out counts");
    cap_on(&m, &c, P_SILENT);
    CHECK(!rdof_modem_set_mode(&m, 2) && m.waiting_for == -1, "no reply: nobody waiting after");
    cap_on(&m, &c, P_UNPLUG_WAITING);
    CHECK(!rdof_modem_start(&m, 3) && c.p.writes == 1 && m.requests == 1 && c.p.replies == 1 &&
              !strcmp(m.failed, "unplugged") && m.waiting_for == -1,
          "unplugged while waiting for the version: no more tries");
    cap_on(&m, &c, P_UNPLUG_WRITING);
    CHECK(!rdof_modem_set_mode(&m, 2) && !strcmp(m.failed, "unplugged") && !strcmp(m.error, "unplugged") &&
              m.waiting_for == -1,
          "the port's own reason kept: %s", m.failed);
    cap_on(&m, &c, P_NAK);
    CHECK(!rdof_modem_set_mode(&m, 2), "refused, error 4");
    c.p.mode = P_BARE_NAK;
    CHECK(!rdof_modem_set_mode(&m, 2) && !strcmp(m.error, "modem refused command 0x03 (error -1)") && m.refused == 2,
          "a NAK without its error: -1, %s", m.error);
    /* a DMR frame one byte short of control + burst: not passed on */
    cap_on(&m, &c, P_SILENT);
    uint8_t d33[RDOF_BURST_BYTES] = {0};
    feed_frame(&c.p, RDOF_MMDVM_DMR_DATA2, d33, RDOF_BURST_BYTES);
    CHECK(c.p.rx == 0 && m.last_rx == 12.5, "33 bytes: heard, not passed on");
    uint8_t d34[RDOF_BURST_BYTES + 1] = {0};
    feed_frame(&c.p, RDOF_MMDVM_DMR_DATA2, d34, sizeof d34);
    CHECK(c.p.rx == 2, "34 bytes: slot 2");
}

static void t_radio_edges(void)
{
    rdof_modem_t m;
    port_t p;
    rdof_radio_t r;
    static const struct {
        rdof_radio_kind_t kind;
        rdof_tuner_kind_t tuner;
        bool hotspot;
        int headers, terminators;
    } kinds[] = {
        {RDOF_RADIO_UNKNOWN, RDOF_TUNER_MODEM, false, 8, 3}, {RDOF_RADIO_HOTSPOT, RDOF_TUNER_MODEM, true, 1, 1},
        {RDOF_RADIO_MMDVM_HS, RDOF_TUNER_MODEM, false, 8, 3}, {RDOF_RADIO_MMDVM, RDOF_TUNER_FIXED, false, 8, 3},
        {RDOF_RADIO_SPLIT, RDOF_TUNER_MODEM, true, 8, 3},
    };
    modem_on(&m, &p, P_ACK);
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        memset(&r, 0x5A, sizeof r);
        rdof_radio_init(&r, &m, kinds[i].kind);
        CHECK(r.modem == &m && r.kind == kinds[i].kind && r.tuner == kinds[i].tuner &&
                  r.hotspot_rssi == kinds[i].hotspot && r.headers == kinds[i].headers &&
                  r.terminators == kinds[i].terminators && r.rssi_map.n == 0 && r.settle_ms == 0 && r.fixed_hz == 0 &&
                  r.config.tx_delay_ms == 100,
              "kind %d", kinds[i].kind);
    }
    /* RSSI maps: ties to even, the int ends, n outside the arrays */
    rdof_rssi_map_t half = {2, {0, 2}, {0, 1}};
    CHECK(rdof_rssi_map_dbm(&half, 1) == 0, "0.5: to even, 0");
    half.dbm[0] = 1;
    half.dbm[1] = 2;
    CHECK(rdof_rssi_map_dbm(&half, 1) == 2, "1.5: to even, 2");
    half.dbm[0] = -1;
    half.dbm[1] = -2;
    CHECK(rdof_rssi_map_dbm(&half, 1) == -2, "-1.5: to even, -2");
    rdof_rssi_map_t ends = {2, {INT_MIN, INT_MAX}, {INT_MIN, INT_MAX}};
    CHECK(rdof_rssi_map_dbm(&ends, 0) == 0 && rdof_rssi_map_dbm(&ends, INT_MAX) == INT_MAX &&
              rdof_rssi_map_dbm(&ends, INT_MIN) == INT_MIN && rdof_rssi_map_dbm(&ends, INT_MIN + 1) == INT_MIN + 1,
          "the int ends: %d", rdof_rssi_map_dbm(&ends, 0));
    rdof_rssi_map_t one = {1, {40}, {-77}};
    CHECK(rdof_rssi_map_dbm(&one, 0) == -77 && rdof_rssi_map_dbm(&one, 40) == -77 && rdof_rssi_map_dbm(&one, 99) == -77,
          "one point");
    rdof_rssi_map_t big;
    big.n = 1000;
    for (int i = 0; i < RDOF_RSSI_POINTS; i++) {
        big.raw[i] = 100 - i;
        big.dbm[i] = -i;
    }
    rdof_rssi_map_sort(&big);
    CHECK(big.raw[0] == 85 && big.raw[15] == 100 && big.dbm[0] == -15, "n past the arrays: 16 sorted");
    CHECK(rdof_rssi_map_dbm(&big, 1000) == 0 && rdof_rssi_map_dbm(&big, 0) == -15, "n past the arrays: 16 read");
    for (int i = 0; i < RDOF_RSSI_POINTS; i++)
        big.raw[i] = 100 - i;
    big.n = -3;
    rdof_rssi_map_sort(&big);
    CHECK(big.raw[0] == 100 && big.raw[15] == 85, "n < 0: nothing");
    rdof_rssi_map_t same = {4, {5, 5, 5, 1}, {3, -1, 2, 9}};
    rdof_rssi_map_sort(&same);
    CHECK(same.raw[0] == 1 && same.dbm[1] == -1 && same.dbm[2] == 2 && same.dbm[3] == 3, "sorted by raw, then dBm");
    /* the modem tuner's offsets */
    capture_t c;
    cap_on(&m, &c, P_ACK);
    rdof_radio_init(&r, &m, RDOF_RADIO_HOTSPOT);
    r.rx_offset_hz = -500;
    r.tx_offset_hz = 250;
    CHECK(rdof_radio_tune_duplex(&r, 461000000, 466000000, 3, 9) && c.last[2] == RDOF_MMDVM_SET_MODE, "tuned");
    /* SET_FREQ was the first of the three writes; check it alone */
    cap_on(&m, &c, P_ACK);
    r.modem = &m;
    rdof_modem_set_freq(&m, 461000000 - 500, 466000000 + 250, 9);
    uint8_t want[17];
    memcpy(want, c.last, 17);
    cap_on(&m, &c, P_SILENT);
    m.timeout_ms = 0;
    CHECK(!rdof_radio_tune_duplex(&r, 461000000, 466000000, 3, 9) && c.len == 17 && !memcmp(c.last, want, 17),
          "the offsets added");
    r.rx_offset_hz = -1000;
    CHECK(!rdof_radio_tune(&r, 500, 3, 9) && c.last[4] == 0x0C && c.last[7] == 0xFF, "below 0: wraps (%02x %02x)",
          c.last[4], c.last[7]);
    /* the fixed tuner's window */
    modem_on(&m, &p, P_ACK);
    rdof_radio_init(&r, &m, RDOF_RADIO_MMDVM);
    r.fixed_hz = 461000000;
    CHECK(rdof_radio_tune(&r, 461000100, 1, 0) && rdof_radio_tune(&r, 460999900, 1, 0), "100 Hz either way");
    CHECK(!rdof_radio_tune(&r, 461000101, 1, 0) && !strcmp(m.error, "this radio is fixed on 461.00000 MHz"), "101: %s",
          m.error);
    CHECK(!rdof_radio_tune(&r, 460999899, 1, 0), "-101");
    r.fixed_hz = 4294967295u;
    CHECK(!rdof_radio_tune(&r, 0, 1, 0) && !strcmp(m.error, "this radio is fixed on 4294.96730 MHz"), "the u32 end: %s",
          m.error);
    CHECK(rdof_radio_tune(&r, 4294967295u, 1, 0), "at the u32 end");
    r.tuner = 7;
    CHECK(!rdof_radio_tune(&r, 0, 1, 0) && !strcmp(m.error, "no tuner"), "an unknown tuner: none");
    modem_on(&m, &p, P_ACK);
    rdof_radio_init(&r, &m, RDOF_RADIO_MMDVM_HS);
    m.protocol = 2;
    const int w = p.writes;
    CHECK(rdof_radio_tune(&r, 461000000, 1, 0) && p.writes == w + 3 && p.sleeps == 0, "set_freq, config, mode");
    modem_on(&m, &p, P_NAK);
    rdof_radio_init(&r, &m, RDOF_RADIO_MMDVM);
    r.fixed_hz = 461000000;
    CHECK(!rdof_radio_tune(&r, 461000000, 1, 0) && p.writes == 1 && strstr(m.error, "0x02"), "a refused config: %s",
          m.error);
}

static void t_radio(void)
{
    rdof_modem_t m;
    port_t p;
    rdof_radio_t r;
    /* RSSI maps (interpolated, clamped at the ends, rounded half to even) */
    rdof_rssi_map_t map = {3, {100, 10, 50}, {-60, -120, -90}};
    rdof_rssi_map_sort(&map);
    CHECK(map.raw[0] == 10 && map.raw[2] == 100 && map.dbm[0] == -120, "sorted");
    CHECK(rdof_rssi_map_dbm(&map, 5) == -120 && rdof_rssi_map_dbm(&map, 10) == -120, "below: the first");
    CHECK(rdof_rssi_map_dbm(&map, 30) == -105 && rdof_rssi_map_dbm(&map, 50) == -90, "between");
    CHECK(rdof_rssi_map_dbm(&map, 75) == -75 && rdof_rssi_map_dbm(&map, 500) == -60, "above: the last");
    rdof_rssi_map_t dup = {2, {20, 20}, {-80, -100}};
    rdof_rssi_map_sort(&dup);
    CHECK(dup.dbm[0] == -100 && rdof_rssi_map_dbm(&dup, 20) == -100 && rdof_rssi_map_dbm(&dup, 21) == -80,
          "equal raws: sorted by dBm, no division by zero");
    modem_on(&m, &p, P_ACK);
    rdof_radio_init(&r, &m, RDOF_RADIO_MMDVM);
    int dbm;
    CHECK(!rdof_radio_rssi(&r, -1, 461e6, &dbm), "no RSSI reported");
    r.rssi_map = map;
    CHECK(rdof_radio_rssi(&r, 30, 461e6, &dbm) && dbm == -105, "through the map");
    CHECK(!rdof_radio_rssi(&r, -1, 461e6, &dbm) && rdof_radio_rssi(&r, 0, 461e6, &dbm), "a map: -1 still none, 0 read");
    r.rssi_map.n = 0;
    r.hotspot_rssi = true;
    CHECK(rdof_radio_rssi(&r, 300, 461e6, &dbm) && dbm == rdof_radio_hotspot_dbm(300, 461e6), "a hotspot's conversion");
    r.hotspot_rssi = false;
    CHECK(!rdof_radio_rssi(&r, 300, 461e6, &dbm), "no conversion");
    /* tuners */
    r.tuner = RDOF_TUNER_NONE;
    CHECK(!rdof_radio_tune(&r, 461e6, 1, 50) && !strcmp(m.error, "no tuner"), "no tuner: %s", m.error);
    r.tuner = RDOF_TUNER_FIXED;
    r.fixed_hz = 461000000;
    CHECK(!rdof_radio_tune(&r, 461001000, 1, 50) && strstr(m.error, "fixed on 461.00000"), "off its channel: %s",
          m.error);
    r.settle_ms = 25;
    const int w = p.writes;
    CHECK(rdof_radio_tune(&r, 461000050, 1, 50) && p.sleeps == 25 && p.writes == w + 2,
          "on it (within 100 Hz): settle, configure, mode");
    CHECK(rdof_radio_tune_duplex(&r, 461000000, 461000000, 1, 50) && p.sleeps == 50, "duplex: settles too");
    CHECK(!rdof_radio_tune_duplex(&r, 466000000, 461500000, 1, 50), "duplex off its channel");
    r.settle_ms = 1;
    CHECK(rdof_radio_tune(&r, 461000000, 1, 50) && p.sleeps == 51, "1 ms settles too");
    r.tuner = RDOF_TUNER_MODEM;
    r.settle_ms = 0;
    CHECK(rdof_radio_tune_duplex(&r, 466000000, 461000000, 1, 50) && p.sleeps == 51, "modem tuner, no settle");
    m.port.sleep_ms = NULL;
    r.settle_ms = 10;
    CHECK(rdof_radio_tune(&r, 461000000, 1, 50), "a settle time with no sleep: skipped");
    modem_on(&m, &p, P_NAK);
    CHECK(!rdof_radio_tune(&r, 461000000, 1, 50) && p.writes == 1, "a refused SET_FREQ stops the tune");
    t_modem_edges();
    t_radio_edges();
}

/* ── bench config ─────────────────────────────────────────────────────────── */

static int radios(const char *list, char *err)
{
    char json[600];
    rdof_bench_radio_t r[RDOF_BENCH_MAX_RADIOS];
    snprintf(json, sizeof json, "{\"bench\": {\"radios\": %s}}", list);
    return rdof_bench_radios_parse(json, strlen(json), r, 2, err, 160);
}

/* Each rejected radio list's whole error text (the Rust's must be the C's, byte for byte). */
static void t_benchcfg_edges(void)
{
    char err[160];
    static const struct {
        const char *list, *error;
    } bad[] = {
        {"[{\"port\": {\"uart\": 1}}]", "bench.radios[0]: a name (up to 23 characters)"},
        {"[{\"name\": \"\", \"port\": {\"uart\": 1}}]", "bench.radios[0]: a name (up to 23 characters)"},
        {"[{\"name\": \"abcdefghijklmnopqrstuvwx\"}]", "bench.radios[0]: a name (up to 23 characters)"},
        {"[{\"name\": 7}]", "bench.radios[0]: a name (up to 23 characters)"},
        {"[{\"name\": \"a b\"}]", "bench radio a b: a name of letters, digits, - and _"},
        {"[{\"name\": \"a.b\"}]", "bench radio a.b: a name of letters, digits, - and _"},
        {"[{\"name\": \"a\", \"proto\": \"dmr\"}]", "bench radio a: proto dmr: odr-debug or mmdvm"},
        {"[{\"name\": \"a\", \"proto\": \"odr-debug-long\"}]", "bench radio a: proto odr-debug or mmdvm"},
        {"[{\"name\": \"a\", \"port\": {}}]", "bench radio a: the port is a uart or usb, one of them"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"usb\": \"uart\"}}]",
         "bench radio a: the port is a uart or usb, one of them"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"baud\": 6000001}}]",
         "bench radio a: baud 1200 to 6000000"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"baud\": 1199}}]",
         "bench radio a: baud 1200 to 6000000"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"baud\": 115200.5}}]",
         "bench radio a: baud 1200 to 6000000"},
        /* 2^32 + 115200: once (long) and the range check passed it where long is 32 bits */
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"baud\": 4295082496}}]",
         "bench radio a: baud 1200 to 6000000"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 0, \"tx\": 1, \"rx\": 2}}]",
         "bench radio a: uart 1 or 2 (UART0 is the console)"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 3, \"tx\": 1, \"rx\": 2}}]",
         "bench radio a: uart 1 or 2 (UART0 is the console)"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": \"1\", \"tx\": 1, \"rx\": 2}}]", NULL}, /* a string number: fine */
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"rx\": 2}}]", "bench radio a: tx, a GPIO 0 to 48"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 49}}]", "bench radio a: rx, a GPIO 0 to 48"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"rts\": -2}}]",
         "bench radio a: rts, a GPIO 0 to 48 (or none)"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"cts\": 49}}]",
         "bench radio a: cts, a GPIO 0 to 48 (or none)"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 3, \"rx\": 3}}]", "bench radio a: tx and rx on the same GPIO"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"rts\": 2}}]",
         "bench radio a: rx and rts on the same GPIO"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"rts\": 5, \"cts\": 5}}]",
         "bench radio a: rts and cts on the same GPIO"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"cts\": 1}}]",
         "bench radio a: tx and cts on the same GPIO"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"rts\": 0, \"cts\": 0}}]",
         "bench radio a: rts and cts on the same GPIO"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 0, \"rx\": 0}}]", "bench radio a: tx and rx on the same GPIO"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"rts\": -1, \"cts\": -1}}]", NULL},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"cdc\"}}]", "bench radio a: usb cdc-acm or uart"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"cdc-acm-long\"}}]", "bench radio a: usb cdc-acm or uart"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": -1}}]", "bench radio a: vid, four hex digits"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": 65536}}]", "bench radio a: vid, four hex digits"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"\"}}]", "bench radio a: vid, four hex digits"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"0403\", \"pid\": \"10000\"}}]",
         "bench radio a: pid, four hex digits"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"0403\", \"pid\": -1}}]",
         "bench radio a: pid, four hex digits"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"pid\": \"6001\"}}]", "bench radio a: a pid needs its vid"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"cdc-acm\", \"vid\": \"0483\"}}]",
         "bench radio a: a cdc-acm radio needs its vid and pid"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"cdc-acm\"}}]", "bench radio a: a cdc-acm radio needs its vid and pid"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"0bda\"}}]",
         "bench radio a: 0BDA is the RTL-SDR's vendor"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"10c4\", \"pid\": \"ea61\"}}]",
         "bench radio a: 10C4:EA61 isn't a USB-serial bridge this firmware knows (FTDI, CP210x, CH34x)"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"10c4\"}}]", NULL}, /* any CP210x */
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"cdc-acm\", \"vid\": \"0483\", \"pid\": \"ea61\"}}]", NULL},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"serial\": \"0123456789012345678901234567890\"}}]", NULL},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"serial\": \"01234567890123456789012345678901\"}}]",
         "bench radio a: serial and usb_port are strings"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\"}}, {\"name\": \"a\", \"port\": {\"usb\": \"uart\"}}]",
         "bench radios: two named a"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 2, \"tx\": 1, \"rx\": 2}}, "
         "{\"name\": \"b\", \"port\": {\"uart\": 2, \"tx\": 3, \"rx\": 4}}]",
         "bench radios a and b: both on UART2"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2}}, "
         "{\"name\": \"b\", \"port\": {\"uart\": 2, \"tx\": 3, \"rx\": 4}}]",
         NULL},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2}}, {\"name\": \"b\", \"port\": 3}]",
         "bench radio b: a port {\"uart\": ...} or {\"usb\": ...}"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2}}, 4]", "bench.radios[1]: an object"},
        {"[{}, {}, {}]", "bench.radios: at most 2"},
        {"\"x\"", "bench.radios: a list"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int n = radios(bad[i].list, err);
        if (bad[i].error)
            CHECK(n < 0 && !strcmp(err, bad[i].error), "%s: want \"%s\", got \"%s\"", bad[i].list, bad[i].error, err);
        else
            CHECK(n > 0 && !err[0], "%s: fine, got \"%s\"", bad[i].list, err);
    }
    /* each field, and the defaults */
    rdof_bench_radio_t r[RDOF_BENCH_MAX_RADIOS];
    memset(r, 0x5A, sizeof r);
    const char *two = "// a comment\n{\"bench\": {\"radios\": [{\"name\": \"odr-1_A\", \"port\": {\"uart\": 2, \"tx\": 17, "
                      "\"rx\": 18, \"cts\": 0}}, {\"name\": \"b\", \"proto\": \"mmdvm\", \"port\": {\"usb\": \"uart\", "
                      "\"vid\": \"1A86\", \"pid\": \"0x7523\", \"baud\": \"921600\", \"serial\": \"A502\", \"usb_port\": \"1.3\"}}]}}";
    CHECK(rdof_bench_radios_parse(two, strlen(two), r, RDOF_BENCH_MAX_RADIOS, err, sizeof err) == 2, "two radios: %s", err);
    CHECK(!strcmp(r[0].name, "odr-1_A") && !strcmp(r[0].proto, "odr-debug") && r[0].port == RDOF_BPORT_UART &&
              r[0].uart == 2 && r[0].tx == 17 && r[0].rx == 18 && r[0].rts == -1 && r[0].cts == 0 && r[0].baud == 115200 &&
              r[0].vid == 0 && r[0].pid == 0 && !r[0].serial[0] && !r[0].usb_port[0],
          "a UART radio's fields");
    CHECK(!strcmp(r[1].proto, "mmdvm") && r[1].port == RDOF_BPORT_USB_UART && r[1].vid == 0x1A86 && r[1].pid == 0x7523 &&
              r[1].baud == 921600 && !strcmp(r[1].serial, "A502") && !strcmp(r[1].usb_port, "1.3") && r[1].rts == -1 &&
              r[1].tx == 0,
          "a USB radio's fields");
    CHECK(r[2].port == 0x5A5A5A5A, "past the count: untouched");
    const char *none[] = {"{}", "{\"bench\": 5}", "{\"bench\": {}}", "{\"bench\": {\"radios\": null}}",
                          "{\"bench\": {\"radios\": []}}", "[1, 2]"};
    for (size_t i = 0; i < sizeof none / sizeof none[0]; i++) {
        strcpy(err, "x");
        CHECK(rdof_bench_radios_parse(none[i], strlen(none[i]), r, 2, err, sizeof err) == 0 && !err[0], "no radios: %s",
              none[i]);
    }
    /* the error cut to the caller's buffer */
    char small[12];
    CHECK(radios("[1]", err) < 0, "setup");
    const char *one = "{\"bench\": {\"radios\": [1]}}";
    CHECK(rdof_bench_radios_parse(one, strlen(one), r, 2, small, sizeof small) < 0 && !strcmp(small, "bench.radio"),
          "cut: %s", small);
    CHECK(rdof_bench_radios_parse(one, strlen(one), r, 0, err, sizeof err) < 0 && !strcmp(err, "bench.radios: at most 0"),
          "max 0: %s", err);
    /* the USB-serial bridges */
    static const struct {
        uint16_t vid, pid;
        char chip;
    } usb[] = {
        {0x0403, 0x6001, 'f'}, {0x0403, 0, 'f'},      {0x0403, 0xFFFF, 'f'}, {0x10C4, 0xEA60, 'c'}, {0x10C4, 0xEA70, 'c'},
        {0x10C4, 0xEA71, 'c'}, {0x10C4, 0xEA61, 0},   {0x10C4, 0, 0},        {0x1A86, 0x7523, 'h'}, {0x1A86, 0x7522, 'h'},
        {0x1A86, 0x5523, 'h'}, {0x1A86, 0x55D4, 0},   {0x0483, 0x5740, 0},   {0, 0, 0},             {0x0404, 0x6001, 0},
        {0xEA60, 0x10C4, 0},
    };
    for (size_t i = 0; i < sizeof usb / sizeof usb[0]; i++)
        CHECK(rdof_bench_usb_bridge(usb[i].vid, usb[i].pid) == usb[i].chip, "%04X:%04X", usb[i].vid, usb[i].pid);
    /* bench.tx */
    rdof_benchtx_policy_t pol;
    static const struct {
        const char *tx, *error;
    } badtx[] = {
        {"5", "bench.tx: an object"},
        {"{\"freqs_hz\": [29999999]}", "bench.tx.freqs_hz[0]: a frequency in whole Hz"},
        {"{\"freqs_hz\": [461137500, 3000000001]}", "bench.tx.freqs_hz[1]: a frequency in whole Hz"},
        {"{\"freqs_hz\": [\"461137500\"]}", "bench.tx.freqs_hz[0]: a frequency in whole Hz"},
        {"{\"freqs_hz\": [461137500.5]}", "bench.tx.freqs_hz[0]: a frequency in whole Hz"},
        {"{\"freqs_hz\": {}}", "bench.tx.freqs_hz: a list of frequencies in Hz"},
        {"{\"max_key_s\": -0.5}", "bench.tx.max_key_s: 0 to 60 seconds"},
        {"{\"max_key_s\": 60.001}", "bench.tx.max_key_s: 0 to 60 seconds"},
        {"{\"rf_level\": -1}", "bench.tx.rf_level: 0 to 255 (0: the lowest)"},
        {"{\"rf_level\": 256}", "bench.tx.rf_level: 0 to 255 (0: the lowest)"},
        {"{\"arm_gpio\": -2}", "bench.tx.arm_gpio: a GPIO 0 to 48 (a jumper to ground), or none"},
        {"{\"arm_gpio\": 1e12}", "bench.tx.arm_gpio: a GPIO 0 to 48 (a jumper to ground), or none"},
        /* a good list, then a bad field: the frequencies don't stay */
        {"{\"freqs_hz\": [461137500], \"rf_level\": 999}", "bench.tx.rf_level: 0 to 255 (0: the lowest)"},
    };
    for (size_t i = 0; i < sizeof badtx / sizeof badtx[0]; i++) {
        char json[300];
        snprintf(json, sizeof json, "{\"bench\": {\"tx\": %s}}", badtx[i].tx);
        memset(&pol, 0x5A, sizeof pol);
        CHECK(rdof_bench_tx_parse(json, strlen(json), &pol, err, sizeof err) < 0 && !strcmp(err, badtx[i].error),
              "%s: want \"%s\", got \"%s\"", badtx[i].tx, badtx[i].error, err);
        CHECK(pol.nfreqs == 0 && pol.max_key_s == 0 && pol.rf_level == 0 && pol.arm_gpio == -1, "%s: no TX", badtx[i].tx);
    }
    const char *full = "{\"bench\": {\"tx\": {\"freqs_hz\": [30000000, 3000000000, 461137500, 461137500, 1, 2, 3, 4], "
                       "\"max_key_s\": 60, \"rf_level\": 255, \"arm_gpio\": 48}}}";
    CHECK(rdof_bench_tx_parse(full, strlen(full), &pol, err, sizeof err) < 0 &&
              !strcmp(err, "bench.tx.freqs_hz[4]: a frequency in whole Hz"),
          "eight, the fifth bad: %s", err);
    const char *ends = "{\"bench\": {\"tx\": {\"freqs_hz\": [30000000, 3000000000, 461137500, 4.611e8, 461137500, "
                       "461137500, 461137500, 461137500], \"max_key_s\": 60, \"rf_level\": 255, \"arm_gpio\": 48}}}";
    CHECK(rdof_bench_tx_parse(ends, strlen(ends), &pol, err, sizeof err) == 0 && pol.nfreqs == 8 &&
              pol.freqs_hz[0] == 30000000 && pol.freqs_hz[1] == 3000000000u && pol.freqs_hz[3] == 461100000 &&
              pol.max_key_s == 60 && pol.rf_level == 255 && pol.arm_gpio == 48 && !err[0],
          "at the ends: %s", err);
    const char *nulls = "{\"bench\": {\"tx\": {\"freqs_hz\": null, \"max_key_s\": null, \"rf_level\": null}}}";
    CHECK(rdof_bench_tx_parse(nulls, strlen(nulls), &pol, err, sizeof err) == 0 && pol.nfreqs == 0 && pol.max_key_s == 10 &&
              pol.rf_level == 0 && pol.arm_gpio == -1,
          "nulls are the defaults: %s", err);
    const char *zero = "{\"bench\": {\"tx\": {\"freqs_hz\": [], \"max_key_s\": 0, \"arm_gpio\": 0}}}";
    CHECK(rdof_bench_tx_parse(zero, strlen(zero), &pol, err, sizeof err) == 0 && pol.nfreqs == 0 && pol.max_key_s == 0 &&
              pol.arm_gpio == 0,
          "zeros: %s", err);
    const char *txnull = "{\"bench\": {\"tx\": null}}";
    memset(&pol, 0x5A, sizeof pol);
    CHECK(rdof_bench_tx_parse(txnull, strlen(txnull), &pol, err, sizeof err) == 0 && pol.nfreqs == 0 && pol.max_key_s == 0 &&
              pol.arm_gpio == -1,
          "tx null: none");
}

static void t_benchcfg(void)
{
    char err[160];
    static const struct {
        const char *list, *why;
    } bad[] = {
        {"[1]", "an object"},
        {"[{\"name\": \"a\", \"proto\": 5, \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2}}]", "proto odr-debug or mmdvm"},
        {"[{\"name\": \"a\", \"port\": []}]", "a port"},
        {"[{\"name\": \"a\"}]", "a port"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"baud\": 300}}]", "baud 1200 to 6000000"},
        {"[{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2, \"baud\": \"fast\"}}]", "baud"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"xyz1\"}}]", "vid, four hex digits"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"10000\"}}]", "vid, four hex digits"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"0403\", \"pid\": true}}]", "pid, four hex digits"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"serial\": 5}}]", "serial and usb_port are strings"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"usb_port\": \"1.2.3.4.5.6.7.8.9.10\"}}]", "strings"},
        {"[{\"name\": \"a\", \"port\": {\"usb\": 3}}]", "usb cdc-acm or uart"},
        {"{\"a\": 1}", "a list"},
        {"[{}, {}, {}]", "at most 2"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(radios(bad[i].list, err) < 0 && strstr(err, bad[i].why), "%s: want \"%s\", got \"%s\"", bad[i].list,
              bad[i].why, err);
    const char *broken = "{\"bench\": ";
    CHECK(rdof_bench_radios_parse(broken, strlen(broken), NULL, 2, err, sizeof err) < 0 && strstr(err, "JSON"),
          "bad JSON: %s", err);
    CHECK(rdof_bench_radios_parse(broken, strlen(broken), NULL, 2, NULL, 0) < 0, "no error buffer");
    /* numbers as well as hex strings */
    rdof_bench_radio_t r[2];
    const char *ok = "{\"bench\": {\"radios\": [{\"name\": \"hs\", \"proto\": \"mmdvm\", \"port\": {\"usb\": \"cdc-acm\", "
                     "\"vid\": 1155, \"pid\": \"5740\", \"serial\": null}}]}}";
    CHECK(rdof_bench_radios_parse(ok, strlen(ok), r, 2, err, sizeof err) == 1 && r[0].vid == 0x0483 && r[0].pid == 0x5740,
          "vid as a number: %s", err);
    /* bench.tx */
    rdof_benchtx_policy_t pol;
    static const struct {
        const char *tx, *why;
    } badtx[] = {
        {"[]", "bench.tx: an object"},
        {"{\"freqs_hz\": 461137500}", "a list of frequencies"},
        {"{\"freqs_hz\": [1, 2, 3, 4, 5, 6, 7, 8, 9]}", "freqs_hz[0]"},
        {"{\"freqs_hz\": [461e6, 461e6, 461e6, 461e6, 461e6, 461e6, 461e6, 461e6, 461e6]}", "at most 8"},
        {"{\"rf_level\": 300}", "rf_level: 0 to 255"},
        {"{\"rf_level\": \"x\"}", "rf_level"},
        {"{\"arm_gpio\": 49}", "arm_gpio"},
        {"{\"max_key_s\": \"5\"}", "max_key_s"},
    };
    for (size_t i = 0; i < sizeof badtx / sizeof badtx[0]; i++) {
        char json[300];
        snprintf(json, sizeof json, "{\"bench\": {\"tx\": %s}}", badtx[i].tx);
        CHECK(rdof_bench_tx_parse(json, strlen(json), &pol, err, sizeof err) < 0 && strstr(err, badtx[i].why),
              "%s: want \"%s\", got \"%s\"", badtx[i].tx, badtx[i].why, err);
    }
    CHECK(rdof_bench_tx_parse(broken, strlen(broken), &pol, err, sizeof err) < 0 && strstr(err, "JSON"), "tx: bad JSON");
    CHECK(rdof_bench_tx_parse(broken, strlen(broken), &pol, NULL, 0) < 0, "tx: no error buffer");
    const char *lvl = "{\"bench\": {\"tx\": {\"freqs_hz\": [461137500], \"rf_level\": \"12\", \"arm_gpio\": null}}}";
    CHECK(rdof_bench_tx_parse(lvl, strlen(lvl), &pol, err, sizeof err) == 0 && pol.rf_level == 12 && pol.arm_gpio == -1,
          "rf_level as a string: %s", err);
    t_benchcfg_edges();
}

/* ── OLED and HBlink helpers ──────────────────────────────────────────────── */

static uint32_t frame_hash(const rdof_frame_buf_t *f)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (size_t i = 0; i < sizeof f->buf; i++)
        h = (h ^ f->buf[i]) * 16777619u;
    return h;
}

static void t_oled(void)
{
    rdof_frame_buf_t f;
    rdof_oled_clear(&f);
    rdof_oled_rect(&f, 10, 10, 5, 4, false, true);
    CHECK(rdof_oled_get(&f, 10, 10) && rdof_oled_get(&f, 14, 13) && rdof_oled_get(&f, 10, 12) && rdof_oled_get(&f, 14, 11) &&
              !rdof_oled_get(&f, 12, 11) && !rdof_oled_get(&f, 15, 10),
          "outline");
    rdof_oled_rect(&f, 10, 10, 5, 4, true, true);
    CHECK(rdof_oled_get(&f, 12, 11), "filled");
    rdof_oled_rect(&f, 10, 10, 5, 4, true, false);
    CHECK(!rdof_oled_get(&f, 10, 10) && !rdof_oled_get(&f, 12, 11), "cleared");
    /* the panel's open commands */
    uint8_t c[32];
    const uint8_t want[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8,
                            0xDA, 0x12, 0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
    CHECK(rdof_oled_init_cmds(false, 0x7F, c, sizeof c) == sizeof want && !memcmp(c, want, sizeof want), "init");
    CHECK(rdof_oled_init_cmds(true, 0x10, c, sizeof c) == sizeof want && c[12] == 0xA0 && c[13] == 0xC0 && c[17] == 0x10,
          "rotated, contrast");
    CHECK(rdof_oled_init_cmds(false, 0x7F, c, 5) == 5, "a short buffer takes what fits");
    CHECK(rdof_oled_page_cmds(true, 3, c, sizeof c) == 3 && c[0] == 0xB3 && c[1] == 0x02 && c[2] == 0x10, "SH1106 page");
    CHECK(rdof_oled_page_cmds(false, 5, c, sizeof c) == 6 && c[0] == 0x21 && c[2] == 127 && c[3] == 0x22 && c[4] == 5 &&
              c[5] == 5,
          "SSD1306 page");
    CHECK(rdof_oled_page_cmds(true, 0, c, 2) == 0 && rdof_oled_page_cmds(false, 0, c, 5) == 0, "no room");
    /* a repeater's line shows both frequencies */
    rdof_oled_status_t s = {.name = "rpt", .ip = "10.0.0.2", .wifi = true, .repeater = true, .freq = 461137500,
                          .tx_freq = 466137500, .state = "on air", .clock = "12:00:00"};
    rdof_oled_render(&f, &s);
    int lit = 0;
    for (int y = 21; y < 28; y++)
        for (int x = 0; x < RDOF_OLED_W; x++)
            lit += rdof_oled_get(&f, x, y);
    CHECK(lit > 100, "repeater line drawn (%d pixels)", lit);
    /* whole screens, pinned by a hash of the frame (the C's output: there's no reference screen to compare with) */
    CHECK(frame_hash(&f) == 0x56478498u, "repeater screen %08x", (unsigned)frame_hash(&f));
    rdof_oled_status_t r = {.name = "a receiver name long enough that the line is cut at 39", .wifi = false,
                          .freq = 461022500, .cc = 12, .rx_line = "T201<9990123", .tx_active = true,
                          .tx_line = "T1>9 ~"};
    rdof_oled_render(&f, &r); /* ip, state, clock NULL; struck-through tags */
    CHECK(frame_hash(&f) == 0xa6d4133fu, "roamer screen %08x", (unsigned)frame_hash(&f));
    /* off the panel */
    rdof_oled_clear(&f);
    CHECK(!rdof_oled_get(&f, -1, 0) && !rdof_oled_get(&f, RDOF_OLED_W, 0) && !rdof_oled_get(&f, 0, RDOF_OLED_H) &&
              !rdof_oled_get(&f, 0, -1),
          "get off the panel");
    rdof_oled_rect(&f, 120, 60, 20, 10, false, true);
    rdof_oled_rect(&f, -5, -5, 8, 8, true, true);
    CHECK(rdof_oled_get(&f, 127, 60) && rdof_oled_get(&f, 120, 63) && !rdof_oled_get(&f, 121, 61) && rdof_oled_get(&f, 2, 2) &&
              !rdof_oled_get(&f, 3, 0) && !rdof_oled_get(&f, 0, 3),
          "rects clipped");
    int end = rdof_oled_text(&f, -3, -2, "Hi", true, false);
    CHECK(end == 9 && rdof_oled_get(&f, 2, 5) && !rdof_oled_get(&f, 2, 6) && rdof_oled_get(&f, 8, 0),
          "text off the top left: end %d", end);
}

/* pixels lit in a box */
static int lit_in(const rdof_frame_buf_t *f, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            n += rdof_oled_get(f, x, y);
    return n;
}

static void t_oled_sdr(void)
{
    char b[8], line[64];
    /* badges: the radio name's short form on the status screen */
    CHECK(!strcmp(rdof_oled_badge("SDR04", b), "4") && !strcmp(rdof_oled_badge("sdr13", b), "13") &&
              !strcmp(rdof_oled_badge("T1", b), "T1") && !strcmp(rdof_oled_badge("ROAM2", b), "R2") &&
              !strcmp(rdof_oled_badge("Ch01", b), "C1") && !strcmp(rdof_oled_badge("north", b), "nor") &&
              !strcmp(rdof_oled_badge("7", b), "7") && !strcmp(rdof_oled_badge("", b), "") &&
              !strcmp(rdof_oled_badge("SDR1234567", b), "34567"),
          "badges: %s", b);
    CHECK(rdof_oled_bars(0) == -1 && rdof_oled_bars(-40) == 4 && rdof_oled_bars(-55) == 4 && rdof_oled_bars(-56) == 3 &&
              rdof_oled_bars(-70) == 2 && rdof_oled_bars(-80) == 1 && rdof_oled_bars(-85) == 1 && rdof_oled_bars(-90) == 0,
          "bars");
    /* the tuner's line */
    rdof_oled_sdr_t t = {.receiving = true, .center_hz = 461047500, .sample_rate = 288000, .cpu_load = 0.189};
    rdof_oled_tuner_line(&t, line, sizeof line);
    CHECK(!strcmp(line, "461.0475 288k cpu 19%"), "tuner line: %s", line);
    t.cpu_load = NAN;
    t.center_hz = 467e6;
    rdof_oled_tuner_line(&t, line, sizeof line);
    CHECK(!strcmp(line, "467 288k"), "no load, whole MHz: %s", line);
    t.receiving = false;
    rdof_oled_tuner_line(&t, line, sizeof line);
    CHECK(!strcmp(line, "starting"), "not receiving: %s", line);
    CHECK(rdof_oled_tuner_line(&t, line, 0) == 0 && rdof_oled_tuner_line(&t, line, 4) == 3, "short buffers");

    /* the W12's two channels, nothing heard */
    rdof_frame_buf_t f;
    rdof_oled_channel_t ch[8] = {{"SDR04", "Towing Bravo", 461087500, true},
                                 {"SDR13", "Short Event 4", 461037500, true}};
    rdof_oled_sdr_t s = {.wifi = true, .wifi_dbm = -62, .ip = "192.0.2.8", .mqtt = RDOF_OLED_UP,
                         .hblink = RDOF_OLED_DOWN, .sdr = RDOF_OLED_UP, .radio = RDOF_OLED_NONE, .clock = "12:41:07",
                         .ch = ch, .nch = 2, .error = "", .receiving = true, .center_hz = 461047500,
                         .sample_rate = 288000, .cpu_load = 0.189};
    rdof_oled_render_sdr(&f, &s);
    CHECK(lit_in(&f, 0, 8, 12, 15) > 40 && lit_in(&f, 0, 16, 127, 17) == 0, "MQ inverted, in the top 16 rows");
    CHECK(rdof_oled_get(&f, 14, 11) && !rdof_oled_get(&f, 14, 10), "HB struck through");
    CHECK(lit_in(&f, 0, 18, 127, 18) == 128 && lit_in(&f, 0, 54, 127, 54) == 128, "the rules");
    CHECK(lit_in(&f, 0, 29, 127, 36) > 60 && lit_in(&f, 0, 37, 127, 44) > 60 && lit_in(&f, 0, 45, 127, 52) == 0,
          "a line for each channel");
    CHECK(frame_hash(&f) == 0x5855a538u, "idle screen %08x", (unsigned)frame_hash(&f));
    /* a call on 13: its badge inverted, the call in place of the channels */
    ch[1].active = true;
    ch[1].src = 9990123;
    ch[1].dst = 9;
    ch[1].group = true;
    rdof_oled_render_sdr(&f, &s);
    CHECK(lit_in(&f, 21, 20, 33, 27) > 50, "13's badge lit");
    CHECK(lit_in(&f, 0, 37, 127, 44) == 0, "one call line");
    CHECK(frame_hash(&f) == 0x6a965956u, "call screen %08x", (unsigned)frame_hash(&f));
    /* more calls than lines; a badge out of the window; a roamer's TX row; no Wi-Fi */
    for (int i = 2; i < 6; i++) {
        static const char *ids[] = {"SDR01", "SDR02", "SDR03", "SDR05"};
        ch[i] = (rdof_oled_channel_t){ids[i - 2], "", 461000000 + 25000.0 * i, i != 3, true, 9990700 + (uint32_t)i, 9993100,
                                      false};
    }
    s.nch = 6;
    s.wifi = false;
    s.ip = NULL;
    s.clock = NULL;
    s.has_tx = true;
    s.tx_active = true;
    s.tx_line = "T9<9990999";
    s.radio = RDOF_OLED_UP;
    rdof_oled_render_sdr(&f, &s);
    CHECK(frame_hash(&f) == 0x440c4baau, "busy screen %08x", (unsigned)frame_hash(&f));
    /* idle with more channels than lines; the TX row idle */
    for (int i = 0; i < 6; i++)
        ch[i].active = false;
    s.tx_active = false;
    s.tx_line = NULL;
    s.ap = s.wifi = true;
    s.wifi_dbm = 0;
    s.ip = "192.168.4.1";
    rdof_oled_render_sdr(&f, &s);
    CHECK(frame_hash(&f) == 0x7c6fa602u, "many channels %08x", (unsigned)frame_hash(&f));
    /* the front end's error in place of the channels, wrapped */
    s.error = "LR2021: no answer on SPI (BUSY stuck high) after reset";
    s.has_tx = false;
    s.receiving = false;
    rdof_oled_render_sdr(&f, &s);
    CHECK(lit_in(&f, 0, 45, 127, 52) > 30, "the error's third line");
    CHECK(frame_hash(&f) == 0xc979c2e0u, "error screen %08x", (unsigned)frame_hash(&f));
    /* no channels; more badges than fit */
    s.nch = 0;
    s.error = "";
    rdof_oled_render_sdr(&f, &s);
    CHECK(frame_hash(&f) == 0x4f6bd5b6u, "no channels %08x", (unsigned)frame_hash(&f));
    static const char *many[] = {"SDR101", "SDR102", "SDR103", "SDR104", "SDR105", "SDR106", "SDR107", "SDR108"};
    for (int i = 0; i < 8; i++)
        ch[i] = (rdof_oled_channel_t){many[i], NULL, 462e6, true};
    s.nch = 8;
    rdof_oled_render_sdr(&f, &s);
    CHECK(lit_in(&f, 0, 20, 11, 27) > 0 && frame_hash(&f) == 0x68fcfc58u, "badges cut with a + %08x",
          (unsigned)frame_hash(&f));

    /* the settings */
    rdof_oled_settings_t o;
    char err[96];
    static const char *const box = "{\"name\": \"w12\", \"radios\": []}";
    CHECK(rdof_oled_settings_parse(box, strlen(box), &o, err, sizeof err) && !o.set && !o.on && o.address == 0x3C &&
              o.sda == -1 && o.scl == -1 && o.refresh_s == 0.5,
          "no oled key: %s", err);
    static const char *const on = "{\"oled\": true}", *const off = "{\"oled\": false /* off */}", *const nul = "{\"oled\": null}";
    CHECK(rdof_oled_settings_parse(on, strlen(on), &o, err, sizeof err) && o.set && o.on, "true");
    CHECK(rdof_oled_settings_parse(off, strlen(off), &o, err, sizeof err) && o.set && !o.on, "false");
    CHECK(rdof_oled_settings_parse(nul, strlen(nul), &o, err, sizeof err) && o.set && !o.on, "null");
    static const char *const full = "{\"device\": {}, \"oled\": {\"address\": 61, \"sh1106\": 1, \"rotate180\": true, "
                                    "\"refresh_s\": 0.25, \"sda\": 17, \"scl\": \"18\",}}";
    CHECK(rdof_oled_settings_parse(full, strlen(full), &o, err, sizeof err) && o.on && o.address == 61 && o.sh1106 &&
              o.rotate180 && o.refresh_s == 0.25 && o.sda == 17 && o.scl == 18,
          "every key: %s", err);
    static const char *const i2c = "{\"oled\": {\"i2c\": {\"address\": \"60\", \"sh1106\": true, \"rotate180\": \"y\"}, \"refresh_s\": \"1\"}}";
    CHECK(rdof_oled_settings_parse(i2c, strlen(i2c), &o, err, sizeof err) && o.address == 60 && o.sh1106 && o.rotate180 &&
              o.refresh_s == 1,
          "under i2c: %s", err);
    static const struct {
        const char *json, *want;
    } bad[] = {
        {"{\"oled\": {\"address\": \"x\"}}", "oled: address isn't a whole number"},
        {"{\"oled\": {\"address\": 60.5}}", "oled: address isn't a whole number"},
        {"{\"oled\": {\"i2c\": {\"address\": []}}}", "oled: address isn't a whole number"},
        {"{\"oled\": {\"address\": 200}}", "oled: address 200 isn't a 7-bit I2C address"},
        {"{\"oled\": {\"refresh_s\": \"x\"}}", "oled: refresh_s isn't a number"},
        {"{\"oled\": {\"refresh_s\": \"\"}}", "oled: refresh_s isn't a number"},
        {"{\"oled\": {\"refresh_s\": false}}", "oled: refresh_s isn't a number"},
        {"{\"oled\": {\"sda\": \"\"}}", "oled: sda isn't a whole number"},
        {"{\"oled\": {\"scl\": {}}}", "oled: scl isn't a whole number"},
        {"{\"oled\": {\"sda\": 49}}", "oled: sda and scl are two GPIOs 0 to 48"},
        {"{\"oled\": {\"sda\": 17, \"scl\": 17}}", "oled: sda and scl are two GPIOs 0 to 48"},
        {"{\"oled\": {\"scl\": -2}}", "oled: sda and scl are two GPIOs 0 to 48"},
        {"[1]", "the settings are a JSON object"},
        {"{", "JSON: "},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(!rdof_oled_settings_parse(bad[i].json, strlen(bad[i].json), &o, err, sizeof err) &&
                  !strncmp(err, bad[i].want, strlen(bad[i].want)),
              "%s: %s", bad[i].json, err);
    CHECK(!rdof_oled_settings_parse("{", 1, &o, NULL, 0), "no error buffer");
}

#if RDOF_FEATURE_TRACKER
typedef struct {
    int n;
    uint8_t pkt[16][RDOF_DMRD_LEN];
} hb_cap_t;

static void hb_cap(void *ctx, const uint8_t *d, size_t len)
{
    hb_cap_t *c = ctx;
    if (c->n < 16 && len == RDOF_DMRD_LEN)
        memcpy(c->pkt[c->n], d, len);
    c->n++;
}

/* The HBlink sink's hold: voice before the late-entry header, the last RDOF_HB_HOLD_FRAMES of it, resent after the
 * synthesized header with its addressing. */
static void t_hb_hold(void)
{
    rdof_hb_sink_t s;
    hb_cap_t cap = {0};
    rdof_hb_sink_init(&s, 9990099);
    s.send = hb_cap;
    s.ctx = &cap;
    rdof_frame_t f = {.slot = 2, .dt = RDOF_DT_VOICE};
    for (int k = 0; k < RDOF_HB_HOLD_FRAMES + 2; k++) {
        f.n = k % 6;
        memset(f.burst, k, sizeof f.burst);
        rdof_hb_sink_on_frame(&s, &f);
    }
    f.dt = RDOF_DT_CSBK; /* only voice is held */
    rdof_hb_sink_on_frame(&s, &f);
    f.slot = 3; /* no such slot */
    f.has_addr = true;
    rdof_hb_sink_on_frame(&s, &f);
    CHECK(cap.n == 0 && s.nheld[1] == RDOF_HB_HOLD_FRAMES && s.nheld[0] == 0, "held %d %d", cap.n, s.nheld[1]);
    rdof_frame_t h = {.slot = 2, .dt = RDOF_DT_VOICE_LC_HEADER, .has_addr = true, .src = 9990003, .dst = 9,
                    .group = true, .new_stream = true, .synthetic = true};
    rdof_hb_sink_on_frame(&s, &h);
    CHECK(cap.n == 1 + RDOF_HB_HOLD_FRAMES && s.nheld[1] == 0, "header and the held frames: %d", cap.n);
    CHECK(rdof_be32(cap.pkt[0] + 16) == 0x12345679 && cap.pkt[0][4] == 0, "stream id without rand32: %08x",
          (unsigned)rdof_be32(cap.pkt[0] + 16));
    int ok = 0;
    for (int k = 1; k <= RDOF_HB_HOLD_FRAMES; k++) /* the first two dropped */
        ok += cap.pkt[k][20] == k + 1 && cap.pkt[k][4] == k && rdof_be24(cap.pkt[k] + 5) == 9990003 &&
              rdof_be24(cap.pkt[k] + 8) == 9 && cap.pkt[k][15] == (0x80 | (k + 1) % 6) &&
              rdof_be32(cap.pkt[k] + 16) == 0x12345679;
    CHECK(ok == RDOF_HB_HOLD_FRAMES, "held frames resent in order, addressed: %d", ok);
}
#endif

static void t_hblink(void)
{
    CHECK(rdof_hb_kind((const uint8_t *)"DMRP", 4) == 'P' && rdof_hb_kind((const uint8_t *)"MSTNAK", 6) == 'N' &&
              rdof_hb_kind((const uint8_t *)"MSTCL", 5) == 'N' && rdof_hb_kind((const uint8_t *)"DMRT", 4) == 'T' &&
              rdof_hb_kind((const uint8_t *)"DMRDxxxx", 8) == 'D',
          "kinds");
    CHECK(!rdof_hb_kind((const uint8_t *)"DMR", 3) && !rdof_hb_kind((const uint8_t *)"RPTL", 4), "short, or unknown");
    /* a DMRC packet longer than the limit is cut to it: only (unsigned) of a negative cc and power gets there */
    uint8_t out[RDOF_DMRC_MAX];
    const uint32_t idle = 461000000;
    const int idle_cc = 1;
    rdof_location_t loc = {41.5, -81.7, 10};
    size_t n = rdof_dmrc_packet(9990301, "N0CALL", 461e6, 466e6, 1, '3', 50,
                              "a version string much longer than forty characters, so it's cut",
                              "software string also much longer than forty characters", &loc, &idle, &idle_cc, out);
    CHECK(n == 8 + 111 + 20 + 11 && !memcmp(out + 8 + 111, "41.5000 -81.7000 010461000000", 29), "DMRC %zu", n);
    n = rdof_dmrc_packet(9990301, "N0CALL", 461e6, 466e6, -1, '3', -1, "v", "s", &loc, &idle, &idle_cc, out);
    CHECK(n == RDOF_DMRC_MAX && !memcmp(out, "DMRC", 4), "DMRC cut to %d: %zu", RDOF_DMRC_MAX, n);
#if RDOF_FEATURE_TRACKER
    t_hb_hold();
#endif
}

/* ── JSON writer and CSBK names ───────────────────────────────────────────── */

static void t_json_writer(void)
{
    char buf[160];
    rdof_json_t j;
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    /* the JSON of "é😀\x01\"\\" and two bytes that aren't UTF-8 (a lone continuation, a cut sequence) */
    rdof_json_str(&j, "s", "\xC3\xA9\xF0\x9F\x98\x80\x01\"\\");
    rdof_json_str(&j, "bad", "a\x80" "b\xE2\x82");
    rdof_json_close(&j);
    CHECK(rdof_json_done(&j) && !strcmp(buf, "{\"s\":\"\\u00e9\\ud83d\\ude00\\u0001\\\"\\\\\",\"bad\":"
                                           "\"a\\ufffdb\\ufffd\\ufffd\"}"),
          "escapes: %s", buf);
    /* overlong and surrogate encodings aren't characters */
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_str(&j, NULL, "\xC0\xAF\xED\xA0\x80");
    CHECK(!strncmp(buf, "\"\\ufffd\\ufffd\\ufffd\\ufffd", 25), "overlong, surrogate: %s", buf);
    /* a full buffer, and none */
    char small[8];
    rdof_json_init(&j, small, sizeof small);
    rdof_json_str(&j, NULL, "much too long");
    CHECK(!rdof_json_done(&j), "overflow");
    rdof_json_init(&j, small, 0);
    rdof_json_bool(&j, NULL, true);
    CHECK(!rdof_json_done(&j), "no buffer");
    char f[40];
    rdof_format_float3(-0.0001, f, sizeof f);
    CHECK(!strcmp(f, "-0.0"), "-0.0001: %s", f); /* -0.0001 rounded to 3 decimals is -0.0 */
    rdof_format_float3(2.5, f, sizeof f);
    CHECK(!strcmp(f, "2.5"), "2.5: %s", f);
}

static void t_json_str_is(const char *s, const char *want)
{
    char buf[200];
    rdof_json_t j;
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_str(&j, NULL, s);
    CHECK(rdof_json_done(&j) && !strcmp(buf, want), "%s: got %s", want, buf);
}

#define FLOAT3_DIGEST 0x9859fffau

static void t_json_writer_edges(void)
{
    /* each UTF-8 length at its ends; lead bytes that start nothing (F5 to FF were taken as three-byte leads) */
    t_json_str_is("\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF", "\"\\u0080\\u07ff\\u0800\\uffff\"");
    t_json_str_is("\xF0\x90\x80\x80\xF4\x8F\xBF\xBF", "\"\\ud800\\udc00\\udbff\\udfff\"");
    t_json_str_is("\xF8\x80\x80", "\"\\ufffd\\ufffd\\ufffd\"");
    t_json_str_is("\xF5\x80\x80\x80", "\"\\ufffd\\ufffd\\ufffd\\ufffd\"");
    t_json_str_is("\xF9\x80\x80\x80\xFC\x80\x80\x80", "\"\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\"");
    t_json_str_is("\xFF", "\"\\ufffd\"");
    t_json_str_is("\xF4\x90\x80\x80", "\"\\ufffd\\ufffd\\ufffd\\ufffd\""); /* past U+10FFFF */
    t_json_str_is("\xE0\x9F\xBF", "\"\\ufffd\\ufffd\\ufffd\"");             /* overlong */
    t_json_str_is("\xF0\x9F\x98", "\"\\ufffd\\ufffd\\ufffd\"");             /* cut by the NUL */
    t_json_str_is("\x1F\x7F/", "\"\\u001f\x7F/\"");
    t_json_str_is("", "\"\"");

    /* nesting: RDOF_JSON_DEPTH objects, the commas right at the deepest; one more overflows */
    char buf[200];
    rdof_json_t j;
    for (int depth = RDOF_JSON_DEPTH; depth <= RDOF_JSON_DEPTH + 1; depth++) {
        rdof_json_init(&j, buf, sizeof buf);
        rdof_json_open(&j, NULL);
        for (int d = 1; d < depth; d++)
            rdof_json_open(&j, "a");
        rdof_json_int(&j, "x", 1);
        rdof_json_bool(&j, "y", false);
        for (int d = 0; d < depth; d++)
            rdof_json_close(&j);
        if (depth == RDOF_JSON_DEPTH)
            CHECK(rdof_json_done(&j) && !strcmp(buf, "{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"x\":1,\"y\":false}}}}}}}}"),
                  "%d deep: %s", depth, buf);
        else
            CHECK(!rdof_json_done(&j), "%d deep overflows", depth);
    }
    rdof_json_close(&j); /* one too many: depth stays 0 */
    CHECK(j.depth == 0, "close at the top");

    /* integers at their ends, raw values, an escaped key */
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_json_int(&j, "min", LLONG_MIN);
    rdof_json_int(&j, "max", LLONG_MAX);
    rdof_json_int(&j, "z", 0);
    rdof_json_int(&j, "m", -7);
    rdof_json_raw(&j, "r", "[1,2]");
    rdof_json_float3(&j, "f", -12.34567);
    rdof_json_str(&j, "k\"\n", "v");
    rdof_json_close(&j);
    CHECK(rdof_json_done(&j) && !strcmp(buf, "{\"min\":-9223372036854775808,\"max\":9223372036854775807,\"z\":0,\"m\":-7,"
                                           "\"r\":[1,2],\"f\":-12.346,\"k\\\"\\u000a\":\"v\"}"),
          "values: %s", buf);
    /* the buffer: exactly enough, one short (what fit stays, NUL terminated) */
    char small[5];
    rdof_json_init(&j, small, 5);
    rdof_json_str(&j, NULL, "ab");
    CHECK(rdof_json_done(&j) && !strcmp(small, "\"ab\""), "fits: %s", small);
    rdof_json_init(&j, small, 4);
    rdof_json_str(&j, NULL, "ab");
    rdof_json_int(&j, NULL, 1);
    CHECK(!rdof_json_done(&j) && !strcmp(small, "\"ab"), "one short: %s", small);

    /* float3 against the C library's %.3f: ties, infinities, NaN, more digits than the buffer, cut buffers */
    const double fv[] = {INFINITY, -INFINITY, NAN, 1e300, -1e300, 0.0005, 0.0015, 0.0025, 1.0005, -0.0004, -0.0005,
                         123456.7895, 1e-300, 2.675, 1e15 + 0.3, 9.9995, -9.9995, 100.0, 0.1};
    const size_t caps[] = {40, 8, 6, 3, 2, 1};
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof fv / sizeof fv[0]; i++)
        for (size_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
            char f[40];
            memset(f, 'X', sizeof f);
            rdof_format_float3(fv[i], f, caps[c]);
            h = fnv(h, f, sizeof f);
        }
    CHECK(h == FLOAT3_DIGEST, "float3: digest %08x", h);
    char f[4] = "XYZ";
    rdof_format_float3(1.5, f, 0);
    CHECK(!strcmp(f, "XYZ"), "no room: nothing written");
    rdof_format_float3(-0.0004, f, 4);
    CHECK(!strcmp(f, "-0."), "-0.0004 in 4 bytes: %s", f);

    /* base64 at each padding, into exactly the room it needs */
    static const struct {
        const char *in, *want;
        size_t n;
    } b64[] = {{"", "", 0}, {"f", "Zg==", 1}, {"fo", "Zm8=", 2}, {"foo", "Zm9v", 3}, {"foob", "Zm9vYg==", 4},
               {"\xFF\xFE\xFD", "//79", 3}, {"\0\0", "AAA=", 2}};
    for (size_t i = 0; i < sizeof b64 / sizeof b64[0]; i++) {
        char *out = malloc(4 * ((b64[i].n + 2) / 3) + 1);
        rdof_base64((const uint8_t *)b64[i].in, b64[i].n, out);
        CHECK(!strcmp(out, b64[i].want), "base64 %s: %s", b64[i].want, out);
        free(out);
    }
}

static void t_csbk_names(void)
{
    static const struct {
        uint8_t op, b3;
        const char *want;
    } ops[] = {
        {0x04, 0, "Unit to Unit Voice Service Request"}, {0x05, 0, "Unit to Unit Voice Answer Response"},
        {0x26, 0, "Negative Acknowledgment Response"},   {0x3D, 0, "Preamble"},
        {0x1F, 0, "Call Alert"},                          {0x20, 0, "Call Alert Ack"},
        {0x24, 0x80, "Radio Check"},                      {0x24, 0, "Radio Check Ack"},
        {0x27, 0, "Call Emergency"},                      {0x38, 0, "CSBK 0x38"},
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        uint8_t pl[12] = {ops[i].op, 0, 0, ops[i].b3}, info[196], burst[RDOF_BURST_BYTES];
        rdof_bptc196_encode(pl, info);
        rdof_build_burst(info, NULL, NULL, burst);
        char d[48];
        rdof_csbk_desc(burst, d, sizeof d);
        CHECK(!strcmp(d, ops[i].want), "CSBK %02x: %s", ops[i].op, d);
    }
}

/* ── the tracker: what the scenarios don't reach ── */

static void t_time_edges(void)
{
    char s[32];
    CHECK(rdof_time_ms(-0.5) == -500 && rdof_time_ms(-1.25) == -1250 && rdof_time_ms(-0.0000004) == 0,
          "before the epoch: %lld %lld %lld", (long long)rdof_time_ms(-0.5), (long long)rdof_time_ms(-1.25),
          (long long)rdof_time_ms(-0.0000004));
    rdof_iso_time(-0.5, s);
    CHECK(!strcmp(s, "1969-12-31T23:59:59.500Z"), "-0.5: %s", s);
    CHECK(rdof_time_ms(NAN) == 0, "NaN: 0");
    {
        static const struct {
            int64_t ms;
            const char *iso;
        } dates[] = {{1709251199999, "2024-02-29T23:59:59.999Z"}, {1769817600000, "2026-01-31T00:00:00.000Z"},
                     {951868800000, "2000-03-01T00:00:00.000Z"}, {-2203934400000, "1900-02-28T12:00:00.000Z"}};
        for (size_t k = 0; k < sizeof dates / sizeof dates[0]; k++) {
            char out[32];
            rdof_iso_ms(dates[k].ms, out);
            CHECK(!strcmp(out, dates[k].iso), "iso %s: %s", dates[k].iso, out);
        }
    }
    CHECK(rdof_time_ms(INFINITY) == (int64_t)((uint64_t)INT64_MAX * 1000u + 807u) && rdof_time_ms(-INFINITY) == 192,
          "infinities: saturated, wrapped: %lld %lld", (long long)rdof_time_ms(INFINITY), (long long)rdof_time_ms(-INFINITY));
    CHECK(rdof_time_ms(1e300) == rdof_time_ms(INFINITY) && rdof_time_ms(-1e300) == 192, "past int64: saturated");
    rdof_iso_ms(-1, s);
    CHECK(!strcmp(s, "1969-12-31T23:59:59.999Z"), "-1 ms: %s", s);
    rdof_iso_ms(-86400000, s);
    CHECK(!strcmp(s, "1969-12-31T00:00:00.000Z"), "-1 day: %s", s);
    rdof_iso_ms(INT64_MIN, s);
    CHECK(!strcmp(s, "-292275055-05-16T16:47:04.192Z"), "INT64_MIN: %s", s);
    rdof_iso_ms(INT64_MAX, s);
    CHECK(!strcmp(s, "292278994-08-17T07:12:55.807Z"), "INT64_MAX: %s", s);
    rdof_iso_ms(253402300800000, s);
    CHECK(!strcmp(s, "10000-01-01T00:00:00.000Z"), "year 10000: %s", s);
    rdof_iso_ms(-62135596800001, s);
    CHECK(!strcmp(s, "0000-12-31T23:59:59.999Z"), "year 0: %s", s);
    rdof_iso_ms(-62198755200000, s);
    CHECK(!strcmp(s, "-001-01-01T00:00:00.000Z"), "year -1: %s", s);
    uint8_t b[RDOF_BURST_BYTES];
    for (int i = 0; i < RDOF_BURST_BYTES; i++)
        b[i] = (uint8_t)(i * 7);
    rdof_burst_t mb;
    memset(&mb, 0x5A, sizeof mb);
    rdof_modem_burst(0x20, b, -80.5, 3, 2, &mb);
    CHECK(!memcmp(mb.burst, b, sizeof b) && mb.dt == RDOF_DT_VOICE_SYNC && mb.n == 0 && mb.slot == 2 &&
              mb.family == RDOF_FAMILY_MS && mb.cc == 3 && mb.power_dbfs == -80.5 && mb.sync_errors == 0 && !mb.utc &&
              mb.utc_slot == 0 && mb.utc_us == 0,
          "modem burst");
}

#if RDOF_FEATURE_TRACKER
typedef struct {
    char log[16384]; /* one line each: J json, R raw, C action src dst */
    size_t n;
    rdof_frame_t frames[64];
    int nframes, raws;
} trk_t;

static void trk_add(trk_t *k, const char *tag, const char *text)
{
    int w = snprintf(k->log + k->n, sizeof k->log - k->n, "%s %s\n", tag, text);
    if (w > 0 && k->n + (size_t)w < sizeof k->log)
        k->n += (size_t)w;
}
static void trk_json(void *ctx, const char *ch, const char *json) { trk_add(ctx, "J", json); }
static void trk_raw(void *ctx, const char *ch, const char *json) { trk_add(ctx, "R", json); }
static void trk_frame(void *ctx, const char *ch, const rdof_frame_t *f)
{
    trk_t *k = ctx;
    if (k->nframes < 64)
        k->frames[k->nframes++] = *f;
}
static void trk_call(void *ctx, const char *ch, const char *action, uint32_t src, uint32_t dst)
{
    char t[64];
    snprintf(t, sizeof t, "%s %u %u", action, (unsigned)src, (unsigned)dst);
    trk_add(ctx, "C", t);
}
static void trk_rawb(void *ctx, const char *ch, const rdof_rawburst_t *r) { ((trk_t *)ctx)->raws++; }

static rdof_tracker_t trk_tr;
static trk_t trk;

static void trk_on(double offset, double freq)
{
    memset(&trk, 0, sizeof trk);
    rdof_tracker_init(&trk_tr, "ch", offset, freq);
    const rdof_sink_t sink = {trk_json, trk_raw, trk_frame, &trk, trk_call, trk_rawb};
    rdof_tracker_add_sink(&trk_tr, sink);
}

static bool trk_said(const char *needle) { return strstr(trk.log, needle) != NULL; }

static int trk_count(const char *needle)
{
    int c = 0;
    for (const char *p = trk.log; (p = strstr(p, needle)); p++)
        c++;
    return c;
}

static void trk_feed_as(int dt, int n, int slot, const uint8_t burst[RDOF_BURST_BYTES], double power, double t,
                        rdof_family_t family, int cc)
{
    rdof_burst_t b;
    memset(&b, 0, sizeof b);
    memcpy(b.burst, burst, RDOF_BURST_BYTES);
    b.dt = dt;
    b.n = n;
    b.slot = slot;
    b.family = family;
    b.cc = cc;
    b.power_dbfs = power;
    rdof_tracker_feed(&trk_tr, &b, t);
}

static void trk_feed(int dt, int n, int slot, const uint8_t burst[RDOF_BURST_BYTES], double power, double t)
{
    trk_feed_as(dt, n, slot, burst, power, t, RDOF_FAMILY_MS, 1);
}

static void trk_lc(int dt, uint32_t src, uint32_t dst, int so, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t lc[9];
    rdof_group_lc(src, dst, true, so, lc);
    rdof_lc_burst(lc, dt, 1, rdof_data_sync(RDOF_MS_VOICE), out);
}

/* A control or data burst: 10 bytes, the CRC under mask, BPTC. */
static void trk_block(uint8_t d[12], uint16_t mask, uint8_t out[RDOF_BURST_BYTES])
{
    const uint16_t crc = rdof_crc16_ccitt(d, 10) ^ mask;
    d[10] = (uint8_t)(crc >> 8);
    d[11] = (uint8_t)crc;
    uint8_t info[196];
    rdof_bptc196_encode(d, info);
    rdof_build_burst(info, NULL, NULL, out);
}

static void trk_csbk(uint8_t op, uint8_t fid, uint8_t b2, uint8_t b3, uint32_t a, uint32_t z, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t d[12] = {op, fid, b2, b3};
    rdof_put_be24(d + 4, a);
    rdof_put_be24(d + 7, z);
    trk_block(d, RDOF_MASK_CSBK, out);
}

/* An unconfirmed data header (DPF 2) for blocks blocks, or a proprietary one (DPF 15). */
static void trk_header(int dpf, uint32_t src, uint32_t dst, int blocks, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t d[12] = {0};
    rdof_set_field(d, 0, 1, 1);
    rdof_set_field(d, 4, 4, (uint64_t)dpf);
    rdof_set_field(d, 16, 24, dst);
    rdof_set_field(d, 40, 24, src);
    rdof_set_field(d, 64, 1, 1);
    rdof_set_field(d, 65, 7, (uint64_t)blocks);
    trk_block(d, RDOF_MASK_DATA_HEADER, out);
}

/* Voice burst n (0 to 5) of a superframe carrying lc9 in its embedded LC, at t. */
static void trk_voice(const uint8_t lc9[9], int n, int slot, double t, double power, rdof_family_t family)
{
    uint8_t raw[128], b[RDOF_BURST_BYTES];
    rdof_embedded_lc_encode(lc9, raw);
    rdof_silence_burst(n, 1, false, n >= 1 && n <= 4 ? raw + 32 * (n - 1) : NULL, b);
    trk_feed_as(n == 0 ? RDOF_DT_VOICE_SYNC : RDOF_DT_VOICE, n, slot, b, power, t, family, 1);
}

/* A voice superframe carrying lc9 in its embedded LC, from voice A at t, 60 ms apart. */
static void trk_superframe(const uint8_t lc9[9], int slot, double t, double power)
{
    for (int n = 0; n < 6; n++)
        trk_voice(lc9, n, slot, t + 0.06 * n, power, RDOF_FAMILY_MS);
}

/* The "value" of the k-th BER report in the log, or "". */
static const char *trk_ber(int k, char out[16])
{
    out[0] = 0;
    const char *p = trk.log;
    for (int i = 0; i <= k; i++)
        if (!(p = strstr(p, "\"BER\"")) || !(p = strstr(p, "\"value\":")))
            return out;
        else
            p += 8;
    size_t n = strcspn(p, "}");
    snprintf(out, 16, "%.*s", (int)(n < 15 ? n : 15), p);
    return out;
}

static void t_tracker_edges(void)
{
    uint8_t b[RDOF_BURST_BYTES], b2[RDOF_BURST_BYTES];
    /* sinks: four, then no more; one without on_raw gets the fields only */
    rdof_tracker_t tr;
    rdof_tracker_init(&tr, "a channel name past twenty-four bytes", 0, 0);
    CHECK(!strcmp(tr.channel, "a channel name past twe"), "channel cut: %s", tr.channel);
    trk_t k;
    memset(&k, 0, sizeof k);
    const rdof_sink_t fields = {NULL, NULL, NULL, &k, NULL, trk_rawb};
    for (int i = 0; i < 4; i++)
        CHECK(rdof_tracker_add_sink(&tr, fields), "sink %d", i);
    CHECK(!rdof_tracker_add_sink(&tr, fields) && tr.nsinks == 4, "a fifth: no");
    tr.nsinks = -1;
    CHECK(!rdof_tracker_add_sink(&tr, fields), "nsinks below 0: no");
    tr.nsinks = 4;
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 9992345, 9, 0, b);
    rdof_burst_t rb = {.dt = RDOF_DT_VOICE_LC_HEADER, .slot = 1, .cc = 1, .power_dbfs = NAN};
    memcpy(rb.burst, b, sizeof b);
    rdof_tracker_feed(&tr, &rb, 100.0);
    CHECK(k.raws == 4 && k.n == 0, "fields to each, no JSON built");
    /* ignored: idle, slot 0 and 3 */
    trk_on(0, 0);
    trk_feed(RDOF_DT_IDLE, 0, 1, b, NAN, 100.0);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 0, b, NAN, 100.0);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 3, b, NAN, 100.0);
    CHECK(trk.n == 0 && trk.nframes == 0 && trk_tr.mode == 0, "idle and bad slots: nothing");
    /* a header, the same again, a new one over it */
    trk_on(-10, 461037500);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, -60.4, 100.0);
    CHECK(trk_said("J {\"MMDVM\":{\"timestamp\":\"1970-01-01T00:01:40.000Z\",\"mode\":\"DMR\"}}") &&
              trk_said("\"action\":\"start\",\"slot\":1,\"src_id\":9992345,\"dst_id\":9,\"group\":\"yes\",\"src_info\":"
                       "\"9992345\",\"frequency\":\"461.03750\",\"colorCode\":1}}") &&
              trk_said("C start 9992345 9") && trk.nframes == 1 && trk.frames[0].new_stream && trk.frames[0].has_addr &&
              trk.frames[0].rssi == -70,
          "start: %s", trk.log);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, -60.6, 100.06);
    CHECK(trk_count("\"action\":\"start\"") == 1 && trk.nframes == 2 && !trk.frames[1].new_stream &&
              trk.frames[1].rssi == -71,
          "the same header: no new call");
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 9992346, 9, RDOF_SO_EMERGENCY, b2);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b2, NAN, 100.12);
    CHECK(trk_said("\"action\":\"lost\",\"slot\":1,\"duration\":0.12,\"ber\":0.0}}") && trk_said("C lost 9992345 9") &&
              trk_said("\"src_id\":9992346,") && trk_said("\"colorCode\":1,\"emergency\":true}}") &&
              trk.frames[2].synthetic && trk.frames[2].dt == RDOF_DT_TERMINATOR_WITH_LC && trk.frames[3].new_stream,
          "a new talker: the old one lost, with its terminator: %s", trk.log);
    /* voice with no power: BER after a second, no RSSI */
    trk_on(0, 0);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 2, b, NAN, 100.0);
    uint8_t v[RDOF_BURST_BYTES], call_lc[9];
    rdof_group_lc(9992345, 9, true, 0, call_lc);
    for (int i = 0; i < 4; i++)
        trk_superframe(call_lc, 2, 100.06 + 0.36 * i, NAN);
    CHECK(trk_said("J {\"BER\":{") && !trk_said("\"RSSI\"") && trk_tr.slots[1].call.rssi_n == 0, "no power: BER only");
    trk_lc(RDOF_DT_TERMINATOR_WITH_LC, 9992345, 9, 0, b2);
    trk_feed(RDOF_DT_TERMINATOR_WITH_LC, 0, 2, b2, NAN, 101.5);
    CHECK(trk_said("\"action\":\"end\",\"slot\":2,\"duration\":1.5,\"ber\":") && !trk_said("\"rssi\""),
          "end, no rssi: %s", trk.log);
    /* a repeat terminator within a second: raw only; after it, or another talker's: a late entry */
    size_t was = trk.n;
    trk_feed(RDOF_DT_TERMINATOR_WITH_LC, 0, 2, b2, NAN, 102.5);
    CHECK(trk_count("\"action\":\"late_entry\"") == 0 && strstr(trk.log + was, "R ") && !strstr(trk.log + was, "J "),
          "a repeat at 1 s: raw");
    trk_feed(RDOF_DT_TERMINATOR_WITH_LC, 0, 2, b2, NAN, 103.6);
    CHECK(trk_count("\"action\":\"late_entry\"") == 1 && trk_count("\"action\":\"end\"") == 2, "after 1 s: its own call");
    trk_lc(RDOF_DT_TERMINATOR_WITH_LC, 7, 9, RDOF_SO_EMERGENCY, b2);
    trk_feed(RDOF_DT_TERMINATOR_WITH_LC, 0, 2, b2, NAN, 103.7);
    CHECK(trk_count("\"action\":\"late_entry\"") == 2 && trk_said("\"src_id\":7,") && trk_said("\"emergency\":true"),
          "another talker's terminator: a late entry");
    /* undecodable headers and terminators: raw, nothing opened */
    trk_on(0, 0);
    memset(b2, 0x55, sizeof b2);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b2, NAN, 100.0);
    trk_feed(RDOF_DT_TERMINATOR_WITH_LC, 0, 1, b2, NAN, 100.1);
    CHECK(!trk_tr.slots[0].call.active && trk_count("R ") == 2 && trk.nframes == 0,
          "garbage LC: %d frames", trk.nframes);
    uint8_t gps[9] = {0x08, 0, 0, 1, 2, 3, 4, 5, 6};
    rdof_lc_burst(gps, RDOF_DT_VOICE_LC_HEADER, 1, rdof_data_sync(RDOF_MS_VOICE), b2);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b2, NAN, 100.2);
    CHECK(!trk_tr.slots[0].call.active && trk_count("R ") == 3, "a header with no source: raw");
    /* late entry from the embedded LC: ETSI and Motorola FIDs; another FID, or another FLCO, isn't one */
    static const struct {
        uint8_t flco, fid;
        bool entry;
    } ent[] = {{0x00, 0x00, true}, {0x00, 0x10, true}, {0x03, 0x00, true}, {0x00, 0x08, false}, {0x04, 0x00, false}};
    for (size_t i = 0; i < sizeof ent / sizeof ent[0]; i++) {
        trk_on(0, 0);
        uint8_t lc9[9] = {ent[i].flco, ent[i].fid, RDOF_SO_EMERGENCY, 0, 0, 9, 0x98, 0x78, 0x99};
        trk_superframe(lc9, 1, 100.0, -70);
        CHECK(trk_said("\"action\":\"late_entry\"") == ent[i].entry, "FLCO %d FID %02x: %s", ent[i].flco, ent[i].fid, trk.log);
        if (ent[i].entry) {
            CHECK(trk_said("\"src_id\":9992345,\"dst_id\":9,") && trk_said("\"emergency\":true") &&
                      trk_tr.slots[0].call.has_lc && !memcmp(trk_tr.slots[0].call.lc, lc9, 9),
                  "late entry %zu", i);
            int hdr = -1;
            for (int f = 0; f < trk.nframes; f++)
                if (trk.frames[f].synthetic && trk.frames[f].dt == RDOF_DT_VOICE_LC_HEADER && trk.frames[f].new_stream)
                    hdr = f;
            CHECK(hdr >= 0, "a header synthesized");
            /* the same LC again: no second entry; a different one: a new call */
            trk_superframe(lc9, 1, 100.36, -70);
            CHECK(trk_count("\"action\":\"late_entry\"") == 1, "the same talker");
            lc9[5] = 10;
            trk_superframe(lc9, 1, 100.72, -70);
            CHECK(trk_count("\"action\":\"late_entry\"") == 2 && trk_said("C lost 9992345 9"), "another dst");
        }
    }
    /* RSSI: the offset added and rounded; past int saturated; a NaN offset 0 */
    trk_on(1e300, 0);
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 9992345, 9, 0, b);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, -50, 100.0);
    CHECK(trk.frames[0].rssi == INT_MAX, "saturated up");
    trk_on(-1e300, 0);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, -50, 100.0);
    CHECK(trk.frames[0].rssi == INT_MIN, "saturated down");
    trk_on(NAN, 0);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, -50, 100.0);
    CHECK(trk.frames[0].rssi == 0, "NaN offset");
    /* RSSI min / max / ave over a call */
    trk_on(0, 0);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0);
    const double pw[] = {-80, -70.4, -90, -75};
    for (int i = 0; i < 4; i++) {
        rdof_silence_burst(i, 1, false, NULL, v);
        trk_feed(i ? RDOF_DT_VOICE : RDOF_DT_VOICE_SYNC, i, 1, v, pw[i], 100.06 * (i + 1));
    }
    trk_lc(RDOF_DT_TERMINATOR_WITH_LC, 9992345, 9, 0, b2);
    trk_feed(RDOF_DT_TERMINATOR_WITH_LC, 0, 1, b2, NAN, 100.3);
    CHECK(trk_said("\"rssi\":{\"min\":-90,\"max\":-70,\"ave\":-79}}}"), "rssi: %s", trk.log);
    /* CSBKs: each name, the addressing, unknown ones and other FIDs quiet */
    static const struct {
        uint8_t op, fid, b2, b3;
        const char *desc; /* NULL: no event */
        uint32_t src, dst;
        bool group;
    } cs[] = {
        {0x04, 0, 0, 0, "Unit to Unit Voice Service Request", 200, 100, false},
        {0x05, 0, 0, 0, "Unit to Unit Voice Answer Response", 200, 100, false},
        {0x26, 0, 0, 0, "Negative Acknowledgment Response", 100, 200, false},
        {0x3D, 0, 0x40, 0, "Preamble", 200, 100, true},
        {0x1F, 0x10, 0, 0, "Call Alert", 200, 100, false},
        {0x20, 0, 0, 0, "Call Alert Ack", 200, 100, false},
        {0x24, 0, 0, 0x80, "Radio Check", 200, 100, false},
        {0x24, 0, 0, 0, "Radio Check Ack", 100, 200, false},
        {0x27, 0, 0, 0, "Call Emergency", 200, 100, true},
        {0x30, 0, 0, 0, NULL, 200, 100, false},
        {0x04, 0x68, 0, 0, NULL, 200, 100, false},
        {0x24, 0x68, 0, 0, NULL, 100, 200, false},
        {0x38, 0x10, 0, 0, NULL, 200, 100, false},
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        trk_on(0, 0);
        trk_csbk(cs[i].op, cs[i].fid, cs[i].b2, cs[i].b3, 100, 200, b);
        trk_feed(RDOF_DT_CSBK, 0, 1, b, NAN, 100.0);
        char want[96];
        snprintf(want, sizeof want, "\"csbk_desc\":\"%s\"", cs[i].desc ? cs[i].desc : "");
        CHECK(cs[i].desc ? trk_said(want) : !trk_said("csbk_desc"), "CSBK %02x/%02x: %s", cs[i].op, cs[i].fid, trk.log);
        CHECK(trk.nframes == 1 && trk.frames[0].src == cs[i].src && trk.frames[0].dst == cs[i].dst &&
                  trk.frames[0].group == cs[i].group && trk.frames[0].new_stream,
              "CSBK %02x/%02x addressing", cs[i].op, cs[i].fid);
    }
    trk_on(0, 0);
    trk_csbk(0x38, 0, 0, 0, 100, 200, b);
    trk_feed(RDOF_DT_CSBK, 0, 1, b, NAN, 100.0);
    b[5] ^= 0xFF;
    trk_feed(RDOF_DT_CSBK, 0, 1, b, NAN, 100.0);
    CHECK(trk.nframes == 0 && !trk_said("R "), "BS_Dwn_Act and a bad CRC: dropped");
    /* preambles: repeats within a second kept quiet, each one moving the window */
    trk_on(0, 0);
    trk_csbk(0x3D, 0, 0, 0, 100, 200, b);
    for (int i = 0; i < 5; i++)
        trk_feed(RDOF_DT_CSBK, 0, 1, b, NAN, 100.0 + 0.9 * i);
    CHECK(trk_count("\"Preamble\"") == 1, "repeats: %d", trk_count("\"Preamble\""));
    trk_feed(RDOF_DT_CSBK, 0, 1, b, NAN, 104.6 + 1.0);
    CHECK(trk_count("\"Preamble\"") == 2, "a second later: again");
    trk_csbk(0x3D, 0, 0, 0, 101, 200, b2);
    trk_feed(RDOF_DT_CSBK, 0, 1, b2, NAN, 105.7);
    trk_csbk(0x3D, 0, 0, 0, 101, 201, b2);
    trk_feed(RDOF_DT_CSBK, 0, 1, b2, NAN, 105.8);
    CHECK(trk_count("\"Preamble\"") == 4, "another dst, another src: each said");
    /* data: a header with blocks, its blocks, a proprietary second header; blocks with no call */
    trk_on(0, 0);
    memset(v, 0, sizeof v);
    trk_feed(RDOF_DT_RATE_12, 0, 1, v, NAN, 99.0);
    trk_feed(RDOF_DT_RATE_34, 0, 1, v, NAN, 99.0);
    trk_feed(RDOF_DT_RATE_1, 0, 1, v, NAN, 99.0);
    trk_feed(RDOF_DT_PI_HEADER, 0, 1, v, NAN, 99.0);
    CHECK(trk.nframes == 4 && !trk.frames[0].has_addr && !trk.frames[3].has_addr && trk_count("R ") == 4,
          "blocks with no call: passed on");
    trk_header(2, 9992345, 9, 3, b);
    trk_feed(RDOF_DT_DATA_HEADER, 0, 1, b, NAN, 100.0);
    CHECK(trk_said("\"action\":\"start\",\"slot\":1,\"src_id\":9992345,\"dst_id\":9,\"group\":\"yes\",\"src_info\":"
                   "\"9992345\",\"frames\":3}}"),
          "data start: %s", trk.log);
    trk_header(15, 0, 0, 0, b2);
    trk_feed(RDOF_DT_DATA_HEADER, 0, 1, b2, NAN, 100.1);
    CHECK(trk_tr.slots[0].call.blocks_left == 2 && trk_count("\"action\":\"start\"") == 1, "proprietary: a block");
    trk_feed(RDOF_DT_PI_HEADER, 0, 1, v, NAN, 100.15);
    CHECK(trk.frames[trk.nframes - 1].has_addr, "PI header in a call: addressed");
    trk_feed(RDOF_DT_RATE_34, 0, 1, v, NAN, 100.2);
    CHECK(trk_tr.slots[0].call.active, "one left");
    trk_feed(RDOF_DT_RATE_1, 0, 1, v, NAN, 100.3);
    CHECK(!trk_tr.slots[0].call.active && trk_said("J {\"DMR\":{\"timestamp\":\"1970-01-01T00:01:40.300Z\",\"action\":"
                                                 "\"end\",\"slot\":1}}"),
          "data end: %s", trk.log);
    trk_feed(RDOF_DT_DATA_HEADER, 0, 1, b2, NAN, 100.4);
    CHECK(!trk_tr.slots[0].call.active && trk_count("\"action\":\"start\"") == 2, "proprietary alone: a call");
    trk_header(2, 9992345, 9, 0, b);
    trk_feed(RDOF_DT_DATA_HEADER, 0, 1, b, NAN, 100.5);
    CHECK(!trk_tr.slots[0].call.active && trk_said("\"frames\":0}}"), "no blocks: ended at once");
    for (int i = 0; i < 12; i++)
        b[i] ^= 0x5A; /* past what BPTC corrects */
    const int fr = trk.nframes;
    trk_feed(RDOF_DT_DATA_HEADER, 0, 1, b, NAN, 100.6);
    CHECK(trk.nframes == fr && trk_count("\"action\":\"start\"") == 4 - 1, "a bad header: raw only");
    /* a voice header during a data call: the data call lost, no terminator for it */
    trk_header(2, 9992345, 9, 3, b);
    trk_feed(RDOF_DT_DATA_HEADER, 0, 1, b, NAN, 101.0);
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 9992345, 9, 0, b2);
    const int fr2 = trk.nframes;
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b2, NAN, 101.1);
    CHECK(trk_said("J {\"DMR\":{\"timestamp\":\"1970-01-01T00:01:41.100Z\",\"action\":\"lost\",\"slot\":1}}") &&
              trk.nframes == fr2 + 1 && trk_tr.slots[0].call.kind == RDOF_CALL_VOICE,
          "data lost to voice: %s", trk.log);
    /* tick: voice lost at 0.5 s, data at 2 s, at the last burst; idle 3 s after the last activity */
    trk_on(0, 0);
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 1, 2, 0, b);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0);
    trk_header(2, 3, 4, 5, b2);
    trk_feed(RDOF_DT_DATA_HEADER, 0, 2, b2, NAN, 100.0);
    rdof_tracker_tick(&trk_tr, 100.5);
    CHECK(trk_tr.slots[0].call.active, "0.5 s: not yet");
    rdof_tracker_tick(&trk_tr, 100.51);
    CHECK(!trk_tr.slots[0].call.active && trk_said("C lost 1 2") &&
              trk_said("\"timestamp\":\"1970-01-01T00:01:40.000Z\",\"action\":\"lost\",\"slot\":1,"),
          "voice lost, at its last burst");
    rdof_tracker_tick(&trk_tr, 102.0);
    CHECK(trk_tr.slots[1].call.active, "data at 2 s: not yet");
    rdof_tracker_tick(&trk_tr, 102.01);
    CHECK(!trk_tr.slots[1].call.active && trk_said("C lost 3 4") && trk_tr.mode == 1, "data lost; still DMR");
    rdof_tracker_tick(&trk_tr, 103.0);
    CHECK(trk_tr.mode == 1, "not idle at 3 s");
    rdof_tracker_tick(&trk_tr, 103.01);
    CHECK(trk_tr.mode == 2 && trk_said("\"mode\":\"idle\"") && trk_count("\"mode\":\"idle\"") == 1, "idle");
    rdof_tracker_tick(&trk_tr, 110);
    CHECK(trk_count("\"mode\":\"idle\"") == 1, "idle once");
    /* idle waits for a call still open */
    trk_on(0, 0);
    trk_header(2, 3, 4, 5, b2);
    trk_feed(RDOF_DT_DATA_HEADER, 0, 2, b2, NAN, 100.0);
    trk_tr.slots[1].call.last = 1e9;
    rdof_tracker_tick(&trk_tr, 104.0);
    CHECK(trk_tr.mode == 1, "a call open: not idle");
    /* a voice call lost without its LC: a terminator from its addressing */
    trk_on(0, 0);
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 1, 2, 0, b);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0);
    trk_tr.slots[0].call.has_lc = false;
    trk_tr.slots[0].call.group = false;
    rdof_tracker_tick(&trk_tr, 101.0);
    uint8_t lc[9], want[RDOF_BURST_BYTES];
    rdof_group_lc(1, 2, false, 0, lc);
    rdof_lc_burst(lc, RDOF_DT_TERMINATOR_WITH_LC, 1, rdof_data_sync(RDOF_MS_VOICE), want);
    CHECK(trk.frames[trk.nframes - 1].synthetic && !memcmp(trk.frames[trk.nframes - 1].burst, want, sizeof want) &&
              !trk.frames[trk.nframes - 1].group,
          "a terminator from the addressing");
    /* lost with its LC: the LC as heard (an emergency here), the call's family and color code */
    trk_on(0, 0);
    rdof_group_lc(1, 2, true, 0x80, lc);
    rdof_lc_burst(lc, RDOF_DT_VOICE_LC_HEADER, 2, rdof_data_sync(RDOF_BS_VOICE), b);
    trk_feed_as(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0, RDOF_FAMILY_BS, 2);
    rdof_tracker_tick(&trk_tr, 101.0);
    rdof_lc_burst(lc, RDOF_DT_TERMINATOR_WITH_LC, 2, rdof_data_sync(RDOF_BS_VOICE), want);
    CHECK(trk.frames[trk.nframes - 1].synthetic && !memcmp(trk.frames[trk.nframes - 1].burst, want, sizeof want),
          "a terminator with the call's LC, family and color code");
    /* a color code below 0 reads as 0; a negative frequency is still one; a huge one is cut */
    trk_on(0, -461025000);
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 1, 2, 0, b);
    trk_feed_as(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0, RDOF_FAMILY_MS, -1);
    CHECK(trk_said("\"frequency\":\"-461.02500\",\"colorCode\":0}"), "negative frequency, cc -1: %s", trk.log);
    trk_on(0, 1e30);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0);
    CHECK(trk_said("\"frequency\":\"99999999999999998322278\","), "frequency cut: %s", trk.log);
    /* a terminator stamped before its header: duration 0 */
    trk_on(0, 0);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0);
    trk_lc(RDOF_DT_TERMINATOR_WITH_LC, 1, 2, 0, b2);
    trk_feed(RDOF_DT_TERMINATOR_WITH_LC, 0, 1, b2, NAN, 99.5);
    CHECK(trk_said("\"action\":\"end\",\"slot\":1,\"duration\":0.0,\"ber\":0.0}}"), "negative duration: %s", trk.log);
    /* a terminator for another call right after one ended: a late entry, not a repeat */
    trk_lc(RDOF_DT_TERMINATOR_WITH_LC, 1, 3, 0, b2);
    trk_feed(RDOF_DT_TERMINATOR_WITH_LC, 0, 1, b2, NAN, 99.6);
    CHECK(trk_said("C late_entry 1 3") && trk_said("C end 1 3"), "another call's terminator: %s", trk.log);
    /* embedded LC fragments: a header, a voice sync and a decode each start over */
    uint8_t x[9];
    rdof_group_lc(7, 8, true, 0, x);
    trk_on(0, 0);
    for (int n = 1; n <= 3; n++)
        trk_voice(x, n, 1, 100.0 + 0.06 * n, NAN, RDOF_FAMILY_MS);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.3);
    trk_voice(x, 4, 1, 100.36, NAN, RDOF_FAMILY_MS);
    CHECK(!trk_said("late_entry") && trk_tr.slots[0].call.dst == 2, "fragments before a header: dropped");
    for (int n = 1; n <= 3; n++)
        trk_voice(x, n, 1, 100.4 + 0.06 * n, NAN, RDOF_FAMILY_MS);
    trk_voice(x, 0, 1, 100.62, NAN, RDOF_FAMILY_MS);
    trk_voice(x, 4, 1, 100.68, NAN, RDOF_FAMILY_MS);
    CHECK(!trk_said("late_entry") && trk_tr.slots[0].call.dst == 2, "fragments before a voice sync: dropped");
    trk_on(0, 0);
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 7, 8, 0, b);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0);
    trk_superframe(x, 1, 100.06, NAN);
    rdof_tracker_tick(&trk_tr, 101.0);
    trk_voice(x, 4, 1, 101.1, NAN, RDOF_FAMILY_MS);
    CHECK(!trk_said("late_entry") && !trk_tr.slots[0].call.active, "fragments used by a decode: dropped");
    /* a late entry's header: the burst's family */
    trk_on(0, 0);
    for (int n = 0; n < 6; n++)
        trk_voice(x, n, 1, 100.0 + 0.06 * n, NAN, RDOF_FAMILY_BS);
    rdof_lc_burst(x, RDOF_DT_VOICE_LC_HEADER, 1, rdof_data_sync(RDOF_BS_VOICE), want);
    CHECK(trk_said("C late_entry 7 8") && trk.frames[4].synthetic && !memcmp(trk.frames[4].burst, want, sizeof want),
          "late entry header in the burst's family");
    /* RSSI above 0: the minimum is the call's own */
    trk_on(100, 0);
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 1, 2, 0, b);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0);
    rdof_group_lc(1, 2, true, 0, lc);
    trk_superframe(lc, 1, 100.06, -80);
    trk_superframe(lc, 1, 100.42, -90);
    trk_lc(RDOF_DT_TERMINATOR_WITH_LC, 1, 2, 0, b2);
    trk_feed(RDOF_DT_TERMINATOR_WITH_LC, 0, 1, b2, NAN, 100.8);
    CHECK(trk_said("\"rssi\":{\"min\":10,\"max\":20,\"ave\":15}"), "positive RSSI: %s", trk.log);
    /* BER each second exactly, of that second's bursts alone */
    trk_on(0, 0);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0);
    for (int i = 1; i <= 30; i++) {
        if (i > 10 && i <= 20)
            trk_voice(lc, i % 6, 1, 100.0 + 0.1 * i, NAN, RDOF_FAMILY_MS);
        else {
            for (int k = 0; k < RDOF_BURST_BYTES; k++)
                b2[k] = (uint8_t)(37 * ((i - 1) % 10) + 11 * k + 5);
            trk_feed(i % 6 ? RDOF_DT_VOICE : RDOF_DT_VOICE_SYNC, i % 6, 1, b2, NAN, 100.0 + 0.1 * i);
        }
        if (i == 10)
            CHECK(trk_count("\"BER\"") == 1 && trk_said("\"timestamp\":\"1970-01-01T00:01:41.000Z\",\"mode\":"),
                  "BER at 1 s exactly: %s", trk.log);
    }
    char v1[16], v2[16], v3[16];
    trk_ber(0, v1), trk_ber(1, v2), trk_ber(2, v3);
    CHECK(trk_count("\"BER\"") == 3 && strcmp(v1, "0.0") && !strcmp(v2, "0.0") && !strcmp(v1, v3),
          "BER per second: %s %s %s", v1, v2, v3);
    /* a preamble repeated 1 s later: reported again */
    trk_on(0, 0);
    trk_csbk(0x3D, 0, 0, 0, 100, 200, b);
    trk_feed(RDOF_DT_CSBK, 0, 1, b, NAN, 100.0);
    trk_feed(RDOF_DT_CSBK, 0, 1, b, NAN, 101.0);
    CHECK(trk_count("\"Preamble\"") == 2, "preamble again at 1 s");
    /* a data block keeps its call: lost 2 s after the last block */
    trk_on(0, 0);
    trk_header(2, 3, 4, 3, b);
    trk_feed(RDOF_DT_DATA_HEADER, 0, 1, b, NAN, 100.0);
    trk_feed(RDOF_DT_RATE_12, 0, 1, b2, NAN, 101.5);
    rdof_tracker_tick(&trk_tr, 102.5);
    CHECK(trk_tr.slots[0].call.active, "a block keeps the data call");
    /* a proprietary header during voice: a data call of its own */
    trk_on(0, 0);
    trk_lc(RDOF_DT_VOICE_LC_HEADER, 1, 2, 0, b);
    trk_feed(RDOF_DT_VOICE_LC_HEADER, 0, 1, b, NAN, 100.0);
    trk_header(15, 0, 0, 0, b2);
    trk_feed(RDOF_DT_DATA_HEADER, 0, 1, b2, NAN, 100.1);
    CHECK(trk_said("C lost 1 2") && trk_said("C start 0 0"), "proprietary during voice: %s", trk.log);
}
#endif

/* ── network calls: what the transmit scenarios don't reach ── */

static void csbk_burst(uint8_t op, uint8_t burst[RDOF_BURST_BYTES])
{
    uint8_t pl[12] = {op}, info[196];
    rdof_bptc196_encode(pl, info);
    rdof_build_burst(info, NULL, NULL, burst);
}

static void t_netcalls(void)
{
    rdof_netcalls_t n;
    rdof_netcalls_init(&n);
    rdof_tx_call_info_t c = {.src = 9992345, .dst = 9, .group = true, .freq = 461037500, .cc = 1, .slot = 1,
                           .frames = -1};
    char out[640], ts[32], want[640];
    const double t = 1758800000.25;
    rdof_iso_time(t, ts);
    CHECK(rdof_netcalls_event(&n, "start", &c, 0, t, out, sizeof out), "start");
    snprintf(want, sizeof want,
             "{\"DMR\":{\"timestamp\":\"%s\",\"source\":\"network\",\"action\":\"start\",\"slot\":1,"
             "\"src_id\":9992345,\"dst_id\":9,\"group\":\"yes\",\"src_info\":\"9992345\",\"frequency\":\"461.03750\","
             "\"colorCode\":1}}",
             ts);
    CHECK(!strcmp(out, want), "start: %s", out);
    /* a data call says its blocks; an end its duration; a slot that isn't 1 is the roamer's 2 */
    c.frames = 0;
    c.slot = 7;
    CHECK(rdof_netcalls_event(&n, "start", &c, 0, t, out, sizeof out) && strstr(out, "\"slot\":2,") &&
              strstr(out, ",\"frames\":0}}"),
          "data start, slot 7: %s", out);
    CHECK(rdof_netcalls_event(&n, "end", &c, 1.5, t, out, sizeof out) && strstr(out, "\"action\":\"end\",\"slot\":2,"
                                                                                "\"duration\":1.5}}"),
          "end: %s", out);
    c.slot = 0;
    CHECK(rdof_netcalls_event(&n, "end", &c, 1, t, out, sizeof out) && strstr(out, "\"slot\":2,"), "slot 0: %s", out);
    /* too small a buffer */
    CHECK(!rdof_netcalls_event(&n, "start", &c, 0, t, out, 100), "start in 100 bytes");

    /* bursts: a repeated header or terminator once per slot, voice every time; a start forgets the last */
    uint8_t b[RDOF_BURST_BYTES], b2[RDOF_BURST_BYTES];
    memset(b, 0x5A, sizeof b);
    memcpy(b2, b, sizeof b2);
    b2[32] ^= 1;
    const uint8_t hdr = RDOF_SYNC_DATA | 1, voice = 3;
    rdof_rawburst_t r;
    c.slot = 1;
    CHECK(rdof_netcalls_burst_raw(&n, &c, hdr, b, t, &r) && r.slot == 1 && r.network && r.addressed && r.dt == 1 &&
              r.src == 9992345 && r.dst == 9 && r.group && r.ms == rdof_time_ms(t) && !memcmp(r.burst, b, sizeof b),
          "header");
    CHECK(!rdof_netcalls_burst_raw(&n, &c, hdr, b, t, &r), "header repeated");
    CHECK(rdof_netcalls_burst_raw(&n, &c, hdr, b2, t, &r), "another header");
    CHECK(rdof_netcalls_burst_raw(&n, &c, RDOF_SYNC_DATA | 2, b2, t, &r) && r.dt == 2, "a terminator, same bits");
    CHECK(rdof_netcalls_burst_raw(&n, &c, voice, b, t, &r) && rdof_netcalls_burst_raw(&n, &c, voice, b, t, &r) &&
              r.dt == RDOF_DT_VOICE && r.n == 3,
          "voice repeated");
    CHECK(rdof_netcalls_burst_raw(&n, &c, hdr, b, t, &r), "header after voice");
    c.slot = 2;
    CHECK(rdof_netcalls_burst_raw(&n, &c, hdr, b, t, &r) && r.slot == 2, "the other slot's own");
    CHECK(!rdof_netcalls_burst_raw(&n, &c, hdr, b, t, &r), "the other slot's repeated");
    rdof_netcalls_event(&n, "start", &c, 0, t, out, sizeof out);
    CHECK(rdof_netcalls_burst_raw(&n, &c, hdr, b, t, &r), "after a start");
    CHECK(rdof_netcalls_burst(&n, &c, voice, b, t, out, sizeof out) && strstr(out, "\"source\":\"network\""),
          "burst JSON: %s", out);
    CHECK(!rdof_netcalls_burst(&n, &c, voice, b, t, out, 40), "burst JSON in 40 bytes");

    /* a lone CSBK: one csbk event at its start, nothing at its end (out untouched), no bursts */
    c.has_csbk = true;
    csbk_burst(0x3D, c.csbk);
    CHECK(rdof_netcalls_event(&n, "start", &c, 0, t, out, sizeof out) &&
              strstr(out, "\"action\":\"csbk\",\"slot\":2,\"src_id\":9992345,") &&
              strstr(out, "\"src_info\":\"9992345\",\"csbk_desc\":\"Preamble\",\"frequency\":\"461.03750\","
                          "\"colorCode\":1}}") &&
              !strstr(out, "frames"),
          "csbk start: %s", out);
    strcpy(out, "untouched");
    CHECK(!rdof_netcalls_event(&n, "end", &c, 1, t, out, sizeof out) && !strcmp(out, "untouched"), "csbk end");
    CHECK(!rdof_netcalls_burst_raw(&n, &c, hdr, b2, t, &r), "csbk burst");

    /* .../tx: the slot as given; duration at the end only */
    c.has_csbk = false;
    c.slot = 0;
    rdof_tx_json("start", &c, 2, t, out, sizeof out);
    snprintf(want, sizeof want,
             "{\"TX\":{\"timestamp\":\"%s\",\"action\":\"start\",\"slot\":0,\"src_id\":9992345,\"dst_id\":9,"
             "\"group\":\"yes\",\"frequency\":\"461.03750\",\"colorCode\":1}}",
             ts);
    CHECK(!strcmp(out, want), "tx start: %s", out);
    rdof_tx_json("end", &c, 2.25, t, out, sizeof out);
    CHECK(strstr(out, "\"colorCode\":1,\"duration\":2.25}}") != NULL, "tx end: %s", out);
    rdof_tx_json("abort", &c, 2.25, t, out, sizeof out);
    CHECK(strstr(out, "\"colorCode\":1}}") != NULL, "tx abort: %s", out);

    /* csbk_desc cut as snprintf; "CSBK 0x%02X" for the rest */
    uint8_t cb[RDOF_BURST_BYTES];
    csbk_burst(0x3D, cb);
    char d[8] = "xxxxxxx";
    rdof_csbk_desc(cb, d, 5);
    CHECK(!strcmp(d, "Prea"), "desc in 5: %s", d);
    memcpy(d, "xxxxxxx", 8);
    rdof_csbk_desc(cb, d, 0);
    CHECK(d[0] == 'x', "desc in 0");
    csbk_burst(0x3F, cb);
    rdof_csbk_desc(cb, out, sizeof out);
    CHECK(!strcmp(out, "CSBK 0x3F"), "desc 3F: %s", out);
}

/* ── status messages: repr() cut to a buffer, the SFR and the slots ── */

static void t_status_edges(void)
{
    /* cut as snprintf: what fits, NUL terminated; nothing at all in 0 */
    char s[8] = "xxxxxxx";
    rdof_format_repr(123456.0, s, 4);
    CHECK(!strcmp(s, "123"), "repr in 4: %s", s);
    rdof_format_repr(123456.0, s, 8);
    CHECK(!strcmp(s, "123456."), "repr in 8: %s", s);
    memcpy(s, "xxxxxxx", 8);
    rdof_format_repr(1.5, s, 0);
    CHECK(s[0] == 'x', "repr in 0");
    rdof_format_repr(1e100, s, 1);
    CHECK(s[0] == 0, "repr in 1");

    char out[512];
    rdof_repeater_receiver_json("R", 461037500, 466037500, 2, "N0CALL", NULL, 2, out, sizeof out);
    CHECK(strstr(out, "\"device\":\"N0CALL\",\"sfr\":{\"inboundSlot\":2,\"outboundSlot\":1}}}") != NULL,
          "sfr: %s", out);
    rdof_repeater_receiver_json("R", 1, 1, 2, "D", NULL, INT_MIN, out, sizeof out);
    CHECK(strstr(out, "\"inboundSlot\":-2147483648,\"outboundSlot\":2147483651}") != NULL, "sfr INT_MIN: %s", out);
    rdof_repeater_status_t r = {.at = 0, .rx = 1, .tx = 2, .slot_configured = {true, false, true},
                              .slot_busy = {true, true, true}};
    rdof_repeater_status_json(&r, out, sizeof out);
    CHECK(strstr(out, "\"slots\":{\"2\":true}}") != NULL, "slot 2 only: %s", out);
    r.slot_configured[2] = false;
    rdof_repeater_status_json(&r, out, sizeof out);
    CHECK(strstr(out, "\"slots\":{}}") != NULL, "no slots: %s", out);
    rdof_roamer_status_t m = {0};
    rdof_roamer_status_json(&m, out, 20);
    CHECK(strlen(out) < 20, "roamer status cut");
}

/* ── the MMDVM protocol: what the vectors don't reach (expected values from the reference) ── */

typedef struct {
    int n, total;
    uint8_t cmd[8];
    size_t len[8];
} frames_seen_t;

static void seen_frame(void *ctx, uint8_t command, const uint8_t *payload, size_t len)
{
    frames_seen_t *f = ctx;
    if (f->n < 8) {
        f->cmd[f->n] = command;
        f->len[f->n] = len;
    }
    f->n++;
    for (size_t i = 0; i < len; i++)
        f->total += payload[i];
}

static void t_mmdvm_edges(void)
{
    int answered = 0;
    for (int c = 0; c < 256; c++)
        answered += !rdof_mmdvm_unanswered((uint8_t)c);
    CHECK(answered == 251 && rdof_mmdvm_unanswered(0x18) && rdof_mmdvm_unanswered(0x1A) && rdof_mmdvm_unanswered(0x1C) &&
              rdof_mmdvm_unanswered(0x1D) && rdof_mmdvm_unanswered(0x1E) && !rdof_mmdvm_unanswered(0x19),
          "unanswered %d", answered);

    /* a frame: 252 bytes of payload at most, and only in a buffer it fits */
    uint8_t pl[256], fr[260];
    for (int i = 0; i < 256; i++)
        pl[i] = (uint8_t)i;
    CHECK(rdof_mmdvm_frame(0x18, pl, 253, fr, sizeof fr) == 0, "frame 253");
    CHECK(rdof_mmdvm_frame(0x18, pl, 252, fr, 254) == 0, "frame one short");
    CHECK(rdof_mmdvm_frame(0x18, pl, 252, fr, 255) == 255 && fr[1] == 255 && fr[254] == 251, "frame 252");
    CHECK(rdof_mmdvm_frame(0x01, NULL, 0, fr, 3) == 3 && fr[0] == 0xE0 && fr[1] == 3 && fr[2] == 1, "frame empty");

    /* levels: int(level * 2.55 + 0.5), clamped; NaN as 0 */
    const double lv[] = {50, 0, -10, 100, 200, 1e300, -1e300, INFINITY, -INFINITY, NAN, 0.19, 0.2};
    const int want[] = {128, 0, 0, 255, 255, 255, 0, 255, 0, 0, 0, 1};
    for (size_t i = 0; i < sizeof lv / sizeof lv[0]; i++)
        CHECK(rdof_mmdvm_pct(lv[i]) == want[i], "pct %g: %d", lv[i], rdof_mmdvm_pct(lv[i]));

    rdof_modem_config_t c;
    rdof_modem_config_default(&c);
    c.rx_invert = c.ptt_invert = true;
    c.tx_delay_ms = -5;
    c.rx_dc_offset = -129; /* wraps, as (x + 128) & 0xFF */
    c.tx_dc_offset = INT_MAX;
    c.dmr_delay = 300;
    c.tx_level = 100;
    uint8_t v1[23], v2[37];
    CHECK(rdof_mmdvm_config_v1(&c, 0x1F, 2, false, v1) == 23, "v1 len");
    CHECK(v1[0] == 0x85 && v1[1] == 2 && v1[2] == 0 && v1[3] == 2 && v1[4] == 128 && v1[5] == 128 && v1[6] == 0x0F &&
              v1[7] == 44 && v1[8] == 128 && v1[10] == 255 && v1[13] == 127 && v1[14] == 255 && v1[22] == 0,
          "v1 %02x %02x %02x %02x %02x", v1[0], v1[2], v1[7], v1[13], v1[14]);
    c.tx_delay_ms = 99999;
    c.tx_invert = true;
    CHECK(rdof_mmdvm_config_v2(&c, 3, 0, true, v2) == 37, "v2 len");
    CHECK(v2[0] == 0x07 && v2[1] == 2 && v2[3] == 255 && v2[4] == 0 && v2[5] == 127 && v2[6] == 255 && v2[7] == 128 &&
              v2[10] == 255 && v2[26] == 3 && v2[27] == 44 && v2[28] == 128 && v2[36] == 0,
          "v2 %02x %02x %02x %02x", v2[0], v2[3], v2[5], v2[6]);

    uint8_t fq[14];
    CHECK(rdof_mmdvm_set_freq_payload(0x1B7D1A6C, 0xFFFFFFFF, 0x1FF, fq) == 14, "freq len");
    CHECK(fq[0] == 0 && fq[1] == 0x6C && fq[4] == 0x1B && fq[5] == 0xFF && fq[9] == 0xFF && fq[10] == 0xFF &&
              fq[13] == 0xFF,
          "freq payload");

    /* the reader: junk, lengths 1 and 2 skipped, a long frame, frames split anywhere */
    static uint8_t in[1200];
    size_t n = 0;
    const uint8_t junk[] = {0x00, 0xE0, 0x01, 0xE0, 0x02, 0x55, 0xE0, 0x03, 0x70, 0xE0, 0x05, 0x01, 9, 9};
    memcpy(in, junk, sizeof junk);
    n = sizeof junk;
    in[n++] = 0xE0, in[n++] = 0, in[n++] = 45, in[n++] = 0x02; /* 300 bytes long: 296 of payload */
    for (int i = 0; i < 296; i++)
        in[n++] = 1;
    in[n++] = 0xE0, in[n++] = 4, in[n++] = 0x7F, in[n++] = 7;
    for (size_t chunk = 1; chunk <= n; chunk = chunk < 8 ? chunk + 1 : chunk * 3) {
        rdof_mmdvm_reader_t r;
        rdof_mmdvm_reader_init(&r);
        frames_seen_t f = {0};
        for (size_t k = 0; k < n; k += chunk)
            rdof_mmdvm_reader_feed(&r, in + k, n - k < chunk ? n - k : chunk, seen_frame, &f);
        CHECK(f.n == 4 && f.cmd[0] == 0x70 && f.len[0] == 0 && f.cmd[1] == 0x01 && f.len[1] == 2 && f.cmd[2] == 0x02 &&
                  f.len[2] == 296 && f.cmd[3] == 0x7F && f.len[3] == 1 && f.total == 18 + 296 + 7 && r.len == 0,
              "reader chunk %zu: %d frames", chunk, f.n);
    }
    /* a partial frame waits; a stream of junk is dropped */
    rdof_mmdvm_reader_t r;
    rdof_mmdvm_reader_init(&r);
    frames_seen_t f = {0};
    rdof_mmdvm_reader_feed(&r, (const uint8_t[]){0xE0}, 1, seen_frame, &f);
    rdof_mmdvm_reader_feed(&r, (const uint8_t[]){0}, 1, seen_frame, &f);
    CHECK(f.n == 0 && r.len == 2, "long frame head kept: %zu", r.len);
    memset(pl, 0x11, sizeof pl);
    rdof_mmdvm_reader_init(&r);
    rdof_mmdvm_reader_feed(&r, pl, sizeof pl, seen_frame, &f);
    CHECK(f.n == 0 && r.len == 0, "junk dropped");

    rdof_mmdvm_status_t st;
    const uint8_t sp[6] = {0x02, 0xC1, 0x40, 0, 7, 9};
    CHECK(!rdof_mmdvm_parse_status(1, sp, 5, &st), "status short");
    CHECK(rdof_mmdvm_parse_status(1, sp, 6, &st) && st.transmitting == 0 && st.carrier == 1 && st.inbound_busy == 0 &&
              st.space == 9 && st.space1 == 7,
          "status v1");
    CHECK(rdof_mmdvm_parse_status(2, sp, 6, &st) && st.transmitting == 1 && st.carrier == 1 && st.inbound_busy == 1,
          "status v2");

    /* the version: cut, then stripped of whitespace (1C-1F too); non-ASCII as '?' */
    char d[16] = "xxxxxxxxxxxxxxx";
    CHECK(rdof_mmdvm_parse_version((const uint8_t *)"\x02", 1, NULL, 0) == 2 && rdof_mmdvm_parse_version(NULL, 0, d, 0) == 1 &&
              d[0] == 'x',
          "version in 0");
    CHECK(rdof_mmdvm_parse_version(NULL, 0, d, sizeof d) == 1 && d[0] == 0, "version empty");
    CHECK(rdof_mmdvm_parse_version((const uint8_t *)"\x02short", 6, d, sizeof d) == 2 && d[0] == 0, "v2 short");
    const uint8_t ver[] = "\x01\x1c\x0b MMDVM \xc3\xa9 x\x1f\t\x00 after";
    CHECK(rdof_mmdvm_parse_version(ver, sizeof ver - 1, d, sizeof d) == 1 && !strcmp(d, "MMDVM ?? x"), "strip: [%s]", d);
    CHECK(rdof_mmdvm_parse_version(ver, sizeof ver - 1, d, 6) == 1 && !strcmp(d, "MM"), "cut then strip: [%s]", d);
    CHECK(rdof_mmdvm_parse_version((const uint8_t *)"\x01\x7f\x80", 3, d, sizeof d) == 1 && !strcmp(d, "\x7f?"),
          "DEL kept: [%s]", d);

    /* detection */
    CHECK(rdof_radio_detect("x split duplex modem") == RDOF_RADIO_SPLIT && rdof_radio_detect("opendigitalradio mmdvm") ==
              RDOF_RADIO_HOTSPOT && rdof_radio_detect("RadioFoundry DM-1701 ba226121") == RDOF_RADIO_HOTSPOT &&
              rdof_radio_detect("radiofoundry md-uv380 SFR TS1>TS2 TS") == RDOF_RADIO_HOTSPOT &&
              rdof_radio_detect("RadioFoundry Gateway SDR modem (MMDVM_HS compatible)") == RDOF_RADIO_MMDVM_HS && rdof_radio_detect("adf7021 MMDVM") == RDOF_RADIO_MMDVM_HS &&
              rdof_radio_detect("Lonestar") == RDOF_RADIO_MMDVM_HS && rdof_radio_detect("mmdv") == RDOF_RADIO_UNKNOWN &&
              rdof_radio_detect("") == RDOF_RADIO_UNKNOWN,
          "detect");
    CHECK(!strcmp(rdof_radio_kind_name((rdof_radio_kind_t)99), "unknown") &&
              !strcmp(rdof_radio_kind_name((rdof_radio_kind_t)-1), "unknown") &&
              !strcmp(rdof_radio_kind_name(RDOF_RADIO_UNKNOWN), "unknown"),
          "kind name past the end");
    CHECK(rdof_radio_kind_of("opengd77") == RDOF_RADIO_HOTSPOT && rdof_radio_kind_of("auto") == RDOF_RADIO_UNKNOWN &&
              rdof_radio_kind_of("unknown") == RDOF_RADIO_UNKNOWN && rdof_radio_kind_of("MMDVM") == RDOF_RADIO_UNKNOWN &&
              rdof_radio_kind_of("") == RDOF_RADIO_UNKNOWN,
          "kind of");

    /* SFR: \bSFR\b, \bTS([12])\s*>\s*TS([12])\b */
    int a = 0, b = 0;
    CHECK(!rdof_sfr_slots("MMDVM SFR TS2>TS1", &a, &b), "sfr not a hotspot");
    CHECK(!rdof_sfr_slots("OpenGD77 SFRX", &a, &b) && !rdof_sfr_slots("OpenGD77 XSFR", &a, &b), "sfr not a word");
    CHECK(rdof_sfr_slots("OpenGD77 sfr", &a, &b) && a == 1 && b == 2, "sfr default");
    CHECK(rdof_sfr_slots("OpenGD77 SFR ts2 \x1c> \x0bts1", &a, &b) && a == 2 && b == 1, "sfr spaced");
    CHECK(rdof_sfr_slots("OpenGD77 SFR XTS2>TS1 TS2>TS12 TS3>TS1 TS2>", &a, &b) && a == 1 && b == 2, "sfr no pair");
    CHECK(rdof_sfr_slots("OpenGD77 SFR TS2>TS1_ TS2>TS2", &a, &b) && a == 2 && b == 2, "sfr second pair");
    CHECK(rdof_sfr_slots("XTS2>TS1 OpenGD77 SFR", &a, &b) && a == 1 && b == 2, "sfr pair after a word char at 1");

    /* framing: \bSFR\s+TS[12]\s*>\s*TS[12]\s+([A-Z]+)\b at its first match */
    char fw[8] = "xxxxxxx";
    rdof_sfr_framing("OpenGD77 SFR TS1>TS2 BS", fw, 0);
    CHECK(fw[0] == 'x', "framing in 0");
    const char *const fd[][2] = {
        {"OpenGD77 sfr ts1>ts2 bs", "BS"},     {"SFR TS1>TS2", ""},          {"SFR TS1>TS2 BS1", ""},
        {"SFR TS1>TS2BS", ""},                 {"SFRTS1>TS2 BS", ""},        {"SFR TS1>TS2 TS SFR TS2>TS1 MS", "TS"},
        {"SFR TS1>TS2 9 SFR TS2>TS1 MS", "MS"}, {"SFR\x1fTS2 > TS1\x1c ms.", "MS"}, {"SFR TS1>TS2 LONGWORD", "LONGWOR"},
        {"SFR TS1>TS2 - SFR TS2>TS1 MS", "MS"},
    };
    for (size_t i = 0; i < sizeof fd / sizeof fd[0]; i++) {
        rdof_sfr_framing(fd[i][0], fw, sizeof fw);
        CHECK(!strcmp(fw, fd[i][1]), "framing %s: [%s]", fd[i][0], fw);
    }

    /* RSSI: floor division below 300 MHz; INT_MAX and INT_MIN clamped, not overflowed */
    CHECK(rdof_radio_hotspot_dbm(-1, 146000000) == -166 && rdof_radio_hotspot_dbm(27, 146000000) == -132 &&
              rdof_radio_hotspot_dbm(-27, 299999999) == -196 && rdof_radio_hotspot_dbm(10, 300000000) == -141 &&
              rdof_radio_hotspot_dbm(INT_MAX, 461000000) == INT_MAX - 151 &&
              rdof_radio_hotspot_dbm(INT_MIN, 461000000) == INT_MIN && rdof_radio_hotspot_dbm(INT_MAX, 1) == INT_MAX &&
              rdof_radio_hotspot_dbm(INT_MIN, 1) == INT_MIN,
          "dbm");

    int dt, vn;
    rdof_control_to_dt(0x43, &dt, &vn);
    CHECK(dt == 3 && vn == 0, "control data");
    rdof_control_to_dt(0x60, &dt, &vn);
    CHECK(dt == 0 && vn == 0, "control data wins over audio");
    rdof_control_to_dt(0x2F, &dt, &vn);
    CHECK(dt == RDOF_DT_VOICE_SYNC && vn == 0, "control audio");
    rdof_control_to_dt(0x8D, &dt, &vn);
    CHECK(dt == RDOF_DT_VOICE && vn == 13, "control voice");

#if RDOF_FEATURE_TX
    /* silence: past F the LCSS is 0 (_LCSS.get(n, 0)), so 6 is 5's burst; any nonzero embedded bit is a 1 */
    uint8_t s5[RDOF_BURST_BYTES], s6[RDOF_BURST_BYTES], s0[RDOF_BURST_BYTES], e1[32], e2[32];
    CHECK(rdof_silence_burst(5, 3, false, NULL, s5) == 5 && rdof_silence_burst(6, 3, false, NULL, s6) == 6 &&
              !memcmp(s5, s6, sizeof s5),
          "silence past F");
    CHECK(rdof_silence_burst(-1, 3, false, NULL, s6) == 0xFF && !memcmp(s5, s6, sizeof s5), "silence -1");
    for (int i = 0; i < 32; i++) {
        e1[i] = (uint8_t)((i * 7) % 3 != 0);
        e2[i] = (uint8_t)(e1[i] ? 2 + i : 0);
    }
    rdof_silence_burst(2, 1, true, e1, s5);
    rdof_silence_burst(2, 1, true, e2, s6);
    CHECK(!memcmp(s5, s6, sizeof s5) && rdof_get_field(s5, 116, 32) == rdof_bits_to_u64(e1, 32), "silence embedded");
    CHECK(rdof_silence_burst(0, 1, true, e2, s0) == RDOF_SYNC_AUDIO && rdof_get_field(s0, 108, 48) == RDOF_BS_VOICE,
          "silence A");
    rdof_silence_burst(0, 1, false, NULL, s5);
    CHECK(rdof_get_field(s5, 108, 48) == RDOF_MS_VOICE && !memcmp(s5, s0, 13) && !memcmp(s5 + 20, s0 + 20, 13),
          "silence A ms");

    /* regenerate in place as into another buffer; an unreadable EMB past F takes LCSS 0 */
    uint8_t g[RDOF_BURST_BYTES], h[RDOF_BURST_BYTES];
    for (int i = 0; i < RDOF_BURST_BYTES; i++)
        g[i] = (uint8_t)(i * 37 + 5);
    uint32_t w = 0;
    for (; w < 0x10000; w++) {
        rdof_set_field(g, 108, 8, w >> 8);
        rdof_set_field(g, 148, 8, w & 0xFF);
        if (!rdof_decode_emb(g, NULL, NULL, NULL, NULL))
            break;
    }
    CHECK(w < 0x10000, "an unreadable EMB");
    for (int control = 0; control < 256; control++) {
        uint8_t in_place[RDOF_BURST_BYTES];
        memcpy(in_place, g, sizeof g);
        rdof_regenerate((uint8_t)control, g, 2, control & 1, h);
        rdof_regenerate((uint8_t)control, in_place, 2, control & 1, in_place);
        CHECK(!memcmp(h, in_place, sizeof h), "regenerate in place %02x", control);
    }
    rdof_regenerate(0x07, g, 2, false, h);
    rdof_regenerate(0x05, g, 2, false, s5);
    CHECK(!memcmp(h, s5, sizeof h), "regenerate past F");
    rdof_regenerate(0x09, g, 2, false, h);
    CHECK(!memcmp(h, s5, sizeof h), "regenerate 9 as F, not B");
    int ecc = -1, epi = -1, elc = -1;
    CHECK(rdof_decode_emb(h, &ecc, &epi, &elc, NULL) && ecc == 2 && epi == 0 && elc == 0, "regenerated EMB");

    /* no preambles: nothing written */
    uint8_t pre[1][RDOF_BURST_BYTES];
    memset(pre, 0xAA, sizeof pre);
    rdof_csbk_preambles(1, 2, true, 0, 1, pre);
    rdof_csbk_preambles(1, 2, true, -3, 1, pre);
    CHECK(pre[0][0] == 0xAA && pre[0][32] == 0xAA, "no preambles");
#endif
}

/* ── PDUs: what the vectors don't reach (expected values from the reference) ── */

static void t_pdu(void)
{
    /* CSBKs 0x21 and 0x38 are addressed with FID 0, not with another */
    for (int k = 0; k < 4; k++) {
        const uint8_t op = k < 2 ? 0x21 : 0x38;
        const int fid = k % 2;
        uint8_t pl[12] = {op, (uint8_t)(fid ? 0x10 : 0), 0, 0, 0x12, 0x34, 0x56, 0, 0, 7}, info[196],
                burst[RDOF_BURST_BYTES];
        rdof_bptc196_encode(pl, info);
        rdof_build_burst(info, NULL, NULL, burst);
        rdof_csbk_t c;
        rdof_decode_csbk(burst, &c);
        CHECK(c.opcode == op && (fid ? c.dst_id == RDOF_NONE && c.src_id == RDOF_NONE
                                     : c.dst_id == 0x123456 && c.src_id == 7),
              "CSBK %02x fid %d: %d -> %d", op, fid, c.src_id, c.dst_id);
    }
    /* full LC FLCO 1 and 9 (either side of the talker alias blocks) with FID 0: nothing parsed */
    for (int flco = 1; flco <= 9; flco += 8) {
        const uint8_t lc9[9] = {(uint8_t)flco, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        rdof_full_lc_t lc;
        rdof_parse_full_lc(lc9, &lc);
        CHECK(lc.flco == flco && lc.ta_nbits == 0 && lc.ta_format == RDOF_NONE && !lc.has_gps && lc.src_id == RDOF_NONE,
              "full LC FLCO %d", flco);
    }
#if RDOF_FEATURE_DATA_BLOCKS
    /* a type that isn't a data block; split_confirmed on what that leaves copies nothing (it copied len - 2,
       -2, bytes), nor on a len past the payload */
    uint8_t burst[RDOF_BURST_BYTES] = {0}, body[24];
    rdof_data_block_t b;
    int dbsn = -1, len = -1;
    CHECK(!rdof_decode_data_block(burst, RDOF_DT_CSBK, &b) && b.len == 0, "data block of a CSBK");
    CHECK(!rdof_split_confirmed(&b, &dbsn, body, &len) && dbsn == 0 && len == 0, "split a non-block: %d %d", dbsn,
          len);
    b.len = 30;
    dbsn = len = -1;
    CHECK(!rdof_split_confirmed(&b, &dbsn, body, &len) && dbsn == 0 && len == 0, "split len 30: %d %d", dbsn, len);
#endif
}

/* ── AMBE+2's Golay(23,12): every error pattern it corrects ── */

#define AMBE_DIGEST 0xc57820dfu

static void t_ambe(void)
{
    /* ambe.golay2312_encode(0xABC) */
    const uint32_t w = rdof_golay2312_encode(0xABC);
    CHECK(w == 0x55E11E, "golay2312 encode %06x", w);
    int bad = 0, e = -1;
    for (int a = 0; a < 23; a++)
        for (int b = a; b < 23; b++)
            for (int c = b; c < 23; c++) {
                /* a, b, c equal or not: one, two or three bits flipped */
                const uint32_t err = (1u << a) ^ (1u << b) ^ (1u << c);
                if (rdof_golay2312_decode(w ^ err, &e) != 0xABC || e != rdof_popcount64(err >> 11))
                    bad++;
            }
    CHECK(bad == 0, "golay2312: %d patterns not corrected", bad);
    /* four errors decode to another codeword: ambe.golay2312_decode(0x55FF1E) == (2743, 1) */
    CHECK(rdof_golay2312_decode(0x55FF1E, &e) == 2743 && e == 1, "golay2312 four errors: %d", e);
    /* errors may be NULL; a word wider than 23 bits as the reference: golay2312_decode(0xFFFFFFFF)[0] */
    CHECK(rdof_golay2312_decode(0xFFFFFFFF, NULL) == 0x1FFFFF, "golay2312 32 bits");

    /* frames round trip; params may be NULL */
    uint8_t p[49], back[49], fr[9];
    uint32_t x = 3;
    bad = 0;
    for (int n = 0; n < 200; n++) {
        for (int i = 0; i < 49; i++)
            p[i] = (uint8_t)((x = x * 1664525u + 1013904223u) >> 31);
        rdof_ambe_encode_frame(p, fr);
        bad += rdof_ambe_decode_frame(fr, back) != 0 || memcmp(p, back, 49) || rdof_ambe_decode_frame(fr, NULL) != 0;
    }
    CHECK(bad == 0, "ambe round trip: %d wrong", bad);
    /* every 12-bit word as Golay data, and as C0 (C1's scrambling is seeded from it) with C1 its bits reversed and
       the rest a pattern that moves with it; the codewords and frames pinned by digest to the C's. The round trip
       above can't see a fault that the encode and the decode share. */
    uint32_t h = 2166136261u;
    bad = 0;
    for (uint32_t d = 0; d < 4096; d++) {
        const uint32_t cw = rdof_golay2312_encode(d);
        bad += rdof_golay2312_decode(cw, &e) != d || e != 0;
        h = fnv(h, &cw, sizeof cw);
        for (int i = 0; i < 12; i++) {
            p[i] = (uint8_t)((d >> (11 - i)) & 1);
            p[12 + i] = (uint8_t)((d >> i) & 1);
        }
        for (int i = 24; i < 49; i++)
            p[i] = (uint8_t)((d * 7 + (uint32_t)i) % 3 == 0);
        rdof_ambe_encode_frame(p, fr);
        h = fnv(h, fr, sizeof fr);
        bad += rdof_ambe_decode_frame(fr, back) != 0 || memcmp(p, back, 49) || rdof_ambe_decode_frame(fr, NULL) != 0;
    }
    CHECK(bad == 0, "every golay2312 word, every C0: %d wrong", bad);
    CHECK(h == AMBE_DIGEST, "every C0: digest %08x", h);
    /* encoding, C0 and C1 take a byte's low bit, the rest any nonzero as a 1 (bits_to_u64, bits_to_bytes) */
    uint8_t want[9];
    memset(p, 2, 49);
    rdof_ambe_encode_frame(p, fr);
    memset(p, 0, 24);
    memset(p + 24, 1, 25);
    rdof_ambe_encode_frame(p, want);
    CHECK(!memcmp(fr, want, 9), "ambe encode of 2s");

#if RDOF_FEATURE_TX
    /* The silence burst's frames (rdof_silence_burst, ETSI -1 D.1's silence) are an AMBE+2 silence frame: they decode
       with no errors to b0 124 or 125 (b0 is params 0-5 and 48, as mbelib lays the frame out), and our encoder
       gives the same 72 bits back from those parameters. So the 9 bytes are the codec's, not taken from elsewhere. */
    uint8_t burst[RDOF_BURST_BYTES];
    rdof_silence_burst(0, 1, true, NULL, burst);
    for (int f = 0; f < 3; f++) {
        uint8_t frame[9] = {0};
        for (int i = 0; i < 72; i++) {
            int b = f == 0 ? i : f == 1 ? (i < 36 ? 72 + i : 156 + i - 36) : 192 + i;
            frame[i / 8] |= (uint8_t)(((burst[b / 8] >> (7 - b % 8)) & 1) << (7 - i % 8));
        }
        int b0 = 0;
        CHECK(rdof_ambe_decode_frame(frame, p) == 0, "silence frame %d: FEC errors", f);
        for (int i = 0; i < 6; i++)
            b0 |= p[i] << (6 - i);
        b0 |= p[48];
        CHECK(b0 == 124 || b0 == 125, "silence frame %d: b0 %d", f, b0);
        rdof_ambe_encode_frame(p, want);
        CHECK(!memcmp(frame, want, 9), "silence frame %d: re-encoded", f);
    }
#endif
}

/* ── the binary burst on .../raw: what the vectors don't reach ── */

static void t_rawburst(void)
{
    /* rawburst.TYPE_NAMES.get(dt, str(dt)) */
    static const struct {
        int dt;
        const char *want;
    } names[] = {
        {0, "voice_pi_header"}, {1, "voice_lc_header"}, {2, "terminator"},      {3, "csbk"},
        {4, "mbc_header"},      {5, "mbc_continuation"}, {6, "data_header"},    {7, "rate_1_2"},
        {8, "rate_3_4"},        {9, "9"},                {10, "rate_1"},        {11, "usbd"},
        {12, "12"},             {0xF0, "voice_sync"},    {0xF1, "voice"},       {0xF2, "242"},
        {-7, "-7"},             {INT_MAX, "2147483647"}, {INT_MIN, "-2147483648"},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char tmp[12];
        const char *s = rdof_raw_type_name(names[i].dt, tmp);
        CHECK(!strcmp(s, names[i].want), "type name %d: %s", names[i].dt, s);
    }
    /* no time (0, or before the epoch) is 0 on the wire; the burst copied as is */
    rdof_rawburst_t r = {.ms = -5, .slot = 1, .dt = 3};
    for (int i = 0; i < RDOF_BURST_BYTES; i++)
        r.burst[i] = (uint8_t)(0xA0 + i);
    uint8_t b[RDOF_RAWBURST_MAX];
    CHECK(rdof_rawburst_encode(&r, b) == RDOF_RAWBURST_BYTES && !memcmp(b + 4, (uint8_t[6]){0}, 6) &&
              !memcmp(b + 16, r.burst, RDOF_BURST_BYTES),
          "rawburst before the epoch");
    /* a buffer too small for the JSON */
    char json[40];
    CHECK(!rdof_rawburst_json(&r, json, sizeof json), "rawburst JSON in 40 bytes");
}

/* ── the slot grid (slotgrid.c / slotgrid.rs) at its edges ─────────────────── */

static void t_grid_edges(void)
{
    /* where int64 wraps: pinned, from the reference with the C's wrapping */
    static const struct {
        int64_t t;
        int off;
        int64_t at_n;
        int at_ts;
        int64_t at_phase, next_n, next_b, in_n;
        int in_ts;
        int64_t in_err, out_n;
        int out_ts;
        int64_t out_err;
    } v[] = {
        {INT64_MIN, 0, -307445734562LL, 1, 5224192LL, -307445734561LL, -9223372036830000000LL, -307445734562LL, 2, 3974192LL, -307445734562LL, 1, 2724192LL},
        {INT64_MIN, 15000, 307445734561LL, 2, 9775808LL, 307445734562LL, -9223372036834551616LL, 307445734561LL, 1, 8525808LL, 307445734561LL, 2, 7275808LL},
        {INT64_MIN, -15000, -307445734562LL, 1, 20224192LL, -307445734561LL, -9223372036845000000LL, -307445734561LL, 1, -11025808LL, -307445734561LL, 2, -12275808LL},
        {INT64_MIN, INT_MAX, 307445662979LL, 2, 1128808LL, 307445662980LL, -9223372036825904616LL, 307445662979LL, 1, -121192LL, 307445662979LL, 2, -1371192LL},
        {INT64_MIN, INT_MIN, -307445662980LL, 1, 28872192LL, -307445662979LL, -9223372036853648000LL, -307445662979LL, 1, -2377808LL, -307445662979LL, 2, -3627808LL},
        {INT64_MIN + 1, 0, -307445734562LL, 1, 5224193LL, -307445734561LL, -9223372036830000000LL, -307445734562LL, 2, 3974193LL, -307445734562LL, 1, 2724193LL},
        {INT64_MIN + 1, -15000, -307445734562LL, 1, 20224193LL, -307445734561LL, -9223372036845000000LL, -307445734561LL, 1, -11025807LL, -307445734561LL, 2, -12275807LL},
        {INT64_MAX, 0, 307445734561LL, 2, 24775807LL, 307445734562LL, -9223372036849551616LL, -307445734562LL, 2, 3974191LL, -307445734562LL, 1, 2724191LL},
        {INT64_MAX, 15000, 307445734561LL, 2, 9775807LL, 307445734562LL, -9223372036834551616LL, 307445734561LL, 1, 8525807LL, 307445734561LL, 2, 7275807LL},
        {INT64_MAX, -15000, -307445734562LL, 1, 20224191LL, -307445734561LL, -9223372036845000000LL, -307445734561LL, 1, -11025809LL, -307445734561LL, 2, -12275809LL},
        {INT64_MAX, INT_MAX, 307445662979LL, 2, 1128807LL, 307445662980LL, -9223372036825904616LL, 307445662979LL, 1, -121193LL, 307445662979LL, 2, -1371193LL},
        {INT64_MAX, INT_MIN, -307445662980LL, 1, 28872191LL, -307445662979LL, -9223372036853648000LL, -307445662979LL, 1, -2377809LL, -307445662979LL, 2, -3627809LL},
        {INT64_MAX - 14999999, 0, 307445734561LL, 2, 9775808LL, 307445734562LL, -9223372036849551616LL, 307445734561LL, 1, 8525808LL, 307445734561LL, 2, 7275808LL},
        {INT64_MAX - 14999999, 15000, 307445734560LL, 1, 24775808LL, 307445734561LL, 9223372036845000000LL, 307445734561LL, 1, -6474192LL, 307445734561LL, 2, -7724192LL},
        {INT64_MAX - 14999999, INT_MAX, 307445662978LL, 1, 16128808LL, 307445662979LL, 9223372036853647000LL, 307445662978LL, 2, 14878808LL, 307445662978LL, 1, 13628808LL},
        {INT64_MAX - 15000000, 0, 307445734561LL, 2, 9775807LL, 307445734562LL, -9223372036849551616LL, 307445734561LL, 1, 8525807LL, 307445734561LL, 2, 7275807LL},
        {INT64_MAX - 15000000, -15000, 307445734561LL, 2, 24775807LL, 307445734562LL, 9223372036845000000LL, -307445734562LL, 2, 3974191LL, -307445734562LL, 1, 2724191LL},
        {INT64_MAX - 15000000, INT_MIN, -307445662980LL, 1, 13872191LL, -307445662979LL, -9223372036853648000LL, -307445662980LL, 2, 12622191LL, -307445662980LL, 1, 11372191LL},
        {0, 15000, -1LL, 2, 15000000LL, 0LL, 15000000LL, -1LL, 1, 13750000LL, -1LL, 2, 12500000LL},
        {0, -15000, 0LL, 1, 15000000LL, 1LL, 15000000LL, 0LL, 2, 13750000LL, 0LL, 1, 12500000LL},
        {0, INT_MAX, -71583LL, 2, 6353000LL, -71582LL, 23647000LL, -71583LL, 1, 5103000LL, -71583LL, 2, 3853000LL},
        {0, INT_MIN, 71582LL, 1, 23648000LL, 71583LL, 6352000LL, 71583LL, 1, -7602000LL, 71583LL, 2, -8852000LL},
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        int ts, its, ots;
        int64_t ph, b, ie, oe;
        int64_t n = rdof_grid_slot_at(v[i].t, v[i].off, &ts, &ph);
        int64_t nn = rdof_grid_next_boundary(v[i].t, v[i].off, &b);
        int64_t in = rdof_grid_inbound(v[i].t, v[i].off, &its, &ie);
        int64_t on = rdof_grid_outbound(v[i].t, v[i].off, &ots, &oe);
        CHECK(n == v[i].at_n && ts == v[i].at_ts && ph == v[i].at_phase, "grid edge %zu slot_at %lld %d %lld", i,
              (long long)n, ts, (long long)ph);
        CHECK(nn == v[i].next_n && b == v[i].next_b, "grid edge %zu next %lld %lld", i, (long long)nn, (long long)b);
        CHECK(in == v[i].in_n && its == v[i].in_ts && ie == v[i].in_err, "grid edge %zu inbound %lld %d %lld", i,
              (long long)in, its, (long long)ie);
        CHECK(on == v[i].out_n && ots == v[i].out_ts && oe == v[i].out_err, "grid edge %zu outbound %lld %d %lld", i,
              (long long)on, ots, (long long)oe);
    }
    /* the relations that hold mod 2^64 everywhere: a time is its slot's boundary plus the phase, the next boundary
       is at most a slot on, and the boundary is n slots plus the offset */
    const int64_t ts_[] = {INT64_MIN, INT64_MIN + 1, -30000001, -30000000, -29999999, -1, 0, 1, 29999999, 30000000,
                           INT64_MAX - 30000000, INT64_MAX - 1, INT64_MAX};
    const int offs[] = {INT_MIN, -15000, -1, 0, 1, 15000, INT_MAX};
    int bad = 0;
    for (size_t i = 0; i < sizeof ts_ / sizeof ts_[0]; i++)
        for (size_t k = 0; k < sizeof offs / sizeof offs[0]; k++) {
            int ts;
            int64_t ph, b;
            int64_t n = rdof_grid_slot_at(ts_[i], offs[k], &ts, &ph);
            int64_t nn = rdof_grid_next_boundary(ts_[i], offs[k], &b);
            uint64_t ahead = (uint64_t)b - (uint64_t)ts_[i];
            if ((uint64_t)rdof_grid_boundary(n, offs[k]) + (uint64_t)ph != (uint64_t)ts_[i] || ph < 0 ||
                ph >= RDOF_SLOT_NS || ts != ((n & 1) ? 2 : 1) || ahead >= (uint64_t)RDOF_SLOT_NS ||
                b != rdof_grid_boundary(nn, offs[k]) || nn - n != (ph != 0))
                bad++;
        }
    CHECK(bad == 0, "grid relations: %d differ", bad);
    /* the boundaries: a burst exactly half a slot late goes to the next slot, a hair less to its own */
    int ts;
    int64_t err;
    const int64_t b0 = rdof_grid_boundary(7, 0);
    CHECK(rdof_grid_inbound(b0 + RDOF_IN_START_NS + RDOF_SLOT_NS / 2, 0, &ts, &err) == 8 && ts == 2 &&
              err == -RDOF_SLOT_NS / 2,
          "inbound half a slot late: %d %lld", ts, (long long)err);
    CHECK(rdof_grid_inbound(b0 + RDOF_IN_START_NS + RDOF_SLOT_NS / 2 - 1, 0, &ts, &err) == 7 && ts == 1 &&
              err == RDOF_SLOT_NS / 2 - 1,
          "inbound a hair under half a slot late");
    CHECK(rdof_grid_outbound(b0 + RDOF_CACH_NS - RDOF_SLOT_NS / 2, 0, &ts, &err) == 7 && ts == 2 &&
              err == -RDOF_SLOT_NS / 2,
          "outbound half a slot early");
    CHECK(rdof_grid_outbound(b0 + RDOF_CACH_NS - RDOF_SLOT_NS / 2 - 1, 0, &ts, &err) == 6 && ts == 1 &&
              err == RDOF_SLOT_NS / 2 - 1,
          "outbound a hair over half a slot early");
}

/* slot_timing from base and own (JSON text, NULL for -1); the describe of the result, or "error: <why>" */
static const char *st_parse(const char *base, const char *own)
{
    static jdoc_t j;
    static char out[160];
    char text[200];
    snprintf(text, sizeof text, "[%s, %s]", base ? base : "0", own ? own : "0");
    int root = jparse(&j, text, strlen(text), 32, sizeof j.buf);
    if (root < 0) {
        snprintf(out, sizeof out, "bad JSON %s", j.doc.error);
        return out;
    }
    rdof_slot_timing_t t;
    char why[96] = "";
    if (rdof_slot_timing_parse(&j.doc, base ? rdof_jat(&j.doc, root, 0) : -1, own ? rdof_jat(&j.doc, root, 1) : -1, &t,
                             why, sizeof why))
        rdof_slot_timing_describe(&t, out, sizeof out);
    else
        snprintf(out, sizeof out, "error: %s", why);
    return out;
}

static void t_slot_timing_edges(void)
{
    static const struct {
        const char *base, *own, *want;
    } v[] = {
        {NULL, NULL, "free,0,20,2000,holdover,60.0"},
        {"null", "null", "free,0,20,2000,holdover,60.0"},
        {"\"utc\"", "{\"offset_us\": -15000}", "utc,-15000,20,2000,holdover,60.0"},
        {"{\"mode\": \"utc\", \"on_loss\": \"free\", \"offset_us\": 5}", "\"free\"", "free,5,20,2000,free,60.0"},
        {"\"free\"", "{\"mode\": \"utc\"}", "utc,0,20,2000,holdover,60.0"},
        /* max_err_us: the radio's replaces the box's whole ({**box, **own}) */
        {"{\"max_err_us\": {\"pps\": 5}}", "{\"max_err_us\": 300}", "free,0,300,300,holdover,60.0"},
        {"{\"max_err_us\": 300}", "{\"max_err_us\": {\"pps\": 5}}", "free,0,5,2000,holdover,60.0"},
        {NULL, "{\"max_err_us\": {}}", "free,0,20,2000,holdover,60.0"},
        {NULL, "{\"max_err_us\": {\"ntp\": 15000, \"pps\": 1}}", "free,0,1,15000,holdover,60.0"},
        /* a key twice: the last (the C read each, and failed on a bad first one) */
        {NULL, "{\"max_err_us\": {\"pps\": 5, \"pps\": 7}}", "free,0,7,2000,holdover,60.0"},
        {NULL, "{\"max_err_us\": {\"pps\": \"x\", \"pps\": 5}}", "free,0,5,2000,holdover,60.0"},
        {NULL, "{\"offset_us\": 1.5, \"offset_us\": 2}", "free,2,20,2000,holdover,60.0"},
        /* the radio's key replaces a bad one of the box's, as the merged dict would */
        {"{\"offset_us\": 99999}", "{\"offset_us\": 1}", "free,1,20,2000,holdover,60.0"},
        {NULL, "{\"offset_us\": 15000}", "free,15000,20,2000,holdover,60.0"},
        {NULL, "{\"offset_us\": -0.0}", "free,0,20,2000,holdover,60.0"},
        {NULL, "{\"offset_us\": 3.0}", "free,3,20,2000,holdover,60.0"},
        {NULL, "{\"max_err_us\": 15000}", "free,0,15000,15000,holdover,60.0"},
        {NULL, "{\"on_loss\": \"stop\"}", "free,0,20,2000,stop,60.0"},
        {"{\"on_loss\": \"stop\"}", "{\"on_loss\": \"holdover\"}", "free,0,20,2000,holdover,60.0"},
        {NULL, "{\"holdover_s\": 0}", "free,0,20,2000,holdover,0.0"},
        {NULL, "{\"holdover_s\": 86400}", "free,0,20,2000,holdover,86400.0"},
        {NULL, "{\"holdover_s\": -0.0}", "free,0,20,2000,holdover,-0.0"},
        /* errors */
        {NULL, "3", "error: slot_timing: free, utc, or {mode, offset_us, max_err_us, on_loss, holdover_s}"},
        {"[1]", "\"utc\"", "error: slot_timing: free, utc, or {mode, offset_us, max_err_us, on_loss, holdover_s}"},
        {"true", NULL, "error: slot_timing: free, utc, or {mode, offset_us, max_err_us, on_loss, holdover_s}"},
        {"{\"mode\": \"utc\", \"drift\": 1}", "\"utc\"", "error: slot_timing: unknown key 'drift'"},
        {NULL, "{\"mode\": 1}", "error: slot_timing.mode: free or utc"},
        {NULL, "\"gps\"", "error: slot_timing.mode: free or utc"},
        {NULL, "{\"mode\": null}", "error: slot_timing.mode: free or utc"},
        {NULL, "{\"mode\": \"UTC\"}", "error: slot_timing.mode: free or utc"},
        {NULL, "{\"offset_us\": 15001}", "error: slot_timing.offset_us 15001: within +-15000 (half a slot)"},
        {NULL, "{\"offset_us\": -15001}", "error: slot_timing.offset_us -15001: within +-15000 (half a slot)"},
        {NULL, "{\"offset_us\": 2e9}", "error: slot_timing.offset_us 2000000000: within +-15000 (half a slot)"},
        {NULL, "{\"offset_us\": -2e9}", "error: slot_timing.offset_us -2000000000: within +-15000 (half a slot)"},
        {NULL, "{\"offset_us\": 2000000001}", "error: slot_timing.offset_us: whole microseconds"},
        {NULL, "{\"offset_us\": -1e308}", "error: slot_timing.offset_us: whole microseconds"},
        {NULL, "{\"offset_us\": 1.5}", "error: slot_timing.offset_us: whole microseconds"},
        {NULL, "{\"offset_us\": \"3\"}", "error: slot_timing.offset_us: whole microseconds"},
        {NULL, "{\"offset_us\": true}", "error: slot_timing.offset_us: whole microseconds"},
        {NULL, "{\"max_err_us\": 0}", "error: slot_timing.max_err_us 0: above 0, at most 15000"},
        {NULL, "{\"max_err_us\": -1}", "error: slot_timing.max_err_us -1: above 0, at most 15000"},
        {NULL, "{\"max_err_us\": 15001}", "error: slot_timing.max_err_us 15001: above 0, at most 15000"},
        {NULL, "{\"max_err_us\": {\"ntp\": 0}}", "error: slot_timing.max_err_us 0: above 0, at most 15000"},
        {NULL, "{\"max_err_us\": {\"gps\": 3}}", "error: slot_timing.max_err_us: unknown source 'gps' (pps, ntp)"},
        {NULL, "{\"max_err_us\": {\"pps\": 5, \"gps\": 3}}",
         "error: slot_timing.max_err_us: unknown source 'gps' (pps, ntp)"},
        {NULL, "{\"max_err_us\": {\"pps\": true}}", "error: slot_timing.max_err_us.pps: whole microseconds"},
        {NULL, "{\"max_err_us\": {\"ntp\": 1.5}}", "error: slot_timing.max_err_us.ntp: whole microseconds"},
        {NULL, "{\"max_err_us\": null}", "error: slot_timing.max_err_us: whole microseconds"},
        {NULL, "{\"max_err_us\": [5]}", "error: slot_timing.max_err_us: whole microseconds"},
        {NULL, "{\"on_loss\": \"panic\"}", "error: slot_timing.on_loss: holdover, free or stop"},
        {NULL, "{\"on_loss\": 1}", "error: slot_timing.on_loss: holdover, free or stop"},
        {NULL, "{\"holdover_s\": 86400.5}", "error: slot_timing.holdover_s: 0 to 86400 s"},
        {NULL, "{\"holdover_s\": -0.001}", "error: slot_timing.holdover_s: 0 to 86400 s"},
        {NULL, "{\"holdover_s\": true}", "error: slot_timing.holdover_s: 0 to 86400 s"},
        {NULL, "{\"holdover_s\": \"1\"}", "error: slot_timing.holdover_s: 0 to 86400 s"},
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        const char *got = st_parse(v[i].base, v[i].own);
        CHECK(!strcmp(got, v[i].want), "slot_timing %zu (%s over %s): %s", i, v[i].own ? v[i].own : "-",
              v[i].base ? v[i].base : "-", got);
    }
    /* a node the parse didn't make is none; the error cut to its buffer */
    jdoc_t j;
    const char *text = "{\"mode\": \"utc\", \"on_loss\": \"later\"}";
    int root = jparse(&j, text, strlen(text), 32, sizeof j.buf);
    rdof_slot_timing_t t;
    char why[8] = "xxxxxxx";
    CHECK(rdof_slot_timing_parse(&j.doc, j.doc.n, 999, &t, why, sizeof why) && !t.utc && t.holdover_s == 60.0,
          "slot_timing from nodes the parse didn't make");
    CHECK(!rdof_slot_timing_parse(&j.doc, -1, root, &t, why, sizeof why) && !strcmp(why, "slot_ti"), "error cut: %s",
          why);
    CHECK(!rdof_slot_timing_parse(&j.doc, -1, root, &t, why, 1) && why[0] == 0, "error in one byte");
    CHECK(!rdof_slot_timing_parse(&j.doc, root, -1, &t, NULL, 0), "error with no buffer");

    /* describe: repr(round(holdover_s, 3)) (from the reference); round(h * 1000) / 1000 rounded these twice */
    static const struct {
        double h;
        const char *want;
    } d[] = {
        {1.0005, "1.0"},         {0.0625, "0.062"},         {0.0015, "0.002"}, {2.5, "2.5"},
        {0.0, "0.0"},            {86400.0, "86400.0"},      {0.1234, "0.123"}, {276.7125, "276.712"},
        {15455.5075, "15455.507"}, {-0.0, "-0.0"},          {0.0005, "0.001"}, {59.9995, "59.999"},
        {INFINITY, "inf"},
    };
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) {
        rdof_slot_timing_default(&t);
        t.holdover_s = d[i].h;
        char out[64], want[64];
        snprintf(want, sizeof want, "free,0,20,2000,holdover,%s", d[i].want);
        rdof_slot_timing_describe(&t, out, sizeof out);
        CHECK(!strcmp(out, want), "describe holdover %.17g: %s", d[i].h, out);
    }
    rdof_slot_timing_default(&t);
    t.utc = true;
    t.offset_us = INT_MIN;
    t.max_err_pps_us = -1;
    t.max_err_ntp_us = INT_MAX;
    t.on_loss = (rdof_slot_loss_t)7;
    char out[64];
    const char *want = "utc,-2147483648,-1,2147483647,?,60.0";
    CHECK(rdof_slot_timing_describe(&t, out, sizeof out) == strlen(want) && !strcmp(out, want), "describe %s", out);
    CHECK(rdof_slot_timing_describe(&t, out, 5) == strlen(want) && !strcmp(out, "utc,"), "describe cut: %s", out);
    CHECK(rdof_slot_timing_describe(&t, NULL, 0) == strlen(want), "describe with no buffer");
}

static void t_slot_gate_edges(void)
{
    rdof_slot_timing_t t;
    rdof_slot_gate_t g;
    rdof_slot_timing_default(&t);
    rdof_slot_gate_init(&g, &t);
    CHECK(g.state == RDOF_GATE_FREE && !strcmp(g.why, "slot timing free") && !strcmp(g.source, "none") &&
              isnan(g.err_us) && !g.has_last_ok,
          "gate free: %s", g.why);
    CHECK(rdof_slot_gate_update(&g, 1, true, "pps", 1) == RDOF_GATE_FREE && !strcmp(g.source, "pps") && g.err_us == 1 &&
              !g.has_last_ok,
          "gate free stays free");
    t.utc = true;
    t.on_loss = RDOF_LOSS_STOP;
    rdof_slot_gate_init(&g, &t);
    CHECK(g.state == RDOF_GATE_STOPPED && !strcmp(g.why, "no clock yet") && !rdof_slot_gate_may_transmit(&g) &&
              !rdof_slot_gate_on_grid(&g),
          "gate stop before any clock");
    t.on_loss = RDOF_LOSS_HOLDOVER;
    t.holdover_s = 0;
    rdof_slot_gate_init(&g, &t);
    CHECK(g.state == RDOF_GATE_FREE && !strcmp(g.why, "no clock yet"), "gate holdover before any clock");
    /* the reasons, and the source cut to its char[8] */
    CHECK(rdof_slot_gate_update(&g, 5, false, "pps", 1) == RDOF_GATE_FREE && !strcmp(g.why, "the clock isn't synced"),
          "gate unsynced: %s", g.why);
    CHECK(rdof_slot_gate_update(&g, 5, true, NULL, 1) == RDOF_GATE_FREE && !strcmp(g.source, "none") &&
              !strcmp(g.why, "a none clock can't time slots"),
          "gate NULL source: %s / %s", g.source, g.why);
    CHECK(rdof_slot_gate_update(&g, 5, true, "browser-ntp", 1) == RDOF_GATE_FREE && !strcmp(g.source, "browser") &&
              !strcmp(g.why, "a browser-ntp clock can't time slots"),
          "gate long source: %s / %s", g.source, g.why);
    char longsrc[100];
    memset(longsrc, 'p', sizeof longsrc - 1);
    longsrc[sizeof longsrc - 1] = 0;
    rdof_slot_gate_update(&g, 5, true, longsrc, 1);
    CHECK(strlen(g.why) == sizeof g.why - 1 && !strncmp(g.why, "a ppp", 5), "gate why cut: %zu", strlen(g.why));
    CHECK(rdof_slot_gate_update(&g, 5, true, "ntp", NAN) == RDOF_GATE_FREE && !strcmp(g.why, "no error estimate"),
          "gate no estimate: %s", g.why);
    CHECK(rdof_slot_gate_update(&g, 5, true, "pps", 20.000001) == RDOF_GATE_FREE &&
              !strcmp(g.why, "clock error over 20 us"),
          "gate over: %s", g.why);
    CHECK(rdof_slot_gate_update(&g, 5, true, "ntp", INFINITY) == RDOF_GATE_FREE &&
              !strcmp(g.why, "clock error over 2000 us"),
          "gate infinite error: %s", g.why);
    CHECK(rdof_slot_gate_holdover_age(&g, 9) == 0, "gate age before any clock");
    /* at the bound is good; a negative estimate is too */
    CHECK(rdof_slot_gate_update(&g, 10, true, "pps", 20) == RDOF_GATE_ALIGNED && g.why[0] == 0 && g.last_ok == 10 &&
              rdof_slot_gate_holdover_age(&g, 99) == 0 && rdof_slot_gate_on_grid(&g),
          "gate at the bound");
    CHECK(rdof_slot_gate_update(&g, 11, true, "ntp", -5) == RDOF_GATE_ALIGNED && g.last_ok == 11, "gate negative error");
    /* holdover_s 0: held at the same moment, free just after */
    CHECK(rdof_slot_gate_update(&g, 11, false, "ntp", NAN) == RDOF_GATE_HOLDOVER && rdof_slot_gate_on_grid(&g) &&
              rdof_slot_gate_holdover_age(&g, 11) == 0,
          "gate holdover 0 at once");
    CHECK(rdof_slot_gate_update(&g, 11.001, false, "ntp", NAN) == RDOF_GATE_FREE &&
              fabs(rdof_slot_gate_holdover_age(&g, 12) - 1) < 1e-12,
          "gate holdover 0 after");
    /* on_loss free: free at once, after being aligned */
    t.on_loss = RDOF_LOSS_FREE;
    t.holdover_s = 86400;
    rdof_slot_gate_init(&g, &t);
    rdof_slot_gate_update(&g, 0, true, "pps", 0);
    CHECK(rdof_slot_gate_update(&g, 0, true, "pps", 21) == RDOF_GATE_FREE && rdof_slot_gate_may_transmit(&g),
          "gate on_loss free");
    /* on_loss stop after aligned */
    t.on_loss = RDOF_LOSS_STOP;
    rdof_slot_gate_init(&g, &t);
    rdof_slot_gate_update(&g, 0, true, "pps", 0);
    CHECK(rdof_slot_gate_update(&g, 1, false, "pps", 0) == RDOF_GATE_STOPPED && rdof_slot_gate_holdover_age(&g, 3) == 3,
          "gate on_loss stop");
    CHECK(!strcmp(rdof_gate_state_name(RDOF_GATE_STOPPED), "stopped") && !strcmp(rdof_gate_state_name(RDOF_GATE_FREE), "free") &&
              !strcmp(rdof_gate_state_name((rdof_gate_state_t)4), "?") &&
              !strcmp(rdof_gate_state_name((rdof_gate_state_t)-1), "?"),
          "gate state names");
}

static void t_sample_clock_edges(void)
{
    rdof_sample_clock_t c;
    int64_t at;
    double idx;
    rdof_sample_clock_init(&c, 1000, 1.0, false);
    CHECK(!rdof_sample_clock_utc(&c, 5, &at) && !rdof_sample_clock_index(&c, 5, &idx), "sample clock before a stamp");
    /* scripted arrivals (from the reference): late ones, one earlier than the line, a window ending exactly on time */
    static const struct {
        int64_t end, arrival, i0, t0;
        int has_err;
        int64_t err;
        uint32_t windows;
        int64_t utc_half; /* utc of end + 0.5 */
    } v[] = {
        {100, 1000000000LL, 100, 1000000000LL, 0, 0LL, 0, 1000500000LL},
        {200, 1107000000LL, 100, 1000000000LL, 0, 0LL, 0, 1100500000LL},
        {300, 1202000000LL, 100, 1000000000LL, 0, 0LL, 0, 1200500000LL},
        {400, 1290000000LL, 400, 1290000000LL, 0, 0LL, 0, 1290500000LL},
        {500, 1401000000LL, 400, 1290000000LL, 0, 0LL, 0, 1390500000LL},
        {1290, 2195000000LL, 400, 1290000000LL, 1, 10000000LL, 1, 2180500000LL},
        {1300, 2203000000LL, 400, 1290000000LL, 1, 10000000LL, 1, 2190500000LL},
        {2290, 3190000000LL, 2290, 3190000000LL, 1, 10000000LL, 2, 3190500000LL},
        {2300, 3150000000LL, 2300, 3150000000LL, 1, 10000000LL, 2, 3150500000LL},
        {2400, 3300000000LL, 2300, 3150000000LL, 1, 10000000LL, 2, 3250500000LL},
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        rdof_sample_clock_add(&c, v[i].end, v[i].arrival);
        bool ok = rdof_sample_clock_utc(&c, (double)v[i].end + 0.5, &at);
        CHECK(ok && c.i0 == v[i].i0 && c.t0 == v[i].t0 && c.has_err == (bool)v[i].has_err &&
                  (!c.has_err || c.err_ns == v[i].err) && c.windows == v[i].windows && at == v[i].utc_half,
              "sample clock step %zu: anchor %lld %lld err %d %lld windows %u utc %lld", i, (long long)c.i0,
              (long long)c.t0, c.has_err, (long long)c.err_ns, c.windows, (long long)at);
    }
    /* the window count wraps as a uint32_t */
    c.windows = UINT32_MAX;
    rdof_sample_clock_add(&c, 3300, 4300000000LL);
    CHECK(c.windows == 0 && c.err_ns == 50000000, "sample clock windows wrap: %u %lld", c.windows,
          (long long)c.err_ns);
    /* window 0: every chunk ends one */
    rdof_sample_clock_init(&c, 1000, 0, false);
    rdof_sample_clock_add(&c, 10, 1000);
    rdof_sample_clock_add(&c, 20, 500); /* 10000500 ns before the line */
    CHECK(c.windows == 1 && c.has_err && c.err_ns == 10000500 && c.i0 == 20 && c.t0 == 500 && !c.has_best &&
              c.moved == 0 && c.win_start == 20,
          "sample clock window 0: %lld", (long long)c.err_ns);
    /* stamped: each stamp is the line */
    rdof_sample_clock_init(&c, 48000, 2.0, true);
    rdof_sample_clock_add(&c, 4800, 1000000000LL);
    rdof_sample_clock_add(&c, 9600, 1050000000LL);
    CHECK(c.i0 == 9600 && c.t0 == 1050000000LL && c.has_err && c.err_ns == 0 && c.windows == 0 && !c.has_best,
          "sample clock stamped");
    c.ppm = 1e6; /* twice the rate */
    CHECK(rdof_sample_clock_utc(&c, 9600 + 96000, &at) && at == 1050000000LL + 1000000000LL, "ppm: %lld",
          (long long)at);
    CHECK(rdof_sample_clock_index(&c, 1050000000LL + 500000000LL, &idx) && idx == 9600 + 48000, "ppm index %f", idx);
    /* out of range: a rate of 0 (NaN at the anchor, +-inf off it), times past int64 saturate, then wrap */
    rdof_sample_clock_init(&c, 0, 2.0, true);
    rdof_sample_clock_add(&c, 100, 5000);
    CHECK(rdof_sample_clock_utc(&c, 100, &at) && at == 5000, "rate 0 at the anchor: %lld", (long long)at);
    CHECK(rdof_sample_clock_utc(&c, 101, &at) && at == INT64_MIN + 4999, "rate 0 after: %lld", (long long)at);
    CHECK(rdof_sample_clock_utc(&c, 99, &at) && at == INT64_MIN + 5000, "rate 0 before: %lld", (long long)at);
    rdof_sample_clock_init(&c, 48000, 2.0, true);
    rdof_sample_clock_add(&c, 0, -1);
    CHECK(rdof_sample_clock_utc(&c, 1e300, &at) && at == INT64_MAX - 1, "far index: %lld", (long long)at);
    CHECK(rdof_sample_clock_utc(&c, NAN, &at) && at == -1, "NaN index: %lld", (long long)at);
    CHECK(rdof_sample_clock_index(&c, INT64_MAX, &idx) && idx == (double)INT64_MIN * 48000 / 1e9, "index wrap: %g",
          idx);
    /* an arrival INT64_MIN ns before the line: the anchor moves; the move (-INT64_MIN) wraps to nothing */
    rdof_sample_clock_init(&c, 1000, 10, false);
    rdof_sample_clock_add(&c, 0, 0);
    rdof_sample_clock_add(&c, 0, INT64_MIN);
    CHECK(c.t0 == INT64_MIN && c.moved == 0 && c.best_r == 0, "sample clock far early: %lld", (long long)c.moved);
}

/* ── box config: each check, and the value conversions (int, float, string) ── */

/* Each template takes two strings: extra top-level keys, then extra keys on the radio. A repeated key overrides
   the template's (the last one wins). */
enum { T_ROAM, T_ROAM_SDR, T_REP };
static const char *const TMPL[] = {
    "{\"name\": \"b\", \"cc\": 1, \"channels\": {\"1\": 461.0375, \"2\": 461.0875}%s,"
    " \"radios\": [{\"type\": \"opengd77\", \"name\": \"R\", \"peer_id\": 1, \"idle\": 1%s}]}",
    "{\"name\": \"b\", \"cc\": 1, \"channels\": {\"1\": 461.0375, \"2\": 461.0875}%s,"
    " \"radios\": [{\"type\": \"opengd77\", \"name\": \"R\", \"peer_id\": 1, \"idle\": 1%s},"
    " {\"type\": \"sdr\", \"name\": \"S\", \"channels\": [2, 1]}]}",
    "{\"name\": \"b\", \"cc\": 1, \"channels\": {\"1\": 461.0375}%s,"
    " \"radios\": [{\"type\": \"repeater\", \"name\": \"R\", \"peer_id\": 3%s}]}",
};
#define FREQS ", \"rx_freq\": 461.0375, \"tx_freq\": 466.0375"
#define SPLIT(tx, rx) FREQS ", \"radio\": {\"type\": \"split\", \"tx\": " tx ", \"rx\": " rx

static bool cfg_try(int t, const char *top, const char *radio, rdof_config_t *cfg, char *error, size_t cap)
{
    static char json[4096];
    snprintf(json, sizeof json, TMPL[t], top, radio);
    return rdof_config_parse(json, strlen(json), cfg, error, cap);
}

/* mode dcdm: one frequency, both slots, a repeater's settings; an sdr_transceiver half or full duplex takes it too. */
static void t_config_dcdm(void)
{
    static rdof_config_t cfg;
    char error[160];
    bool ok = cfg_try(T_REP, "", ", \"mode\": \"dcdm\", \"freq\": 461.0375", &cfg, error, sizeof error);
    CHECK(ok && cfg.mode == RDOF_CFG_REPEATER && cfg.dcdm && !cfg.sfr && cfg.rx_freq == 461037500 &&
              cfg.tx_freq == 461037500 && cfg.slot1 && cfg.slot2,
          "dcdm: %s", ok ? "" : error);
    ok = cfg_try(T_REP, "", ", \"mode\": \"dcdm\", \"freq\": 461.0375, \"slots\": [2], \"radio\": {\"type\": \"opengd77\"}",
                 &cfg, error, sizeof error);
    CHECK(ok && cfg.dcdm && !cfg.slot1 && cfg.slot2 && !strcmp(cfg.radio_type, "radio_hotspot"),
          "dcdm on a radio_hotspot, slot 2 only: %s", ok ? "" : error);
    ok = cfg_try(T_REP, "", FREQS, &cfg, error, sizeof error);
    CHECK(ok && !cfg.dcdm, "duplex isn't dcdm");
    for (int full = 0; full < 2; full++) {
        ok = cfg_try(T_ROAM_SDR, "",
                     full ? ", \"type\": \"sdr_transceiver\", \"duplex\": \"full\", \"mode\": \"dcdm\", \"freq\": 461.0375"
                          : ", \"type\": \"sdr_transceiver\", \"mode\": \"dcdm\", \"freq\": 461.0375",
                     &cfg, error, sizeof error);
        CHECK(ok && cfg.mode == RDOF_CFG_REPEATER && cfg.dcdm && !strcmp(cfg.modem_port, "sdr") &&
                  cfg.sdr_duplex == (full ? RDOF_SDR_DUPLEX_FULL : RDOF_SDR_DUPLEX_HALF),
              "sdr_transceiver dcdm (full %d): %s", full, ok ? "" : error);
    }
}

static void t_config_errors(void)
{
    static const struct {
        int t;
        const char *top, *radio, *want;
    } bad[] = {
        /* the plan */
        {T_ROAM, ", \"cc\": \"one\"", "", "cc isn't a number"},
        {T_ROAM, ", \"channels\": {\"x\": 461}", "", "channel x: not a number"},
        {T_ROAM, ", \"channels\": {\"3\": {\"cc\": 1}}", "", "channel 3: no freq"},
        {T_ROAM, ", \"channels\": {\"3\": \"high\"}", "", "channel: bad frequency"},
        {T_ROAM, ", \"channels\": {\"3\": {\"freq\": 461.2, \"power\": \"x\"}}", "", "channel: power isn't a number"},
        {T_ROAM, ", \"cc\": null, \"channels\": {\"1\": 461.0}", "",
         "channel 1: no color code (set cc on it or at the top)"},
        /* a roamer's channels and idle */
        {T_ROAM, "", ", \"channels\": 5", "channels: numbers, or all"},
        {T_ROAM, "", ", \"channels\": [{\"cc\": 2}]", "a channel entry needs ch"},
        {T_ROAM, "", ", \"channels\": [[1]]", "bad channel reference"},
        {T_ROAM, "", ", \"channels\": [9]", "channel 9 isn't in the plan (channels:)"},
        {T_ROAM, "", ", \"channels\": [1, {\"ch\": 2, \"power\": \"x\"}]", "channel: power isn't a number"},
        {T_ROAM, "", ", \"channels\": [{\"ch\": 1, \"cc\": 16}]", "color code 16"},
        {T_ROAM, "", ", \"channels\": [{\"ch\": 1, \"cc\": 1.5}]", "color code 1.5"},
        {T_ROAM, "", ", \"idle\": {\"freq\": 461.1}", "idle: freq and cc"},
        {T_ROAM, "", ", \"idle\": {\"freq\": 461.1, \"cc\": 16}", "color code 16"},
        {T_ROAM, "", ", \"channels\": [], \"idle\": {\"freq\": 461.1, \"cc\": 1}",
         "no channels: list the simplex channels this radio may use"},
        {T_ROAM, "", ", \"idle\": 7", "channel 7 isn't in the plan (channels:)"},
        /* a roamer's numbers */
        {T_ROAM, "", ", \"peer_id\": \"x\"", "no peer_id"},
        {T_ROAM, "", ", \"listen_s\": \"soon\"", "radio: listen_s isn't a number"},
        {T_ROAM, "", ", \"prebuffer\": \"6.5\"", "radio: prebuffer isn't a whole number"},
        {T_ROAM, "", ", \"csbk_preambles\": [1]", "radio: csbk_preambles isn't a number"},
        /* scan */
        {T_ROAM, "", ", \"scan\": {\"channels\": [1, 2], \"dwell_s\": 0}",
         "scan: dwell_s > 0, hang_s >= 0, and carrier_s (0 = off) under dwell_s"},
        {T_ROAM, "", ", \"scan\": {\"carrier_s\": 1}",
         "scan: dwell_s > 0, hang_s >= 0, and carrier_s (0 = off) under dwell_s"},
        {T_ROAM, "", ", \"scan\": {\"channels\": [9]}", "channel 9 isn't in the plan (channels:)"},
        {T_ROAM, "", ", \"scan\": {\"channels\": [{\"ch\": 1, \"cc\": \"x\"}]}", "scan: cc isn't a number"},
        {T_ROAM, "", ", \"scan\": {\"channels\": [{\"ch\": 1, \"cc\": 20}]}", "color code 20"},
        {T_ROAM, "", ", \"scan\": {\"channels\": []}", "scan: no channels"},
        {T_ROAM, "", ", \"scan\": {\"dwell_s\": \"x\"}", "scan: dwell_s isn't a number"},
        /* radio settings */
        {T_ROAM, "", ", \"rx_level\": \"loud\"", "radio.rx_level isn't a number"},
        {T_ROAM, "", ", \"radio\": {\"tuner\": {\"type\": \"fixed\", \"freq\": \"x\"}}", "radio.tuner.freq"},
        {T_ROAM, "", ", \"radio\": {\"tuner\": \"rigctld\"}",
         "radio.tuner rigctld: not on this board (modem or fixed only)"},
        {T_ROAM, "", ", \"radio\": {\"tuner\": \"magic\"}", "radio.tuner magic"},
        {T_ROAM, "", ", \"radio\": {\"rssi\": 5}", "radio.rssi wants [[raw, dBm], ...]"},
        {T_ROAM, "", ", \"radio\": {\"rssi\": [[1]]}", "radio.rssi wants [[raw, dBm], ...]"},
        {T_ROAM, "", ", \"radio\": {\"protocol\": \"UDP\"}", "radio.protocol udp needs radio.modem_address"},
        {T_ROAM, "", ", \"radio\": {\"protocol\": \"udp\", \"modem_address\": \"10.0.0.9\"}",
         "radio.protocol udp: not on this board"},
        {T_ROAM, "", ", \"radio\": {\"protocol\": \"tcp\"}", "radio.protocol tcp: uart or udp"},
        /* location */
        {T_ROAM, "", ", \"location\": {\"lat\": 1}", "location: latitude and longitude"},
        {T_ROAM, ", \"location\": {\"lat\": 91, \"lon\": 0}", "", "location 91, 0 out of range"},
        {T_ROAM, "", ", \"location\": {\"lat\": 41, \"lon\": -81, \"height\": \"tall\"}",
         "location: height isn't a number"},
        /* the port, MQTT, HBlink4, the OLED */
        {T_ROAM, "", ", \"uart\": {\"num\": 3}", "uart.num 3: 1 or 2 (UART0 is the console)"},
        {T_ROAM, "", ", \"uart\": {\"tx\": \"a\"}", "uart: tx isn't a whole number"},
        {T_ROAM, ", \"uart\": {\"rx\": [1]}", "", "uart: rx isn't a number"},
        {T_ROAM, "", ", \"uart\": {\"num\": \"x\"}", "uart: num isn't a whole number"},
        {T_ROAM, "", ", \"speed\": \"fast\"", "radio: speed isn't a whole number"},
        {T_ROAM, ", \"mqtt\": {\"port\": \"x\"}", "", "mqtt: port isn't a whole number"},
        {T_ROAM, ", \"hblink\": {\"port\": \"x\"}", "", "hblink: port isn't a whole number"},
        {T_ROAM, ", \"oled\": {\"address\": \"x\"}", "", "oled: address isn't a whole number"},
        {T_ROAM, ", \"oled\": {\"i2c\": {\"address\": []}}", "", "oled: address isn't a number"},
        {T_ROAM, ", \"oled\": {\"refresh_s\": \"x\"}", "", "oled: refresh_s isn't a number"},
        {T_ROAM, ", \"oled\": {\"sda\": \"x\"}", "", "oled: sda isn't a whole number"},
        {T_ROAM, ", \"oled\": {\"scl\": \"x\"}", "", "oled: scl isn't a whole number"},
        /* which radio */
        {T_ROAM, ", \"run\": [\"R\", 5]", "", "run: a radio's name or a list of names"},
        {T_ROAM_SDR, "", ", \"type\": \"sdr_transceiver\", \"tx_power_dbm\": \"x\"",
         "radio: tx_power_dbm isn't a number"},
        {T_ROAM_SDR, "", ", \"type\": \"sdr_transceiver\", \"duplex\": \"full\", \"mode\": \"sfr\"",
         "an sdr_transceiver with duplex: full is a duplex repeater (no mode: sfr)"},
        /* a repeater */
        {T_REP, "", "", "no rx_freq (MHz)"},
        {T_REP, "", ", \"rx_freq\": 461", "no tx_freq (MHz)"},
        {T_REP, "", ", \"rx_freq\": \"x\", \"tx_freq\": 466", "rx_freq: bad frequency"},
        {T_REP, "", ", \"mode\": \"sfr\"", "no freq (MHz)"},
        {T_REP, "", ", \"mode\": \"sfr\", \"freq\": \"x\"", "freq: bad frequency"},
        {T_REP, "", ", \"mode\": \"sfr\", \"freq\": 461.0375, \"inbound_slot\": \"x\"",
         "radio: inbound_slot isn't a whole number"},
        {T_REP, "", ", \"mode\": \"sfr\", \"freq\": 461.0375, \"inbound_slot\": 3", "inbound_slot: 1 or 2"},
        {T_REP, "", ", \"mode\": \"sfr\", \"freq\": 461.0375, \"radio\": {\"type\": \"mmdvm\"}",
         "radio.type mmdvm: a single frequency repeater is a radio_hotspot (a radio in hotspot mode with SFR "
         "firmware)"},
        {T_REP, "", FREQS ", \"mode\": \"simplex\"", "mode simplex: duplex, sfr or dcdm"},
        {T_REP, "", ", \"mode\": \"dcdm\"", "no freq (MHz)"},
        {T_REP, "", ", \"mode\": \"dcdm\", \"freq\": 461.0375, \"radio\": {\"type\": \"split\"}",
         "radio.type split: a split is a duplex pair; dual capacity direct mode is one radio on one frequency"},
        {T_REP, "", ", \"mode\": \"dcdm\", \"freq\": 461.0375, \"slot_timing\": \"utc\"",
         "slot_timing utc: in dual capacity direct mode the modem times its slots from the channel"},
        {T_REP, "", FREQS ", \"cc\": \"x\"", "radio: cc isn't a whole number"},
        {T_REP, ", \"cc\": null", FREQS, "channel 1: no color code (set cc on it or at the top)"},
        {T_REP, "", FREQS ", \"cc\": 16", "color code 16"},
        {T_REP, "", FREQS ", \"peer_id\": [3]", "no peer_id"},
        {T_REP, "", FREQS ", \"power\": \"x\"", "radio: power isn't a whole number"},
        {T_REP, "", FREQS ", \"hang_s\": \"x\"", "radio: hang_s isn't a number"},
        {T_REP, "", FREQS ", \"prebuffer\": \"x\"", "radio: prebuffer isn't a whole number"},
        {T_REP, "", FREQS ", \"hang_s\": 0", "hang_s > 0 and call_hang_s >= 0"},
        {T_REP, "", FREQS ", \"slots\": [3]", "slots: 1, 2 or both"},
        {T_REP, "", FREQS ", \"slots\": []", "slots: 1, 2 or both"},
        {T_REP, "", FREQS ", \"radio\": {\"type\": \"yaesu\"}",
         "radio type yaesu: auto or one of radio_hotspot, mmdvm_hs, mmdvm, split"},
        {T_REP, "", FREQS ", \"radio\": {\"type\": \"opengd77\"}",
         "a radio_hotspot (a radio in hotspot mode) is simplex: a repeater needs a duplex MMDVM_HS board, an MMDVM "
         "board or two radios as a split (but see mode: sfr)"},
        {T_REP, "", FREQS ", \"radio\": {\"rx_level\": \"x\"}", "radio.rx_level isn't a number"},
        /* a split's halves */
        {T_REP, "", FREQS ", \"radio\": {\"type\": \"split\"}", "radio type split needs radio.tx: the TX half's port"},
        {T_REP, "", SPLIT("{}", "5") "}", "radio type split needs radio.rx: the RX half's port"},
        {T_REP, "", SPLIT("{\"type\": \"mmdvm\"}", "{}") "}",
         "radio.tx.type mmdvm: a split half is a radio in hotspot mode with the split duplex extension "
         "(OpenDigitalRadio)"},
        {T_REP, "", SPLIT("{\"protocol\": \"tcp\"}", "{}") "}", "radio.tx.protocol tcp: uart or udp"},
        {T_REP, "", SPLIT("{}", "{\"protocol\": \"UDP\"}") "}", "radio.rx.protocol udp needs radio.rx.modem_address"},
        {T_REP, "", SPLIT("{\"protocol\": \"udp\", \"modem_address\": \"h\"}", "{}") "}",
         "radio.tx.protocol udp: not on this board"},
        {T_REP, "", SPLIT("{\"modem\": \"wifi\"}", "{}") "}",
         "radio.tx.modem wifi: usb, uart, or sdr (the board's SDR modem)"},
        {T_REP, "", SPLIT("{\"uart\": {\"tx\": \"a\"}}", "{}") "}", "uart: tx isn't a whole number"},
        {T_REP, "", SPLIT("{\"uart\": {\"rx\": \"a\"}}", "{}") "}", "uart: rx isn't a whole number"},
        {T_REP, "", SPLIT("{\"uart\": {\"num\": \"a\"}}", "{}") "}", "uart: num isn't a whole number"},
        {T_REP, "", SPLIT("{}", "{\"modem\": \"uart\", \"uart\": {\"num\": 3}}") "}",
         "radio.rx.uart.num 3: 1 or 2 (UART0 is the console)"},
        {T_REP, "", SPLIT("{\"speed\": \"x\"}", "{}") "}", "radio: speed isn't a whole number"},
        {T_REP, "", SPLIT("{\"modem\": \"uart\"}", "{\"modem\": \"uart\"}") "}",
         "radio.tx and radio.rx are both on UART1: give one \"uart\": {\"num\": 2, ...}"},
        {T_REP, "", SPLIT("{\"modem\": \"uart\"}", "{\"modem\": \"uart\", \"uart\": {\"num\": 2}}") "}",
         "radio.tx and radio.rx share a UART pin"},
        {T_REP, "", SPLIT("{\"modem\": \"uart\"}", "{\"modem\": \"uart\", \"uart\": {\"num\": 2, \"tx\": 6, \"rx\": 7}}") "}",
         "radio.tx and radio.rx share a UART pin"},
        {T_REP, "", SPLIT("{}", "{\"modem\": \"uart\"}") ", \"power_cap\": 300}", "radio.power_cap: 0 to 255"},
        {T_REP, "", SPLIT("{}", "{\"modem\": \"uart\"}") ", \"lease_s\": \"x\"}", "radio.lease_s: 0 to 255"},
        {T_REP, "", SPLIT("{}", "{\"modem\": \"uart\"}") ", \"max_key_s\": 700}", "radio.max_key_s: 0 to 600"},
        /* NaN, hex, huge numbers, and keys that aren't integers */
        {T_REP, "", SPLIT("{}", "{\"modem\": \"uart\"}") ", \"power_cap\": \"nan\"}", "radio.power_cap: 0 to 255"},
        {T_REP, "", SPLIT("{}", "{\"modem\": \"uart\"}") ", \"max_key_s\": \"nan\"}", "radio.max_key_s: 0 to 600"},
        {T_ROAM, "", ", \"location\": {\"lat\": \"nan\", \"lon\": 0}", "location nan, 0 out of range"},
        {T_ROAM, "", ", \"listen_s\": \"0x10\"", "radio: listen_s isn't a number"},
        {T_ROAM, "", ", \"radio\": {\"tuner\": {\"type\": \"fixed\", \"settle_ms\": \"x\"}}", "tuner: settle_ms isn't a number"},
        {T_ROAM, ", \"channels\": {\"\": 461}", "", "channel : not a number"},
        {T_ROAM, ", \"channels\": {\"4294967297\": 461}", "", "channel 4294967297: not a number"},
        {T_ROAM, "", ", \"channels\": [4294967297]", "channel 4294967297 isn't in the plan (channels:)"},
        {T_ROAM, "", ", \"channels\": [1e300]", "channel 9223372036854775807 isn't in the plan (channels:)"},
        {T_ROAM, "", ", \"channels\": [-1e300]", "channel -9223372036854775808 isn't in the plan (channels:)"},
        {T_ROAM, ", \"channels\": {\"1\": {\"freq\": 461.0375, \"cc\": 1e300}}", "", "color code 2.14748e+09"},
        {T_ROAM, "", ", \"idle\": {\"freq\": 461.1, \"cc\": 1e300}", "color code 9.22337e+18"},
        {T_REP, "", FREQS ", \"cc\": 1e300", "color code 9223372036854775807"},
        {T_REP, "", FREQS ", \"slots\": [1e300]", "slots: 1, 2 or both"},
        {T_REP, ", \"cc\": null, \"channels\": {\"1\": {\"freq\": 461.0375, \"cc\": 1}}", FREQS,
         "no color code (set cc on it or at the top)"},
        {T_ROAM, "", ", \"channels\": [\"nan\"]", "channel 0 isn't in the plan (channels:)"},
        /* each side of each check */
        {T_ROAM, "", ", \"prebuffer\": \"\"", "radio: prebuffer isn't a whole number"},
        /* the jitter buffer */
        {T_ROAM, "", ", \"jitter_buffer\": \"x\"", "radio: jitter_buffer: \"auto\" or milliseconds, 0 to 3000"},
        {T_ROAM, "", ", \"jitter_buffer\": true", "radio: jitter_buffer: \"auto\" or milliseconds, 0 to 3000"},
        {T_ROAM, "", ", \"jitter_buffer\": -1", "radio: jitter_buffer: \"auto\" or milliseconds, 0 to 3000"},
        {T_ROAM, "", ", \"jitter_buffer\": 3000.5", "radio: jitter_buffer: \"auto\" or milliseconds, 0 to 3000"},
        {T_ROAM, "", ", \"jitter_buffer\": null", "radio: jitter_buffer: \"auto\" or milliseconds, 0 to 3000"},
        {T_REP, "", FREQS ", \"jitter_buffer\": \"Auto\"", "radio: jitter_buffer: \"auto\" or milliseconds, 0 to 3000"},
        {T_ROAM, "", ", \"jitter_buffer\": 300, \"prebuffer\": 5",
         "radio: prebuffer and jitter_buffer: set one (prebuffer is a fixed size in bursts)"},
        {T_REP, "", FREQS ", \"jitter_buffer\": \"auto\", \"prebuffer\": 5",
         "radio: prebuffer and jitter_buffer: set one (prebuffer is a fixed size in bursts)"},
        {T_ROAM, "", ", \"jitter_min_ms\": \"x\"", "radio: jitter_min_ms isn't a number"},
        {T_ROAM, "", ", \"jitter_max_ms\": \"x\"", "radio: jitter_max_ms isn't a number"},
        {T_ROAM, "", ", \"jitter_min_ms\": 59.9", "radio: jitter_min_ms and jitter_max_ms: 60 <= min <= max <= 3000"},
        {T_ROAM, "", ", \"jitter_min_ms\": 500", "radio: jitter_min_ms and jitter_max_ms: 60 <= min <= max <= 3000"},
        {T_REP, "", FREQS ", \"jitter_max_ms\": 3001", "radio: jitter_min_ms and jitter_max_ms: 60 <= min <= max <= 3000"},
        {T_REP, "", FREQS ", \"jitter_min_ms\": \"nan\"", "radio: jitter_min_ms and jitter_max_ms: 60 <= min <= max <= 3000"},
        {T_ROAM, "", ", \"channels\": [{\"ch\": \"x\"}]", "a channel entry needs ch"},
        {T_ROAM, "", ", \"channels\": [{\"ch\": 1, \"cc\": \"x\"}]", "channel: cc isn't a number"},
        {T_ROAM, "", ", \"idle\": {\"freq\": \"x\", \"cc\": 1}", "idle: freq and cc"},
        {T_ROAM, ", \"channels\": {\"1\": {\"freq\": 461.0375, \"cc\": 16}}", ", \"channels\": []", "color code 16"},
        {T_ROAM, "", ", \"location\": {\"lon\": 1}", "location: latitude and longitude"},
        {T_ROAM, "", ", \"location\": {\"lat\": -91, \"lon\": 0}", "location -91, 0 out of range"},
        {T_ROAM, "", ", \"location\": {\"lat\": 0, \"lon\": 181}", "location 0, 181 out of range"},
        {T_ROAM, "", ", \"location\": {\"lat\": 0, \"lon\": -181}", "location 0, -181 out of range"},
        {T_ROAM, "", ", \"radio\": {\"tuner\": \"command\"}", "radio.tuner command: not on this board (modem or fixed only)"},
        {T_ROAM, "", ", \"radio\": {\"rssi\": [5]}", "radio.rssi wants [[raw, dBm], ...]"},
        {T_ROAM, "", ", \"radio\": {\"rssi\": [[1, 2, 3]]}", "radio.rssi wants [[raw, dBm], ...]"},
        {T_ROAM, "", ", \"radio\": {\"rssi\": [[\"x\", 2]]}", "radio.rssi wants [[raw, dBm], ...]"},
        {T_ROAM, "", ", \"radio\": {\"rssi\": [[1, \"x\"]]}", "radio.rssi wants [[raw, dBm], ...]"},
        {T_ROAM, "", ", \"scan\": {\"hang_s\": \"x\"}", "scan: hang_s isn't a number"},
        {T_ROAM, "", ", \"scan\": {\"carrier_s\": \"x\"}", "scan: carrier_s isn't a number"},
        {T_ROAM, "", ", \"scan\": {\"hang_s\": -1}",
         "scan: dwell_s > 0, hang_s >= 0, and carrier_s (0 = off) under dwell_s"},
        {T_ROAM, "", ", \"scan\": {\"carrier_s\": -0.1}",
         "scan: dwell_s > 0, hang_s >= 0, and carrier_s (0 = off) under dwell_s"},
        {T_REP, "", ", \"rx_freq\": 461, \"tx_freq\": \"x\"", "tx_freq: bad frequency"},
        {T_REP, "", FREQS ", \"cc\": -1", "color code -1"},
        {T_REP, "", FREQS ", \"call_hang_s\": \"x\"", "radio: call_hang_s isn't a number"},
        {T_REP, "", FREQS ", \"cwid_every_s\": \"x\"", "radio: cwid_every_s isn't a number"},
        {T_REP, "", FREQS ", \"call_hang_s\": -1", "hang_s > 0 and call_hang_s >= 0"},
        {T_REP, "", FREQS ", \"slots\": 2", "slots: 1, 2 or both"},
        {T_REP, "", FREQS ", \"slots\": [\"x\"]", "slots: 1, 2 or both"},
        {T_REP, "", SPLIT("{\"modem\": \"uart\"}", "{\"modem\": \"uart\", \"uart\": {\"num\": 2, \"tx\": 7, \"rx\": 5}}") "}",
         "radio.tx and radio.rx share a UART pin"},
        {T_REP, "", SPLIT("{\"modem\": \"uart\"}", "{\"modem\": \"uart\", \"uart\": {\"num\": 2, \"tx\": 8, \"rx\": 6}}") "}",
         "radio.tx and radio.rx share a UART pin"},
        {T_REP, "", SPLIT("{}", "{\"modem\": \"uart\"}") ", \"max_key_s\": \"x\"}", "radio.max_key_s: 0 to 600"},
    };
    static rdof_config_t cfg;
    char error[200];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bool ok = cfg_try(bad[i].t, bad[i].top, bad[i].radio, &cfg, error, sizeof error);
        CHECK(!ok && !strcmp(error, bad[i].want), "config %zu (%s%s): want \"%s\", got %s", i, bad[i].top,
              bad[i].radio, bad[i].want, ok ? "ok" : error);
    }
    /* not JSON, not an object, and no error buffer */
    CHECK(!rdof_config_parse("{", 1, &cfg, error, sizeof error) && strstr(error, "JSON:"), "bad JSON: %s", error);
    CHECK(!rdof_config_parse("[1]", 3, &cfg, error, sizeof error) && !strcmp(error, "the settings are a JSON object"), "array: %s", error);
    CHECK(!rdof_config_parse("[1]", 3, &cfg, NULL, 0), "no error buffer");
    /* two SDRs named in run; an SDR's channel that isn't in the plan */
    static const char *const whole[][2] = {
        {"{\"cc\": 1, \"run\": [\"S\", \"T\"], \"radios\": [{\"type\": \"sdr\", \"name\": \"S\"}, {\"type\": \"sdr\","
         " \"name\": \"T\"}]}", "run: one SDR a board"},
        {"{\"radios\": {\"a\": 1}}", "radios: a list"},
        {"{\"radios\": \"R\"}", "radios: a list"},
        {"{\"radios\": null}", "no radios"},
        {"{\"cc\": 1}", "no radios"},
        {"{\"cc\": 1, \"channels\": {\"1\": 461.0375}, \"radios\": [{\"type\": \"mmdvm_hs\", \"idle\": 1}]}", "no peer_id"},
        {"{\"cc\": 1, \"channels\": {\"1\": 461.0375}, \"radios\": [{\"type\": \"repeater\", \"rx_freq\": 461,"
         " \"tx_freq\": 466}]}", "no peer_id"},
        /* a number in run isn't a name, not even "" */
        {"{\"run\": [5], \"radios\": [{\"type\": \"sdr\", \"name\": \"\"}, {\"type\": \"sdr\", \"name\": \"\"}]}",
         "run: a radio's name or a list of names"},
        {"{\"cc\": 1, \"channels\": {\"1\": 461.0375}, \"radios\": [{\"type\": \"mmdvm_hs\", \"peer_id\": 1, \"idle\": 1},"
         " {\"type\": \"sdr\", \"channels\": [9]}]}", "channel 9 isn't in the plan (channels:)"},
    };
    for (size_t i = 0; i < sizeof whole / sizeof whole[0]; i++)
        CHECK(!rdof_config_parse(whole[i][0], strlen(whole[i][0]), &cfg, error, sizeof error) && !strcmp(error, whole[i][1]),
              "whole %zu: want \"%s\", got %s", i, whole[i][1], error);
    /* more channels than the plan holds, and than a roamer takes */
    static char json[8192];
    size_t n = (size_t)snprintf(json, sizeof json, "{\"cc\": 1, \"channels\": {");
    for (int k = 1; k <= 2 * RDOF_CFG_CHANNELS + 1; k++)
        n += (size_t)snprintf(json + n, sizeof json - n, "%s\"%d\": %.4f", k > 1 ? ", " : "", k, 450 + k * 0.0125);
    snprintf(json + n, sizeof json - n, "}, \"radios\": [{\"type\": \"mmdvm_hs\", \"peer_id\": 1, \"idle\": 1}]}");
    CHECK(!rdof_config_parse(json, strlen(json), &cfg, error, sizeof error) && !strcmp(error, "too many channels in the plan"),
          "plan: %s", error);
    char *cut = strstr(json, ", \"65\"");
    snprintf(cut, sizeof json - (size_t)(cut - json), "}, \"radios\": [{\"type\": \"mmdvm_hs\", \"peer_id\": 1, \"idle\": 1}]}");
    CHECK(!rdof_config_parse(json, strlen(json), &cfg, error, sizeof error) && !strcmp(error, "too many channels (32 at most)"),
          "roamer, all: %s", error);
    cut = strstr(json, "\"radios\"");
    snprintf(cut, sizeof json - (size_t)(cut - json), "\"radios\": [{\"type\": \"mmdvm_hs\", \"peer_id\": 1, \"idle\": 1, "
             "\"channels\": [1], \"scan\": {\"channels\": [%s]}}]}", "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,"
             "28,29,30,31,32,33");
    CHECK(!rdof_config_parse(json, strlen(json), &cfg, error, sizeof error) && !strcmp(error, "scan: too many channels"),
          "scan: %s", error);
    cut = strstr(json, "\"radios\"");
    snprintf(cut, sizeof json - (size_t)(cut - json), "\"radios\": [{\"type\": \"mmdvm_hs\", \"peer_id\": 1, \"idle\": 33,"
             " \"channels\": [%s]}]}", "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,"
             "31,32");
    CHECK(!rdof_config_parse(json, strlen(json), &cfg, error, sizeof error) && !strcmp(error, "too many channels"),
          "idle past 32: %s", error);
}

static void t_config_values(void)
{
    static rdof_config_t cfg;
    char error[200];
    /* str() of each JSON kind, float() of a bool and of padded text */
    bool ok = cfg_try(T_ROAM, "", ", \"callsign\": 123, \"cwid\": null, \"listen_s\": true, \"clear_s\": \"2.5  \"",
                      &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.callsign, "123") && !strcmp(cfg.cwid, "None") && cfg.listen_s == 1 && cfg.clear_s == 2.5,
          "values: %s %s %g %g %s", cfg.callsign, cfg.cwid, cfg.listen_s, cfg.clear_s, error);
    ok = cfg_try(T_ROAM, "", ", \"callsign\": 1.5, \"cwid\": true", &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.callsign, "1.5") && !strcmp(cfg.cwid, "True"), "values: %s %s", cfg.callsign, cfg.cwid);
    ok = cfg_try(T_ROAM, "", ", \"cwid\": [1]", &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.cwid, "?"), "a list as text: %s", cfg.cwid);
    /* the MQTT root's slashes stripped, both ends */
    ok = cfg_try(T_ROAM, ", \"mqtt\": {\"host\": \"h\", \"root\": \"/a/b//\"}", "", &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.mqtt_root, "a/b"), "root: '%s' %s", cfg.mqtt_root, error);
    ok = cfg_try(T_ROAM, ", \"mqtt\": {\"host\": \"h\", \"root\": \"///\"}", "", &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.mqtt_root, ""), "root of slashes: '%s' %s", cfg.mqtt_root, error);
    /* tuners, RSSI map, headers and terminators */
    ok = cfg_try(T_ROAM, "", ", \"radio\": {\"tuner\": \"modem\", \"rssi\": [[100, -60], [10, -120]], \"headers\": null,"
                 " \"terminators\": 3}", &cfg, error, sizeof error);
    CHECK(ok && cfg.tuner == RDOF_TUNER_MODEM && cfg.rssi.n == 2 && cfg.rssi.raw[0] == 10 && cfg.headers == -1 &&
              cfg.terminators == 3,
          "radio settings: %s", error);
    ok = cfg_try(T_ROAM, "", ", \"radio\": {\"tuner\": {\"type\": \"fixed\", \"freq\": 461.0375, \"settle_ms\": 20}}", &cfg,
                 error, sizeof error);
    CHECK(ok && cfg.tuner == RDOF_TUNER_FIXED && cfg.tuner_freq == 461037500 && cfg.settle_ms == 20, "fixed tuner: %s",
          error);
    /* location: height clamped; idle by frequency */
    ok = cfg_try(T_ROAM, "", ", \"location\": {\"latitude\": 41.5, \"longitude\": -81.7, \"height\": 5000}, "
                 "\"idle\": {\"freq\": 461.1, \"cc\": 2}", &cfg, error, sizeof error);
    CHECK(ok && cfg.has_location && cfg.location.height_m == 999 && cfg.idle.freq == 461100000 && cfg.idle.cc == 2,
          "location and idle: %s", error);
    /* the box's SDR channels, sorted */
    ok = cfg_try(T_ROAM_SDR, "", "", &cfg, error, sizeof error);
    CHECK(ok && cfg.ncovered == 2 && cfg.covered[0] == 461037500 && cfg.covered[1] == 461087500, "covered: %s", error);
    /* scan on given channels */
    ok = cfg_try(T_ROAM, "", ", \"scan\": {\"channels\": [{\"ch\": 2, \"cc\": 3}]}", &cfg, error, sizeof error);
    CHECK(ok && cfg.has_scan && cfg.nscan == 1 && cfg.scan_channels[0].cc == 3, "scan: %s", error);
    /* the OLED's settings under i2c */
    ok = cfg_try(T_ROAM, ", \"oled\": {\"i2c\": {\"address\": 61, \"sh1106\": true, \"rotate180\": true}}", "", &cfg,
                 error, sizeof error);
    CHECK(ok && cfg.has_oled && cfg.oled_address == 61 && cfg.oled_sh1106 && cfg.oled_rotate180, "oled: %s", error);
    /* a repeater: SFR on slot 2, and slot 2 only */
    ok = cfg_try(T_REP, "", ", \"mode\": \"sfr\", \"freq\": 461.0375, \"inbound_slot\": 2", &cfg, error, sizeof error);
    CHECK(ok && cfg.sfr && cfg.slot1 && !cfg.slot2 && cfg.oled_channel == 1, "sfr: %s", error);
    ok = cfg_try(T_REP, "", FREQS ", \"slots\": [2]", &cfg, error, sizeof error);
    CHECK(ok && !cfg.slot1 && cfg.slot2, "slot 2: %s", error);
    /* a split's two ports */
    ok = cfg_try(T_REP, "", SPLIT("{\"modem\": \"uart\"}", "{\"modem\": \"uart\", \"uart\": {\"num\": 2, \"tx\": 17,"
                                  " \"rx\": 18}}") "}", &cfg, error, sizeof error);
    rdof_cfg_port_t p[2];
    CHECK(ok && rdof_config_ports(&cfg, p) == 2 && p[0].uart_num == 1 && p[1].uart_num == 2 && p[1].uart_tx == 17 &&
              !rdof_config_uses_usb(&cfg),
          "split on two UARTs: %s", error);
}

/* the value conversions at their edges, names past the roles' cut, and the buffers at theirs */
static void t_config_edges(void)
{
    static rdof_config_t cfg, other;
    char error[200];
    /* str(): -0 is 0, a float as repr */
    bool ok = cfg_try(T_ROAM, "", ", \"callsign\": 461.0375, \"cwid\": -0", &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.callsign, "461.0375") && !strcmp(cfg.cwid, "0"), "str: %s %s %s", cfg.callsign, cfg.cwid,
          error);
    ok = cfg_try(T_ROAM, "", ", \"callsign\": 1e22, \"cwid\": 0.1", &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.callsign, "1e+22") && !strcmp(cfg.cwid, "0.1"), "str: %s %s", cfg.callsign, cfg.cwid);
    /* int() saturates; NaN is 0 */
    ok = cfg_try(T_ROAM, "", ", \"radio\": {\"tx_delay_ms\": 1e300, \"rx_offset_hz\": -1e300, \"tx_offset_hz\": 1e300,"
                 " \"dmr_delay\": \"nan\", \"headers\": 1e300, \"terminators\": -1e300, \"rssi\": [[1e300, -1e300]]},"
                 " \"location\": {\"lat\": 1, \"lon\": 2, \"height\": 1e300}", &cfg, error, sizeof error);
    CHECK(ok && cfg.modem.tx_delay_ms == INT_MAX && cfg.rx_offset_hz == INT_MIN && cfg.tx_offset_hz == INT_MAX &&
              cfg.modem.dmr_delay == 0 && cfg.headers == INT_MAX && cfg.terminators == INT_MIN && cfg.rssi.n == 1 &&
              cfg.rssi.raw[0] == INT_MAX && cfg.rssi.dbm[0] == INT_MIN && cfg.location.height_m == 999,
          "huge: %s", error);
    ok = cfg_try(T_ROAM, "", ", \"location\": {\"lat\": 1, \"lon\": 2, \"height\": -1e300}", &cfg, error, sizeof error);
    CHECK(ok && cfg.location.height_m == 0, "height -1e300: %d %s", cfg.location.height_m, error);
    /* frequencies and powers past their range */
    ok = cfg_try(T_ROAM, ", \"channels\": {\"1\": 461.0375, \"2\": -5, \"3\": 1e300}",
                 ", \"channels\": [{\"ch\": 1, \"power\": 1e300}, {\"ch\": 2, \"power\": -1e300}, 3]", &cfg, error,
                 sizeof error);
    CHECK(ok && cfg.nchannels == 3 && cfg.channels[0].power == 255 && cfg.channels[1].freq == 0 &&
              cfg.channels[1].power == 0 && cfg.channels[2].freq == UINT32_MAX,
          "range: %u %u %s", cfg.channels[1].freq, cfg.channels[2].freq, error);
    /* "1" and "01" are one channel: the last */
    ok = cfg_try(T_ROAM, ", \"channels\": {\"1\": 461.0375, \"01\": 461.1}", "", &cfg, error, sizeof error);
    CHECK(ok && cfg.nchannels == 1 && cfg.channels[0].freq == 461100000 && cfg.idle.freq == 461100000,
          "duplicate keys: %d %u %s", cfg.nchannels, cfg.channels[0].freq, error);
    /* a zero-size error buffer is left alone; none at all */
    char z[4] = "Z";
    CHECK(!rdof_config_parse("[1]", 3, &cfg, z, 0) && z[0] == 'Z', "config, cap 0: %c", z[0]);
    rdof_roles_t roles;
    CHECK(!rdof_box_roles("[1]", 3, &roles, z, 0) && z[0] == 'Z', "roles, cap 0: %c", z[0]);
    CHECK(!rdof_box_roles("[1]", 3, &roles, NULL, 0), "roles, no buffer");
    CHECK(!rdof_config_parse("{", 1, &cfg, z, 1) && z[0] == 0, "cap 1: %d", z[0]);
    /* names past rdof_roles_t's 23 characters: a radio's own, and the default from a long box name */
    static const char named[] =
        "{\"name\": \"b\", \"cc\": 1, \"channels\": {\"1\": 461.0375}, \"radios\": [{\"type\": \"mmdvm_hs\","
        " \"name\": \"a-radio-name-longer-than-23\", \"peer_id\": 1, \"idle\": 1}]}";
    ok = rdof_box_roles(named, strlen(named), &roles, error, sizeof error) &&
         rdof_config_parse_radio(named, strlen(named), roles.radios[0], &cfg, error, sizeof error);
    CHECK(ok && !strcmp(roles.radios[0], "a-radio-name-longer-tha") && !strcmp(cfg.name, roles.radios[0]) &&
              strlen(cfg.name) == 23,
          "a long radio name: %s %s", roles.radios[0], error);
    static const char boxed[] =
        "{\"name\": \"boxboxboxboxboxboxboxboxboxbox\", \"cc\": 1, \"channels\": {\"1\": 461.0375},"
        " \"run\": \"boxboxboxboxboxboxboxboxboxbox-mmdvm_hs1\", \"radios\": [{\"type\": \"mmdvm_hs\", \"peer_id\": 1,"
        " \"idle\": 1}]}";
    ok = rdof_box_roles(boxed, strlen(boxed), &roles, error, sizeof error);
    CHECK(ok && !strcmp(roles.box, "boxboxboxboxboxboxboxboxboxbox") && !strcmp(roles.radios[0],
              "boxboxboxboxboxboxboxbo"),
          "a long box name: %s %s %s", roles.box, roles.radios[0], error);
    ok = rdof_config_parse(boxed, strlen(boxed), &cfg, error, sizeof error) &&
         rdof_config_parse_radio(boxed, strlen(boxed), roles.radios[0], &other, error, sizeof error);
    CHECK(ok && !strcmp(cfg.name, "boxboxboxboxboxboxboxbo") && !strcmp(cfg.callsign, "boxboxbo") &&
              !strcmp(other.name, cfg.name),
          "a long box name, parsed: %s %s", cfg.name, error);
    /* ports: no error buffer, whatever cap comes with it */
    ok = cfg_try(T_ROAM, "", ", \"modem\": \"uart\"", &cfg, error, sizeof error);
    other = cfg;
    const rdof_config_t *same[2] = {&cfg, &other};
    CHECK(ok && !rdof_config_check_ports(same, 2, NULL, 100) && !rdof_config_check_ports(same, 2, NULL, 0) &&
              rdof_config_check_ports(same, 1, NULL, 100) && rdof_config_check_ports(same, 0, error, sizeof error),
          "ports, no buffer: %s", error);
    /* describe: cut to the buffer, nothing for cap 0, and counts past their arrays as far as they go */
    ok = cfg_try(T_ROAM_SDR, "", ", \"scan\": true, \"radio\": {\"rssi\": [[1, -120], [2, -110], [3, -100], [4, -90],"
                 " [5, -80], [6, -70], [7, -60], [8, -50], [9, -40], [10, -30], [11, -20], [12, -10], [13, 0], [14, 10],"
                 " [15, 20], [16, 30], [17, 40]]}", &cfg, error, sizeof error);
    CHECK(ok && cfg.rssi.n == RDOF_RSSI_POINTS, "16 RSSI points: %d %s", cfg.rssi.n, error);
    static char full[4096], cut[16];
    size_t n = rdof_config_describe(&cfg, full, sizeof full);
    CHECK(n == strlen(full) && n > 100, "describe: %zu", n);
    memset(cut, 'x', sizeof cut);
    CHECK(rdof_config_describe(&cfg, cut, 10) == 9 && !strncmp(cut, full, 9) && cut[9] == 0 && cut[10] == 'x',
          "describe, cut: %s", cut);
    CHECK(rdof_config_describe(&cfg, NULL, 0) == 0, "describe, cap 0");
    other = cfg;
    for (int k = cfg.nchannels; k < RDOF_CFG_CHANNELS; k++)
        other.channels[k] = cfg.channels[0], other.scan_channels[k] = cfg.channels[0],
        other.covered[k] = cfg.covered[0];
    other.nchannels = other.nscan = other.ncovered = RDOF_CFG_CHANNELS;
    static char at_most[8192], past[8192];
    rdof_config_describe(&other, at_most, sizeof at_most);
    other.rssi.n = 20;
    other.nchannels = other.nscan = other.ncovered = 40;
    rdof_config_describe(&other, past, sizeof past);
    CHECK(!strcmp(at_most, past), "describe past the arrays:\n%s\n%s", at_most, past);
    /* the ends of each range, which are in it */
    ok = cfg_try(T_ROAM, ", \"channels\": {\"1\": 461.0375, \"2\": 99999, \"3\": 100000}",
                 ", \"location\": {\"lat\": 90, \"lon\": 180}, \"radio\": {\"tuner\": {\"freq\": 10000}}", &cfg, error,
                 sizeof error);
    CHECK(ok && cfg.channels[1].freq == UINT32_MAX && cfg.channels[2].freq == 100000 && cfg.has_location &&
              cfg.tuner_freq == 10000,
          "ends: %u %u %u %s", cfg.channels[1].freq, cfg.channels[2].freq, cfg.tuner_freq, error);
    ok = cfg_try(T_ROAM, "", ", \"location\": {\"lat\": -90, \"lon\": -180}, \"radio\": {\"tuner\": {\"freq\": 9999.5}}",
                 &cfg, error, sizeof error);
    CHECK(ok && cfg.has_location && cfg.tuner_freq == UINT32_MAX, "ends: %u %s", cfg.tuner_freq, error);
    ok = cfg_try(T_REP, "", SPLIT("{}", "{\"modem\": \"uart\"}") ", \"power_cap\": 255, \"lease_s\": 0, \"max_key_s\": 600,"
                 " \"prebuffer\": 4}, \"prebuffer\": 9, \"cc\": 15", &cfg, error, sizeof error);
    CHECK(ok && cfg.split_power_cap == 255 && cfg.split_lease_s == 0 && cfg.split_max_key_s == 600 &&
              cfg.split_prebuffer == 4 && cfg.prebuffer == 9 && cfg.cc == 15,
          "split's ends: %s", error);
    /* the OLED's channel: the plan's within 100 Hz of the repeater's receive frequency */
    ok = cfg_try(T_REP, ", \"channels\": {\"1\": 461.0, \"2\": 461.0005}", ", \"rx_freq\": 461.0005, \"tx_freq\": 466",
                 &cfg, error, sizeof error);
    CHECK(ok && cfg.oled_channel == 2, "oled channel, 0 Hz: %d %s", cfg.oled_channel, error);
    ok = cfg_try(T_REP, ", \"channels\": {\"1\": 461.0001}", ", \"rx_freq\": 461.0002, \"tx_freq\": 466", &cfg, error,
                 sizeof error);
    CHECK(ok && cfg.oled_channel == 1, "oled channel, 100 Hz: %d %s", cfg.oled_channel, error);
    ok = cfg_try(T_REP, ", \"channels\": {\"1\": 461.0001}", ", \"rx_freq\": 461.000201, \"tx_freq\": 466", &cfg, error,
                 sizeof error);
    CHECK(ok && cfg.oled_channel == 0, "oled channel, 101 Hz: %d %s", cfg.oled_channel, error);
    ok = cfg_try(T_REP, ", \"channels\": {\"1\": 461.0001}", ", \"rx_freq\": 461.0, \"tx_freq\": 466", &cfg, error,
                 sizeof error);
    CHECK(ok && cfg.oled_channel == 1, "oled channel, -100 Hz: %d %s", cfg.oled_channel, error);
    /* int() of text is decimal; an empty modem_address is none; a success leaves no error */
    snprintf(error, sizeof error, "junk");
    ok = cfg_try(T_ROAM, "", ", \"prebuffer\": \"010\"", &cfg, error, sizeof error);
    CHECK(ok && cfg.prebuffer == 10 && !error[0], "\"010\": %d %s", cfg.prebuffer, error);
    /* the jitter buffer: auto by default; prebuffer fixes it (bursts); jitter_buffer "auto" or ms, rounded up to
     * whole 60 ms bursts; the bounds likewise */
    ok = cfg_try(T_ROAM, "", "", &cfg, error, sizeof error);
    CHECK(ok && cfg.jitter_auto && cfg.prebuffer == 6 && cfg.jitter_min_ms == 180 && cfg.jitter_max_ms == 480,
          "jitter default: %d %d %d %d", cfg.jitter_auto, cfg.prebuffer, cfg.jitter_min_ms, cfg.jitter_max_ms);
    ok = cfg_try(T_ROAM, "", ", \"prebuffer\": 4", &cfg, error, sizeof error);
    CHECK(ok && !cfg.jitter_auto && cfg.prebuffer == 4, "prebuffer 4: %d %d", cfg.jitter_auto, cfg.prebuffer);
    static const struct {
        int t;
        const char *radio;
        bool aut;
        int prebuffer, lo, hi;
        const char *describe;
    } jv[] = {
        {T_ROAM, ", \"jitter_buffer\": \"auto\"", true, 6, 180, 480, ";prebuffer=6;jitter=auto:180-480;preambles=2;"},
        {T_ROAM, ", \"jitter_buffer\": 250", false, 5, 180, 480, ";prebuffer=5;jitter=fixed;preambles=2;"},
        {T_ROAM, ", \"jitter_buffer\": \"360\"", false, 6, 180, 480, ";prebuffer=6;jitter=fixed;"},
        {T_ROAM, ", \"jitter_buffer\": 0", false, 0, 180, 480, ";prebuffer=0;jitter=fixed;"},
        {T_ROAM, ", \"jitter_buffer\": 3000", false, 50, 180, 480, ";prebuffer=50;jitter=fixed;"},
        {T_ROAM, ", \"jitter_buffer\": 60.001", false, 2, 180, 480, ";prebuffer=2;jitter=fixed;"},
        {T_ROAM, ", \"jitter_min_ms\": 200, \"jitter_max_ms\": \"200\"", true, 6, 240, 240, ";jitter=auto:240-240;"},
        {T_ROAM, ", \"jitter_min_ms\": 60, \"jitter_max_ms\": 3000", true, 6, 60, 3000, ";jitter=auto:60-3000;"},
        {T_REP, FREQS, true, 6, 180, 480, ";prebuffer=6;jitter=auto:180-480;sfr=0;"},
        {T_REP, FREQS ", \"jitter_buffer\": 120", false, 2, 180, 480, ";prebuffer=2;jitter=fixed;sfr=0;"},
        {T_REP, FREQS ", \"prebuffer\": 9, \"jitter_max_ms\": 900", false, 9, 180, 900, ";prebuffer=9;jitter=fixed;"},
    };
    for (size_t i = 0; i < sizeof jv / sizeof jv[0]; i++) {
        ok = cfg_try(jv[i].t, "", jv[i].radio, &cfg, error, sizeof error);
        char d[2048];
        rdof_config_describe(&cfg, d, sizeof d);
        CHECK(ok && cfg.jitter_auto == jv[i].aut && cfg.prebuffer == jv[i].prebuffer && cfg.jitter_min_ms == jv[i].lo &&
                  cfg.jitter_max_ms == jv[i].hi && strstr(d, jv[i].describe),
              "jitter %s: %d %d %d %d %s %s", jv[i].radio, cfg.jitter_auto, cfg.prebuffer, cfg.jitter_min_ms,
              cfg.jitter_max_ms, error, d);
    }
    ok = cfg_try(T_ROAM, "", ", \"radio\": {\"protocol\": \"udp\", \"modem_address\": \"\"}", &cfg, error, sizeof error);
    CHECK(!ok && !strcmp(error, "radio.protocol udp needs radio.modem_address"), "empty modem_address: %s", error);
    /* env references: falsy, and "" as text; almost-references are text */
    ok = cfg_try(T_ROAM, "", ", \"callsign\": \"${CALL}\", \"cwid\": \"${CWID}\"", &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.callsign, "R") && !strcmp(cfg.cwid, ""), "env: %s %s %s", cfg.callsign, cfg.cwid, error);
    ok = cfg_try(T_ROAM, "", ", \"callsign\": \"$x}\", \"cwid\": \"${\"", &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.callsign, "$x}") && !strcmp(cfg.cwid, "${"), "not env: %s %s", cfg.callsign, cfg.cwid);
    ok = cfg_try(T_ROAM, "", ", \"callsign\": \"{x}\", \"cwid\": \"${ab\"", &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.callsign, "{x}") && !strcmp(cfg.cwid, "${ab"), "not env: %s %s", cfg.callsign, cfg.cwid);
    /* falsy: 0, [], {}; float(false) is 0 */
    static const char *const falsy[] = {"0", "[]", "{}", "false", "\"\""};
    for (size_t i = 0; i < sizeof falsy / sizeof falsy[0]; i++) {
        char radio[64];
        snprintf(radio, sizeof radio, ", \"callsign\": %s, \"listen_s\": false", falsy[i]);
        ok = cfg_try(T_ROAM, "", radio, &cfg, error, sizeof error);
        CHECK(ok && !strcmp(cfg.callsign, "R") && cfg.listen_s == 0, "falsy %s: %s %g %s", falsy[i], cfg.callsign,
              cfg.listen_s, error);
    }
    /* an SDR channel listed twice is covered once */
    static const char twice[] =
        "{\"cc\": 1, \"channels\": {\"1\": 461.0375, \"2\": 461.0875}, \"radios\": [{\"type\": \"mmdvm_hs\", \"peer_id\": 1,"
        " \"idle\": 1}, {\"type\": \"sdr\", \"channels\": [2, 1, 2]}]}";
    ok = rdof_config_parse(twice, strlen(twice), &cfg, error, sizeof error);
    CHECK(ok && cfg.ncovered == 2 && cfg.covered[0] == 461037500 && cfg.covered[1] == 461087500, "covered twice: %d %s",
          cfg.ncovered, error);
    /* the box's slot timing reaches a duplex repeater on an auto radio, not a single frequency one */
    ok = cfg_try(T_REP, ", \"slot_timing\": \"utc\"", FREQS, &cfg, error, sizeof error);
    CHECK(ok && cfg.slot_timing.utc, "box utc, duplex: %s", error);
    ok = cfg_try(T_REP, ", \"slot_timing\": \"utc\"", ", \"mode\": \"sfr\", \"freq\": 461.0375", &cfg, error, sizeof error);
    CHECK(ok && !cfg.slot_timing.utc, "box utc, sfr: %s", error);
    ok = cfg_try(T_REP, "", ", \"mode\": \"sfr\", \"freq\": 461.0375, \"slot_timing\": \"utc\"", &cfg, error, sizeof error);
    CHECK(!ok && !strcmp(error, "slot_timing utc: a single frequency repeater's radio times its own slots"),
          "sfr's own utc: %s", error);
    /* offsets, headers and terminators that aren't numbers are left at their defaults; a tuner with no type */
    ok = cfg_try(T_ROAM, "", ", \"radio\": {\"rx_offset_hz\": \"x\", \"tx_offset_hz\": [1], \"headers\": \"x\","
                 " \"terminators\": null, \"tuner\": {\"freq\": 461.1}}", &cfg, error, sizeof error);
    CHECK(ok && cfg.rx_offset_hz == 0 && cfg.tx_offset_hz == 0 && cfg.headers == -1 && cfg.terminators == -1 &&
              cfg.tuner == RDOF_TUNER_NONE && cfg.tuner_freq == 461100000,
          "not numbers: %s", error);
    ok = cfg_try(T_ROAM, "", ", \"radio\": {\"tuner\": {\"type\": \"fixed\", \"freq\": 461.0375, \"settle_ms\": 20}}", &cfg,
                 error, sizeof error);
    CHECK(ok && rdof_config_describe(&cfg, full, sizeof full) && strstr(full, ";tuner=fixed:461037500:20;"),
          "describe, fixed tuner: %s", full);
    ok = cfg_try(T_ROAM, "", ", \"radio\": {\"tuner\": \"modem\"}", &cfg, error, sizeof error);
    CHECK(ok && rdof_config_describe(&cfg, full, sizeof full) && strstr(full, ";tuner=modem:0:0;"),
          "describe, modem tuner: %s", full);
    /* scan on all channels, by name */
    ok = cfg_try(T_ROAM, "", ", \"scan\": {\"channels\": \"all\"}", &cfg, error, sizeof error);
    CHECK(ok && cfg.has_scan && cfg.nscan == 2, "scan all: %d %s", cfg.nscan, error);
    /* a repeater's power clamped, slot 1 alone */
    ok = cfg_try(T_REP, "", FREQS ", \"power\": -5, \"slots\": [1]", &cfg, error, sizeof error);
    CHECK(ok && cfg.power == 0 && cfg.slot1 && !cfg.slot2, "power -5, slot 1: %d %s", cfg.power, error);
    ok = cfg_try(T_REP, "", FREQS ", \"power\": 300", &cfg, error, sizeof error);
    CHECK(ok && cfg.power == 255, "power 300: %d %s", cfg.power, error);
    /* the OLED off; its own rotate180 over i2c's */
    ok = cfg_try(T_ROAM, ", \"oled\": false", "", &cfg, error, sizeof error);
    CHECK(ok && !cfg.has_oled, "oled false: %s", error);
    ok = cfg_try(T_ROAM, ", \"oled\": {\"rotate180\": true, \"sh1106\": false, \"i2c\": {\"rotate180\": false,"
                 " \"sh1106\": true}}", "", &cfg, error, sizeof error);
    CHECK(ok && cfg.has_oled && cfg.oled_rotate180 && !cfg.oled_sh1106, "oled's own: %s", error);
    /* roles: not JSON; an SDR config with radios isn't a plain one */
    CHECK(!rdof_box_roles("{", 1, &roles, error, sizeof error) && !strncmp(error, "JSON: ", 6), "roles, JSON: %s", error);
    static const char sdr_radios[] = "{\"device\": {}, \"radios\": [{\"type\": \"sdr\", \"name\": \"S\"}]}";
    ok = rdof_box_roles(sdr_radios, strlen(sdr_radios), &roles, error, sizeof error);
    CHECK(ok && roles.has_sdr && !roles.sdr_plain && !strcmp(roles.sdr, "S"), "roles, device and radios: %s", error);
    /* ports: each pin pair, and USB devices told apart or not */
    ok = cfg_try(T_ROAM, "", ", \"modem\": \"uart\"", &cfg, error, sizeof error);
    other = cfg;
    other.uart_num = 2;
    static const int pins[][2] = {{5, 7}, {7, 6}, {6, 8}, {8, 5}, {7, 8}};
    for (size_t i = 0; i < sizeof pins / sizeof pins[0]; i++) {
        other.uart_tx = pins[i][0];
        other.uart_rx = pins[i][1];
        bool apart = rdof_config_check_ports(same, 2, error, sizeof error);
        CHECK(apart == (i == 4) && (apart || !strcmp(error, "R and R share a UART pin")), "pins %d/%d: %s",
              other.uart_tx, other.uart_rx, error);
    }
    ok = cfg_try(T_ROAM, "", ", \"modem\": \"usb\"", &cfg, error, sizeof error);
    static const struct {
        const char *serial[2], *port[2];
        bool apart;
    } usb[] = {
        {{"", ""}, {"1", ""}, false},  {{"A", "A"}, {"1", "2"}, true},  {{"A", "A"}, {"1", "1"}, false},
        {{"A", "A"}, {"1", ""}, false}, {{"", ""}, {"1", "1"}, false},   {{"", ""}, {"1", "2"}, true},
        {{"A", ""}, {"1", "1"}, true},  {{"A", "B"}, {"", ""}, true},   {{"", "B"}, {"", ""}, false},
    };
    for (size_t i = 0; i < sizeof usb / sizeof usb[0]; i++) {
        other = cfg;
        snprintf(cfg.usb_serial, sizeof cfg.usb_serial, "%s", usb[i].serial[0]);
        snprintf(other.usb_serial, sizeof other.usb_serial, "%s", usb[i].serial[1]);
        snprintf(cfg.usb_port, sizeof cfg.usb_port, "%s", usb[i].port[0]);
        snprintf(other.usb_port, sizeof other.usb_port, "%s", usb[i].port[1]);
        bool apart = rdof_config_check_ports(same, 2, error, sizeof error);
        CHECK(apart == usb[i].apart, "usb %zu: %s", i, apart ? "apart" : error);
    }
}

void test_core(void)
{
    t_crcs();
    t_hamming();
    t_bits();
    t_burst_fields();
    t_json();
    t_json_edges();
    t_jsonc_edges();
    t_modem();
    t_radio();
    t_benchcfg();
    t_oled();
    t_oled_sdr();
    t_hblink();
    t_config_errors();
    t_config_dcdm();
    t_config_values();
    t_config_edges();
    t_json_writer();
    t_json_writer_edges();
    t_csbk_names();
    t_netcalls();
    t_time_edges();
#if RDOF_FEATURE_TRACKER
    t_tracker_edges();
#endif
    t_status_edges();
    t_mmdvm_edges();
    t_pdu();
    t_ambe();
    t_rawburst();
    t_grid_edges();
    t_slot_timing_edges();
    t_slot_gate_edges();
    t_sample_clock_edges();
}
