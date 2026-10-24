// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Builds the synthetic data call in the fixtures and prints the expected tables, taken from the C
 * (README.md, "The data call").
 *
 * The call: an MS on slot 2 sends 16 preamble CSBKs, then an unconfirmed data header and two rate 1/2 blocks
 * (SAP 3, ten octets of ASCII text), from 9990001 to 9990002. It is built with this project's encoders
 * (rdof_data_burst, rdof_ipgw_build_dmr) and modulated with sdr/src/mod.c.
 *
 *   rdof_gen_data_call SDR_DATA_DIR > tables.txt
 *
 * writes SDR_DATA_DIR/demod_sms_ms_slot2.cu8 and SDR_DATA_DIR/e2e_288k.cu8, and prints, in order:
 * v_steps_sms_data_call and v_outs_sms_data_call (vectors.h), the demod case's v_dbursts rows and its v_demod row, and
 * v_e2e_chans, v_e2e_outs and V_STATUS_JSON (sdr_vectors.h). The noise comes from a fixed seed, so a rerun gives
 * the same files.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/hblink.h"
#include "rdof/ipgw.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"
#include "rdof/rawburst.h"
#include "rdof/tracker.h"
#include "rdof/sdr/agc.h"
#include "rdof/sdr/demod.h"
#include "rdof/sdr/fm.h"
#include "rdof/sdr/iq.h"
#include "rdof/sdr/mod.h"
#include "rdof/sdr/receiver.h"
#include "rdof/sdr/status.h"

#define SRC_ID 9990001u
#define DST_ID 9990002u
#define SAP 3
#define PREAMBLES 16
#define CALL_BURSTS (PREAMBLES + 3)
static const uint8_t TEXT[10] = {'R', 'F', 'G', 'W', ' ', 't', 'e', 's', 't', '.'};

static void fail(const char *what)
{
    fprintf(stderr, "gen_data_call: %s\n", what);
    exit(1);
}

/* The call's bursts on color code cc, with MS data sync. */
static void build_call(int cc, uint8_t out[CALL_BURSTS][RDOF_BURST_BYTES], int dt[CALL_BURSTS])
{
    const uint64_t sync = rdof_data_sync(RDOF_MS_VOICE);
    static rdof_ipgw_dmr_t p;
    if (!rdof_ipgw_build_dmr(SRC_ID, DST_ID, SAP, TEXT, sizeof TEXT, cc, sync, &p) || p.nbursts != 3)
        fail("data packet");
    for (int i = 0; i < PREAMBLES; i++) {
        uint8_t d[12] = {0};
        d[0] = 0x80 | RDOF_CSBK_PREAMBLE; /* last block */
        d[2] = 0x80;                      /* data follows, individual */
        d[3] = (uint8_t)(PREAMBLES - 1 - i + p.nbursts);
        rdof_put_be24(d + 4, DST_ID);
        rdof_put_be24(d + 7, SRC_ID);
        uint16_t crc = rdof_crc16_ccitt(d, 10) ^ RDOF_MASK_CSBK;
        d[10] = (uint8_t)(crc >> 8);
        d[11] = (uint8_t)crc;
        rdof_data_burst(d, RDOF_DT_CSBK, cc, sync, out[i]);
        dt[i] = RDOF_DT_CSBK;
        rdof_csbk_t c;
        rdof_decode_csbk(out[i], &c);
        if (!c.crc_ok || c.src_id != (int32_t)SRC_ID || c.blocks_to_follow != d[3])
            fail("preamble");
    }
    for (int i = 0; i < p.nbursts; i++) {
        memcpy(out[PREAMBLES + i], p.bursts[i], RDOF_BURST_BYTES);
        dt[PREAMBLES + i] = i ? RDOF_DT_RATE_12 : RDOF_DT_DATA_HEADER;
    }
}

static void tohex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
    out[2 * n] = 0;
}

static void unhex(const char *s, uint8_t *out)
{
    for (size_t i = 0; s[2 * i]; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* text as a C string literal */
static void cstr(const char *text)
{
    putchar('"');
    for (const char *c = text; *c; c++) {
        if (*c == '"' || *c == '\\')
            putchar('\\');
        putchar(*c);
    }
    putchar('"');
}

/* Gaussian noise from a fixed seed (xorshift64, Box-Muller). */
static uint64_t rng = 0x2545F4914F6CDD1DULL;
static double gauss(void)
{
    double u[2];
    for (int i = 0; i < 2; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        u[i] = ((rng >> 11) + 0.5) / 9007199254740992.0;
    }
    return sqrt(-2 * log(u[0])) * cos(2 * 3.14159265358979323846 * u[1]);
}

/* x plus noise of sigma (each of I and Q), to cu8, written to path; x is replaced by what the file holds. */
static void write_cu8(const char *path, rdof_cf_t *x, int n, double sigma)
{
    uint8_t *raw = malloc(2 * (size_t)n);
    for (int i = 0; i < n; i++) {
        double v[2] = {x[i].re + sigma * gauss(), x[i].im + sigma * gauss()};
        for (int k = 0; k < 2; k++) {
            double q = floor(v[k] * 127.5 + 127.5 + 0.5);
            raw[2 * i + k] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q);
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(raw, 2, (size_t)n, f) != (size_t)n || fclose(f))
        fail(path);
    rdof_iq_from_cu8(raw, (size_t)n, x);
    free(raw);
}

/* ── vectors.h: the tracker scenario ──────────────────────────────────────── */

#define SC_T0 1760000400.0

struct capture {
    int rand_k;
};

static void out_line(const char *kind, const char *text)
{
    printf("    {\"%s\", ", kind);
    cstr(text);
    printf("},\n");
}

static void cap_json(void *ctx, const char *ch, const char *json) { out_line("json", json); }
static void cap_raw(void *ctx, const char *ch, const char *json) { out_line("raw", json); }

static void cap_raw_burst(void *ctx, const char *ch, const rdof_rawburst_t *r)
{
    uint8_t b[RDOF_RAWBURST_MAX];
    char hex[2 * RDOF_RAWBURST_MAX + 1];
    tohex(b, rdof_rawburst_encode(r, b), hex);
    out_line("rawb", hex);
}

static void cap_frame(void *ctx, const char *ch, const rdof_frame_t *f)
{
    char text[200], hex[67], src[16] = "-", dst[16] = "-";
    if (f->has_addr) {
        snprintf(src, sizeof src, "%lu", (unsigned long)f->src);
        snprintf(dst, sizeof dst, "%lu", (unsigned long)f->dst);
    }
    tohex(f->burst, 33, hex);
    snprintf(text, sizeof text, "%d,%d,%d,%s,%s,%d,%d,%d,%d,%d,%s", f->slot, f->dt, f->n, src, dst, f->group,
             f->new_stream, f->synthetic, f->errors, f->rssi, hex);
    out_line("frame", text);
}

static void hb_send(void *ctx, const uint8_t *data, size_t len)
{
    char hex[2 * RDOF_DMRD_LEN + 1];
    tohex(data, len, hex);
    out_line("dmrd", hex);
}

static void hb_before(void *ctx) { out_line("before", ""); }

/* test_main.c's v_stream_ids: 4096 + 7919 k */
static uint32_t hb_rand(void *ctx)
{
    struct capture *c = ctx;
    return (uint32_t)(4096 + 7919 * c->rand_k++) - 1;
}

static void scenario(void)
{
    uint8_t b[CALL_BURSTS][RDOF_BURST_BYTES];
    int dt[CALL_BURSTS];
    build_call(5, b, dt);
    char ts[CALL_BURSTS + 1][32];
    printf("static const struct v_step v_steps_sms_data_call[] = {\n");
    for (int i = 0; i < CALL_BURSTS; i++) {
        char hex[67];
        tohex(b[i], RDOF_BURST_BYTES, hex);
        snprintf(ts[i], sizeof ts[i], "%.2f", SC_T0 + 0.06 * i);
        printf("    {'f', \"%s\", %d, 0, 1, 1, -1, %d, -40.0, %s},\n", hex, dt[i], i % 3, ts[i]);
    }
    snprintf(ts[CALL_BURSTS], sizeof ts[0], "%.2f", SC_T0 + 0.06 * (CALL_BURSTS - 1) + 3.06);
    printf("    {'t', \"\", 0, 0, 0, 0, 0, 0, 0.0, %s},\n};\n", ts[CALL_BURSTS]);
    printf("#define V_STEPS_SMS_DATA_CALL_N (sizeof v_steps_sms_data_call / sizeof v_steps_sms_data_call[0])\n\n");

    printf("static const struct v_out v_outs_sms_data_call[] = {\n");
    struct capture cap = {0};
    static rdof_tracker_t tr;
    static rdof_hb_sink_t hb;
    rdof_tracker_init(&tr, "ROAM1", 0.0, 0.0);
    rdof_sink_t sink = {cap_json, cap_raw, cap_frame, &cap, .on_raw_burst = cap_raw_burst};
    rdof_tracker_add_sink(&tr, sink);
    rdof_hb_sink_init(&hb, 9990003);
    hb.send = hb_send;
    hb.before_stream = hb_before;
    hb.rand32 = hb_rand;
    hb.ctx = &cap;
    rdof_tracker_add_sink(&tr, rdof_hb_sink_as_sink(&hb));
    for (int i = 0; i < CALL_BURSTS; i++) {
        rdof_burst_t r;
        memset(&r, 0, sizeof r);
        memcpy(r.burst, b[i], RDOF_BURST_BYTES);
        r.dt = dt[i];
        r.slot = 1;
        r.family = RDOF_FAMILY_MS;
        r.cc = -1;
        r.sync_errors = i % 3;
        r.power_dbfs = -40.0;
        rdof_tracker_feed(&tr, &r, strtod(ts[i], NULL));
    }
    rdof_tracker_tick(&tr, strtod(ts[CALL_BURSTS], NULL));
    printf("};\n#define V_OUTS_SMS_DATA_CALL_N (sizeof v_outs_sms_data_call / sizeof v_outs_sms_data_call[0])\n\n");
}

/* ── sdr_vectors.h: the demod case ────────────────────────────────────────── */

#define DM_FS 48000
#define DM_SAMPLES 58720
#define DM_FIRST 2040 /* the first burst's reported position */
#define DM_CHUNK 2400

static void demod_case(const char *dir)
{
    uint8_t b[CALL_BURSTS][RDOF_BURST_BYTES];
    int dt[CALL_BURSTS];
    build_call(2, b, dt);
    static rdof_mod_t m;
    if (rdof_mod_init(&m, DM_FS, 127.0, 4.0, 0.5f, CALL_BURSTS, 0))
        fail("mod init");
    for (int i = 0; i < CALL_BURSTS; i++)
        if (rdof_mod_queue_burst(&m, DM_FIRST - RDOF_FM_DELAY + 2880 * i, b[i], NULL, RDOF_MOD_RAMPS))
            fail("mod queue");
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * DM_SAMPLES);
    rdof_mod_iq(&m, x, DM_SAMPLES);
    rdof_mod_free(&m);
    char path[1024];
    snprintf(path, sizeof path, "%s/demod_sms_ms_slot2.cu8", dir);
    write_cu8(path, x, DM_SAMPLES, 0.0765);

    rdof_demod_t d;
    if (rdof_demod_init(&d, 2, 2, RDOF_DEMOD_SQUELCH_HZ, DM_CHUNK))
        fail("demod init");
    int k = 0;
    printf("/* v_dbursts rows of sms_ms_slot2 */\n");
    for (int i = 0; i < DM_SAMPLES; i += DM_CHUNK) {
        int nb;
        const rdof_sdr_burst_t *bs = rdof_demod_feed(&d, x + i, DM_SAMPLES - i < DM_CHUNK ? DM_SAMPLES - i : DM_CHUNK, &nb);
        for (int j = 0; j < nb; j++, k++) {
            const rdof_sdr_burst_t *s = &bs[j];
            char hex[67];
            tohex(s->b.burst, 33, hex);
            printf("    {%lld, %d, %d, %d, %d, %d, %d, %.17g, %.17g, %.17g, \"%s\"},\n", (long long)s->position, s->b.dt,
                   s->b.n, s->b.slot, (int)s->b.family, s->b.cc, s->b.sync_errors, s->b.power_dbfs,
                   (double)s->freq_offset_hz, (double)s->deviation_hz, hex);
            if (k < CALL_BURSTS && memcmp(s->b.burst, b[k], RDOF_BURST_BYTES))
                fail("demod: a burst differs from the one sent");
        }
    }
    if (k != CALL_BURSTS)
        fail("demod: not every burst came back");
    printf("/* v_demod row */\n    {\"sms_ms_slot2\", 2, 2, %d, %d, FIRST, %d},\n\n", DM_CHUNK, DM_SAMPLES, k);
    rdof_demod_free(&d);
    free(x);
}

/* ── sdr_vectors.h: the e2e recording ─────────────────────────────────────── */

#define E2E_FS 288000
#define E2E_SAMPLES 216000
#define E2E_CHUNK 32768
#define E2E_T0 1760000000.0
#define E2E_RSSI_OFFSET -55.0
#define E2E_BEFORE 8640 /* samples modulated before the recording starts */
#define E2E_LAG 48 /* reported position (48 kS/s) minus symbol 0's peak, through the channelizer and FM stage */

/* The other three channels' bursts: the calls the e2e recording has always had (an MS voice call on SDR04, a
 * repeater's outbound on SDR06 with a late entry, a direct mode call on SDR10), as they are received. */
static const char *const SDR04[] = {
    "05390a900dc00dd85790a901c87d5d7f77fd75789b441f3000e0bb80e6c02f809b",
    "5adbe3420a5729bcd2f792e3c7b7f7d5dd57dfd7f337e1dfa435aba7f8a2f2e6e5",
    "ddfba6913b3f395e9ff41f43e7b2306030a09c4b7f10e5f10280628a0126c6b1df",
    "1fa2b97453463aef01117b14e802703060c0321827b547a24bb27d557af55ccf0e",
    "206ac43d38deac537f37dc0859327171d3a14212a4aadc41a44ec09adbf2268ff9",
    "fce8c69573b6ee83d5fa4131c672505223936526783f002fbecd358678bfef8d41",
    "0fc450c246529ddba5ecd5350802100000000b7b0d3827734b7f2fde9e50ef94bf",
    "05560a440d700da057e0a9a1c8bd5d7f77fd757fce501c080780b7c0eec03680a8",
};
static const char *const SDR10[] = {
    "01b402c09f4c0d4055c0f941446d7557f5ff7f5e33781c580560a3c0e6883e12e9",
    "5adbe3420a5729bcd2f792e3c7b7dffd5f55d5f7f337e1dfa435aba7f8a2f2e6e5",
    "ddfba6913b3f395e9ff41f43e7b13060a030091b7f10e5f10280628a0126c6b1df",
    "1fa2b97453463aef01117b14e80170a440c0374827b547a24bb27d557af55ccf0e",
    "206ac43d38deac537f37dc0859317171e305f742a4aadc41a44ec09adbf2268ff9",
    "fce8c69573b6ee83d5fa4131c671506603a74076783f002fbecd358678bfef8d41",
    "0fc450c246529ddba5ecd5350801100000000e2b0d3827734b7f2fde9e50ef94bf",
    "01db02149ffc0d3855b0f9e144ad7557f5ff7f59666c1f600200af80ee882712da",
};
/* SDR06, every 30 ms from slot 1: idle bursts, and on slot 1 a voice superframe with no header */
#define IDLE_BS "0000000000000000000000000a4dff57d75df5df80000000000000000000000000"
static const char *const SDR06[] = {
    IDLE_BS, IDLE_BS, IDLE_BS, IDLE_BS,
    "5adbe3420a5729bcd2f792e3c7b755fd7df75f77f337e1dfa435aba7f8a2f2e6e5", IDLE_BS,
    "ddfba6913b3f395e9ff41f43e7b2306030a09c4b7f10e5f10280628a0126c6b1df", IDLE_BS,
    "1fa2b97453463aef01117b14e80270a060c0621827b547a24bb27d557af55ccf0e", IDLE_BS,
    "206ac43d38deac537f37dc0859327141b1712212a4aadc41a44ec09adbf2268ff9", IDLE_BS,
    "fce8c69573b6ee83d5fa4131c67250905181e526783f002fbecd358678bfef8d41", IDLE_BS,
    "0fc450c246529ddba5ecd5350802100000000b7b0d3827734b7f2fde9e50ef94bf", IDLE_BS,
    IDLE_BS, IDLE_BS, IDLE_BS,
};
#define E2E_SDR08_BURSTS 12 /* the data call's first 12 preambles: the recording ends there */

struct e2e_chan {
    const char *id;
    double freq;
    int cc, slot;
    double tx_offset_hz; /* the transmitter's error, for the AFC */
    float amplitude;
};
static const struct e2e_chan CHANS[4] = {
    {"SDR04", 461087500.0, 2, 1, 255.0, 0.2f},
    {"SDR06", 461137500.0, 2, 1, -298.0, 0.15f},
    {"SDR08", 461187500.0, 2, 2, 87.0, 0.2f},
    {"SDR10", 461237500.0, 1, 1, -90.0, 0.1f},
};
#define E2E_CENTER 461162500.0

static void e2e_out(void *ctx, const char *ch, const char *kind, const char *text)
{
    (*(int *)ctx)++;
    printf("    {\"%s\", \"%s\", ", ch, kind);
    cstr(text);
    printf("},\n");
}
static void e2e_json(void *ctx, const char *ch, const char *json) { e2e_out(ctx, ch, "json", json); }
static void e2e_raw(void *ctx, const char *ch, const char *json) { e2e_out(ctx, ch, "raw", json); }

static void e2e(const char *dir)
{
    rdof_cf_t *x = calloc(E2E_SAMPLES, sizeof(rdof_cf_t)), *y = malloc(sizeof(rdof_cf_t) * E2E_SAMPLES);
    uint8_t call[CALL_BURSTS][RDOF_BURST_BYTES];
    int dt[CALL_BURSTS];
    build_call(2, call, dt);
    for (int c = 0; c < 4; c++) {
        static rdof_mod_t m;
        if (rdof_mod_init(&m, E2E_FS, CHANS[c].freq - E2E_CENTER + CHANS[c].tx_offset_hz, 4.0, CHANS[c].amplitude, 32,
                          -E2E_BEFORE))
            fail("mod init");
        if (c == 1) { /* a repeater: continuous, a CACH before each burst */
            const int n = (int)(sizeof SDR06 / sizeof SDR06[0]);
            for (int i = 0; i < n; i++) {
                uint8_t burst[33], cach24[24], cach[3] = {0};
                unhex(SDR06[i], burst);
                rdof_encode_cach(1 + i % 2, 1, 0, cach24);
                for (int k = 0; k < 24; k++)
                    cach[k / 8] |= (uint8_t)(cach24[k] << (7 - k % 8));
                unsigned flags = (i == 0 ? RDOF_MOD_RAMP_UP : 0) | (i == n - 1 ? RDOF_MOD_RAMP_DOWN : 0);
                if (rdof_mod_queue_burst(&m, 6 * (168 - E2E_LAG + 1440 * i), burst, cach, flags))
                    fail("mod queue");
            }
        } else {
            const int n = c == 2 ? E2E_SDR08_BURSTS : 8;
            for (int i = 0; i < n; i++) {
                uint8_t burst[33];
                if (c == 2)
                    memcpy(burst, call[i], 33);
                else
                    unhex(c == 0 ? SDR04[i] : SDR10[i], burst);
                if (rdof_mod_queue_burst(&m, 6 * (2048 - E2E_LAG + 2880 * i), burst, NULL, RDOF_MOD_RAMPS))
                    fail("mod queue");
            }
        }
        rdof_mod_iq(&m, y, E2E_BEFORE); /* SDR06 was keyed before the recording starts */
        rdof_mod_iq(&m, y, E2E_SAMPLES);
        rdof_mod_free(&m);
        for (int i = 0; i < E2E_SAMPLES; i++) {
            x[i].re += y[i].re;
            x[i].im += y[i].im;
        }
    }
    free(y);
    char path[1024];
    snprintf(path, sizeof path, "%s/e2e_288k.cu8", dir);
    write_cu8(path, x, E2E_SAMPLES, 0.0056);

    rdof_sdr_channel_t ch[4];
    for (int i = 0; i < 4; i++)
        ch[i] = (rdof_sdr_channel_t){CHANS[i].id, CHANS[i].freq, CHANS[i].cc, CHANS[i].slot};
    rdof_sdr_rx_config_t cfg = {288e3, 0.0, E2E_RSSI_OFFSET, 0.0, true, E2E_CHUNK};
    int outs = 0;
    rdof_sink_t sink = {.on_json = e2e_json, .on_raw = e2e_raw, .ctx = &outs};
    static rdof_sdr_rx_t rx;
    printf("static const struct v_e2e_out v_e2e_outs[] = {\n");
    if (rdof_sdr_rx_init(&rx, &cfg, ch, 4, &sink, E2E_T0) || rx.center_hz != E2E_CENTER)
        fail("rx init");
    for (int i = 0; i < E2E_SAMPLES; i += E2E_CHUNK)
        rdof_sdr_rx_process(&rx, x + i, E2E_SAMPLES - i < E2E_CHUNK ? E2E_SAMPLES - i : E2E_CHUNK);
    printf("};\n#define V_E2E_OUTS_N (%d)\n\n", outs);
    printf("static const struct v_e2e_chan v_e2e_chans[] = {\n");
    for (int i = 0; i < 4; i++)
        printf("    {\"%s\", %.1f, %d, %d, %.17g},\n", CHANS[i].id, CHANS[i].freq, CHANS[i].cc, CHANS[i].slot,
               rx.chz.correction_hz[i]);
    printf("};\n\n");

    /* test_sdr.c test_e2e's status message, with its fixed extras */
    rdof_level_stats_t ls = {0};
    rdof_level_stats_add(&ls, 131072, 0.5, 0.0004);
    rdof_level_stats_add(&ls, 131072, 0.8125, 0.0);
    static const char *const hb_ids[] = {"SDR04", "SDR06"};
    static const bool hb_up[] = {true, false};
    rdof_sdr_status_t st = {.name = "e2e-288k", .at = E2E_T0 + 1.25, .cpu_load = 3.21 / 12.5, .overflows = 7,
                            .has_level = true, .has_gain = true, .gain_db = 28.04, .has_agc = true,
                            .agc_ceiling_db = 28.0, .agc_min_db = 0.0, .agc_changes = 2, .n_hblink = 2,
                            .hblink_ids = hb_ids, .hblink_up = hb_up};
    rdof_level_stats_take(&ls, &st.clip_pct, &st.peak_dbfs);
    static char sj[4096];
    if (!rdof_sdr_status_json(&rx, &st, sj, sizeof sj))
        fail("status");
    printf("#define V_STATUS_JSON ");
    cstr(sj);
    printf("\n");
    rdof_sdr_rx_free(&rx);
    free(x);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: rdof_gen_data_call SDR_DATA_DIR > tables.txt\n");
        return 2;
    }
    scenario();
    demod_case(argv[1]);
    e2e(argv[1]);
    return 0;
}
