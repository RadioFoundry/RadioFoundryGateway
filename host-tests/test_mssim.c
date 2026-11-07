// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The timed confirmed-data responder (rdof/mssim.h): answer positions for N slots after the last block or the
 * terminator, learning the end, ACK / NACK / SACK / none / ACK twice, the SACK's flag map and CRC-32 against a real
 * radio's, the sync family, the profile JSON, the log. */
#include <stdio.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/layer2.h"
#include "rdof/mssim.h"
#include "rdof/pdu.h"

static int checks, failures;

#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        checks++;                                                                                                    \
        if (!(c)) {                                                                                                  \
            failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                      \
        }                                                                                                            \
    } while (0)

#define SENDER 3105738u
#define ME 3101276u
#define F48 2880

static struct {
    int n;
    int64_t at[8];
    uint8_t b[8][33];
    int64_t late; /* placed this much after the ask */
    bool refuse;
} T;

static bool sendcb(void *ctx, int64_t at48, const uint8_t burst[33], int64_t *placed)
{
    if (T.refuse || T.n >= 8)
        return false;
    T.at[T.n] = at48;
    memcpy(T.b[T.n++], burst, 33);
    *placed = at48 + T.late;
    return true;
}

static uint64_t sync_of(rdof_family_t f) { return rdof_data_sync(rdof_family_voice_sync(f)); }

static rdof_burst_t burst_of(int dt, rdof_family_t f)
{
    rdof_burst_t b;
    memset(&b, 0, sizeof b);
    b.dt = dt;
    b.family = f;
    b.cc = 2;
    b.slot = 1;
    return b;
}

/* A confirmed data header: blocks to follow, N(S), FSN. */
static rdof_burst_t header(int blocks, int ns, rdof_family_t f, bool group)
{
    uint8_t d[12] = {0};
    d[0] = (uint8_t)(0x40 | RDOF_DPF_CONFIRMED); /* A=1 */
    if (group)
        d[0] |= 0x80;
    d[1] = 4 << 4; /* SAP 4 */
    rdof_put_be24(d + 2, ME);
    rdof_put_be24(d + 5, SENDER);
    d[8] = (uint8_t)(0x80 | blocks);
    d[9] = (uint8_t)(ns << 4 | 0);
    uint16_t crc = rdof_crc16_ccitt(d, 10) ^ RDOF_MASK_DATA_HEADER;
    d[10] = (uint8_t)(crc >> 8);
    d[11] = (uint8_t)crc;
    rdof_burst_t b = burst_of(RDOF_DT_DATA_HEADER, f);
    rdof_data_burst(d, RDOF_DT_DATA_HEADER, 2, sync_of(f), b.burst);
    return b;
}

/* A confirmed rate 1/2 block with serial number dbsn and a good CRC-9. */
static rdof_burst_t block(int dbsn, rdof_family_t f)
{
    rdof_burst_t b = burst_of(RDOF_DT_RATE_12, f);
    for (int crc = 0; crc < 512; crc++) {
        uint8_t d[12];
        for (int i = 2; i < 12; i++)
            d[i] = (uint8_t)(0x30 + i + dbsn);
        d[0] = (uint8_t)(dbsn << 1 | crc >> 8);
        d[1] = (uint8_t)crc;
        rdof_data_burst(d, RDOF_DT_RATE_12, 2, sync_of(f), b.burst);
        rdof_data_block_t blk;
        int n, len;
        uint8_t body[24];
        if (rdof_decode_data_block(b.burst, RDOF_DT_RATE_12, &blk) && rdof_split_confirmed(&blk, &n, body, &len))
            return b;
    }
    return b;
}

static rdof_burst_t terminator(rdof_family_t f)
{
    rdof_burst_t b = burst_of(RDOF_DT_TERMINATOR_WITH_LC, f);
    uint8_t lc[9] = {0};
    rdof_lc_burst(lc, RDOF_DT_TERMINATOR_WITH_LC, 2, sync_of(f), b.burst);
    return b;
}

static rdof_mssim_t S;

static void arm(const char *json)
{
    rdof_mssim_profile_t p;
    char err[96];
    bool ok = rdof_mssim_parse(json, strlen(json), &p, err, sizeof err);
    if (!ok)
        printf("profile %s: %s\n", json, err);
    CHECK(ok);
    memset(&T, 0, sizeof T);
    rdof_mssim_init(&S, sendcb, NULL);
    rdof_mssim_arm(&S, &p);
}

/* A packet heard from t0: header, blocks every 2880, a terminator after if asked; poll each 480 (10 ms). */
static void packet(int64_t t0, int blocks, int ns, bool term, rdof_family_t f, int64_t lead)
{
    rdof_burst_t h = header(blocks, ns, f, false);
    int64_t end = t0 + (int64_t)(blocks + (term ? 1 : 0)) * F48;
    for (int64_t now = t0; now < end + 20 * F48; now += 480) {
        if (now == t0)
            rdof_mssim_heard(&S, &h, t0);
        for (int k = 1; k <= blocks; k++)
            if (now == t0 + (int64_t)k * F48) {
                rdof_burst_t b = block(k - 1, f);
                rdof_mssim_heard(&S, &b, now);
            }
        if (term && now == end) {
            rdof_burst_t t = terminator(f);
            rdof_mssim_heard(&S, &t, now);
        }
        rdof_mssim_poll(&S, now + 480, lead);
    }
}

static void parse_resp(const uint8_t b[33], rdof_data_header_t *h) { rdof_decode_data_header(b, h); }

static void test_sack_crc_real(void)
{
    /* A radio's SACK block (blocks 1-6 asked again): flag map 81ffffffffffffff, CRC-32 1f8cd5c0 as stored. */
    const uint8_t map[8] = {0x81, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint32_t crc = rdof_crc32_dmr(map, 8);
    CHECK(crc == 0xc0d58c1fu);
    uint8_t b[33], d[12];
    rdof_mssim_sack_block(~0x7EULL, 2, sync_of(RDOF_FAMILY_MS), b);
    CHECK(rdof_bptc196_decode(b, d));
    const uint8_t want[12] = {0x81, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x8c, 0xd5, 0xc0};
    CHECK(!memcmp(d, want, 12));
    CHECK(rdof_check_crc32(d, 12));
}

static void test_ack_positions(void)
{
    for (int n = 1; n <= 3; n++) {
        char j[64];
        snprintf(j, sizeof j, "{\"answer\":\"ack\",\"slots\":%d}", n);
        arm(j);
        packet(100000, 3, 5, false, RDOF_FAMILY_MS, 1500);
        CHECK(T.n == 1);
        CHECK(T.at[0] == 100000 + 3 * F48 + n * 1440);
        rdof_data_header_t h;
        parse_resp(T.b[0], &h);
        CHECK(h.crc_ok && h.dpf == RDOF_DPF_RESPONSE);
        CHECK(h.response_class == 0 && h.response_type == 1 && h.response_status == 5);
        CHECK(h.src_id == ME && h.dst_id == SENDER && h.sap == 4 && h.blocks == 0);
        rdof_family_t f;
        bool data;
        int errs;
        CHECK(rdof_detect_sync(T.b[0], 0, &f, &data, &errs) && f == RDOF_FAMILY_MS && data);
        CHECK(S.log[0].sent && S.log[0].end_heard && S.log[0].heard == 3 && S.log[0].dbsn == 7);
    }
}

static void test_terminator_and_learn(void)
{
    arm("{\"answer\":\"ack\",\"slots\":1,\"end\":\"terminator\"}");
    packet(0, 2, 1, true, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 1 && T.at[0] == 3 * F48 + 1440);
    CHECK(S.log[0].terminator && S.log[0].end_used == RDOF_MSSIM_END_TERMINATOR);

    /* none came: the expected place, logged as not heard */
    arm("{\"answer\":\"ack\",\"slots\":2,\"end\":\"terminator\"}");
    packet(0, 2, 1, false, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 1 && T.at[0] == 3 * F48 + 2880);
    CHECK(!S.log[0].terminator && !S.log[0].end_heard);

    /* learn: the first packet's terminator makes the second wait for one */
    arm("{\"answers\":[{\"answer\":\"ack\",\"slots\":1}],\"end\":\"learn\"}");
    packet(0, 2, 1, true, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 1 && T.at[0] == 2 * F48 + 1440);
    packet(200000, 2, 2, true, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 2 && T.at[1] == 200000 + 3 * F48 + 1440);
    CHECK(S.log[1].end_used == RDOF_MSSIM_END_TERMINATOR);
}

static void test_kinds(void)
{
    rdof_data_header_t h;
    arm("{\"answer\":\"nack\",\"slots\":1,\"nack_type\":1}");
    packet(0, 2, 3, false, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 1);
    parse_resp(T.b[0], &h);
    CHECK(h.response_class == 1 && h.response_type == 1 && h.response_status == 3);

    arm("{\"answer\":\"sack\",\"slots\":1,\"resend\":[1,2]}");
    packet(0, 4, 6, false, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 2 && T.at[1] == T.at[0] + F48);
    parse_resp(T.b[0], &h);
    CHECK(h.response_class == 2 && h.response_type == 0 && h.response_status == 6 && h.blocks == 1);
    uint8_t d[12];
    rdof_bptc196_decode(T.b[1], d);
    CHECK(d[0] == 0xF9 && d[1] == 0xFF && rdof_check_crc32(d, 12));

    arm("{\"answer\":\"none\"}");
    packet(0, 2, 3, false, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 0 && S.nlog == 1 && !S.log[0].sent && !S.log[0].failed);

    arm("{\"answer\":\"ack2\",\"slots\":3}");
    packet(0, 2, 3, false, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 2 && T.at[1] == T.at[0] + F48 && !memcmp(T.b[0], T.b[1], 33));

    /* steps: SACK, then ACK for the retry; count stops it */
    arm("{\"answers\":[{\"answer\":\"sack\",\"resend\":[0]},{\"answer\":\"ack\",\"slots\":2}],\"count\":2}");
    packet(0, 2, 1, false, RDOF_FAMILY_MS, 1500);
    packet(100000, 1, 1, false, RDOF_FAMILY_MS, 1500);
    packet(200000, 1, 2, false, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 3 && T.at[2] == 100000 + F48 + 2880);
    CHECK(S.nlog == 2 && !S.armed);
}

static void test_family_and_filters(void)
{
    rdof_family_t f;
    bool data;
    int errs;
    arm("{\"answer\":\"ack\",\"slots\":1}");
    packet(0, 1, 0, false, RDOF_FAMILY_TS1, 1500);
    CHECK(T.n == 1 && rdof_detect_sync(T.b[0], 0, &f, &data, &errs) && f == RDOF_FAMILY_TS2);
    arm("{\"answer\":\"ack\",\"slots\":2}");
    packet(0, 1, 0, false, RDOF_FAMILY_TS1, 1500);
    CHECK(T.n == 1 && rdof_detect_sync(T.b[0], 0, &f, &data, &errs) && f == RDOF_FAMILY_TS1);
    arm("{\"answer\":\"ack\",\"slots\":1}");
    packet(0, 1, 0, false, RDOF_FAMILY_BS, 1500);
    CHECK(T.n == 1 && rdof_detect_sync(T.b[0], 0, &f, &data, &errs) && f == RDOF_FAMILY_MS);
    /* forced: MS to a TDMA direct mode TS1 sender at one slot, BS for a repeater's answer */
    arm("{\"answer\":\"ack\",\"slots\":1,\"sync\":\"ms\"}");
    packet(0, 1, 0, false, RDOF_FAMILY_TS1, 1500);
    CHECK(T.n == 1 && rdof_detect_sync(T.b[0], 0, &f, &data, &errs) && f == RDOF_FAMILY_MS && data);
    CHECK(S.log[0].family == RDOF_FAMILY_TS1 && S.log[0].sync == RDOF_FAMILY_MS);
    {
        char js[1024];
        uint32_t next;
        rdof_mssim_log_json(&S, 0, js, sizeof js, &next);
        CHECK(strstr(js, "\"family\":\"ts1\",\"sync\":\"ms\"") != NULL);
    }
    arm("{\"answer\":\"sack\",\"slots\":2,\"sync\":\"bs\"}");
    packet(0, 1, 0, false, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 2 && rdof_detect_sync(T.b[0], 0, &f, &data, &errs) && f == RDOF_FAMILY_BS);
    CHECK(rdof_detect_sync(T.b[1], 0, &f, &data, &errs) && f == RDOF_FAMILY_BS);

    arm("{\"answer\":\"ack\",\"to\":123}");
    packet(0, 1, 0, false, RDOF_FAMILY_MS, 1500);
    CHECK(T.n == 0 && S.nlog == 0);

    /* a group packet isn't answered */
    arm("{\"answer\":\"ack\"}");
    rdof_burst_t g = header(1, 0, RDOF_FAMILY_MS, true);
    rdof_mssim_heard(&S, &g, 0);
    CHECK(!S.active);
}

static void test_log(void)
{
    arm("{\"answer\":\"ack\",\"slots\":1}");
    T.late = 48; /* 1 ms late */
    packet(0, 2, 4, false, RDOF_FAMILY_MS, 1500);
    rdof_mssim_echo(&S, 2 * F48 + 1440 + 96);
    char buf[2048];
    uint32_t next = 0;
    size_t n = rdof_mssim_log_json(&S, 0, buf, sizeof buf, &next);
    CHECK(n > 0 && next == 1);
    CHECK(strstr(buf, "\"turnaround_us\":31000") != NULL);
    CHECK(strstr(buf, "\"gap_us\":3500") != NULL);
    CHECK(strstr(buf, "\"air_late_us\":1000") != NULL);
    CHECK(strstr(buf, "\"answer\":\"ack\"") != NULL);
    n = rdof_mssim_log_json(&S, next, buf, sizeof buf, &next);
    CHECK(n == 2 && !strcmp(buf, "[]"));
    CHECK(rdof_mssim_log_json(&S, 0, buf, 10, &next) == 2 && next == 0);

    /* placement refused: logged failed */
    arm("{\"answer\":\"ack\"}");
    T.refuse = true;
    packet(0, 1, 0, false, RDOF_FAMILY_MS, 1500);
    CHECK(S.log[0].failed && !S.log[0].sent);
}

static void test_profile_errors(void)
{
    rdof_mssim_profile_t p;
    char err[96];
    CHECK(!rdof_mssim_parse("{\"answer\":\"maybe\"}", 18, &p, err, sizeof err));
    CHECK(!rdof_mssim_parse("{\"slots\":0}", 11, &p, err, sizeof err));
    CHECK(!rdof_mssim_parse("{\"end\":\"x\"}", 11, &p, err, sizeof err));
    CHECK(!rdof_mssim_parse("{\"sync\":\"dmo\"}", 14, &p, err, sizeof err));
    CHECK(rdof_mssim_parse("{\"sync\":\"auto\"}", 15, &p, err, sizeof err) && p.sync == 0);
    CHECK(!rdof_mssim_parse("[1]", 3, &p, err, sizeof err));
    CHECK(!rdof_mssim_parse("{\"resend\":[64]}", 15, &p, err, sizeof err));
    CHECK(rdof_mssim_parse("{}", 2, &p, err, sizeof err) && p.nsteps == 1 && p.step[0].kind == RDOF_MSSIM_ACK &&
          p.step[0].slots == 1);
}

int main(void)
{
    test_sack_crc_real();
    test_ack_positions();
    test_terminator_and_learn();
    test_kinds();
    test_family_and_filters();
    test_log();
    test_profile_errors();
    printf("mssim: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
