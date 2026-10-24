// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for netclock's portable parts. The clock runs are smartlaps'
// test/sim_clock.c: a local clock that runs 18 ppm off true time (plus a slow
// +/-2 ppm temperature wander over 6 h), referenced as NTP does on the bench
// (2 min polls until the rate is learned, then 5 min; 0-8 ms one-way jitter,
// 0-60 ms in the last run; 5% slow round trips of 80-300 ms). Each reports
// the absolute error and the error over 60 s, steered vs offset-only (step
// to each reference, no rate).

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netclock_core.h"
#include "netclock_model.h"
#include "netclock_ntp.h"
#include "netclock_pps.h"
#include "netclock_select.h"
#include "netclock_status.h"
#include "netclock_tempcal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define UTC0 1790000000000000LL  // us, 2026

static int checks, fails;

#define CHECK(cond, ...)                         \
    do {                                         \
        checks++;                                \
        if (!(cond)) {                           \
            fails++;                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
        }                                        \
    } while (0)

// ── the clock model against a drifting, jittery clock (sim_clock) ─────────────

static double g_ppm = 18, g_wander = 2, g_prior = 0, g_jit_ms = 8;
#define WANDER_S 21600.0  // period: sun on the case over an afternoon

// Local us at true time T (s since boot).
static int64_t local_at(double T)
{
    double w = 2 * M_PI / WANDER_S;
    double drift = g_ppm * T + g_wander * (1 - cos(w * T)) / w;
    return (int64_t)llround(T * 1e6 - drift);
}

static double jitter_us(void)
{
    double j = (rand() / (double)RAND_MAX) * g_jit_ms * 1000;
    if (rand() % 100 < 5) {
        j += 80000 + rand() % 220000;
    }
    return j;
}

static void run(const char *name, double hours, double max60_us, double rate_tol_ppm)
{
    static nc_model_t m, raw;
    nc_model_init(&m, g_prior * 1e-6);
    nc_model_init(&raw, 0);
    srand(7);
    double next_ref = 3, max_abs = 0, max_iv = 0, max_abs_raw = 0, max_iv_raw = 0, sum_abs = 0;
    long n_abs = 0;
    int refs = 0;
    // Stamps as a firmware would make them, live, kept 60 s back.
    static double st[61], st_raw[61];
    for (int T = 0; T < hours * 3600; T++) {
        if (T >= next_ref) {
            int64_t t = local_at(T);
            int64_t u = UTC0 + (int64_t)llround(T * 1e6 - jitter_us());  // server time arrives late
            nc_model_add(&m, t, u, false);
            nc_model_init(&raw, 0);
            nc_model_add(&raw, t, u, false);
            refs++;
            next_ref += m.rate_learned && m.rate_n >= 8 ? 300 : 120;  // as netclock polls
        }
        int64_t t = local_at(T);
        double truth = UTC0 + T * 1e6;
        st[T % 61] = (double)(nc_model_utc(&m, t) - UTC0);
        st_raw[T % 61] = (double)(nc_model_utc(&raw, t) - UTC0);
        if (T < 3600 || !m.valid) {
            continue;  // score after the first hour
        }
        double e = nc_model_utc(&m, t) - truth;
        double iv = st[T % 61] - st[(T - 60) % 61] - 60e6;
        double er = nc_model_utc(&raw, t) - truth;
        double ivr = st_raw[T % 61] - st_raw[(T - 60) % 61] - 60e6;
        max_abs = fmax(max_abs, fabs(e));
        max_iv = fmax(max_iv, fabs(iv));
        max_abs_raw = fmax(max_abs_raw, fabs(er));
        max_iv_raw = fmax(max_iv_raw, fabs(ivr));
        sum_abs += fabs(e);
        n_abs++;
    }
    double true_ppm_end = g_ppm + g_wander * sin(2 * M_PI / WANDER_S * hours * 3600);
    printf("  %-34s %3d refs: abs err max %6.2f ms (mean %5.2f), 60 s err max %6.1f us | "
           "offset-only: abs %6.2f ms, 60 s %7.1f us | rate %+6.2f ppm (true %+6.2f), %u outliers\n",
           name, refs, max_abs / 1000, sum_abs / n_abs / 1000, max_iv, max_abs_raw / 1000, max_iv_raw,
           m.rate * 1e6, true_ppm_end, m.outliers);
    CHECK(max_iv <= max60_us, "%s: 60 s error %.1f us over %.0f", name, max_iv, max60_us);
    CHECK(fabs(m.rate * 1e6 - true_ppm_end) <= rate_tol_ppm, "%s: rate %.2f ppm, true %.2f", name, m.rate * 1e6,
          true_ppm_end);
}

static void model_runs(void)
{
    printf("clock model:\n");
    run("18 ppm, +/-2 ppm wander (6 h), 8 h", 8, 250, 1.5);  // a 3 h fit lags a moving rate
    g_wander = 0;
    run("18 ppm steady, 8 h", 8, 250, 1.0);
    g_ppm = -35;
    run("-35 ppm steady, 8 h", 8, 250, 1.0);
    g_ppm = 18;
    g_prior = 17;
    run("18 ppm, stored rate 17 ppm, 2 h", 2, 250, 1.0);
    g_prior = 0;
    g_jit_ms = 60;  // Wi-Fi + internet: NTP offsets scatter by tens of ms
    run("18 ppm, 0-60 ms jitter, 8 h", 8, 300, 1.5);
    g_jit_ms = 8;
    g_wander = 2;
}

static void model_steps(void)
{
    nc_model_t m;
    nc_model_init(&m, 0);
#define AT(t, off) (t), (UTC0 + (t) - 1000000 + (off))
    CHECK(nc_model_add(&m, AT(1000000, 0), false) == NC_STEP, "first reference steps");
    CHECK(nc_model_utc(&m, 2000000) == UTC0 + 1000000, "runs at rate 0");
    // One reference 1 s off: an outlier, twice; the third in a row steps.
    CHECK(nc_model_add(&m, AT(61000000, 0), false) == NC_SLEW, "on time: slew");
    CHECK(nc_model_add(&m, AT(121000000, 1000000), false) == NC_OUTLIER, "1 s off: outlier");
    CHECK(nc_model_add(&m, AT(181000000, 1000000), false) == NC_OUTLIER, "again: outlier");
    CHECK(nc_model_add(&m, AT(241000000, 1000000), false) == NC_STEP, "third: step");
    CHECK(m.outliers == 2, "2 outliers, got %u", m.outliers);
    CHECK(nc_model_utc(&m, 241000000) == UTC0 + 241000000, "stepped to it");
    // A small offset is slewed: 30% of it, at 2 us/s.
    CHECK(nc_model_add(&m, AT(301000000, 1000000 + 5000), false) == NC_SLEW, "5 ms off: slew");
    int64_t left = nc_model_slew_left(&m, 301000000 + 100000000);
    CHECK(left == 1500 - 200, "slewing 30%% of 5 ms at 2 us/s: %lld left after 100 s", (long long)left);
#undef AT
    // A new source steps.
    CHECK(nc_model_add(&m, 400000000, UTC0 + 500000000, true) == NC_STEP, "new source steps");
    // A prior beyond 500 ppm is nonsense: dropped.
    nc_model_init(&m, 900e-6);
    CHECK(m.rate == 0, "absurd prior dropped");
}

// ── selection ─────────────────────────────────────────────────────────────────

static void sel(const char *name, int n, const double *off, const double *lam, const char *want, double want_off)
{
    nc_cand_t c[8];
    char mark[9] = {0};
    for (int i = 0; i < n; i++) {
        c[i] = (nc_cand_t){.ok = lam[i] > 0, .off_us = off[i] * 1000, .lambda_us = lam[i] * 1000};
    }
    double o = 0;
    int b = nc_select(c, n, mark, &o);
    CHECK(strcmp(mark, want) == 0 && (b < 0 || fabs(o / 1000 - want_off) < 0.05), "select %s: marks %s offset %.2f",
          name, mark, b < 0 ? NAN : o / 1000);
}

static void selection(void)
{
    sel("3 agree", 3, (double[]){1, 2, 1.5}, (double[]){5, 5, 10}, "*++", 1.48);
    sel("1 falseticker of 3", 3, (double[]){1, 40, 2}, (double[]){5, 5, 5}, "*x+", 1.5);
    sel("2 disagree: tighter wins", 2, (double[]){1, 40}, (double[]){5, 3}, "x*", 40);
    sel("2 vs 2: no majority", 4, (double[]){0, 1, 40, 41}, (double[]){3, 3, 3, 3}, "xxxx", 0);
    sel("one silent, two agree", 3, (double[]){0, 2, 3}, (double[]){0, 4, 8}, "?*+", 2.2);
}

// ── NTP packets and a poll ────────────────────────────────────────────────────

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (56 - 8 * i));
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (24 - 8 * i));
    }
}

// A server reply to s's request: true UTC = local + off_us; the request takes
// d1 us out and d2 back, the server holds it hold_us.
static int reply_for(nc_src_t *s, uint8_t *p, int64_t off_us, int64_t d1, int64_t d2, int64_t hold, int stratum,
                     int64_t *t4)
{
    memset(p, 0, 48);
    p[0] = (0 << 6) | (4 << 3) | 4;
    p[1] = (uint8_t)stratum;
    put32(p + 4, (uint32_t)(0.010 * 65536));  // root delay 10 ms
    put32(p + 8, (uint32_t)(0.002 * 65536));  // root dispersion 2 ms
    put32(p + 12, 0x47505300);                // "GPS"
    put64(p + 24, s->cookie);
    int64_t t2 = s->t1 + d1 + off_us, t3 = t2 + hold;
    put64(p + 32, nc_us_to_ntp(t2));
    put64(p + 40, nc_us_to_ntp(t3));
    *t4 = s->t1 + d1 + hold + d2;
    return 48;
}

static bool model_off(void *ctx, int64_t t, int64_t u, double *off)
{
    return nc_clock_offset((const nc_clock_t *)ctx, t, u, off);
}

static void ntp(void)
{
    // Timestamps both ways, through 2036.
    for (int64_t us = UTC0; us < 4102444800000000LL; us += 7LL * 365 * 86400 * 1000000 + 123457) {  // to 2100
        int64_t back = nc_ntp_to_us(nc_us_to_ntp(us));
        CHECK(llabs(back - us) <= 1, "NTP time round trip at %lld: %lld", (long long)us, (long long)back);
    }
    CHECK(nc_ntp_to_us(0xE6D4D0800000000ULL << 4) > 0, "an era 0 time");

    // Server lists and pools.
    nc_src_t s[NETCLOCK_MAX_SRC];
    int n = nc_parse_servers(" time.example.org, pool.ntp.org;10.0.0.1 extra", s, NETCLOCK_MAX_SRC);
    CHECK(n == 6, "1 + 3 + 1 + 1 servers, got %d", n);
    CHECK(!strcmp(s[0].name, "time.example.org") && !strcmp(s[1].name, "0.pool.ntp.org") &&
              !strcmp(s[3].name, "2.pool.ntp.org") && !strcmp(s[4].name, "10.0.0.1") && !strcmp(s[5].name, "extra"),
          "names %s %s %s %s", s[0].name, s[1].name, s[3].name, s[4].name);
    CHECK(nc_parse_servers("a b c d e f g", s, NETCLOCK_MAX_SRC) == NETCLOCK_MAX_SRC, "at most NETCLOCK_MAX_SRC");
    CHECK(nc_parse_servers("0.pool.ntp.org", s, 6) == 1, "a numbered pool member is one server");
    CHECK(nc_parse_servers("", s, 6) == 0, "no servers");

    // The request.
    uint8_t p[48];
    nc_ntp_request(p, 0x1122334455667788ULL, 300);
    CHECK(p[0] == 0x23 && p[2] == 8 && p[40] == 0x11 && p[47] == 0x88, "request header %02x poll %d", p[0], p[2]);

    // Three servers 0.5 ms apart in offset, one 40 ms off: a poll combines the three.
    static nc_clock_t clk;
    nc_clock_init(&clk, 0, false);
    n = nc_parse_servers("a b c d", s, NETCLOCK_MAX_SRC);
    const int64_t off[4] = {UTC0 + 1000, UTC0 + 1500, UTC0 + 500, UTC0 + 40000};
    int64_t now = 10000000;
    for (int round = 0; round < 6; round++) {
        for (int i = 0; i < n; i++) {
            s[i].resolved = true;
            s[i].ip[3] = (uint8_t)(i + 1);
            s[i].got = false;
            s[i].cookie = 0xABCD000000000000ULL + (uint64_t)(round * 16 + i);
            s[i].t1 = now + i * 1000;
            int64_t t4;
            int len = reply_for(&s[i], p, off[i], 3000, 3000 + (round == 2 && i == 0 ? 60000 : 0), 50, 1, &t4);
            nc_reply_t r = nc_src_reply(&s[i], p, len, t4, false);
            CHECK(r == NC_REPLY_SAMPLE, "round %d server %d: a sample (%d)", round, i, r);
            CHECK(nc_src_reply(&s[i], p, len, t4, false) == NC_REPLY_IGNORED, "a second copy is ignored");
            nc_src_poll_end(&s[i]);
        }
        int64_t t = 0, u = 0;
        bool had;
        int bi = nc_combine(s, n, model_off, &clk, &t, &u, &had);
        CHECK(bi >= 0 && had, "round %d: a reference", round);
        if (round == 2) {
            CHECK(s[0].mark == '~', "a 60 ms slower round trip is a delay spike: %c", s[0].mark);
        }
        if (bi >= 0) {
            CHECK(s[3].mark == 'x', "round %d: the 40 ms server is a falseticker: %c", round, s[3].mark);
            // The mean of 1000, 1500 and 500 (of 1500 and 500 in round 2): 1000 us.
            CHECK(fabs((double)(u - t - UTC0) - 1000) < 300, "round %d: combined offset %lld us", round,
                  (long long)(u - t - UTC0));
            nc_clock_add(&clk, NETCLOCK_SRC_NTP, t, u, s[bi].name, NAN, NULL);
        }
        now += 120000000;
    }
    CHECK(s[1].reach == 0x3F, "reach after 6 polls: %o", s[1].reach);
    CHECK(s[1].stratum == 1, "stratum");
    netclock_src_view_t v;
    nc_src_view(&s[1], now, &v);
    CHECK(!strcmp(v.ip, "0.0.0.2") && !strcmp(v.refid, "GPS") && v.np > 1, "view: ip %s refid %s np %d", v.ip,
          v.refid, v.np);
    CHECK(fabs(v.delay_ms - 6.0) < 0.01 && fabs(v.root_delay_ms - 10.0) < 0.01, "delay %.3f root %.3f", v.delay_ms,
          v.root_delay_ms);

    // Kiss-o'-death, an unsynchronized server, a stale reply.
    s[0].got = false;
    s[0].cookie = 77;
    s[0].t1 = now;
    int64_t t4;
    int len = reply_for(&s[0], p, UTC0, 3000, 3000, 50, 0, &t4);
    CHECK(nc_src_reply(&s[0], p, len, t4, false) == NC_REPLY_KOD && s[0].kod_skip == NC_NTP_KOD_SKIP, "KoD");
    s[0].got = false;
    len = reply_for(&s[0], p, UTC0, 3000, 3000, 50, 2, &t4);
    p[0] |= 3 << 6;
    CHECK(nc_src_reply(&s[0], p, len, t4, false) == NC_REPLY_UNUSABLE, "LI 3: unusable");
    s[0].got = false;
    len = reply_for(&s[0], p, UTC0, 3000, 3000, 50, 2, &t4);
    p[31] ^= 1;
    CHECK(nc_src_reply(&s[0], p, len, t4, false) == NC_REPLY_IGNORED, "wrong cookie: ignored");
    // iburst keeps the quickest reply.
    s[0].got = false;
    len = reply_for(&s[0], p, UTC0, 9000, 9000, 50, 2, &t4);
    CHECK(nc_src_reply(&s[0], p, len, t4, true) == NC_REPLY_KEPT, "iburst: a slower reply is not taken");
}

// ── the clock core and the status JSON ────────────────────────────────────────

static void core(void)
{
    static nc_clock_t c;
    nc_clock_init(&c, 17e-6, true);
    int64_t u;
    CHECK(!nc_clock_utc(&c, 0, &u), "no time before a reference");
    CHECK(c.m.rate == 17e-6, "stored rate is the prior");
    double rate, skew;
    bool learned;
    CHECK(nc_clock_rate(&c, &rate, &skew, &learned) && rate == 17e-6 && skew == 0 && !learned, "the stored rate");
    static nc_clock_t none;
    nc_clock_init(&none, 0, false);
    CHECK(!nc_clock_rate(&none, &rate, &skew, &learned), "no rate before any");
    CHECK(nc_clock_add(&c, NETCLOCK_SRC_BROWSER, 1000000, UTC0, NULL, 30.5f, NULL) == NC_STEP, "browser steps");
    CHECK(c.source == NETCLOCK_SRC_BROWSER && nc_clock_utc(&c, 1000000, &u) && u == UTC0, "browser time");
    CHECK(nc_clock_add(&c, NETCLOCK_SRC_NTP, 2000000, UTC0 + 1000000 + 400, "x", NAN, NULL) == NC_STEP,
          "NTP is a new source: step");
    // Learn a rate over 20 references 2 min apart, 10 ppm.
    bool store = false, stored_once = false;
    for (int i = 1; i <= 20; i++) {
        int64_t t = 2000000 + i * 120000000LL;
        nc_clock_add(&c, NETCLOCK_SRC_NTP, t, UTC0 + 1000400 + (int64_t)(i * 120000000LL * (1 + 10e-6)), "x", NAN,
                     &store);
        if (store) {
            nc_clock_stored(&c, c.m.rate, t);
            stored_once = true;
        }
    }
    CHECK(stored_once && fabs(c.rate_stored - 10e-6) < 0.5e-6, "learned rate stored: %.2f ppm", c.rate_stored * 1e6);
    CHECK(nc_clock_rate_settled(&c), "settled");
    CHECK(nc_clock_rate(&c, &rate, &skew, &learned) && learned && fabs(rate - 10e-6) < 0.5e-6 && skew >= 0 &&
              skew < 0.1e-6, "the learned rate %.3f ppm, skew %.4f", rate * 1e6, skew * 1e6);

    double q0, q1;
    CHECK(!nc_clock_quality(&none, 0, &q0), "no quality before a reference");
    CHECK(nc_clock_quality(&c, 2000000 + 20 * 120000000LL, &q0) &&
              nc_clock_quality(&c, 2000000 + 20 * 120000000LL + 3600000000LL, &q1) && q0 >= 0 && q0 < 1000 && q1 >= c.m.rate_skew * 3.5e9,
          "quality %.1f us (slewing), an hour on %.1f us (the rate error)", q0, q1);

    static netclock_status_t st;
    memset(&st, 0, sizeof(st));
    st.die_c = NAN;
    nc_clock_status(&c, 2000000 + 21 * 120000000LL, &st);
    CHECK(st.source == NETCLOCK_SRC_NTP && st.valid && st.fit_ok, "status");
    CHECK(st.n_log == 22 && st.log[0].src == NETCLOCK_SRC_BROWSER && st.log[0].result == NC_STEP, "log %d", st.n_log);
    CHECK(!strcmp(st.rate_src, "learned") && fabs(st.rate_ppm - 10) < 0.5, "rate %s %.2f", st.rate_src, st.rate_ppm);
    CHECK(st.since_sync_s == 120, "since sync %d", st.since_sync_s);
    st.n_src = 1;
    snprintf(st.src[0].name, sizeof(st.src[0].name), "a\"b");
    st.src[0].mark = '*';
    st.src[0].offset_ms = 0.25f;
    st.src[0].last_rx_s = -1;
    st.poll_s = 300;
    static char buf[16384];
    size_t n = netclock_status_json(&st, buf, sizeof(buf));
    CHECK(n > 0 && n == strlen(buf), "JSON written (%zu)", n);
    CHECK(!strncmp(buf, "{\"source\":\"ntp\",\"synced\":true,\"since_sync_s\":120,\"poll_s\":300,", 61), "JSON head %.80s",
          buf);
    CHECK(strstr(buf, "\"name\":\"a\\\"b\"") && strstr(buf, "\"offset_ms\":0.25,") && strstr(buf, "\"last_rx_s\":-1"),
          "source row %.300s", strstr(buf, "\"sources\""));
    CHECK(strstr(buf, "\"log\":[{\"utc_ms\":1790000000000,\"t_s\":1,\"src\":\"browser\",\"peer\":\"\",\"result\":\"step\""),
          "log head %.200s", strstr(buf, "\"log\""));
    CHECK(strstr(buf, "\"die_c\":30.5") && buf[n - 1] == '}' && buf[n - 2] == ']', "die and the end");
    int depth = 0, ok = 1;
    for (size_t i = 0; i < n; i++) {
        depth += (buf[i] == '{' || buf[i] == '[') - (buf[i] == '}' || buf[i] == ']');
        ok &= depth >= 0;
    }
    CHECK(ok && depth == 0, "balanced");
    CHECK(netclock_status_json(&st, buf, 100) == 0 && buf[0] == 0, "too small: 0");
}

// ── PPS ───────────────────────────────────────────────────────────────────────

static void pps_edges(void)
{
    nc_pps_t p;
    nc_pps_init(&p);
    int64_t ref = 0;
    CHECK(!nc_pps_edge(&p, 1000000, false, 0, &ref) && p.unlabeled == 1, "clock not set: unnumbered");
    CHECK(!nc_pps_edge(&p, 2000000, true, UTC0 + 400000, &ref) && p.unlabeled == 2, "400 ms from a second: unnumbered");
    // 20 good edges, the clock 120 ms behind, the crystal 50 ppm slow: the 20th is a reference on its second.
    int takes = 0;
    for (int i = 1; i <= 20; i++) {
        int64_t t = 3000000 + (int64_t)llround(i * 1e6 * (1 - 50e-6));
        if (nc_pps_edge(&p, t, true, UTC0 + i * 1000000LL - 120000, &ref)) {
            takes++;
            CHECK(i == 20 && ref == UTC0 + 20000000, "edge %d: reference %lld", i, (long long)(ref - UTC0));
        }
    }
    CHECK(takes == 1 && p.bad == 0, "one reference per %d edges (%d), none out of step", NC_PPS_EVERY, takes);
    // A glitch half a second after an edge: out of step, the count starts over.
    int64_t t20 = 3000000 + (int64_t)llround(20 * 1e6 * (1 - 50e-6));
    CHECK(!nc_pps_edge(&p, t20 + 500000, true, UTC0 + 21000000 - 120000 + 0, &ref) && p.bad == 1 && p.good == 1,
          "glitch: out of step (bad %u good %d)", p.bad, p.good);
    // A missed pulse (2 s gap) numbered 2 apart is fine.
    nc_pps_init(&p);
    nc_pps_edge(&p, 1000000, true, UTC0 + 1000000, &ref);
    nc_pps_edge(&p, 3000000, true, UTC0 + 3000000, &ref);
    CHECK(p.bad == 0 && p.good == 2, "a missed pulse: still in step");

    // The receiver's label (its NMEA): numbers edges with no clock, and over a clock a second off.
    nc_pps_init(&p);
    CHECK(!nc_pps_edge(&p, 1000000, false, 0, &ref) && p.unlabeled == 1, "no clock, no label: unnumbered");
    nc_pps_hint(&p, 1000000, UTC0 / 1000000);
    takes = 0;
    for (int i = 1; i <= 20; i++) {
        int64_t t = 1000000 + (int64_t)llround(i * 1e6 * (1 + 30e-6));
        if (nc_pps_edge(&p, t, false, 0, &ref)) {
            takes++;
            CHECK(i == 20 && ref == UTC0 + 20000000, "labeled edge %d: reference %lld", i, (long long)(ref - UTC0));
        }
    }
    CHECK(takes == 1 && p.by_hint == 20 && p.unlabeled == 1 && p.bad == 0, "20 edges numbered by the label alone");
    // A clock a whole second off (a browser): the label wins
    nc_pps_init(&p);
    nc_pps_hint(&p, 1000000, UTC0 / 1000000);
    nc_pps_edge(&p, 2000000, true, UTC0 + 2000000 + 1000000, &ref);
    CHECK(p.by_hint == 1 && p.last_sec == UTC0 / 1000000 + 1, "over a clock a second off");
    // Past NC_PPS_HINT_S the label's no longer used; the clock numbers again
    int64_t far = 1000000 + (NC_PPS_HINT_S + 1) * 1000000LL;
    nc_pps_edge(&p, far, true, UTC0 + far - 1000000, &ref);
    CHECK(p.by_hint == 1 && p.last_sec == (UTC0 + far - 1000000) / 1000000, "a stale label: the clock's number");
    CHECK(!nc_pps_edge(&p, far + 1000000 + 1000000 / 2, false, 0, &ref) && p.unlabeled == 1, "stale and no clock: unnumbered");
    // An edge half a second from the label's grid isn't numbered by it
    nc_pps_init(&p);
    nc_pps_hint(&p, 1000000, UTC0 / 1000000);
    CHECK(!nc_pps_edge(&p, 1500000, false, 0, &ref) && p.by_hint == 0 && p.unlabeled == 1, "off the grid");
    CHECK(!nc_pps_edge(&p, 500000, false, 0, &ref) && p.by_hint == 0, "before the labeled edge");
}

// NTP for the first hour, then a pulse per second (2 us stamp jitter) for 2 h, then the pulse stops: the error
// against true time, by stage. The local clock is sim_clock's (18 ppm, wandering).
static void pps_run(void)
{
    static nc_clock_t c;
    nc_clock_init(&c, 0, false);
    nc_pps_t p;
    nc_pps_init(&p);
    srand(11);
    double next_ntp = 3, max_pps = 0, max_iv = 0, prev = 0;
    int pps_refs = 0, ntp_skipped = 0;
    const double pps_from = 3600, pps_to = 3 * 3600, end = 4 * 3600;
    for (int T = 0; T < end; T++) {
        int64_t t = local_at(T);
        if (T >= next_ntp) {
            int64_t u = UTC0 + (int64_t)llround(T * 1e6 - jitter_us());
            if (nc_clock_takes(&c, NETCLOCK_SRC_NTP, t)) {
                nc_clock_add(&c, NETCLOCK_SRC_NTP, t, u, "ntp", NAN, NULL);
            } else {
                ntp_skipped++;
            }
            next_ntp += nc_clock_rate_settled(&c) ? 300 : 120;
        }
        if (T >= pps_from && T < pps_to) {
            int64_t te = t + (rand() % 5) - 2, u, ref;
            bool ok = nc_clock_utc(&c, te, &u);
            if (nc_pps_edge(&p, te, ok, u, &ref) && nc_clock_takes(&c, NETCLOCK_SRC_PPS, te)) {
                nc_clock_add(&c, NETCLOCK_SRC_PPS, te, ref, NULL, NAN, NULL);
                pps_refs++;
            }
        }
        int64_t now_u;
        nc_clock_utc(&c, t, &now_u);
        double e = (double)(now_u - UTC0) - T * 1e6;
        if (T >= pps_from + 1800 && T < pps_to) {  // half an hour into the pulse
            max_pps = fmax(max_pps, fabs(e));
            if (T % 60 == 0 && prev != 0) {
                max_iv = fmax(max_iv, fabs(e - prev));
            }
        }
        if (T % 60 == 0) {
            prev = e;
        }
        if (T == pps_to + 300) {
            CHECK(c.source == NETCLOCK_SRC_NTP, "5 min after the pulse stops NTP leads (%d)", c.source);
        }
    }
    printf("  PPS after NTP, 2 h: %d references, abs err max %.1f us, 60 s err max %.1f us, rate %+.3f ppm; "
           "%d NTP polls skipped while it led\n", pps_refs, max_pps, max_iv, c.m.rate * 1e6, ntp_skipped);
    CHECK(c.source == NETCLOCK_SRC_NTP, "NTP leads after the pulse");
    CHECK(max_pps < 50, "with the pulse: abs error %.1f us", max_pps);
    CHECK(max_iv < 10, "with the pulse: 60 s error %.1f us", max_iv);
    CHECK(ntp_skipped > 10, "NTP skipped while the pulse leads (%d)", ntp_skipped);
}

// ── the temperature table and the clock following it ─────────────────────────

// The table: bins by temperature, averaged within a bin, interpolated
// between, flat beyond, the least recently learned bin dropped when full.
static void tcal(void)
{
    nc_tcal_t c;
    nc_tcal_init(&c);
    double v = 0;
    CHECK(!nc_tcal_get(&c, 30, &v) && nc_tcal_valid(&c), "tcal: empty");
    nc_tcal_put(&c, 51.4, -2.0); // bin 52
    nc_tcal_put(&c, 30.2, -3.0); // bin 30
    nc_tcal_put(&c, 52.6, -2.2); // bin 52 again: averaged
    CHECK(c.count == 2 && c.e[0].temp_c == 30 && c.e[1].temp_c == 52 && c.e[1].n == 2, "tcal: two bins, %d", c.count);
    CHECK(fabs(c.e[1].v - -2.1) < 1e-6, "tcal: bin 52 averages to %.3f", c.e[1].v);
    CHECK(nc_tcal_get(&c, 41, &v) && fabs(v - -2.55) < 1e-6, "tcal: 41 C interpolates to %.3f", v);
    CHECK(nc_tcal_get(&c, 10, &v) && v == c.e[0].v, "tcal: colder than seen: the coldest bin");
    CHECK(nc_tcal_get(&c, 70, &v) && v == c.e[1].v, "tcal: warmer than seen: the warmest bin");
    // A bad learning (8 ppm off) fades: 8 good ones after it bring the bin within 0.5.
    nc_tcal_put(&c, 30, 5.0);
    for (int i = 0; i < 8; i++) {
        nc_tcal_put(&c, 30, -3.0);
    }
    CHECK(fabs(c.e[0].v - -3.0) < 0.5, "tcal: a bad learning faded to %.3f", c.e[0].v);
    // Full: the least recently learned goes (bin 52, learned before the 30s).
    for (int t = -60; t < -60 + 2 * (NC_TCAL_N - 2); t += 2) {
        nc_tcal_put(&c, t, 0.1 * t);
    }
    CHECK(c.count == NC_TCAL_N && nc_tcal_valid(&c), "tcal: full, %d", c.count);
    nc_tcal_put(&c, 80, 1.0);
    CHECK(c.count == NC_TCAL_N && !nc_tcal_find(&c, 52) && nc_tcal_find(&c, 80) && nc_tcal_find(&c, 30) &&
              nc_tcal_valid(&c),
          "tcal: bin 52 dropped for 80");
    CHECK(!nc_tcal_find(&c, NAN), "tcal: no temperature, no bin");
    nc_tcal_t bad = c;
    bad.version = 99;
    CHECK(!nc_tcal_valid(&bad), "tcal: another version isn't read");
    // Settling: a die warming 41 to 51 C over 8 minutes, then wandering a degree.
    nc_settle_t st;
    nc_settle_init(&st, 60, 600, 2.0);
    double at = -1;
    for (double t = 0; t <= 1800; t += 5) {
        nc_settle_add(&st, t, (t < 480 ? 41 + 10 * t / 480 : 51) + ((int)(t / 60) % 2));
        if (at < 0 && nc_settle_ok(&st, t)) {
            at = t;
        }
    }
    CHECK(at >= 480 + 420 && at <= 480 + 660, "settle: the die settled at %.0f s", at);
    nc_settle_add(&st, 1805, NAN);
    nc_settle_add(&st, 1810, 51);
    CHECK(!nc_settle_ok(&st, 1810), "settle: a gap starts over");
    printf("temperature table: 2 C bins, interpolated, %d kept; settled %.0f s into a warm-up\n", NC_TCAL_N, at);
}

// A crystal at 10 ppm at 30 C and 0.2 ppm more a degree. One run learns it at
// 30 C (3 h), then warmed to 50 C (4.5 h), with NTP every 5 min (0 to 4 ms
// jitter). The next boots on that table at 30 C, takes one reference, and
// warms to 50 C over 20 minutes with no more for 2 h: the rate follows the
// table, where a single stored rate drifts off by the 4 ppm.
static double tc_ppm(double die)
{
    return 10 + 0.2 * (die - 30);
}

static void tc_ref(nc_clock_t *c, double T, double local, double die, bool jitter)
{
    bool store = false;
    double j = jitter ? (rand() / (double)RAND_MAX) * 4000 : 0;
    nc_clock_add(c, NETCLOCK_SRC_NTP, (int64_t)llround(local), UTC0 + (int64_t)llround(T * 1e6 + j), "x", (float)die,
                 &store);
    if (store) {
        nc_clock_stored(c, c->m.fit_rate, (int64_t)llround(local));
    }
}

// Holdover: one reference at boot, then 2 h warming 30 to 50 C. Returns the error at the end (ms).
static double tc_holdover(nc_clock_t *c)
{
    double T = 0, local = 0;
    tc_ref(c, T, local, 30, false);
    for (int k = 1; k <= 720; k++) {
        double die = T < 600 ? 30 : T < 1800 ? 30 + 20 * (T - 600) / 1200 : 50;
        local += 10e6 * (1 - tc_ppm(die) * 1e-6);
        T += 10;
        if (k % 3 == 0) {
            nc_clock_temp(c, (int64_t)llround(local), (float)die);
        }
    }
    int64_t u;
    nc_clock_utc(c, (int64_t)llround(local), &u);
    return (double)(u - (UTC0 + (int64_t)llround(T * 1e6))) / 1000;
}

static void temperature(void)
{
    static nc_clock_t a;
    srand(7);
    nc_clock_init(&a, 0, false);
    double T = 0, local = 0;
    const double ramp = 3 * 3600, end = ramp + 600 + 4.5 * 3600;
    for (int k = 0; T <= end; k++) {
        double die = T < ramp ? 30 : T < ramp + 600 ? 30 + 20 * (T - ramp) / 600 : 50;
        if (k % 30 == 0) {
            tc_ref(&a, T, local, die, true);
        } else if (k % 3 == 0) {
            nc_clock_temp(&a, (int64_t)llround(local), (float)die);
        }
        local += 10e6 * (1 - tc_ppm(die) * 1e-6);
        T += 10;
    }
    const nc_tcal_entry_t *c30 = nc_tcal_find(&a.tcal, 30), *c50 = nc_tcal_find(&a.tcal, 50);
    CHECK(c30 && c50 && a.tcal.count == 2, "temperature: learned at 30 and 50 C (%d bins)", a.tcal.count);
    CHECK(c30 && fabs(c30->v - tc_ppm(30)) < 0.3 && c50 && fabs(c50->v - tc_ppm(50)) < 0.3,
          "temperature: 30 C %.2f ppm, 50 C %.2f (want %.1f, %.1f)", c30 ? c30->v : NAN, c50 ? c50->v : NAN,
          tc_ppm(30), tc_ppm(50));

    static nc_clock_t b, flat;
    nc_clock_init(&b, 0, false);
    nc_clock_set_tcal(&b, &a.tcal, 30);
    CHECK(fabs(b.m.rate * 1e6 - c30->v) < 1e-6 && b.rate_from_store, "temperature: the prior from the table at 30 C");
    double err = tc_holdover(&b);
    nc_clock_init(&flat, c30->v * 1e-6, true);
    double err_flat = tc_holdover(&flat);
    CHECK(fabs(err) < 3 && fabs(err_flat) > 20, "temperature: after 2 h warming, %.1f ms off with the table, %.1f without",
          err, err_flat);
    CHECK(fabs(b.comp * 1e6 - (c50->v - c30->v)) < 0.01, "temperature: compensating %.2f ppm at 50 C", b.comp * 1e6);
    printf("temperature: learned %.2f ppm at 30 C, %.2f at 50 C; 2 h holdover warming 30 to 50 C %.1f ms off "
           "(%.1f on a stored rate)\n", c30->v, c50->v, err, err_flat);
}

// ── edges: what the main runs don't reach ─────────────────────────────────────

// An NTP reply for s with the origin / transmit times given, returning the packet.
static void raw_reply(nc_src_t *s, uint8_t *p, int64_t t2, int64_t t3, int stratum, uint32_t refid)
{
    memset(p, 0, 48);
    p[0] = (0 << 6) | (4 << 3) | 4;
    p[1] = (uint8_t)stratum;
    put32(p + 12, refid);
    put64(p + 24, s->cookie);
    put64(p + 32, nc_us_to_ntp(t2));
    put64(p + 40, nc_us_to_ntp(t3));
}

static void model_edges(void)
{
    nc_model_t m;
    nc_model_init(&m, 0);
    double r;
    CHECK(!nc_model_fit_resid(&m, 1, UTC0, &r) && !nc_model_in_fit(&m, 1), "no residual and no fit membership before a fit");
    nc_model_add(&m, 1000000, UTC0, false);
    nc_model_set_rate(&m, 2000000, 5e-6);
    CHECK(m.rate == 5e-6, "rate set");
    nc_model_set_rate(&m, 3000000, 600e-6);
    nc_model_set_rate(&m, 3000000, -600e-6);
    nc_model_set_rate(&m, 3000000, NAN);
    nc_model_set_rate(&m, 3000000, INFINITY);
    CHECK(m.rate == 5e-6, "a compensation beyond 500 ppm, NaN or infinite is refused (%g)", m.rate);
    CHECK(nc_model_utc(&m, 4000000) > 0, "and the model still reads a time");
    nc_model_init(&m, NAN);
    CHECK(m.rate == 0, "a NaN prior is dropped too");

    // A clock 600 ppm slow: the line through the references says so, and a rate that large is not taken.
    static const int64_t T[4] = {0, 250000000, 500000000, 750000000};
    nc_model_init(&m, 0);
    nc_result_t res[4];
    for (int i = 0; i < 4; i++) {
        res[i] = nc_model_add(&m, 1000000 + T[i], UTC0 + T[i] + (int64_t)llround(T[i] * 600e-6), false);
    }
    CHECK(res[1] == NC_SLEW && res[2] == NC_SLEW && res[3] == NC_SLEW && m.outliers == 0, "the references are accepted");
    CHECK(!m.fit_ok && m.rate == 0 && !m.rate_learned, "a fit of 600 ppm is nonsense: rate stays 0 (%g)", m.rate);

    // Three references at the same instant that agree, two others far off the line: the fit has nothing to stand on.
    nc_model_init(&m, 0);
    static const int64_t T2[5] = {0, 60000000, 700000000, 700000000, 700000000};
    static const int64_t O2[5] = {-100000, -150000, 0, 0, 0};
    int n_after4 = 0;
    for (int i = 0; i < 5; i++) {
        nc_result_t rr = nc_model_add(&m, T2[i], UTC0 + T2[i] + O2[i], false);
        CHECK(i == 0 ? rr == NC_STEP : rr == NC_SLEW, "reference %d accepted (%d)", i, rr);
        if (i == 3) {
            n_after4 = m.rate_n;
        }
    }
    CHECK(m.fit_ok && n_after4 == 4 && m.rate_n == 4, "the fifth reference leaves the last fit in place (%d refs, then %d)", n_after4, m.rate_n);
}

static void core_edges(void)
{
    static nc_clock_t c;
    nc_tcal_t t;
    nc_tcal_init(&t);
    nc_tcal_put(&t, 30, 5.0);
    // a table that isn't valid is ignored
    nc_clock_init(&c, 0, false);
    nc_tcal_t bad = t;
    bad.version = 9;
    nc_clock_set_tcal(&c, &bad, 30);
    CHECK(c.tcal.count == 0 && c.m.rate == 0 && isnan(c.base_die_c), "invalid table: nothing taken");
    // a valid one with no temperature to look up: kept, but the prior is not moved
    nc_clock_set_tcal(&c, &t, NAN);
    CHECK(c.tcal.count == 1 && c.m.rate == 0 && isnan(c.base_die_c), "table kept, no die reading: prior unchanged");
    // with one: the prior becomes the table's value there
    nc_clock_init(&c, 0, false);
    nc_clock_set_tcal(&c, &t, 30);
    CHECK(fabs(c.m.rate - 5e-6) < 1e-12 && c.rate_from_store && c.base_die_c == 30 && c.die_c == 30, "prior from the table: %g", c.m.rate);
    // a clock that has a reference already keeps its rate
    nc_clock_init(&c, 0, false);
    bool store = false;
    nc_clock_add(&c, NETCLOCK_SRC_NTP, 1000000, UTC0, "a", NAN, &store);
    nc_clock_set_tcal(&c, &t, 30);
    CHECK(c.tcal.count == 1 && c.m.rate == 0 && isnan(c.base_die_c), "set after a reference: the table is kept, the rate is not touched");
}

static void core_state(void)
{
    static nc_clock_t c;
    static netclock_status_t st;
    // before any reference
    nc_clock_init(&c, 0, false);
    memset(&st, 0xAA, sizeof(st));
    st.utc_us = 0;
    nc_clock_status(&c, 5000000, &st);
    CHECK(st.source == NETCLOCK_SRC_NONE && !st.valid && st.since_sync_s == -1 && !strcmp(st.rate_src, "none") && st.n_log == 0 &&
              st.slew_left_ms == 0 && !st.fit_ok,
          "status of a clock never set");
    double x, y;
    bool l;
    int64_t u;
    CHECK(!nc_clock_quality(&c, 1000, &x) && !nc_clock_rate(&c, &x, &y, &l) && !l && !nc_clock_rate_settled(&c) &&
              !nc_clock_offset(&c, 1000, UTC0, &x) && !nc_clock_utc(&c, 1000, &u),
          "no quality, rate, offset or time");
    // a stored rate is reported as such, and has no skew
    nc_clock_init(&c, 17e-6, true);
    nc_clock_status(&c, 0, &st);
    CHECK(!strcmp(st.rate_src, "stored") && fabs(st.rate_ppm - 17) < 1e-9, "stored rate in the status");
    nc_clock_init(&c, 0, false);
    c.rate_from_store = true; // a stored rate of zero is still a stored rate
    CHECK(nc_clock_rate(&c, &x, &y, &l) && x == 0 && y == 0 && !l, "stored zero rate");

    // sources: PPS leads, then NTP, then a browser
    nc_clock_init(&c, 0, false);
    CHECK(nc_clock_takes(&c, NETCLOCK_SRC_BROWSER, 0) && nc_clock_takes(&c, NETCLOCK_SRC_NTP, 0) && nc_clock_takes(&c, NETCLOCK_SRC_PPS, 0),
          "anything sets a clock that has no source");
    bool store;
    int64_t t = 1000000;
    nc_clock_add(&c, NETCLOCK_SRC_BROWSER, t, UTC0, "b", NAN, &store);
    CHECK(nc_clock_takes(&c, NETCLOCK_SRC_BROWSER, t) && nc_clock_takes(&c, NETCLOCK_SRC_NTP, t), "browser, then more of it or NTP");
    nc_clock_add(&c, NETCLOCK_SRC_NTP, t + 1000000, UTC0 + 1000000, "n", NAN, &store);
    CHECK(!nc_clock_takes(&c, NETCLOCK_SRC_BROWSER, t + 5000000) && nc_clock_takes(&c, NETCLOCK_SRC_NTP, t + 5000000), "a browser never displaces NTP");
    nc_clock_add(&c, NETCLOCK_SRC_PPS, t + 2000000, UTC0 + 2000000, "pps", NAN, &store);
    CHECK(!nc_clock_takes(&c, NETCLOCK_SRC_NTP, t + 2000000 + NC_PPS_HOLD_S * 1000000LL) &&
              nc_clock_takes(&c, NETCLOCK_SRC_NTP, t + 2000001 + NC_PPS_HOLD_S * 1000000LL),
          "NTP takes over from a silent pulse after %d s, not before", NC_PPS_HOLD_S);
    CHECK(!nc_clock_takes(&c, NETCLOCK_SRC_BROWSER, t + 999000000LL), "a browser never displaces PPS");
    CHECK(nc_clock_takes(&c, NETCLOCK_SRC_PPS, t), "PPS goes on being taken");
    CHECK(!nc_clock_takes(&c, NETCLOCK_SRC_GNSS, t + 2000000 + NC_PPS_HOLD_S * 1000000LL) &&
              nc_clock_takes(&c, NETCLOCK_SRC_GNSS, t + 2000001 + NC_PPS_HOLD_S * 1000000LL),
          "GNSS NMEA alone takes over from a silent pulse too");
    {
        // GNSS NMEA time: above a browser, below NTP
        nc_clock_t g;
        nc_clock_init(&g, 0, false);
        nc_clock_add(&g, NETCLOCK_SRC_BROWSER, t, UTC0, "b", NAN, &store);
        CHECK(nc_clock_takes(&g, NETCLOCK_SRC_GNSS, t), "GNSS displaces a browser");
        nc_clock_add(&g, NETCLOCK_SRC_GNSS, t + 1000000, UTC0 + 1000000, NULL, NAN, &store);
        CHECK(!nc_clock_takes(&g, NETCLOCK_SRC_BROWSER, t + 2000000) && nc_clock_takes(&g, NETCLOCK_SRC_NTP, t + 2000000),
              "a browser never displaces GNSS; NTP does");
        nc_clock_add(&g, NETCLOCK_SRC_NTP, t + 2000000, UTC0 + 2000000, "n", NAN, &store);
        CHECK(!nc_clock_takes(&g, NETCLOCK_SRC_GNSS, t + 999000000LL), "GNSS never displaces NTP");
        CHECK(!strcmp(netclock_source_name(NETCLOCK_SRC_GNSS), "gnss"), "its name");
    }

    // quality counts the time since the last reference, and not a negative one
    CHECK(nc_clock_quality(&c, t + 100000000, &x) && x > 0 && isfinite(x), "quality after a reference: %g us", x);
    double before;
    CHECK(nc_clock_quality(&c, t + 2000000, &before) && nc_clock_quality(&c, t, &y) && y <= before, "a time before the last reference counts as no time");

    // the log keeps the last NETCLOCK_LOG references, oldest first
    nc_clock_init(&c, 0, false);
    for (int i = 0; i < NETCLOCK_LOG + 8; i++) {
        nc_clock_add(&c, NETCLOCK_SRC_NTP, 1000000LL * (i + 1), UTC0 + 1000000LL * (i + 1), "p", NAN, &store);
    }
    nc_clock_status(&c, 100000000, &st);
    CHECK(st.n_log == NETCLOCK_LOG && st.log[0].t_us == 1000000LL * 9 && st.log[NETCLOCK_LOG - 1].t_us == 1000000LL * (NETCLOCK_LOG + 8),
          "log: %d entries from %lld s", st.n_log, (long long)(st.log[0].t_us / 1000000));
    CHECK(st.since_sync_s == 100 - (NETCLOCK_LOG + 8) && st.valid && st.utc_us > 0, "since the last reference");
}

static void ntp_edges(void)
{
    nc_src_t s[NETCLOCK_MAX_SRC + 3];
    CHECK(nc_parse_servers(" , ; ", s, 4) == 0 && nc_parse_servers("a, ", s, 4) == 1 && nc_parse_servers("\t", s, 4) == 0, "only separators");
    CHECK(nc_parse_servers(NULL, s, 4) == 0 && nc_parse_servers("a b", s, 0) == 0, "no list, no room");
    int n = nc_parse_servers("a,b;c d", s, 2);
    CHECK(n == 2 && !strcmp(s[1].name, "b") && s[0].mark == '-', "stops at the room given; unresolved rows are marked '-'");
    char longname[80];
    memset(longname, 'x', 79);
    longname[79] = 0;
    n = nc_parse_servers(longname, s, 3);
    CHECK(n == 1 && strlen(s[0].name) == 39, "a long name is cut to 39 characters (%zu)", strlen(s[0].name));
    n = nc_parse_servers("europe-west.pool.ntp.org", s, 5);
    CHECK(n == 3 && !strcmp(s[0].name, "0.europe-west.pool.ntp.org") && !strcmp(s[2].name, "2.europe-west.pool.ntp.org"), "a pool is three servers");
    CHECK(nc_parse_servers("pool.ntp.org", s, 2) == 2, "a pool is cut by the room left");
    CHECK(nc_parse_servers("1.1.1.1.pool.ntp.org", s, 5) == 1, "a name starting with a digit is not a pool");

    // replies: a server holding a request longer than the round trip (negative delay), too slow, times reversed
    nc_src_t a;
    memset(&a, 0, sizeof(a));
    a.cookie = 99;
    a.t1 = 5000000;
    uint8_t p[48];
    int64_t t2 = UTC0 + 3000, t3 = UTC0 + 4000;
    raw_reply(&a, p, t2, t3, 2, 0x0A010203);
    CHECK(nc_src_reply(&a, p, 48, a.t1 + 100, false) == NC_REPLY_SAMPLE, "t4 - t1 less than the server's hold: a sample");
    CHECK(a.delay_us == 0, "with a delay of 0, not negative (%g)", a.delay_us);
    a.got = false;
    a.t1 = 9000000;
    a.cookie = 100;
    raw_reply(&a, p, t2, t3, 2, 0);
    double old_delay = a.delay_us;
    CHECK(nc_src_reply(&a, p, 48, a.t1 + NC_NTP_MAX_DELAY + 2000, false) == NC_REPLY_UNUSABLE && a.delay_us == old_delay, "a round trip over 500 ms is unusable, nothing stored");
    CHECK(nc_src_reply(&a, p, 48, a.t1 + 1, false) == NC_REPLY_IGNORED, "and it answered once already");
    a.got = false;
    a.cookie = 101;
    raw_reply(&a, p, t3, t2, 2, 0); // transmit before receive
    CHECK(nc_src_reply(&a, p, 48, a.t1 + 5000, false) == NC_REPLY_UNUSABLE, "a server whose transmit time precedes its receive time");
    CHECK(a.got, "it still counts for reach");
    a.got = false;
    a.cookie = 102;
    raw_reply(&a, p, t2, t3, 16, 0);
    CHECK(nc_src_reply(&a, p, 48, a.t1 + 5000, false) == NC_REPLY_UNUSABLE, "stratum 16");
    a.got = false;
    a.cookie = 103;
    raw_reply(&a, p, t2, t3, 2, 0);
    CHECK(nc_src_reply(&a, p, 47, a.t1 + 5000, false) == NC_REPLY_IGNORED && !a.got, "a short packet");
    p[0] = (0 << 6) | (4 << 3) | 3;
    CHECK(nc_src_reply(&a, p, 48, a.t1 + 5000, false) == NC_REPLY_IGNORED, "a client-mode packet");
    nc_src_t idle;
    memset(&idle, 0, sizeof(idle));
    raw_reply(&idle, p, t2, t3, 2, 0);
    CHECK(nc_src_reply(&idle, p, 48, 100, false) == NC_REPLY_IGNORED, "a reply with no request outstanding");

    // a poll with no answer counts a miss and shifts the reach register
    nc_src_t q;
    memset(&q, 0, sizeof(q));
    q.reach = 0xFF;
    q.t1 = 1000;
    nc_src_poll_end(&q);
    CHECK(q.misses == 1 && q.reach == 0xFE, "a missed poll: misses %d, reach %02x", q.misses, q.reach);
    q.t1 = 0;
    nc_src_poll_end(&q);
    CHECK(q.misses == 1 && q.reach == 0xFC, "no request sent: not a miss");

    // views: a stratum 2 server's refid is its upstream's address; a stratum 1 one's is text, unprintable bytes dropped
    nc_src_t v;
    memset(&v, 0, sizeof(v));
    snprintf(v.name, sizeof(v.name), "srv");
    v.stratum = 2;
    v.refid = 0x0A010203;
    v.last_rx_us = 5000000;
    netclock_src_view_t view;
    nc_src_view(&v, 9000000, &view);
    CHECK(!strcmp(view.refid, "10.1.2.3") && view.ip[0] == 0 && view.last_rx_s == 4, "stratum 2: refid %s, ip '%s', last rx %d", view.refid, view.ip, view.last_rx_s);
    v.stratum = 1;
    v.refid = 0x47500001;
    v.resolved = true;
    v.ip[0] = 192;
    v.ip[3] = 7;
    v.last_rx_us = 0;
    nc_src_view(&v, 9000000, &view);
    CHECK(!strcmp(view.refid, "GP") && !strcmp(view.ip, "192.0.0.7") && view.last_rx_s == -1, "stratum 1: refid '%s', ip %s", view.refid, view.ip);
    v.stratum = 0;
    nc_src_view(&v, 9000000, &view);
    CHECK(view.refid[0] == 0, "stratum 0: no refid");

    // combining: nothing answered; no majority; more servers than the table holds
    static nc_clock_t clk;
    nc_clock_init(&clk, 0, false);
    n = nc_parse_servers("a b c d", s, 4);
    int64_t t = 0, u = 0;
    bool had = true;
    CHECK(nc_combine(s, n, model_off, &clk, &t, &u, &had) == -1 && !had, "no replies: no reference, no sample");
    CHECK(s[0].mark == '-' && s[3].mark == '-', "unresolved rows stay '-'");
    int64_t now = 20000000;
    const int64_t off[4] = {UTC0, UTC0 + 500, UTC0 + 30000, UTC0 + 30500}; // two pairs 30 ms apart
    for (int i = 0; i < 4; i++) {
        s[i].resolved = true;
        s[i].cookie = 500 + (uint64_t)i;
        s[i].t1 = now;
        int64_t t4;
        int len = reply_for(&s[i], p, off[i], 3000, 3000, 50, 2, &t4);
        CHECK(nc_src_reply(&s[i], p, len, t4, false) == NC_REPLY_SAMPLE, "sample %d", i);
        nc_src_poll_end(&s[i]);
    }
    CHECK(nc_combine(s, n, model_off, &clk, &t, &u, &had) == -1 && had, "two pairs that disagree: samples but no majority");
    CHECK(s[0].mark == 'x' && s[1].mark == 'x' && s[2].mark == 'x' && s[3].mark == 'x', "all falsetickers: %c%c%c%c", s[0].mark, s[1].mark, s[2].mark, s[3].mark);
    // the table holds NETCLOCK_MAX_SRC: a longer list is cut to it
    nc_src_t many[NETCLOCK_MAX_SRC + 3];
    memset(many, 0, sizeof(many));
    for (int i = 0; i < NETCLOCK_MAX_SRC + 3; i++) {
        many[i].resolved = true;
        many[i].mark = 'z';
    }
    CHECK(nc_combine(many, NETCLOCK_MAX_SRC + 3, NULL, NULL, &t, &u, &had) == -1 && !had, "a long list with no replies");
    CHECK(many[NETCLOCK_MAX_SRC - 1].mark == '?' && many[NETCLOCK_MAX_SRC].mark == 'z', "only the first %d rows are looked at", NETCLOCK_MAX_SRC);
}

static void select_edges(void)
{
    const double off[3] = {0, 0, 0}, none[3] = {0, 0, 0};
    sel("nobody answered", 3, off, none, "???", 0);
    sel("none at all", 0, off, none, "", 0);
    // intervals that only touch still overlap: [-5, 0] and [0, 5]
    sel("touching", 2, (double[]){-2.5, 2.5}, (double[]){2.5, 2.5}, "*+", 0);
    // the same spans given in the other order sort the same
    sel("touching, reversed", 2, (double[]){2.5, -2.5}, (double[]){2.5, 2.5}, "*+", 0);
    // identical samples
    sel("identical", 3, (double[]){1, 1, 1}, (double[]){4, 4, 4}, "*++", 1);
    // more candidates than NC_SELECT_MAX: the extra ones are not looked at
    double o[NC_SELECT_MAX + 2], l[NC_SELECT_MAX + 2];
    nc_cand_t c[NC_SELECT_MAX + 2];
    char mark[NC_SELECT_MAX + 4];
    memset(mark, 'z', sizeof(mark));
    for (int i = 0; i < NC_SELECT_MAX + 2; i++) {
        o[i] = 0;
        l[i] = 5;
        c[i] = (nc_cand_t){.ok = true, .off_us = o[i], .lambda_us = l[i]};
    }
    double out = 0;
    int bi = nc_select(c, NC_SELECT_MAX + 2, mark, &out);
    CHECK(bi >= 0 && mark[NC_SELECT_MAX - 1] != 'z' && mark[NC_SELECT_MAX] == 'z', "only NC_SELECT_MAX candidates are marked");
}

static void tcal_edges(void)
{
    nc_tcal_t t;
    nc_tcal_init(&t);
    CHECK(nc_tcal_valid(&t), "empty is valid");
    nc_tcal_put(&t, 20, 1.0);
    nc_tcal_put(&t, 30, 2.0);
    nc_tcal_put(&t, 40, 4.0);
    CHECK(nc_tcal_valid(&t) && t.count == 3, "three bins");
    nc_tcal_t b = t;
    b.version = 0;
    CHECK(!nc_tcal_valid(&b), "another version");
    b = t;
    b.count = NC_TCAL_N + 1;
    CHECK(!nc_tcal_valid(&b), "more bins than fit");
    b = t;
    b.e[1].v = NAN;
    CHECK(!nc_tcal_valid(&b), "a NaN value");
    b = t;
    b.e[1].v = INFINITY;
    CHECK(!nc_tcal_valid(&b), "an infinite value");
    b = t;
    b.e[1].temp_c = 20;
    CHECK(!nc_tcal_valid(&b), "bins out of order (equal)");
    b = t;
    b.e[2].temp_c = 10;
    CHECK(!nc_tcal_valid(&b), "bins out of order");

    // put refuses what isn't a number or a temperature the table can hold
    uint32_t stamp = t.stamp;
    nc_tcal_put(&t, NAN, 1.0);
    nc_tcal_put(&t, 25, NAN);
    nc_tcal_put(&t, 25, INFINITY);
    nc_tcal_put(&t, 300, 1.0);
    nc_tcal_put(&t, -300, 1.0);
    nc_tcal_put(&t, 127, 1.0); // its bin is 128: past an int8
    nc_tcal_put(&t, -131, 1.0); // -132
    CHECK(t.stamp == stamp && t.count == 3, "none of those is learned (stamp +%u, %d bins)", t.stamp - stamp, t.count);
    nc_tcal_put(&t, 124, 1.0); // the last bin that fits is 126
    nc_tcal_put(&t, -128, 1.0);
    CHECK(t.stamp == stamp + 2 && t.count == 5 && nc_tcal_valid(&t), "the ends of the range are (stamp +%u, %d bins)", t.stamp - stamp, t.count);
    CHECK(nc_tcal_find(&t, NAN) == NULL && nc_tcal_find(&t, 25) == NULL && nc_tcal_find(&t, 30) != NULL, "find");
    CHECK(nc_tcal_bin(-3.0) == -4 && nc_tcal_bin(-2.9) == -2 && nc_tcal_bin(29.0) == 28 && nc_tcal_bin(31.0) == 32, "bins round to the nearest even (%d %d %d %d)",
          nc_tcal_bin(-3.0), nc_tcal_bin(-2.9), nc_tcal_bin(29.0), nc_tcal_bin(31.0));

    // get: between two bins, in the second span of three, at a bin, beyond the ends, empty
    nc_tcal_t g;
    nc_tcal_init(&g);
    double v;
    CHECK(!nc_tcal_get(&g, 20, &v), "empty table");
    nc_tcal_put(&g, 20, 1.0);
    nc_tcal_put(&g, 30, 2.0);
    nc_tcal_put(&g, 40, 4.0);
    CHECK(!nc_tcal_get(&g, NAN, &v), "NaN temperature");
    CHECK(nc_tcal_get(&g, 15, &v) && v == 1.0, "colder than any bin: the coldest");
    CHECK(nc_tcal_get(&g, 50, &v) && v == 4.0, "warmer than any: the warmest");
    CHECK(nc_tcal_get(&g, 25, &v) && fabs(v - 1.5) < 1e-6, "between the first two: %g", v);
    CHECK(nc_tcal_get(&g, 35, &v) && fabs(v - 3.0) < 1e-6, "between the last two: %g", v);
    CHECK(nc_tcal_get(&g, 30, &v) && fabs(v - 2.0) < 1e-6, "at a bin: %g", v);
    CHECK(nc_tcal_get(&g, 20, &v) && v == 1.0 && nc_tcal_get(&g, 40, &v) && v == 4.0, "at the ends");
}

static void status_edges(void)
{
    static netclock_status_t st;
    memset(&st, 0, sizeof(st));
    char buf[16384];
    CHECK(!strcmp(netclock_source_name(NETCLOCK_SRC_PPS), "pps") && !strcmp(netclock_source_name(NETCLOCK_SRC_NONE), "none") &&
              !strcmp(netclock_source_name(NETCLOCK_SRC_BROWSER), "browser") && !strcmp(netclock_source_name((netclock_source_t)99), "none"),
          "source names");
    CHECK(netclock_status_json(&st, buf, 0) == 0, "no buffer");
    st.die_c = NAN;
    st.base_die_c = NAN;
    st.source = NETCLOCK_SRC_PPS;
    st.last_err_ms = NAN;
    st.skew_ppm = INFINITY;
    st.rate_src = NULL;
    size_t n = netclock_status_json(&st, buf, sizeof(buf));
    CHECK(strstr(buf, "\"rate_src\":\"none\""), "no rate source: none");
    st.rate_src = "stored";
    st.last_err_ms = -0.0001; // rounds to -0.000: written as 0
    n = netclock_status_json(&st, buf, sizeof(buf));
    CHECK(strstr(buf, "\"last_err_ms\":0,"), "negative zero is 0: %.300s", buf);
    st.last_err_ms = NAN;
    n = netclock_status_json(&st, buf, sizeof(buf));
    CHECK(n > 0 && n == strlen(buf), "json");
    CHECK(strstr(buf, "\"source\":\"pps\"") && strstr(buf, "\"last_err_ms\":null") && strstr(buf, "\"skew_ppm\":null"), "NaN and infinity are null: %.400s", buf);
    CHECK(!strstr(buf, "die_c") && !strstr(buf, "base_die_c"), "no die temperature when unknown");
    CHECK(strstr(buf, "\"tcal\":[]") && strstr(buf, "\"log\":[]"), "empty table and log");

    // the temperature, the table, a log with control characters in a name
    st.die_c = 41.26f;
    st.base_die_c = 38.5;
    nc_tcal_init(&st.tcal);
    nc_tcal_put(&st.tcal, 30, 2.5);
    nc_tcal_put(&st.tcal, 40, -1.25);
    st.n_src = 1;
    snprintf(st.src[0].name, sizeof(st.src[0].name), "bad\"name\\\n\x01");
    st.src[0].mark = 0;
    st.n_log = 3;
    for (int i = 0; i < 3; i++) {
        st.log[i] = (netclock_ref_t){.t_us = 1000000 * (i + 1), .utc_us = UTC0, .err_us = 1500 * (i - 1), .src = NETCLOCK_SRC_NTP,
                                      .result = (uint8_t)(i + 1), .rate_ppm = 12.5f, .die_c = i == 1 ? 39.0f : NAN};
        snprintf(st.log[i].peer, sizeof(st.log[i].peer), "p%d", i);
        st.resid_us[i] = i == 2 ? NAN : 250.0f * i;
        st.in_fit[i] = i == 0;
    }
    n = netclock_status_json(&st, buf, sizeof(buf));
    CHECK(n > 0, "json with a table");
    CHECK(strstr(buf, "\"die_c\":41.3,") && strstr(buf, "\"base_die_c\":38.5,"), "die temperatures: %.600s", buf);
    CHECK(strstr(buf, "\"tcal\":[{\"c\":30,\"ppm\":2.5,\"n\":1},{\"c\":40,\"ppm\":-1.25,\"n\":1}]"), "table");
    CHECK(strstr(buf, "\"name\":\"bad\\\"name\\\\\\u000a\\u0001\""), "quote, backslash and control characters escaped");
    CHECK(strstr(buf, "\"mark\":\"?\""), "a missing mark is ?");
    CHECK(strstr(buf, "\"result\":\"slew\"") && strstr(buf, "\"result\":\"outlier\"") && strstr(buf, "\"result\":\"?\""), "results");
    CHECK(strstr(buf, "\"resid_ms\":0.25,") && strstr(buf, "\"die_c\":39}"), "residual and die temperature only when known");
    CHECK(!strchr(buf, '\n') && !strchr(buf, 1), "no raw control characters");
    // every shorter buffer is refused cleanly
    int bad = 0;
    char small[16384];
    for (size_t cap = 1; cap < n; cap += 13) {
        memset(small, 'z', sizeof(small));
        if (netclock_status_json(&st, small, cap) != 0 || small[0] != 0) {
            bad++;
        }
    }
    CHECK(!bad, "%d short buffers not refused", bad);
    CHECK(netclock_status_json(&st, small, n + 1) == n && netclock_status_json(&st, small, n) == 0, "exact fit, one byte short");
}

int main(void)
{
    model_steps();
    selection();
    ntp();
    core();
    pps_edges();
    model_runs();
    tcal();
    model_edges();
    core_edges();
    core_state();
    ntp_edges();
    select_edges();
    tcal_edges();
    status_edges();
    temperature();
    printf("pulse per second:\n");
    pps_run();
    printf("netclock: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
