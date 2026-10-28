// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// netclock on Linux and macOS (see netclock_posix.h): the clock behind a
// mutex, a state file, and a pthread for NTP. It follows src/netclock.c step
// for step; the maths are in netclock_core.c, netclock_ntp.c,
// netclock_model.c and netclock_select.c.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // ntp_adjtime, clock_settime
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "netclock_posix.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/timex.h>
#endif

#include "netclock_core.h"
#include "netclock_ntp.h"

#define SERVERS_LEN    128     // as NETCLOCK_SERVERS_LEN on the board
#define NTP_PORT       123
#define ROUND_WAIT_MS  1500    // replies later than this are dropped
#define IBURST_GAP_MS  2000
#define RESOLVE_EVERY  48      // re-resolve names every this many polls (pool members come and go)
#define POLL_S         300
#define POLL_FAST_S    120
#define TEMP_EVERY_MS  30000   // the rate follows the temperature this often between references
#define OS_STEP_MS     100
#define STATE_VERSION  1

// The clock: readers take s_mu (a copy, or a short computation on it); a writer works on s_scratch under s_wmu,
// then swaps it in under s_mu.
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_wmu = PTHREAD_MUTEX_INITIALIZER;
static nc_clock_t s_clock, s_scratch;
static netclock_source_t s_source;  // under s_mu

static netclock_posix_opts_t s_o;
static char s_servers[SERVERS_LEN];
static char *s_path;
static bool s_os_warned;

// NTP: s_src belongs to the thread; the view and counters are what the status shows (under s_mu).
static nc_src_t s_src[NETCLOCK_MAX_SRC];
static uint16_t s_port[NETCLOCK_MAX_SRC];
static int s_n;
static netclock_src_view_t s_view[NETCLOCK_MAX_SRC];
static int s_nview;
static int64_t s_view_t;
static uint32_t s_poll_s, s_rounds, s_no_majority;

static pthread_t s_thr;
static bool s_running;  // the thread exists (start / stop only)
static int s_wake[2] = {-1, -1};
static atomic_int s_stop;
static uint64_t s_rng;

// What the state file holds, so a store of one part writes the other again.
static double s_rate_kept;
static bool s_have_rate;
static nc_tcal_t s_tcal_kept;
static bool s_have_tcal;

static void logf_(const char *fmt, ...)
{
    if (!s_o.log) {
        return;
    }
    char m[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    s_o.log(s_o.log_ctx, m);
}

int64_t netclock_posix_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int64_t mono_us(void)
{
    return netclock_posix_mono_ns() / 1000;
}

static int64_t real_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static float temp_c(void)
{
    return s_o.temp_c ? (float)s_o.temp_c(s_o.temp_ctx) : NAN;
}

static uint64_t rng64(void)
{
    // splitmix64, seeded from /dev/urandom at init: the cookie only has to be unguessable off the path.
    uint64_t z = (s_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// ── the state file ───────────────────────────────────────────────────────────
// Text: "netclock 1", "rate_ppb <int>", and "tcal <hex of nc_tcal_t>" when there is a table. The table is this
// machine's struct bytes: the file stays on the machine that wrote it.

static bool state_load(int32_t *ppb, bool *have_rate, nc_tcal_t *tcal)
{
    *have_rate = false;
    nc_tcal_init(tcal);
    if (!s_path) {
        return false;
    }
    FILE *f = fopen(s_path, "r");
    if (!f) {
        return false;
    }
    char line[2 * sizeof(nc_tcal_t) + 64];
    int version = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "netclock ", 9)) {
            version = atoi(line + 9);
        } else if (!strncmp(line, "rate_ppb ", 9) && version == STATE_VERSION) {
            char *end;
            long long v = strtoll(line + 9, &end, 10);
            if (end != line + 9 && !*end && v > INT32_MIN && v < INT32_MAX) {
                *ppb = (int32_t)v;
                *have_rate = true;
                s_rate_kept = v / 1e9;
                s_have_rate = true;
            }
        } else if (!strncmp(line, "tcal ", 5) && version == STATE_VERSION) {
            const char *h = line + 5;
            nc_tcal_t t;
            uint8_t *b = (uint8_t *)&t;
            bool ok = strlen(h) == 2 * sizeof(t);
            for (size_t i = 0; ok && i < sizeof(t); i++) {
                unsigned v = 0;
                ok = isxdigit((unsigned char)h[2 * i]) && isxdigit((unsigned char)h[2 * i + 1]) &&
                     sscanf(h + 2 * i, "%2x", &v) == 1;
                b[i] = (uint8_t)v;
            }
            if (ok && nc_tcal_valid(&t)) {
                *tcal = t;
                s_tcal_kept = t;
                s_have_tcal = true;
            }
        }
    }
    fclose(f);
    return true;
}

// rate, tcal: NULL to keep what the file has. Returns 0 or an errno.
static int state_store(const double *rate, const nc_tcal_t *tcal)
{
    if (rate) {
        s_rate_kept = *rate;
        s_have_rate = true;
    }
    if (tcal) {
        s_tcal_kept = *tcal;
        s_have_tcal = true;
    }
    if (!s_path) {
        return 0;
    }
    size_t n = strlen(s_path) + 5;
    char *tmp = malloc(n);
    if (!tmp) {
        return ENOMEM;
    }
    snprintf(tmp, n, "%s.tmp", s_path);
    FILE *f = fopen(tmp, "w");
    bool ok = f != NULL;
    if (ok) {
        fprintf(f, "netclock %d\n", STATE_VERSION);
        if (s_have_rate) {
            fprintf(f, "rate_ppb %ld\n", lround(s_rate_kept * 1e9));
        }
        if (s_have_tcal) {
            fputs("tcal ", f);
            const uint8_t *b = (const uint8_t *)&s_tcal_kept;
            for (size_t i = 0; i < sizeof(s_tcal_kept); i++) {
                fprintf(f, "%02x", b[i]);
            }
            fputs("\n", f);
        }
        ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
        ok = fclose(f) == 0 && ok;
    }
    int e = 0;
    if (ok && rename(tmp, s_path) == 0) {
        logf_("stored clock rate %+.2f ppm%s", s_rate_kept * 1e6, tcal ? " and the temperature table" : "");
    } else {
        e = errno ? errno : EIO;
        logf_("%s: %s", ok ? s_path : tmp, strerror(e));
        unlink(tmp);
    }
    free(tmp);
    return e;
}

// ── the clock ────────────────────────────────────────────────────────────────

// Discipline mode, after a reference: the OS clock to the model, if asked and it's off by more than os_step_ms.
static void os_clock_follow(void)
{
    if (!s_o.set_os_clock) {
        return;
    }
    const int64_t m = netclock_posix_mono_ns();
    int64_t u = 0;
    pthread_mutex_lock(&s_mu);
    bool ok = nc_clock_utc(&s_clock, m / 1000, &u);
    pthread_mutex_unlock(&s_mu);
    const int64_t want = u * 1000 + m % 1000, diff = want - real_ns() - (netclock_posix_mono_ns() - m);
    const int64_t lim = (int64_t)(s_o.os_step_ms ? s_o.os_step_ms : OS_STEP_MS) * 1000000;
    if (!ok || (diff < lim && diff > -lim)) {
        return;
    }
    const int64_t now = want + (netclock_posix_mono_ns() - m);
    struct timespec ts = {.tv_sec = (time_t)(now / 1000000000), .tv_nsec = (long)(now % 1000000000)};
    if (clock_settime(CLOCK_REALTIME, &ts) == 0) {
        logf_("stepped the OS clock %+.3f s", diff / 1e9);
    } else if (!s_os_warned) {
        s_os_warned = true;
        logf_("can't set the OS clock: %s", strerror(errno));
    }
}

// A reference from src: the clock read utc_us at local t_us. *ignored (if given): a better source leads.
static nc_result_t add_reference(netclock_source_t src, int64_t t_us, int64_t utc_us, const char *peer, bool *ignored)
{
    float die = temp_c();
    pthread_mutex_lock(&s_wmu);
    pthread_mutex_lock(&s_mu);
    s_scratch = s_clock;
    pthread_mutex_unlock(&s_mu);
    bool skip = !nc_clock_takes(&s_scratch, src, t_us);
    if (ignored) {
        *ignored = skip;
    }
    if (skip) {
        pthread_mutex_unlock(&s_wmu);
        return NC_OUTLIER;
    }
    bool store = false;
    nc_result_t r = nc_clock_add(&s_scratch, src, t_us, utc_us, peer, die, &store);
    bool tcal = s_scratch.tcal_changed;
    static nc_tcal_t tcal_copy;  // under s_wmu
    if (tcal) {
        tcal_copy = s_scratch.tcal;
        const nc_tcal_entry_t *e = nc_tcal_find(&tcal_copy, s_scratch.base_die_c);
        logf_("learned %+.2f ppm at %.1f C: the %d C bin is %+.2f over %d, %d temperatures in the table",
              s_scratch.m.fit_rate * 1e6, s_scratch.base_die_c, e ? e->temp_c : 0, e ? e->v : NAN, e ? e->n : 0,
              tcal_copy.count);
    }
    if (store) {
        nc_clock_stored(&s_scratch, s_scratch.m.fit_rate, t_us);
    }
    const nc_model_t m = s_scratch.m;
    bool from_store = s_scratch.rate_from_store;
    pthread_mutex_lock(&s_mu);
    s_clock = s_scratch;
    s_source = src;
    pthread_mutex_unlock(&s_mu);
    logf_("%s%s%s ref: %s, err %+.1f ms, rate %+.2f ppm (%s, %d refs over %.0f min, rms %.1f ms), temp %.1f C",
          netclock_source_name(src), peer ? " " : "", peer ? peer : "",
          r == NC_STEP ? "step" : r == NC_SLEW ? "slew" : "OUTLIER, ignored", m.last_err_us / 1000.0, m.rate * 1e6,
          m.rate_learned ? "learned" : from_store ? "stored" : "none", m.n, m.rate_span_s / 60,
          m.resid_rms_us / 1000, die);
    if (store) {
        state_store(&m.fit_rate, tcal ? &tcal_copy : NULL);
    }
    pthread_mutex_unlock(&s_wmu);
    if (r != NC_OUTLIER) {
        os_clock_follow();
    }
    return r;
}

// Between references: the rate follows the temperature (nc_clock_temp). Skipped while a reference is going in.
static void temp_tick(void)
{
    float t = temp_c();
    if (isnan(t) || pthread_mutex_trylock(&s_wmu) != 0) {
        return;
    }
    pthread_mutex_lock(&s_mu);
    s_scratch = s_clock;
    pthread_mutex_unlock(&s_mu);
    nc_clock_temp(&s_scratch, mono_us(), t);
    pthread_mutex_lock(&s_mu);
    s_clock = s_scratch;
    pthread_mutex_unlock(&s_mu);
    pthread_mutex_unlock(&s_wmu);
}

static bool offset_vs_model(void *ctx, int64_t t_us, int64_t utc_us, double *off_us)
{
    pthread_mutex_lock(&s_mu);
    bool ok = nc_clock_offset(&s_clock, t_us, utc_us, off_us);
    pthread_mutex_unlock(&s_mu);
    return ok;
}

void netclock_posix_os(netclock_posix_os_t *out)
{
    memset(out, 0, sizeof(*out));
    out->esterror_us = out->maxerror_us = NAN;
#ifdef __linux__
    struct timex tx;
    memset(&tx, 0, sizeof(tx));  // modes 0: read only, no privilege needed
    int st = ntp_adjtime(&tx);
    if (st < 0) {
        return;
    }
    out->known = true;
    out->synced = st != TIME_ERROR && !(tx.status & STA_UNSYNC);
    out->esterror_us = (double)tx.esterror;
    out->maxerror_us = (double)tx.maxerror;
#endif
}

static bool os_synced(void)
{
    netclock_posix_os_t os;
    netclock_posix_os(&os);
    return !os.known || os.synced;
}

int64_t netclock_posix_utc_ns(int64_t mono_ns, bool *synced)
{
    if (s_o.mode == NETCLOCK_POSIX_OS) {
        if (synced) {
            *synced = os_synced();
        }
        return real_ns() - (netclock_posix_mono_ns() - mono_ns);
    }
    int64_t u = 0;
    pthread_mutex_lock(&s_mu);
    bool valid = nc_clock_utc(&s_clock, mono_ns / 1000, &u);
    pthread_mutex_unlock(&s_mu);
    if (synced) {
        *synced = valid;
    }
    if (valid) {
        return u * 1000 + mono_ns % 1000;
    }
    return real_ns() - (netclock_posix_mono_ns() - mono_ns);
}

int64_t netclock_posix_now_ns(void)
{
    return netclock_posix_utc_ns(netclock_posix_mono_ns(), NULL);
}

netclock_source_t netclock_posix_source(void)
{
    if (s_o.mode == NETCLOCK_POSIX_OS) {
        return NETCLOCK_SRC_OS;
    }
    pthread_mutex_lock(&s_mu);
    netclock_source_t s = s_source;
    pthread_mutex_unlock(&s_mu);
    return s;
}

bool netclock_posix_synced(void)
{
    if (s_o.mode == NETCLOCK_POSIX_OS) {
        return os_synced();
    }
    return netclock_posix_source() != NETCLOCK_SRC_NONE;
}

bool netclock_posix_quality(netclock_source_t *source, double *err_us)
{
    if (s_o.mode == NETCLOCK_POSIX_OS) {
        netclock_posix_os_t os;
        netclock_posix_os(&os);
        if (source) {
            *source = NETCLOCK_SRC_OS;
        }
        if (os.known && !os.synced) {
            return false;
        }
        *err_us = os.esterror_us;
        return true;
    }
    const int64_t now = mono_us();
    pthread_mutex_lock(&s_mu);
    bool ok = nc_clock_quality(&s_clock, now, err_us);
    netclock_source_t src = s_clock.source;
    pthread_mutex_unlock(&s_mu);
    if (source) {
        *source = src;
    }
    return ok;
}

bool netclock_posix_rate(double *ppm, double *skew_ppm, bool *learned)
{
    double r = 0, k = 0;
    bool l = false, ok = false;
    if (s_o.mode != NETCLOCK_POSIX_OS) {
        pthread_mutex_lock(&s_mu);
        ok = nc_clock_rate(&s_clock, &r, &k, &l);
        pthread_mutex_unlock(&s_mu);
    }
    *ppm = r * 1e6;
    *skew_ppm = k * 1e6;
    if (learned) {
        *learned = l;
    }
    return ok;
}

bool netclock_posix_set_from_client(int64_t utc_us)
{
    if (s_o.mode == NETCLOCK_POSIX_OS) {
        return false;
    }
    bool ignored = false;
    add_reference(NETCLOCK_SRC_BROWSER, mono_us(), utc_us, NULL, &ignored);
    return !ignored;
}

void netclock_posix_status(netclock_status_t *out)
{
    const int64_t now = mono_us();
    memset(out, 0, sizeof(*out));
    out->pps_gpio = -1;
    out->pps_last_s = -1;
    out->gnss_last_s = -1;
    out->gnss_lag_ms = NAN;
    out->die_c = temp_c();
    if (s_o.mode == NETCLOCK_POSIX_OS) {
        out->source = NETCLOCK_SRC_OS;
        out->valid = os_synced();
        out->utc_us = real_ns() / 1000;
        out->since_sync_s = -1;
        out->rate_src = "none";
        out->base_die_c = NAN;
        return;
    }
    pthread_mutex_lock(&s_wmu);
    pthread_mutex_lock(&s_mu);
    s_scratch = s_clock;
    out->n_src = s_nview;
    memcpy(out->src, s_view, sizeof(s_view[0]) * s_nview);
    int64_t view_t = s_view_t;
    out->poll_s = s_poll_s;
    out->polls = s_rounds;
    out->no_majority = s_no_majority;
    pthread_mutex_unlock(&s_mu);
    nc_clock_status(&s_scratch, now, out);
    pthread_mutex_unlock(&s_wmu);
    if (!out->valid) {
        out->utc_us = netclock_posix_utc_ns(now * 1000, NULL) / 1000;
    }
    for (int i = 0; i < out->n_src; i++) {
        // The view was taken at the end of the last poll: age "last rx" to now.
        if (out->src[i].last_rx_s >= 0) {
            out->src[i].last_rx_s += (int32_t)((now - view_t) / 1000000);
        }
    }
}

static void json_num(char *t, size_t cap, double v)
{
    if (isfinite(v)) {
        snprintf(t, cap, "%.0f", v);
    } else {
        snprintf(t, cap, "null");
    }
}

size_t netclock_posix_status_json(char *buf, size_t cap)
{
    netclock_status_t *st = malloc(sizeof(*st));
    if (!st || !cap) {
        free(st);
        if (cap) {
            buf[0] = 0;
        }
        return 0;
    }
    netclock_posix_status(st);
    size_t n = netclock_status_json(st, buf, cap);
    free(st);
    if (!n) {
        return 0;
    }
    netclock_posix_os_t os;
    netclock_posix_os(&os);
    char e[32], m[32];
    json_num(e, sizeof(e), os.esterror_us);
    json_num(m, sizeof(m), os.maxerror_us);
    // Into the object: its closing brace is the last byte.
    int k = snprintf(buf + n - 1, cap - (n - 1),
                     ",\"mode\":\"%s\",\"os\":{\"known\":%s,\"synced\":%s,\"esterror_us\":%s,\"maxerror_us\":%s}}",
                     s_o.mode == NETCLOCK_POSIX_OS ? "os" : "discipline", os.known ? "true" : "false",
                     os.synced ? "true" : "false", e, m);
    if (k < 0 || (size_t)k >= cap - (n - 1)) {
        buf[0] = 0;
        return 0;
    }
    return n - 1 + (size_t)k;
}

int netclock_posix_save(void)
{
    if (s_o.mode == NETCLOCK_POSIX_OS) {
        return 0;
    }
    pthread_mutex_lock(&s_wmu);
    pthread_mutex_lock(&s_mu);
    s_scratch = s_clock;
    pthread_mutex_unlock(&s_mu);
    const bool learned = s_scratch.m.rate_learned && s_scratch.m.fit_ok;
    const double rate = s_scratch.m.fit_rate;
    int e = state_store(learned ? &rate : NULL, s_scratch.tcal.count ? &s_scratch.tcal : NULL);
    if (!e && learned) {
        nc_clock_stored(&s_scratch, rate, mono_us());
        pthread_mutex_lock(&s_mu);
        s_clock.rate_stored = s_scratch.rate_stored;
        s_clock.rate_saved_us = s_scratch.rate_saved_us;
        s_clock.tcal_changed = false;
        pthread_mutex_unlock(&s_mu);
    }
    pthread_mutex_unlock(&s_wmu);
    return e;
}

// ── NTP ──────────────────────────────────────────────────────────────────────

// "host:port" (one colon): the host into h, the port; else the name and 123.
static uint16_t split_port(const char *name, char *h, size_t cap)
{
    const char *c = strchr(name, ':');
    if (!c || strchr(c + 1, ':')) {
        snprintf(h, cap, "%s", name);
        return NTP_PORT;
    }
    snprintf(h, cap, "%.*s", (int)(c - name), name);
    long p = strtol(c + 1, NULL, 10);
    return p > 0 && p < 65536 ? (uint16_t)p : NTP_PORT;
}

static void load_servers(void)
{
    s_n = nc_parse_servers(s_servers, s_src, NETCLOCK_MAX_SRC);
    logf_("%d NTP servers from \"%s\"", s_n, s_servers);
}

static void resolve(int i)
{
    nc_src_t *s = &s_src[i];
    char host[sizeof(s->name)], port[8];
    uint16_t p = split_port(s->name, host, sizeof(host));
    snprintf(port, sizeof(port), "%u", p);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        logf_("%s: DNS failed", s->name);
        s->resolved = false;
        return;
    }
    uint8_t ip[4];
    memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr, 4);
    freeaddrinfo(res);
    if (!s->resolved || memcmp(ip, s->ip, 4) || s_port[i] != p) {
        s->hn = s->hh = 0;  // a different server now: its history isn't this one's
    }
    memcpy(s->ip, ip, 4);
    s_port[i] = p;
    s->resolved = true;
}

static void send_req(int sock, int i, uint32_t poll_s)
{
    nc_src_t *s = &s_src[i];
    uint8_t p[48];
    s->cookie = rng64();
    nc_ntp_request(p, s->cookie, poll_s);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(s_port[i]);
    memcpy(&to.sin_addr.s_addr, s->ip, 4);
    s->got = false;
    s->t1 = mono_us();
    if (sendto(sock, p, sizeof(p), 0, (struct sockaddr *)&to, sizeof(to)) != (ssize_t)sizeof(p)) {
        s->t1 = 0;
    }
}

static void publish_view(uint32_t poll_s)
{
    const int64_t now = mono_us();
    netclock_src_view_t v[NETCLOCK_MAX_SRC];
    for (int i = 0; i < s_n; i++) {
        nc_src_view(&s_src[i], now, &v[i]);
    }
    pthread_mutex_lock(&s_mu);
    memcpy(s_view, v, sizeof(v[0]) * s_n);
    s_nview = s_n;
    s_view_t = now;
    s_poll_s = poll_s;
    pthread_mutex_unlock(&s_mu);
}

static uint32_t poll_interval_s(void)
{
    uint32_t slow = s_o.poll_s ? s_o.poll_s : POLL_S;
    uint32_t fast = POLL_FAST_S < slow ? POLL_FAST_S : slow;
    pthread_mutex_lock(&s_mu);
    bool settled = nc_clock_rate_settled(&s_clock);
    pthread_mutex_unlock(&s_mu);
    return settled ? slow : fast;
}

// Wait up to ms for the socket (sock >= 0) or a stop. Returns 1: the socket is readable, 0: timed out, -1: stop.
static int wait_for(int sock, int ms)
{
    struct pollfd p[2] = {{.fd = s_wake[0], .events = POLLIN}, {.fd = sock, .events = POLLIN}};
    int r = poll(p, sock >= 0 ? 2 : 1, ms < 0 ? 0 : ms);
    if (s_stop || (r > 0 && p[0].revents)) {
        return -1;
    }
    return r > 0 && (p[1].revents & POLLIN) ? 1 : 0;
}

static void *ntp_thread(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        logf_("socket: %s", strerror(errno));
        return NULL;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    load_servers();
    s_rounds = 0;  // iburst again
    publish_view(s_poll_s);
    int64_t next_temp = mono_us() + TEMP_EVERY_MS * 1000LL;
    while (!s_stop) {
        for (int i = 0; i < s_n; i++) {
            nc_src_t *s = &s_src[i];
            if (!s->resolved || s->misses >= 4 || s_rounds % RESOLVE_EVERY == 0) {
                resolve(i);
                s->misses = 0;
            }
        }
        // Two names on one address are one server.
        for (int i = 0; i < s_n; i++) {
            s_src[i].dup = false;
            for (int k = 0; k < i; k++) {
                if (s_src[k].resolved && s_src[i].resolved && !memcmp(s_src[k].ip, s_src[i].ip, 4) &&
                    s_port[k] == s_port[i]) {
                    s_src[i].dup = true;
                }
            }
        }
        int sent = 0;
        for (int i = 0; i < s_n; i++) {
            nc_src_t *s = &s_src[i];
            s->got = false;
            s->t1 = 0;
            if (!s->resolved || s->dup) {
                continue;
            }
            if (s->kod_skip > 0) {
                s->kod_skip--;
                continue;
            }
            send_req(sock, i, s_poll_s);
            sent += s->t1 != 0;
        }
        const int64_t deadline = mono_us() + (s_o.test_wait_ms ? s_o.test_wait_ms : ROUND_WAIT_MS) * 1000LL;
        int got = 0;
        const bool iburst = s_rounds > 0 && s_rounds < NC_NTP_IBURST;
        while (sent && got < sent && !s_stop) {
            int64_t left = deadline - mono_us();
            if (left <= 0) {
                break;
            }
            int w = wait_for(sock, (int)((left + 999) / 1000));
            if (w < 0) {
                break;
            }
            if (!w) {
                continue;
            }
            uint8_t p[68];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t len = recvfrom(sock, p, sizeof(p), 0, (struct sockaddr *)&from, &fl);
            const int64_t t4 = mono_us();
            if (len <= 0) {
                continue;
            }
            for (int i = 0; i < s_n; i++) {
                nc_src_t *s = &s_src[i];
                if (s->resolved && !s->dup && !s->got && !memcmp(&from.sin_addr.s_addr, s->ip, 4) &&
                    ntohs(from.sin_port) == s_port[i]) {
                    if (nc_src_reply(s, p, (int)len, t4, iburst) == NC_REPLY_KOD) {
                        logf_("%s: kiss-o'-death, backing off", s->name);
                    }
                    got += s->got;
                    break;
                }
            }
        }
        if (s_stop) {
            break;
        }
        for (int i = 0; i < s_n; i++) {
            nc_src_poll_end(&s_src[i]);
        }
        if (s_rounds >= NC_NTP_IBURST - 1) {
            int64_t t = 0, u = 0;
            bool had = false;
            int bi = nc_combine(s_src, s_n, offset_vs_model, NULL, &t, &u, &had);
            if (bi >= 0) {
                add_reference(NETCLOCK_SRC_NTP, t, u, s_src[bi].name, NULL);
            } else if (had) {
                pthread_mutex_lock(&s_mu);
                s_no_majority++;
                pthread_mutex_unlock(&s_mu);
                logf_("no majority among NTP servers; clock not updated this poll");
            } else if (s_n) {
                logf_("no usable NTP sample this poll (delay spikes or no replies)");
            }
        }
        const uint32_t poll_s = poll_interval_s();
        pthread_mutex_lock(&s_mu);
        s_rounds++;
        pthread_mutex_unlock(&s_mu);
        publish_view(poll_s);
        // Sleep the poll, following the temperature meanwhile; a stop wakes it.
        int64_t ms = s_rounds < NC_NTP_IBURST ? (s_o.test_gap_ms ? s_o.test_gap_ms : IBURST_GAP_MS)
                   : s_o.test_poll_ms         ? s_o.test_poll_ms
                                              : poll_s * 1000LL;
        const int64_t wake = mono_us() + ms * 1000;
        for (;;) {
            const int64_t now = mono_us();
            if (now >= next_temp) {
                temp_tick();
                next_temp = now + TEMP_EVERY_MS * 1000LL;
            }
            if (now >= wake) {
                break;
            }
            const int64_t until = wake < next_temp ? wake : next_temp;
            if (wait_for(-1, (int)((until - now + 999) / 1000)) < 0) {
                break;
            }
        }
    }
    close(sock);
    return NULL;
}

// ── setup ────────────────────────────────────────────────────────────────────

static bool check_servers(const char *s)
{
    if (strlen(s) >= SERVERS_LEN) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!isalnum((unsigned char)*p) && !strchr(".-_:, ;", *p)) {
            return false;
        }
    }
    return true;
}

int netclock_posix_init(const netclock_posix_opts_t *o)
{
    static const netclock_posix_opts_t none;
    if (!o) {
        o = &none;
    }
    if (s_running) {
        errno = EBUSY;
        return -1;
    }
    const char *servers = o->servers ? o->servers : "";
    if (!check_servers(servers) || (o->poll_s && (o->poll_s < 16 || o->poll_s > 3600))) {
        errno = EINVAL;
        return -1;
    }
    char *path = NULL;
    if (o->state_path && o->mode != NETCLOCK_POSIX_OS) {
        path = strdup(o->state_path);
        if (!path) {
            errno = ENOMEM;
            return -1;
        }
    }
    pthread_mutex_lock(&s_wmu);
    free(s_path);
    s_path = path;
    s_o = *o;
    s_o.servers = s_servers;
    s_o.state_path = s_path;
    snprintf(s_servers, sizeof(s_servers), "%s", servers);
    s_os_warned = false;
    s_have_rate = s_have_tcal = false;
    s_n = 0;
    {
        uint64_t seed = 0;
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            if (read(fd, &seed, sizeof(seed)) != (ssize_t)sizeof(seed)) {
                seed = 0;
            }
            close(fd);
        }
        s_rng = seed ^ (uint64_t)real_ns() ^ ((uint64_t)getpid() << 32);
    }
    int32_t ppb = 0;
    bool have_rate = false;
    static nc_tcal_t tcal;  // under s_wmu
    state_load(&ppb, &have_rate, &tcal);
    nc_clock_init(&s_scratch, ppb / 1e9, have_rate);
    float t = temp_c();
    nc_clock_set_tcal(&s_scratch, &tcal, t);
    pthread_mutex_lock(&s_mu);
    s_clock = s_scratch;
    s_source = NETCLOCK_SRC_NONE;
    s_nview = 0;
    s_view_t = 0;
    s_poll_s = POLL_FAST_S < (s_o.poll_s ? s_o.poll_s : POLL_S) ? POLL_FAST_S : s_o.poll_s;
    s_rounds = s_no_majority = 0;
    pthread_mutex_unlock(&s_mu);
    pthread_mutex_unlock(&s_wmu);
    if (s_o.mode == NETCLOCK_POSIX_OS) {
        logf_("using the OS clock as it is");
    } else if (!isnan(s_clock.base_die_c)) {
        logf_("clock rate prior %+.2f ppm (the temperature table, %d temperatures, at %.1f C); servers \"%s\"",
              s_clock.m.rate * 1e6, tcal.count, t, s_servers);
    } else {
        logf_("clock rate prior %+.2f ppm (%s); servers \"%s\"", ppb / 1e3, have_rate ? "stored" : "none", s_servers);
    }
    return 0;
}

int netclock_posix_start(void)
{
    if (s_running || s_o.mode == NETCLOCK_POSIX_OS) {
        return 0;
    }
    if (pipe(s_wake) != 0) {
        return errno;
    }
    s_stop = 0;
    int e = pthread_create(&s_thr, NULL, ntp_thread, NULL);
    if (e) {
        close(s_wake[0]);
        close(s_wake[1]);
        s_wake[0] = s_wake[1] = -1;
        return e;
    }
    s_running = true;
    return 0;
}

void netclock_posix_stop(void)
{
    if (!s_running) {
        return;
    }
    s_stop = 1;
    const char b = 1;
    if (write(s_wake[1], &b, 1) != 1) {
        // The flag alone ends it within a poll wait.
    }
    pthread_join(s_thr, NULL);
    close(s_wake[0]);
    close(s_wake[1]);
    s_wake[0] = s_wake[1] = -1;
    s_running = false;
}
