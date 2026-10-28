// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// netclock_posix against a fake NTP server on 127.0.0.1: a thread that
// answers with the OS clock plus a set offset. Short polls (the test_*
// options), so the whole run takes a second or two.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "netclock_ntp.h"
#include "netclock_posix.h"
#include "netclock_tempcal.h"

static int checks, fails;

#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

// On a busy CI machine a reply can sit in a queue for a few ms; the model also slews a part of each error in.
#define TOL_NS 25000000LL

static int64_t real_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

// ── the fake server ──────────────────────────────────────────────────────────

typedef struct {
    int sock;
    uint16_t port;
    _Atomic int64_t offset_ns;
    atomic_int stop;
    atomic_int answered;
    pthread_t thr;
} fake_t;

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (56 - 8 * i));
    }
}

static void *fake_run(void *arg)
{
    fake_t *f = arg;
    while (!f->stop) {
        struct pollfd p = {.fd = f->sock, .events = POLLIN};
        if (poll(&p, 1, 20) <= 0) {
            continue;
        }
        uint8_t q[68], r[48];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(f->sock, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
        if (n < 48 || (q[0] & 7) != 3) {
            continue;
        }
        const int64_t rx = (real_ns() + f->offset_ns) / 1000;
        memset(r, 0, sizeof(r));
        r[0] = (0 << 6) | (4 << 3) | 4;  // LI 0, version 4, server
        r[1] = 2;                        // stratum 2
        r[2] = q[2];
        r[3] = (uint8_t)-20;
        r[5] = 0x01;                     // root delay and dispersion about 4 ms
        r[9] = 0x01;
        r[12] = 127, r[15] = 1;          // refid 127.0.0.1
        put64(r + 16, nc_us_to_ntp(rx));
        memcpy(r + 24, q + 40, 8);       // origin: the request's transmit stamp
        put64(r + 32, nc_us_to_ntp(rx));
        put64(r + 40, nc_us_to_ntp((real_ns() + f->offset_ns) / 1000));
        sendto(f->sock, r, sizeof(r), 0, (struct sockaddr *)&from, fl);
        f->answered++;
    }
    return NULL;
}

static void fake_start(fake_t *f, int64_t offset_ns)
{
    memset(f, 0, sizeof(*f));
    f->offset_ns = offset_ns;
    f->sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof(a);
    if (f->sock < 0 || bind(f->sock, (struct sockaddr *)&a, sizeof(a)) || getsockname(f->sock, (struct sockaddr *)&a, &al)) {
        printf("FAIL: no fake server socket\n");
        exit(1);
    }
    f->port = ntohs(a.sin_port);
    pthread_create(&f->thr, NULL, fake_run, f);
}

static void fake_stop(fake_t *f)
{
    f->stop = 1;
    pthread_join(f->thr, NULL);
    close(f->sock);
}

// A free UDP port with nothing behind it.
static uint16_t dead_port(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof(a);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    getsockname(s, (struct sockaddr *)&a, &al);
    close(s);
    return ntohs(a.sin_port);
}

// ── helpers ──────────────────────────────────────────────────────────────────

static int verbose;

static void log_cb(void *ctx, const char *m)
{
    atomic_fetch_add((atomic_int *)ctx, 1);
    if (verbose) {
        printf("  netclock: %s\n", m);
    }
}

static double temp_cb(void *ctx)
{
    return *(double *)ctx;
}

static atomic_int logs;
static double temp = 41.0;

static netclock_posix_opts_t opts(const char *servers, const char *state)
{
    return (netclock_posix_opts_t){
        .servers = servers, .state_path = state, .log = log_cb, .log_ctx = &logs,
        .temp_c = temp_cb, .temp_ctx = &temp,
        .test_gap_ms = 30, .test_poll_ms = 60, .test_wait_ms = 150,
    };
}

static bool wait_synced(int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (netclock_posix_synced()) {
            return true;
        }
        sleep_ms(10);
    }
    return netclock_posix_synced();
}

static char tmpdir[256];

// ── tests ────────────────────────────────────────────────────────────────────

static void test_sync(void)
{
    fake_t f;
    const int64_t off = 123456789012LL;  // the server is 123.457 s ahead of this machine
    fake_start(&f, off);
    char servers[64];
    snprintf(servers, sizeof(servers), "127.0.0.1:%u", f.port);
    netclock_posix_opts_t o = opts(servers, NULL);
    CHECK(netclock_posix_init(&o) == 0, "init");
    CHECK(!netclock_posix_synced() && netclock_posix_source() == NETCLOCK_SRC_NONE, "not synced before start");
    bool synced = true;
    int64_t d = netclock_posix_now_ns() - real_ns();
    netclock_posix_utc_ns(netclock_posix_mono_ns(), &synced);
    CHECK(!synced && llabs(d) < TOL_NS, "OS fallback before a reference: %+" PRId64 " ns", d);
    CHECK(netclock_posix_start() == 0, "start");
    CHECK(wait_synced(3000), "synced (the server answered %d)", (int)f.answered);
    CHECK(netclock_posix_source() == NETCLOCK_SRC_NTP, "source ntp, got %s",
          netclock_source_name(netclock_posix_source()));
    sleep_ms(200);  // a few slews in
    d = netclock_posix_now_ns() - (real_ns() + off);
    CHECK(llabs(d) < TOL_NS, "UTC is the server's: %+.3f ms", d / 1e6);
    const int64_t m = netclock_posix_mono_ns();
    d = netclock_posix_utc_ns(m, &synced) - netclock_posix_utc_ns(m + 1000000000, NULL);
    CHECK(synced && llabs(d + 1000000000) < 1000000, "a second later is a second later: %+" PRId64, d);
    netclock_source_t src = NETCLOCK_SRC_NONE;
    double err = -1;
    CHECK(netclock_posix_quality(&src, &err) && src == NETCLOCK_SRC_NTP && err >= 0 && err < 1e6,
          "quality: %s, %.0f us", netclock_source_name(src), err);
    CHECK(!netclock_posix_set_from_client(real_ns() / 1000), "a browser doesn't displace NTP");

    netclock_status_t *st = malloc(sizeof(*st));
    netclock_posix_status(st);
    CHECK(st->valid && st->source == NETCLOCK_SRC_NTP && st->n_src == 1 && st->polls >= 4 && st->pps_gpio == -1,
          "status: valid %d, %d servers, %u polls", st->valid, st->n_src, (unsigned)st->polls);
    CHECK(st->n_src == 1 && st->src[0].mark == '*' && st->src[0].reach && !strcmp(st->src[0].ip, "127.0.0.1"),
          "the server's row: mark %c, reach %u, ip %s", st->src[0].mark, st->src[0].reach, st->src[0].ip);
    CHECK(fabsf(st->die_c - 41.0f) < 0.01f && st->n_log >= 1 && fabsf(st->log[0].die_c - 41.0f) < 0.01f,
          "the temperature callback: %.1f C", st->die_c);
    free(st);
    char *json = malloc(16384);
    size_t n = netclock_posix_status_json(json, 16384);
    CHECK(n > 0 && n == strlen(json) && !strncmp(json, "{\"source\":\"ntp\",\"synced\":true", 29) &&
              strstr(json, "\"mode\":\"discipline\",\"os\":{\"known\":") && json[n - 1] == '}' && json[n - 2] == '}',
          "status JSON: %.60s ... %s", json, n > 80 ? json + n - 80 : json);
    CHECK(netclock_posix_status_json(json, 200) == 0 && json[0] == 0, "too small a buffer");
    free(json);
    CHECK(atomic_load(&logs) > 0, "the log callback");

    // The server jumps 2 s: three outliers in a row, then a step (the board's rule).
    f.offset_ns = off + 2000000000LL;
    for (int i = 0; i < 300; i++) {
        d = netclock_posix_now_ns() - (real_ns() + f.offset_ns);
        if (llabs(d) < TOL_NS) {
            break;
        }
        sleep_ms(10);
    }
    CHECK(llabs(d) < TOL_NS, "follows a 2 s jump: %+.3f ms", d / 1e6);
    netclock_posix_stop();
    // Stopped: the model keeps running.
    d = netclock_posix_now_ns() - (real_ns() + f.offset_ns);
    CHECK(netclock_posix_synced() && llabs(d) < TOL_NS, "still synced after stop: %+.3f ms", d / 1e6);
    fake_stop(&f);
}

static void test_unreachable(void)
{
    char servers[64];
    snprintf(servers, sizeof(servers), "127.0.0.1:%u", dead_port());
    netclock_posix_opts_t o = opts(servers, NULL);
    CHECK(netclock_posix_init(&o) == 0, "init");
    CHECK(netclock_posix_start() == 0, "start");
    sleep_ms(1000);  // the iburst and a few polls (each waits 150 ms for nothing)
    bool synced = true;
    const int64_t d = netclock_posix_utc_ns(netclock_posix_mono_ns(), &synced) - real_ns();
    CHECK(!synced && !netclock_posix_synced() && netclock_posix_source() == NETCLOCK_SRC_NONE, "not synced");
    CHECK(llabs(d) < TOL_NS, "the OS clock: %+.3f ms", d / 1e6);
    double err;
    CHECK(!netclock_posix_quality(NULL, &err), "no quality");
    netclock_status_t *st = malloc(sizeof(*st));
    netclock_posix_status(st);
    CHECK(!st->valid && st->n_src == 1 && st->src[0].reach == 0 && st->src[0].mark == '?' && st->polls >= 3,
          "status: valid %d, reach %u, mark %c, %u polls", st->valid, st->n_src ? st->src[0].reach : 0,
          st->n_src ? st->src[0].mark : ' ', (unsigned)st->polls);
    CHECK(llabs(st->utc_us - real_ns() / 1000) < TOL_NS / 1000, "status UTC from the OS clock");
    free(st);
    // A browser can set it when there's no NTP.
    CHECK(netclock_posix_set_from_client(real_ns() / 1000 + 5000000), "a browser reference taken");
    CHECK(netclock_posix_source() == NETCLOCK_SRC_BROWSER, "source browser");
    const int64_t b = netclock_posix_now_ns() - (real_ns() + 5000000000LL);
    CHECK(llabs(b) < TOL_NS, "UTC is the browser's: %+.3f ms", b / 1e6);
    netclock_posix_stop();
}

static void test_state(void)
{
    char path[300], tmp[310];
    snprintf(path, sizeof(path), "%s/clock.state", tmpdir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    // A file as a last run left it: a rate and a two-bin table.
    nc_tcal_t t;
    nc_tcal_init(&t);
    nc_tcal_put(&t, 30.0, 11.0);
    nc_tcal_put(&t, 50.0, 13.0);
    FILE *f = fopen(path, "w");
    fprintf(f, "netclock 1\nrate_ppb 12345\ntcal ");
    for (size_t i = 0; i < sizeof(t); i++) {
        fprintf(f, "%02x", ((const uint8_t *)&t)[i]);
    }
    fprintf(f, "\n");
    fclose(f);

    temp = 40.0;  // halfway between the bins: 12 ppm
    netclock_posix_opts_t o = opts("", path);
    CHECK(netclock_posix_init(&o) == 0, "init");
    double ppm = 0, skew = 1;
    bool learned = true;
    CHECK(netclock_posix_rate(&ppm, &skew, &learned) && !learned && fabs(ppm - 12.0) < 1e-6 && skew == 0,
          "the prior from the table: %.4f ppm", ppm);
    netclock_status_t *st = malloc(sizeof(*st));
    netclock_posix_status(st);
    CHECK(st->tcal.count == 2 && fabs(st->stored_ppm - 12.345) < 1e-9 && !strcmp(st->rate_src, "stored"),
          "status: %d bins, stored %.3f ppm, %s", st->tcal.count, st->stored_ppm, st->rate_src);

    // Written back (nothing learned: the same rate and table), atomically, and read again.
    unlink(path);
    CHECK(netclock_posix_save() == 0, "save");
    CHECK(access(path, F_OK) == 0 && access(tmp, F_OK) != 0, "the file, and no temp file left");
    temp = NAN;  // no temperature: the stored rate is the prior
    CHECK(netclock_posix_init(&o) == 0, "init again");
    CHECK(netclock_posix_rate(&ppm, &skew, &learned) && !learned && fabs(ppm - 12.345) < 1e-6,
          "the stored rate: %.4f ppm", ppm);
    netclock_posix_status(st);
    CHECK(st->tcal.count == 2 && st->tcal.e[0].temp_c == 30 && fabsf(st->tcal.e[1].v - 13.0f) < 1e-6f,
          "the table round-trips: %d bins", st->tcal.count);
    CHECK(isnan(st->die_c), "no temperature");

    // A damaged file: ignored, not fatal.
    f = fopen(path, "w");
    fprintf(f, "netclock 1\nrate_ppb x\ntcal 0011\n");
    fclose(f);
    CHECK(netclock_posix_init(&o) == 0, "init with a damaged file");
    CHECK(!netclock_posix_rate(&ppm, &skew, &learned), "no rate from it");
    netclock_posix_status(st);
    CHECK(st->tcal.count == 0, "no table from it");
    // No file at all, and a directory that doesn't exist: save fails softly.
    snprintf(path, sizeof(path), "%s/none/clock.state", tmpdir);
    o.state_path = path;
    CHECK(netclock_posix_init(&o) == 0 && !netclock_posix_rate(&ppm, &skew, &learned), "no file");
    CHECK(netclock_posix_save() != 0, "save into a missing directory fails");
    free(st);
    temp = 41.0;
}

static void test_restart(void)
{
    fake_t f;
    fake_start(&f, 0);
    char servers[64];
    snprintf(servers, sizeof(servers), "127.0.0.1:%u localhost:%u", f.port, f.port);
    netclock_posix_opts_t o = opts(servers, NULL);
    CHECK(netclock_posix_init(&o) == 0, "init");
    for (int i = 0; i < 20; i++) {
        CHECK(netclock_posix_start() == 0, "start %d", i);
        CHECK(netclock_posix_start() == 0, "start again is a no-op");
        CHECK(netclock_posix_init(&o) != 0, "no init while running");
        sleep_ms(i % 4 * 7);  // stop in different places: sending, waiting, sleeping
        netclock_posix_stop();
        netclock_posix_stop();
    }
    CHECK(netclock_posix_start() == 0, "start after the restarts");
    CHECK(wait_synced(3000), "synced after the restarts");
    netclock_status_t *st = malloc(sizeof(*st));
    netclock_posix_status(st);
    CHECK(st->n_src == 2 && st->src[1].mark == '-', "the same server twice is a duplicate: %c",
          st->n_src == 2 ? st->src[1].mark : ' ');
    free(st);
    netclock_posix_stop();
    fake_stop(&f);
}

static void test_os_mode(void)
{
    netclock_posix_opts_t o = opts("127.0.0.1:9", NULL);
    o.mode = NETCLOCK_POSIX_OS;
    CHECK(netclock_posix_init(&o) == 0, "init");
    CHECK(netclock_posix_start() == 0, "start does nothing");
    netclock_posix_os_t os;
    netclock_posix_os(&os);
#ifdef __linux__
    CHECK(os.known, "Linux says");
#else
    CHECK(!os.known && isnan(os.esterror_us), "unknown here");
#endif
    CHECK(netclock_posix_source() == NETCLOCK_SRC_OS, "source os");
    CHECK(netclock_posix_synced() == (!os.known || os.synced), "synced as the OS says");
    const int64_t d = netclock_posix_now_ns() - real_ns();
    CHECK(llabs(d) < TOL_NS, "the OS clock: %+.3f ms", d / 1e6);
    netclock_source_t src = NETCLOCK_SRC_NONE;
    double err = 0;
    bool q = netclock_posix_quality(&src, &err);
    CHECK(src == NETCLOCK_SRC_OS && q == (!os.known || os.synced), "quality");
    double ppm, skew;
    CHECK(!netclock_posix_rate(&ppm, &skew, NULL), "no rate");
    CHECK(!netclock_posix_set_from_client(real_ns() / 1000), "no browser");
    CHECK(netclock_posix_save() == 0, "save does nothing");
    char json[16384];
    size_t n = netclock_posix_status_json(json, sizeof(json));
    CHECK(n && !strncmp(json, "{\"source\":\"os\"", 14) && strstr(json, "\"mode\":\"os\""), "JSON: %.80s", json);
    netclock_posix_stop();
    // Back to discipline mode for whatever runs next.
    o.mode = NETCLOCK_POSIX_DISCIPLINE;
    netclock_posix_init(&o);
}

static void test_bad_opts(void)
{
    netclock_posix_opts_t o = opts("bad/name", NULL);
    CHECK(netclock_posix_init(&o) != 0, "a bad server list");
    o = opts("", NULL);
    o.poll_s = 5;
    CHECK(netclock_posix_init(&o) != 0, "too short a poll");
    CHECK(netclock_posix_init(NULL) == 0, "defaults");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    const char *base = getenv("TMPDIR");
    snprintf(tmpdir, sizeof(tmpdir), "%s/netclock_posix_XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(tmpdir)) {
        printf("FAIL: mkdtemp\n");
        return 1;
    }
    test_bad_opts();
    test_sync();
    test_unreachable();
    test_state();
    test_restart();
    test_os_mode();
    char p[300];
    snprintf(p, sizeof(p), "%s/clock.state", tmpdir);
    unlink(p);
    rmdir(tmpdir);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
