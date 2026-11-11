// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The host web server (rdof/hostweb.h). One thread: poll the listen socket and a wake pipe, serve one connection
 * at a time (Connection: close; the page's requests are small and local). The API's routes go to the shared code
 * (rdof/webapi.h) with the host as the platform. Published state is copied in under short locks and read out the
 * same way, so the tool's real-time loop never waits for a socket.
 */
#include "rdof/hostweb.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#include "assets.h"
#include "rdof/json.h"
#include "rdof/lastheard.h"
#include "rdof/webapi.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#define LOG_BYTES (64 * 1024) /* the log ring */
#define LOG_LINE 192
#define RESTART_ENV "RDOF_HOSTWEB_RESTARTED"
#define SPEC_ROWS 64          /* the spectrum's ring */
#define SPEC_MAX 16           /* rows an answer, at most */

typedef struct {
    uint8_t rows[SPEC_ROWS][RDOF_HOSTWEB_SPECTRUM_MAX];
    uint32_t seq;
    int bins, row_ms;
    double center, rate;
    int npiece; /* 0: one piece, the span */
    double f0[RDOF_HOSTWEB_SPECTRUM_PIECES], bin_hz, pass_hz;
    int n[RDOF_HOSTWEB_SPECTRUM_PIECES];
    atomic_llong want_ms; /* mono ms: the page asked until then */
} spec_t;

static struct {
    atomic_bool running;
    int lfd, wake[2];
    pthread_t thr;
    char name[32];
    double t0;         /* mono_s() at start: the uptime (the wall clock can step) */
    const char *reset; /* why this process started: "start", or "restart" after rdof_hostweb_reexec */
    /* the status: trylock to publish */
    pthread_mutex_t st_mu;
    rdof_hostweb_status_t st;
    bool have_st;
    /* the log ring */
    pthread_mutex_t log_mu;
    char log_buf[LOG_BYTES];
    rdof_bring_t log;
    /* settings */
    pthread_mutex_t set_mu;
    rdof_hostweb_settings_t set;
    bool have_set;
    /* the board's clock, when the tool runs it; the version */
    rdof_hostweb_clock_t clk;
    bool have_clk;
    const char *version;
    /* last heard */
    pthread_mutex_t lh_mu;
    rdof_lh_t lh;
    rdof_lh_entry_t lh_e[RDOF_LH_DEFAULT];
    /* the spectrum: each SDR's rows, newest at seq */
    pthread_mutex_t sp_mu;
    spec_t sp[RDOF_HOSTWEB_SDRS];
    /* the supervisor's record of the last exit (rdof_hostweb_last_exit) */
    const char *exit_path;
    /* the program's update (rdof_hostweb_ota), under set_mu */
    rdof_hostweb_ota_t ota;
    bool have_ota;
} W = {.st_mu = PTHREAD_MUTEX_INITIALIZER, .log_mu = PTHREAD_MUTEX_INITIALIZER, .lh_mu = PTHREAD_MUTEX_INITIALIZER,
       .set_mu = PTHREAD_MUTEX_INITIALIZER, .sp_mu = PTHREAD_MUTEX_INITIALIZER};

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void rdof_hostweb_settings(const rdof_hostweb_settings_t *s)
{
    pthread_mutex_lock(&W.set_mu);
    if (s)
        W.set = *s;
    W.set.source[sizeof W.set.source - 1] = 0;
    W.have_set = s != NULL;
    pthread_mutex_unlock(&W.set_mu);
}

void rdof_hostweb_ota(const rdof_hostweb_ota_t *o)
{
    pthread_mutex_lock(&W.set_mu);
    if (o)
        W.ota = *o;
    W.ota.path[sizeof W.ota.path - 1] = 0;
    W.have_ota = o != NULL && o->path[0];
    pthread_mutex_unlock(&W.set_mu);
}

/* ── publishing ────────────────────────────────────────────────────────────── */

void rdof_hostweb_status(const rdof_hostweb_status_t *s)
{
    if (!W.running || !s)
        return;
    if (pthread_mutex_trylock(&W.st_mu))
        return; /* the server is copying: the next publish replaces this one */
    W.st = *s;
    W.have_st = true;
    pthread_mutex_unlock(&W.st_mu);
}

void rdof_hostweb_logf(const char *fmt, ...)
{
    char line[LOG_LINE + 1]; /* kept from before the server starts, so a tool's startup lines show */
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t t = ts.tv_sec;
    gmtime_r(&t, &tm);
    int n = snprintf(line, LOG_LINE, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, LOG_LINE - (size_t)n, fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    line[len++] = '\n';
    pthread_mutex_lock(&W.log_mu);
    if (!W.log.buf)
        rdof_bring_init(&W.log, W.log_buf, sizeof W.log_buf);
    rdof_bring_put(&W.log, line, len);
    pthread_mutex_unlock(&W.log_mu);
}

void rdof_hostweb_tracker_json(const char *role, const char *radio, const char *channel, const char *json)
{
    if (!W.running || !json)
        return;
    pthread_mutex_lock(&W.lh_mu);
    rdof_lh_tracker_json(&W.lh, role, radio, channel, json, mono_s(), wall_ms());
    pthread_mutex_unlock(&W.lh_mu);
}

void rdof_hostweb_net_call(const char *role, const char *radio, const char *action, const rdof_tx_call_info_t *call,
                           double duration)
{
    if (!W.running || !call)
        return;
    pthread_mutex_lock(&W.lh_mu);
    rdof_lh_tx(&W.lh, role, radio, action, call, duration, mono_s(), wall_ms());
    pthread_mutex_unlock(&W.lh_mu);
}

/* ── the host as the API's platform ────────────────────────────────────────── */

static void host_board(void *ctx, rdof_webapi_board_t *b)
{
    b->name = W.name;
    b->version = W.version ? W.version : "host";
    b->idf = NULL;
    b->uptime_s = (long long)(mono_s() - W.t0);
    b->reset = W.reset;
}

/* The process's resident size now and its peak, and the system's memory: total and what a program can still get
 * (bytes, 0 when unknown). */
static bool host_memory(void *ctx, rdof_webapi_mem_t *m)
{
    m->has_proc = m->has_sys = true;
    struct rusage ru;
    if (!getrusage(RUSAGE_SELF, &ru)) {
#ifdef __APPLE__
        m->rss_peak = ru.ru_maxrss; /* bytes */
#else
        m->rss_peak = (long long)ru.ru_maxrss * 1024; /* KB */
#endif
    }
#ifdef __APPLE__
    mach_task_basic_info_data_t t;
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&t, &n) == KERN_SUCCESS)
        m->rss = (long long)t.resident_size;
    int64_t total = 0;
    size_t l = sizeof total;
    if (!sysctlbyname("hw.memsize", &total, &l, NULL, 0))
        m->sys_total = total;
    vm_statistics64_data_t v;
    mach_msg_type_number_t vn = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &vn) == KERN_SUCCESS)
        m->sys_avail = ((long long)v.free_count + (long long)v.inactive_count) * (long long)page;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        long size, res;
        if (fscanf(f, "%ld %ld", &size, &res) == 2)
            m->rss = (long long)res * sysconf(_SC_PAGESIZE);
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        long long kb;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %lld kB", &kb) == 1)
                m->sys_total = kb * 1024;
            else if (sscanf(line, "MemAvailable: %lld kB", &kb) == 1)
                m->sys_avail = kb * 1024;
        }
        fclose(f);
    }
#endif
    if (!m->rss)
        m->rss = m->rss_peak;
    return true;
}

void rdof_hostweb_clock(const rdof_hostweb_clock_t *c)
{
    W.have_clk = false;
    if (c)
        W.clk = *c;
    W.have_clk = c != NULL;
}

void rdof_hostweb_version(const char *v) { W.version = v; }

static size_t clk_json(void *ctx, char *out, size_t cap) { return W.clk.json(out, cap); }
static bool clk_set(void *ctx, int64_t utc_us, const char **source)
{
    const bool ok = W.clk.set && W.clk.set(utc_us);
    *source = W.clk.source();
    return ok;
}
static size_t clk_settings(void *ctx, char *out, size_t cap) { return W.clk.settings_json(out, cap); }

/* The board's clock when the tool runs it; else the kernel's NTP state, read only (modes 0: nothing is set or
 * adjusted). */
static bool host_clock(void *ctx, rdof_webapi_clock_t *c)
{
    if (W.have_clk) {
        c->source = W.clk.source();
        return true;
    }
    struct timex tx;
    memset(&tx, 0, sizeof tx);
    tx.modes = 0;
    const int r = ntp_adjtime(&tx);
    c->source = "os";
    c->has_os = true;
    c->os_source = "system clock (OS NTP)";
    c->known = r >= 0;
    if (r >= 0) {
        c->synced = r != TIME_ERROR && !(tx.status & STA_UNSYNC);
        c->est_error_us = tx.esterror;
        c->max_error_us = tx.maxerror;
#ifdef STA_NANO
        const bool nano = (tx.status & STA_NANO) != 0;
#else
        const bool nano = false;
#endif
        c->offset_us = nano ? (double)tx.offset / 1000.0 : (double)tx.offset;
        c->freq_ppm = (double)tx.freq / 65536.0;
    }
    return true;
}

static void slot_json(rdof_json_t *j, const char *key, const rdof_hostweb_slot_t *s)
{
    rdof_json_open(j, key);
    rdof_json_bool(j, "active", s->active);
    rdof_json_int(j, "src", s->src);
    rdof_json_int(j, "dst", s->dst);
    rdof_json_bool(j, "group", s->group);
    if (s->has_ber)
        rdof_json_float3(j, "ber", s->ber);
    if (s->has_rssi)
        rdof_json_int(j, "rssi", s->rssi);
    rdof_json_close(j);
}

/* The tool's state, as the firmware's roamer or repeater, and the host's own cards. */
/* a name from the settings, for a JSON string: no quotes, backslashes or control characters */
static void json_safe(char *out, size_t cap, const char *in, size_t in_cap)
{
    size_t k = 0;
    for (size_t i = 0; i < in_cap && in[i] && k + 1 < cap; i++)
        if ((unsigned char)in[i] >= 0x20 && in[i] != '"' && in[i] != '\\')
            out[k++] = in[i];
    out[k] = 0;
}

/* Objects written one at a time into an array: the writer has none. "[" to start, each object's text, "]". */
typedef struct {
    char *buf;
    size_t cap, len;
} jarr_t;

static void jarr_add(jarr_t *a, const char *text)
{
    const size_t n = strlen(text);
    if (a->len + n + 3 > a->cap)
        return;
    a->buf[a->len] = a->len ? ',' : '[';
    a->len++;
    memcpy(a->buf + a->len, text, n);
    a->len += n;
}

static void jarr_put(rdof_json_t *j, const char *key, jarr_t *a)
{
    if (a->len == 0)
        a->buf[a->len++] = '[';
    a->buf[a->len++] = ']';
    a->buf[a->len] = 0;
    rdof_json_raw(j, key, a->buf);
}

/* The SDRs, as the board's SDR (sdrs: the page's SDR cards, one each): the board's names where they mean the same */
static void sdrs_json(rdof_json_t *j, rdof_hostweb_status_t *s)
{
    if (s->nsdr <= 0)
        return;
    const size_t one = 6144, all = RDOF_HOSTWEB_SDRS * 6144;
    char *obj = malloc(one), *chans = malloc(16 * 200), *buf = malloc(all);
    if (!obj || !chans || !buf)
        goto out;
    jarr_t arr = {buf, all - 1, 0};
    for (int i = 0; i < s->nsdr && i < RDOF_HOSTWEB_SDRS; i++) {
        rdof_hostweb_sdr_t *d = &s->sdrs[i];
        d->name[sizeof d->name - 1] = d->device[sizeof d->device - 1] = d->error[sizeof d->error - 1] = 0;
        rdof_json_t o;
        rdof_json_init(&o, obj, one);
        rdof_json_open(&o, NULL);
        rdof_json_str(&o, "name", d->name);
        rdof_json_bool(&o, "started", true);
        rdof_json_str(&o, "frontend", "host");
        rdof_json_bool(&o, "rtl_open", d->open);
        rdof_json_str(&o, "rtl", d->device);
        if (d->error[0])
            rdof_json_str(&o, "rtl_error", d->error);
        rdof_json_bool(&o, "receiving", d->receiving && !d->error[0]);
        if (d->calibrating)
            rdof_json_bool(&o, "calibrating", true);
        rdof_json_int(&o, "sample_rate", llround(d->rate));
        rdof_json_int(&o, "center_hz", llround(d->center_hz));
        if (!isnan(d->gain_db))
            rdof_json_float3(&o, "gain_db", d->gain_db);
        rdof_json_bool(&o, "agc", d->agc);
        if (d->samples_per_s >= 0)
            rdof_json_int(&o, "samples_per_s", llround(d->samples_per_s));
        rdof_json_int(&o, "samples_expected_per_s", llround(d->rate));
        rdof_json_int(&o, "read_errors", (long long)d->read_errors);
        rdof_json_int(&o, "overflows", (long long)d->overflows);
        rdof_json_float3(&o, "lost_ms", d->lost_ms);
        if (d->load >= 0) {
            rdof_json_float3(&o, "cpu_load", d->load);
            rdof_json_str(&o, "load_of", "the receive loop");
            if (d->nstage > 0) {
                rdof_json_open(&o, "cpu_stages");
                for (int k = 0; k < d->nstage && k < 6; k++) {
                    d->stage[k].name[sizeof d->stage[k].name - 1] = 0;
                    rdof_json_float3(&o, d->stage[k].name, d->stage[k].share);
                }
                rdof_json_close(&o);
            }
        }
        if (d->tx) {
            rdof_json_int(&o, "tx_hz", d->tx_hz);
            rdof_json_int(&o, "tx_lo_hz", d->tx_lo_hz);
            rdof_json_float3(&o, "tx_gain_db", d->tx_gain_db);
            rdof_json_bool(&o, "tx_on", d->tx_on);
            rdof_json_int(&o, "tx_errors", (long long)d->tx_errors);
        }
        if (d->has_mqtt) {
            rdof_json_bool(&o, "mqtt", d->mqtt);
            rdof_json_int(&o, "mqtt_dropped", d->mqtt_dropped);
        }
        if (d->hblink_peers > 0) {
            rdof_json_int(&o, "hblink_peers", d->hblink_peers);
            rdof_json_int(&o, "hblink_up", d->hblink_up);
        }
        size_t n = 1;
        chans[0] = '[';
        for (int c = 0; c < d->nchan && c < 16 && n < 16 * 200 - 2; c++) {
            char id[24], name[32];
            json_safe(id, sizeof id, d->chan[c].id, sizeof d->chan[c].id);
            json_safe(name, sizeof name, d->chan[c].name, sizeof d->chan[c].name);
            n += (size_t)snprintf(chans + n, 16 * 200 - n,
                                  "%s{\"id\":\"%s\",\"name\":\"%s\",\"freq\":%u,\"in\":%s,\"bursts\":%llu,\"rssi\":%d,"
                                  "\"afc\":%.1f,\"idle\":%.0f}",
                                  c ? "," : "", id, name, (unsigned)d->chan[c].freq, d->chan[c].in ? "true" : "false",
                                  (unsigned long long)d->chan[c].bursts, d->chan[c].rssi, d->chan[c].afc_hz,
                                  d->chan[c].idle_pct);
        }
        if (n < 16 * 200 - 1) {
            chans[n++] = ']';
            chans[n] = 0;
            rdof_json_raw(&o, "channels", chans);
        }
        rdof_json_close(&o);
        if (rdof_json_done(&o))
            jarr_add(&arr, obj);
    }
    jarr_put(j, "sdrs", &arr);
out:
    free(obj);
    free(chans);
    free(buf);
}

/* The radios and modems on ports (modems: a card each) */
static void modems_json(rdof_json_t *j, rdof_hostweb_status_t *s)
{
    if (s->nmodem <= 0)
        return;
    char obj[1536], buf[RDOF_HOSTWEB_MODEMS * 1536 + 4];
    jarr_t arr = {buf, sizeof buf - 1, 0};
    for (int i = 0; i < s->nmodem && i < RDOF_HOSTWEB_MODEMS; i++) {
        rdof_hostweb_modem_t *m = &s->modems[i];
        m->name[sizeof m->name - 1] = m->kind[sizeof m->kind - 1] = m->role[sizeof m->role - 1] = 0;
        m->port[sizeof m->port - 1] = m->version[sizeof m->version - 1] = 0;
        m->state[sizeof m->state - 1] = m->error[sizeof m->error - 1] = 0;
        rdof_json_t o;
        rdof_json_init(&o, obj, sizeof obj);
        rdof_json_open(&o, NULL);
        rdof_json_str(&o, "name", m->name);
        rdof_json_str(&o, "kind", m->kind);
        rdof_json_str(&o, "role", m->role);
        rdof_json_str(&o, "port", m->port);
        rdof_json_bool(&o, "open", m->open);
        rdof_json_bool(&o, "ready", m->ready && !m->error[0]);
        rdof_json_str(&o, "version", m->version);
        rdof_json_str(&o, "state", m->state);
        if (m->error[0])
            rdof_json_str(&o, "error", m->error);
        for (int k = 0; k < m->nextra && k < 8; k++) {
            m->extra[k].name[sizeof m->extra[k].name - 1] = 0;
            rdof_json_int(&o, m->extra[k].name, m->extra[k].value);
        }
        rdof_json_close(&o);
        if (rdof_json_done(&o))
            jarr_add(&arr, obj);
    }
    jarr_put(j, "modems", &arr);
}

/* Each core's busy share and this process's (of one core; past 1 with threads on several), since the last status
 * (cpu and cpu_process, as the board's cpu). Nothing the first time, or where the system doesn't say. */
#define CPU_MAX 64
static void host_cpu(rdof_json_t *j)
{
    static uint64_t busy0[CPU_MAX], total0[CPU_MAX];
    static double proc0 = -1, t0;
    uint64_t busy[CPU_MAX], total[CPU_MAX];
    int n = 0;
#ifdef __APPLE__
    natural_t ncpu = 0;
    processor_info_array_t info;
    mach_msg_type_number_t ninfo;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &ncpu, &info, &ninfo) == KERN_SUCCESS) {
        for (natural_t c = 0; c < ncpu && n < CPU_MAX; c++, n++) {
            const integer_t *t = &info[c * CPU_STATE_MAX];
            busy[n] = (uint64_t)t[CPU_STATE_USER] + (uint64_t)t[CPU_STATE_SYSTEM] + (uint64_t)t[CPU_STATE_NICE];
            total[n] = busy[n] + (uint64_t)t[CPU_STATE_IDLE];
        }
        vm_deallocate(mach_task_self(), (vm_address_t)info, ninfo * sizeof *info);
    }
#else
    FILE *f = fopen("/proc/stat", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f) && n < CPU_MAX) {
        unsigned long long v[8] = {0};
        int c;
        if (strncmp(line, "cpu", 3) || line[3] < '0' || line[3] > '9' ||
            sscanf(line + 3, "%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                   &v[6], &v[7]) < 5)
            continue;
        total[n] = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
        busy[n] = total[n] - v[3] - v[4]; /* less idle and iowait */
        n++;
    }
    if (f)
        fclose(f);
#endif
    struct rusage ru;
    const double now = mono_s();
    double proc = -1;
    if (!getrusage(RUSAGE_SELF, &ru))
        proc = (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    static int n0;
    if (n > 0 && n == n0) {
        char a[CPU_MAX * 8 + 2] = "[";
        size_t k = 1;
        for (int c = 0; c < n; c++) {
            const uint64_t dt = total[c] - total0[c];
            k += (size_t)snprintf(a + k, sizeof a - k, "%s%.3f", c ? "," : "",
                                  dt ? (double)(busy[c] - busy0[c]) / (double)dt : 0.0);
        }
        snprintf(a + k, sizeof a - k, "]");
        rdof_json_raw(j, "cpu", a);
    }
    if (proc >= 0 && proc0 >= 0 && now > t0)
        rdof_json_float3(j, "cpu_process", (proc - proc0) / (now - t0));
    memcpy(busy0, busy, sizeof busy[0] * (size_t)n);
    memcpy(total0, total, sizeof total[0] * (size_t)n);
    n0 = n, proc0 = proc, t0 = now;
}

/* ── the last exit, as the supervisor recorded it ──────────────────────────── */

void rdof_hostweb_last_exit(const char *path) { W.exit_path = path; }

#define EXIT_MAX 16384

/* The record (malloc'd, NUL terminated), or NULL when there's none. */
static char *exit_read(size_t *n)
{
    const char *path = W.exit_path;
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f)
        return NULL;
    char *b = malloc(EXIT_MAX);
    if (b) {
        *n = fread(b, 1, EXIT_MAX - 1, f);
        b[*n] = 0;
    }
    fclose(f);
    return b;
}

static const char *signal_name(int sig)
{
    static const struct {
        int n;
        const char *name;
    } S[] = {{SIGSEGV, "SIGSEGV (segmentation fault)"}, {SIGBUS, "SIGBUS (bus error)"},
             {SIGILL, "SIGILL (illegal instruction)"},  {SIGFPE, "SIGFPE (arithmetic)"},
             {SIGABRT, "SIGABRT (abort)"},              {SIGKILL, "SIGKILL (killed: out of memory?)"},
             {SIGTERM, "SIGTERM"},                       {SIGINT, "SIGINT"},
             {SIGPIPE, "SIGPIPE"},                       {SIGHUP, "SIGHUP"}};
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++)
        if (S[i].n == sig)
            return S[i].name;
    return NULL;
}

/* "last_exit": how it ended (rc, the signal), when, and the log's last line before it */
static void exit_json(rdof_json_t *j)
{
    size_t n = 0;
    char *b = exit_read(&n);
    if (!b)
        return;
    const char *p = strstr(b, "rc=");
    const int rc = p ? atoi(p + 3) : -1;
    p = strstr(b, "at=");
    const long long at = p ? atoll(p + 3) : 0;
    const char *tail = strstr(b, "\n---\n");
    tail = tail ? tail + 5 : b + n;
    /* the last line of the log that isn't the supervisor's own ("== ...") */
    char last[192] = "";
    for (const char *e = b + n; e > tail;) {
        const char *s0 = e - 1;
        while (s0 > tail && s0[-1] != '\n')
            s0--;
        const size_t len = (size_t)(e - s0);
        if (len > 1 && strncmp(s0, "== ", 3)) {
            const size_t k = len - (s0[len - 1] == '\n') < sizeof last ? len - (s0[len - 1] == '\n') : sizeof last - 1;
            memcpy(last, s0, k);
            last[k] = 0;
            break;
        }
        e = s0;
    }
    rdof_json_open(j, "last_exit");
    rdof_json_int(j, "rc", rc);
    if (rc > 128) {
        const char *name = signal_name(rc - 128);
        rdof_json_int(j, "signal", rc - 128);
        if (name)
            rdof_json_str(j, "signal_name", name);
    }
    if (at > 0)
        rdof_json_int(j, "at", at);
    rdof_json_str(j, "last_line", last);
    rdof_json_close(j);
    free(b);
}

static void host_status(void *ctx, rdof_json_t *j, rdof_webapi_at_t at)
{
    if (at == RDOF_WEBAPI_AT_BOARD) {
        host_cpu(j);
        exit_json(j);
    }
    if (at != RDOF_WEBAPI_AT_RADIOS)
        return;
    rdof_hostweb_status_t s;
    memset(&s, 0, sizeof s);
    s.cpu_load = -1;
    pthread_mutex_lock(&W.st_mu);
    if (W.have_st)
        s = W.st;
    const bool have = W.have_st;
    pthread_mutex_unlock(&W.st_mu);
    rdof_json_bool(j, "build_roamer", true);
    if (!have)
        return;
    s.name[sizeof s.name - 1] = s.radio[sizeof s.radio - 1] = s.modem[sizeof s.modem - 1] = 0;
    s.protocol[sizeof s.protocol - 1] = s.state[sizeof s.state - 1] = s.error[sizeof s.error - 1] = 0;
    s.heard[sizeof s.heard - 1] = s.sending[sizeof s.sending - 1] = s.parrot_state[sizeof s.parrot_state - 1] = 0;

    rdof_json_open(j, "roamer");
    rdof_json_str(j, "name", s.name[0] ? s.name : W.name);
    rdof_json_str(j, "mode", s.role == RDOF_HOSTWEB_REPEATER ? "repeater" : "roamer");
    rdof_json_bool(j, "started", true);
    rdof_json_str(j, "port", "");
    rdof_json_bool(j, "modem_open", true);
    rdof_json_bool(j, "radio_ready", s.ready && !s.error[0]);
    if (s.error[0])
        rdof_json_str(j, "radio_error", s.error);
    rdof_json_str(j, "modem", s.modem);
    rdof_json_str(j, "protocol", s.protocol[0] ? s.protocol : "host");
    rdof_json_str(j, "radio", s.radio);
    rdof_json_str(j, "state", s.state);
    rdof_json_int(j, "frequency", s.tx_hz);
    rdof_json_int(j, "colorCode", s.cc);
    if (s.role == RDOF_HOSTWEB_REPEATER) {
        rdof_json_bool(j, "keyed", s.keyed);
        rdof_json_bool(j, "busy1", s.slot[0].active);
        rdof_json_bool(j, "busy2", s.slot[1].active);
        rdof_json_int(j, "dropped", (long long)s.dropped);
    } else {
        rdof_json_int(j, "queue_dropped", (long long)s.dropped);
        if (s.keyed)
            rdof_json_bool(j, "keyed", true);
    }
    rdof_json_str(j, "heard", s.heard);
    rdof_json_str(j, "sending", s.sending);
    rdof_json_int(j, "bursts_heard", (long long)s.bursts_heard);
    rdof_json_int(j, "bursts_sent", (long long)s.bursts_sent);
    rdof_json_bool(j, "hblink", s.hblink);
    rdof_json_int(j, "hblink_tx", s.hblink_tx);
    rdof_json_int(j, "hblink_rx", s.hblink_rx);
    if (s.brandmeister[0])
        rdof_json_raw(j, "brandmeister", s.brandmeister);
    if (s.has_mqtt)
        rdof_json_bool(j, "mqtt", s.mqtt);
    if (s.nchan > 0) {
        char a[16 * 120] = "[";
        size_t n = 1;
        for (int i = 0; i < s.nchan && i < 16; i++) {
            s.chan[i].id[sizeof s.chan[i].id - 1] = 0;
            char esc_id[24];
            size_t k = 0; /* an id from the settings: no quotes, backslashes or control characters in the JSON */
            for (const char *c = s.chan[i].id; *c && k + 1 < sizeof esc_id; c++)
                if ((unsigned char)*c >= 0x20 && *c != '"' && *c != '\\')
                    esc_id[k++] = *c;
            esc_id[k] = 0;
            n += (size_t)snprintf(a + n, sizeof a - n, "%s{\"id\":\"%s\",\"freq\":%u,\"bursts\":%llu,\"rssi\":%d%s%s}",
                                  i ? "," : "", esc_id, (unsigned)s.chan[i].freq,
                                  (unsigned long long)s.chan[i].bursts, s.chan[i].rssi, s.chan[i].peer ? ",\"hblink\":" : "",
                                  s.chan[i].peer ? (s.chan[i].up ? "true" : "false") : "");
            if (n >= sizeof a - 2)
                break;
        }
        if (n < sizeof a - 1) {
            a[n++] = ']';
            a[n] = 0;
            rdof_json_raw(j, "channels", a);
        }
    }
    if (s.has_mqtt)
        rdof_json_int(j, "mqtt_dropped", s.mqtt_dropped);
    rdof_json_close(j);
    /* the page lists keys it doesn't know as cards: the host's own */
    rdof_json_open(j, "host");
    rdof_json_int(j, "rx_hz", s.rx_hz);
    rdof_json_int(j, "tx_hz", s.tx_hz);
    if (s.cpu_load >= 0)
        rdof_json_float3(j, "cpu_load", s.cpu_load);
    for (int i = 0; i < s.nextra && i < 8; i++) {
        s.extra[i].name[sizeof s.extra[i].name - 1] = 0;
        rdof_json_int(j, s.extra[i].name, s.extra[i].value);
    }
    rdof_json_close(j);
    rdof_json_open(j, "slots");
    slot_json(j, "slot1", &s.slot[0]);
    slot_json(j, "slot2", &s.slot[1]);
    rdof_json_close(j);
    if (s.parrot_on) {
        rdof_json_open(j, "parrot");
        rdof_json_str(j, "state", s.parrot_state);
        rdof_json_int(j, "id", s.parrot_id);
        rdof_json_int(j, "slot", s.parrot_slot);
        rdof_json_int(j, "src", s.parrot_src);
        rdof_json_int(j, "dst", s.parrot_dst);
        rdof_json_int(j, "replayed", s.parrot_replayed);
        rdof_json_int(j, "ignored", s.parrot_ignored);
        rdof_json_int(j, "cut", s.parrot_cut);
        rdof_json_close(j);
    }
    s.apps[sizeof s.apps - 1] = 0;
    if (s.apps[0] == '[')
        rdof_json_raw(j, "apps", s.apps);
    sdrs_json(j, &s);
    modems_json(j, &s);
}

static size_t host_log(void *ctx, char *out, size_t cap)
{
    pthread_mutex_lock(&W.log_mu);
    size_t n = rdof_bring_read(&W.log, out, cap);
    pthread_mutex_unlock(&W.log_mu);
    return n;
}

static size_t host_lastheard(void *ctx, uint32_t since, char *out, size_t cap)
{
    pthread_mutex_lock(&W.lh_mu);
    size_t n = rdof_lh_json(&W.lh, since, mono_s(), wall_ms(), out, cap);
    pthread_mutex_unlock(&W.lh_mu);
    return n;
}

/* The tool's hooks, from the copy a request takes (settings may change between requests, not within one). */
static char *set_get(void *ctx)
{
    const rdof_hostweb_settings_t *s = ctx;
    return s->get(s->ctx);
}

static bool set_save(void *ctx, char *text, size_t len, char *err, size_t cap)
{
    const rdof_hostweb_settings_t *s = ctx;
    return s->save(s->ctx, text, len, err, cap);
}

static size_t set_resolved(void *ctx, char *out, size_t cap)
{
    const rdof_hostweb_settings_t *s = ctx;
    return s->resolved(s->ctx, out, cap);
}

static void set_restart(void *ctx)
{
    const rdof_hostweb_settings_t *s = ctx;
    rdof_hostweb_logf("restart asked for from the web page");
    s->restart(s->ctx);
}

/* The platform for one request; set holds the tool's hooks meanwhile. */
/* ── the spectrum ──────────────────────────────────────────────────────────── */

bool rdof_hostweb_spectrum_wanted_sdr(int sdr)
{
    return sdr >= 0 && sdr < RDOF_HOSTWEB_SDRS && atomic_load(&W.running) &&
           (long long)(mono_s() * 1e3) < atomic_load(&W.sp[sdr].want_ms);
}

bool rdof_hostweb_spectrum_wanted(void) { return rdof_hostweb_spectrum_wanted_sdr(0); }

void rdof_hostweb_spectrum_row_sdr(int sdr, const uint8_t *row, int bins, double center_hz, double rate, int row_ms)
{
    if (sdr < 0 || sdr >= RDOF_HOSTWEB_SDRS || bins <= 0 || bins > RDOF_HOSTWEB_SPECTRUM_BINS)
        return;
    pthread_mutex_lock(&W.sp_mu);
    spec_t *p = &W.sp[sdr];
    if (bins != p->bins || p->npiece) /* a new size or layout: the old rows don't fit it */
        p->seq = 0;
    p->bins = bins, p->row_ms = row_ms, p->center = center_hz, p->rate = rate, p->npiece = 0;
    memcpy(p->rows[(p->seq + 1) % SPEC_ROWS], row, (size_t)bins);
    p->seq++;
    pthread_mutex_unlock(&W.sp_mu);
}

void rdof_hostweb_spectrum_pieces(int sdr, const uint8_t *row, const double *f0, const int *n, int npiece, double bin_hz,
                                  double pass_hz, double center_hz, double rate, int row_ms)
{
    int bins = 0;
    for (int k = 0; k < npiece; k++)
        bins += n[k] > 0 ? n[k] : 0;
    if (sdr < 0 || sdr >= RDOF_HOSTWEB_SDRS || npiece <= 0 || npiece > RDOF_HOSTWEB_SPECTRUM_PIECES || bins <= 0 ||
        bins > RDOF_HOSTWEB_SPECTRUM_MAX || bin_hz <= 0)
        return;
    pthread_mutex_lock(&W.sp_mu);
    spec_t *p = &W.sp[sdr];
    bool same = bins == p->bins && npiece == p->npiece && bin_hz == p->bin_hz;
    for (int k = 0; same && k < npiece; k++)
        same = f0[k] == p->f0[k] && n[k] == p->n[k];
    if (!same) /* a new layout: the old rows don't fit it */
        p->seq = 0;
    p->bins = bins, p->row_ms = row_ms, p->center = center_hz, p->rate = rate, p->npiece = npiece, p->bin_hz = bin_hz,
    p->pass_hz = pass_hz;
    for (int k = 0; k < npiece; k++)
        p->f0[k] = f0[k], p->n[k] = n[k] > 0 ? n[k] : 0;
    memcpy(p->rows[(p->seq + 1) % SPEC_ROWS], row, (size_t)bins);
    p->seq++;
    pthread_mutex_unlock(&W.sp_mu);
}

void rdof_hostweb_spectrum_row(const uint8_t *row, int bins, double center_hz, double rate, int row_ms)
{
    rdof_hostweb_spectrum_row_sdr(0, row, bins, center_hz, rate, row_ms);
}

/* a channel's band on the page, when it's in the span */
static void spec_chan(char *out, size_t cap, size_t *len, const char *id, uint32_t hz, double center, double rate)
{
    if (!hz || *len >= cap)
        return;
    char safe[32];
    size_t k = 0;
    for (; id[k] && k < sizeof safe - 1; k++)
        safe[k] = (id[k] == '"' || id[k] == '\\' || (unsigned char)id[k] < 32) ? '_' : id[k];
    safe[k] = 0;
    const bool in = rate > 0 && fabs((double)hz - center) < rate / 2;
    *len += (size_t)snprintf(out + *len, cap - *len, "%s{\"id\":\"%s\",\"freq\":%u,\"in\":%s}",
                             out[*len - 1] == '[' ? "" : ",", safe, (unsigned)hz, in ? "true" : "false");
}

/* GET /api/spectrum?since=N[&sdr=K]: SDR K's rows newer than row N, as the board's (esp_idf/main/web.c), and the
 * channels; a row in pieces (rdof_hostweb_spectrum_pieces) adds "pieces": [{f0, n, bw}], in the row's order */
static char *spectrum_json(const char *query, size_t *n)
{
    char v[16];
    const uint32_t since = rdof_webapi_query(query, "since", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0;
    int sdr = rdof_webapi_query(query, "sdr", v, sizeof v) ? atoi(v) : 0;
    if (sdr < 0 || sdr >= RDOF_HOSTWEB_SDRS)
        sdr = 0;
    atomic_store(&W.sp[sdr].want_ms, (long long)(mono_s() * 1e3) + 10000);
    static uint8_t rows[SPEC_MAX][RDOF_HOSTWEB_SPECTRUM_MAX]; /* the server's thread only */
    pthread_mutex_lock(&W.sp_mu);
    const spec_t *p = &W.sp[sdr];
    const uint32_t last = p->seq;
    uint32_t from = since > last ? 0 : since; /* ahead of us: the tool restarted */
    if (last - from > SPEC_MAX)
        from = last - SPEC_MAX;
    if (last - from > SPEC_ROWS)
        from = last - SPEC_ROWS;
    int k = 0;
    for (uint32_t q = from + 1; q <= last; q++, k++)
        memcpy(rows[k], p->rows[q % SPEC_ROWS], (size_t)p->bins);
    const int bins = p->bins, row_ms = p->row_ms, npiece = p->npiece;
    const double center = p->center, rate = p->rate, bin_hz = p->bin_hz, pass_hz = p->pass_hz;
    double f0[RDOF_HOSTWEB_SPECTRUM_PIECES];
    int pn[RDOF_HOSTWEB_SPECTRUM_PIECES];
    memcpy(f0, p->f0, sizeof f0), memcpy(pn, p->n, sizeof pn);
    pthread_mutex_unlock(&W.sp_mu);

    const size_t cap = 1024 + 24 * 64 + 48 * RDOF_HOSTWEB_SPECTRUM_PIECES + (size_t)k * (4 * ((size_t)bins + 2) / 3 + 8);
    char *out = malloc(cap);
    if (!out)
        return NULL;
    size_t len = (size_t)snprintf(out, cap, "{\"sdr\":%d,\"seq\":%u,\"bins\":%d,\"row_ms\":%d,\"rate\":%.0f,\"center\":%.0f,"
                                  "\"rows\":[", sdr, (unsigned)last, bins, row_ms, rate, center);
    for (int i = 0; i < k; i++) {
        if (i)
            out[len++] = ',';
        out[len++] = '"';
        rdof_base64(rows[i], (size_t)bins, out + len);
        len += strlen(out + len);
        out[len++] = '"';
    }
    if (npiece) { /* a row in pieces: each one's low edge, bins and bin width */
        len += (size_t)snprintf(out + len, cap - len, "],\"pieces\":[");
        for (int i = 0; i < npiece; i++)
            len += (size_t)snprintf(out + len, cap - len, "%s{\"f0\":%.0f,\"n\":%d,\"bw\":%.1f,\"pass\":%.0f}", i ? "," : "",
                                    f0[i], pn[i], bin_hz, pass_hz);
    }
    len += (size_t)snprintf(out + len, cap - len, "],\"channels\":[");
    rdof_hostweb_status_t *st = malloc(sizeof *st);
    if (st) {
        pthread_mutex_lock(&W.st_mu);
        const bool have = W.have_st;
        if (have)
            *st = W.st;
        pthread_mutex_unlock(&W.st_mu);
        if (have && sdr > 0 && sdr < st->nsdr) { /* another SDR: its own channels */
            const rdof_hostweb_sdr_t *d = &st->sdrs[sdr];
            for (int i = 0; i < d->nchan && i < 16; i++) {
                char id[24];
                memcpy(id, d->chan[i].id, sizeof id);
                id[sizeof id - 1] = 0;
                spec_chan(out, cap, &len, id, d->chan[i].freq, center, rate);
            }
        } else if (have) {
            st->name[sizeof st->name - 1] = 0;
            spec_chan(out, cap, &len, st->name[0] ? st->name : "RX", st->rx_hz, center, rate);
            if (st->tx_hz != st->rx_hz)
                spec_chan(out, cap, &len, "TX", st->tx_hz, center, rate);
            for (int i = 0; i < st->nchan && i < 16; i++) {
                st->chan[i].id[sizeof st->chan[i].id - 1] = 0;
                spec_chan(out, cap, &len, st->chan[i].id, st->chan[i].freq, center, rate);
            }
        }
        free(st);
    }
    len += (size_t)snprintf(out + len, cap - len, "]}");
    *n = len < cap ? len : cap - 1;
    return out;
}

static void platform(rdof_webapi_platform_t *p, rdof_hostweb_settings_t *set)
{
    pthread_mutex_lock(&W.set_mu);
    const bool have = W.have_set, ota = W.have_ota;
    if (have)
        *set = W.set;
    pthread_mutex_unlock(&W.set_mu);
    memset(p, 0, sizeof *p);
    p->caps = RDOF_WEBAPI_CAP_SYS_MEM | (W.have_clk ? RDOF_WEBAPI_CAP_NETCLOCK : RDOF_WEBAPI_CAP_OS_CLOCK);
    if (ota)
        p->caps |= RDOF_WEBAPI_CAP_OTA;
    if (W.exit_path)
        p->caps |= RDOF_WEBAPI_CAP_CRASH;
    pthread_mutex_lock(&W.st_mu);
    if (W.have_st && W.st.links)
        p->caps |= RDOF_WEBAPI_CAP_LINKS;
    if (W.have_st && W.st.spectrum)
        p->caps |= RDOF_WEBAPI_CAP_SPECTRUM;
    pthread_mutex_unlock(&W.st_mu);
    if (W.have_clk) {
        p->clock_json = clk_json;
        p->clock_set = clk_set;
        p->clock_settings_json = W.clk.settings_json ? clk_settings : NULL;
    }
    p->board = host_board;
    p->memory = host_memory;
    p->clock = host_clock;
    p->status_json = host_status;
    p->status_cap = 32768; /* four SDRs of 16 channels, the modems */
    p->log_cap = LOG_BYTES + 1;
    p->resolved_cap = 65536; /* a version 3 site's view (rdof_site_resolved_text) */
    p->log_read = host_log;
    p->lastheard = host_lastheard;
    if (!have)
        return;
    p->ctx = set;
    if (set->get) {
        p->caps |= RDOF_WEBAPI_CAP_SETTINGS;
        p->settings_get = set_get;
        p->settings_source = set->source;
        if (set->resolved)
            p->settings_resolved = set_resolved;
        if (set->save) {
            p->settings_save = set_save; /* read only: the POST is refused, with source */
            if (set->writable)
                p->caps |= RDOF_WEBAPI_CAP_SETTINGS_EDIT;
        }
    }
    if (set->restart) {
        p->caps |= RDOF_WEBAPI_CAP_RESTART;
        p->restart = set_restart;
    }
}

/* ── files and the restart ─────────────────────────────────────────────────── */

bool rdof_hostweb_save_file(const char *path, const char *text, size_t len, char *err, size_t cap)
{
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid()) >= (int)sizeof tmp) {
        snprintf(err, cap, "%s: path too long", path);
        return false;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(err, cap, "%s: %s", tmp, strerror(errno));
        return false;
    }
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, text + done, len - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            snprintf(err, cap, "%s: %s", tmp, w < 0 ? strerror(errno) : "short write");
            close(fd);
            unlink(tmp);
            return false;
        }
        done += (size_t)w;
    }
    if (fsync(fd) || close(fd)) {
        snprintf(err, cap, "%s: %s", tmp, strerror(errno));
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path)) {
        snprintf(err, cap, "%s: %s", path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

int rdof_hostweb_reexec(char *const argv[])
{
    setenv(RESTART_ENV, "1", 1);
    execvp(argv[0], argv);
    const int e = errno;
    unsetenv(RESTART_ENV);
    errno = e;
    return -1;
}

/* ── HTTP ──────────────────────────────────────────────────────────────────── */

static bool send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, 0);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        c += w;
        n -= (size_t)w;
    }
    return true;
}

static void respond_with(int fd, int code, const char *type, const void *body, size_t n, const char *filename,
                         bool head)
{
    static const struct {
        int code;
        const char *text;
    } reasons[] = {{200, "OK"},          {400, "Bad Request"},           {403, "Forbidden"},
                   {404, "Not Found"},   {405, "Method Not Allowed"},    {413, "Content Too Large"},
                   {500, "Internal Server Error"}, {501, "Not Implemented"}};
    const char *why = "OK";
    for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++)
        if (reasons[i].code == code)
            why = reasons[i].text;
    char disp[128] = "";
    if (filename && *filename)
        snprintf(disp, sizeof disp, "Content-Disposition: attachment; filename=\"%s\"\r\n", filename);
    char h[384];
    int hn = snprintf(h, sizeof h,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                      "%sConnection: close\r\n\r\n",
                      code, why, type, n, disp);
    if (send_all(fd, h, (size_t)hn) && n && !head)
        send_all(fd, body, n);
}

static void respond(int fd, int code, const char *type, const void *body, size_t n)
{
    respond_with(fd, code, type, body, n, NULL, false);
}

/* the first case-insensitive match of what in s (strcasestr is a GNU extension) */
static const char *find_nocase(const char *s, const char *what)
{
    const size_t n = strlen(what);
    for (; *s; s++)
        if (!strncasecmp(s, what, n))
            return s;
    return NULL;
}

static void respond_str(int fd, int code, const char *type, const char *s)
{
    respond(fd, code, type, s, strlen(s));
}

static void not_here(int fd, const char *path)
{
    char b[256], p[128];
    size_t k = 0;
    for (; path[k] && k < sizeof p - 1; k++)
        p[k] = (path[k] == '"' || path[k] == '\\' || (unsigned char)path[k] < 32) ? '_' : path[k];
    p[k] = 0;
    snprintf(b, sizeof b, "{\"error\":\"not on this platform\",\"detail\":\"%s is served by the firmware, not by the host tool\"}", p);
    respond_str(fd, 501, "application/json", b);
}

/* Read the rest of a body: have bytes of it are at start. Returns it NUL terminated (malloc'd), or NULL. */
static char *read_body(int fd, const char *start, size_t have, size_t len)
{
    char *b = malloc(len + 1);
    if (!b)
        return NULL;
    if (have > len)
        have = len;
    memcpy(b, start, have);
    while (have < len) {
        ssize_t r = recv(fd, b + have, len - have, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            free(b);
            return NULL;
        }
        have += (size_t)r;
    }
    b[len] = 0;
    return b;
}

/* Read and drop a body this server won't take, so the close doesn't reset the answer. */
static void drop_body(int fd, long have, long len)
{
    char drop[1024];
    while (len > have && len < (1L << 20)) {
        ssize_t r = recv(fd, drop, sizeof drop, 0);
        if (r <= 0)
            break;
        have += r;
    }
}

/* POST /api/ota: checked, written whole, answered, then the tool asked to stop */
static void serve_ota(int fd, const rdof_hostweb_ota_t *o, const char *body_at, size_t body_have, long body_len)
{
    char err[300] = "";
    if (body_len <= 0 || (o->max && (size_t)body_len > o->max)) {
        drop_body(fd, (long)body_have, body_len);
        snprintf(err, sizeof err, "an image of 1 to %zu bytes, as the body\n", o->max);
        respond_str(fd, body_len <= 0 ? 400 : 413, "text/plain", err);
        return;
    }
    char *img = read_body(fd, body_at, body_have, (size_t)body_len);
    if (!img) {
        respond_str(fd, 400, "text/plain", "the image didn't come whole\n");
        return;
    }
    const size_t len = (size_t)body_len;
    rdof_hostweb_logf("OTA: %zu bytes", len);
    if (o->check && !o->check(o->ctx, (const uint8_t *)img, len, err, sizeof err - 1)) {
        rdof_hostweb_logf("OTA: refused: %s", err);
        strcat(err, "\n");
        respond_str(fd, 400, "text/plain", err);
    } else if (!rdof_hostweb_save_file(o->path, img, len, err, sizeof err - 1)) {
        rdof_hostweb_logf("OTA: %s", err);
        strcat(err, "\n");
        respond_str(fd, 500, "text/plain", err);
    } else {
        rdof_hostweb_logf("OTA: written to %s; restarting", o->path);
        respond_str(fd, 200, "text/plain", "updated; restarting");
        if (o->done)
            o->done(o->ctx);
    }
    free(img);
}

static void serve(int fd)
{
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    char req[4096];
    size_t n = 0;
    char *end = NULL;
    while (n < sizeof req - 1) {
        ssize_t r = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return;
        n += (size_t)r;
        req[n] = 0;
        if ((end = strstr(req, "\r\n\r\n")))
            break;
    }
    if (!end) {
        respond_str(fd, 400, "text/plain", "bad request\n");
        return;
    }
    const char *cl = find_nocase(req, "\r\ncontent-length:");
    const long body_len = cl ? strtol(cl + 17, NULL, 10) : 0;
    const char *body_at = end + 4;
    const size_t body_have = n - (size_t)(body_at - req);

    char method[8], target[256];
    if (sscanf(req, "%7s %255s", method, target) != 2 || body_len < 0) {
        respond_str(fd, 400, "text/plain", "bad request\n");
        return;
    }
    char *query = strchr(target, '?');
    if (query)
        *query++ = 0;
    const bool head = !strcmp(method, "HEAD");
    const bool get = !strcmp(method, "GET") || head;

    if (get && !strcmp(target, "/api/spectrum")) {
        drop_body(fd, (long)body_have, body_len);
        size_t len = 0;
        char *js = spectrum_json(query, &len);
        if (js)
            respond(fd, 200, "application/json", js, len);
        else
            respond_str(fd, 500, "text/plain", "no memory\n");
        free(js);
        return;
    }
    if (!strcmp(target, "/api/crash") && W.exit_path && (get || !strcmp(method, "DELETE"))) {
        drop_body(fd, (long)body_have, body_len);
        if (!get) {
            const bool gone = !unlink(W.exit_path) || errno == ENOENT;
            respond_str(fd, gone ? 200 : 500, "text/plain", gone ? "erased\n" : "couldn't erase it\n");
            return;
        }
        size_t len = 0;
        char *b = exit_read(&len);
        if (b)
            respond_with(fd, 200, "text/plain; charset=utf-8", b, len, NULL, head);
        else
            respond_str(fd, 404, "text/plain", "no exit recorded\n");
        free(b);
        return;
    }
    if (!strcmp(target, "/api/ota") && !strcmp(method, "POST")) {
        pthread_mutex_lock(&W.set_mu);
        const bool have = W.have_ota;
        const rdof_hostweb_ota_t o = W.ota;
        pthread_mutex_unlock(&W.set_mu);
        if (have) {
            serve_ota(fd, &o, body_at, body_have, body_len);
            return;
        }
    }
    if (!strncmp(target, "/api/", 5)) {
        rdof_hostweb_settings_t set;
        rdof_webapi_platform_t p;
        platform(&p, &set);
        char *body = NULL;
        if (body_len > 0 && rdof_webapi_body_ok(target, (size_t)body_len)) {
            body = read_body(fd, body_at, body_have, (size_t)body_len);
            if (!body) {
                respond_str(fd, 400, "text/plain", "the body didn't come\n");
                return;
            }
        } else {
            drop_body(fd, (long)body_have, body_len);
        }
        rdof_webapi_resp_t r;
        if (rdof_webapi_handle(&p, method, target, query, body, (size_t)body_len, &r)) {
            respond_with(fd, r.status, r.type, r.body, r.len, r.filename, head);
            const bool restart = r.restart;
            rdof_webapi_done(&p, &r);
            if (restart)
                rdof_webapi_restart(&p);
        } else {
            not_here(fd, target);
        }
        free(body);
        return;
    }
    drop_body(fd, (long)body_have, body_len);
    if (!get) {
        respond_str(fd, 405, "text/plain", "method not allowed\n");
        return;
    }
    if (!strcmp(target, "/index.html"))
        strcpy(target, "/");
    for (const rdof_hostweb_asset_t *a = rdof_hostweb_assets; a->path; a++) {
        if (strcmp(a->path, target))
            continue;
        bool js = strstr(a->path, ".js") != NULL;
        respond_with(fd, 200, js ? "text/javascript; charset=utf-8" : "text/html; charset=utf-8", a->data, a->len, NULL,
                     head);
        return;
    }
    respond_str(fd, 404, "text/plain", "not found\n");
}

static void *server(void *arg)
{
    (void)arg;
    for (;;) {
        struct pollfd p[2] = {{W.lfd, POLLIN, 0}, {W.wake[0], POLLIN, 0}};
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (p[1].revents)
            break;
        if (p[0].revents & POLLIN) {
            int fd = accept(W.lfd, NULL, NULL);
            if (fd < 0)
                continue;
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            serve(fd);
            close(fd);
        }
    }
    return NULL;
}

int rdof_hostweb_start(const char *bind_addr, int port, const char *name)
{
    if (W.running) {
        errno = EALREADY;
        return -1;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr && *bind_addr ? bind_addr : "127.0.0.1", &a.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC); /* a restart (exec) mustn't keep the port */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    socklen_t al = sizeof a;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 16) || getsockname(fd, (struct sockaddr *)&a, &al) ||
        pipe(W.wake)) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    fcntl(W.wake[0], F_SETFD, FD_CLOEXEC);
    fcntl(W.wake[1], F_SETFD, FD_CLOEXEC);
    W.lfd = fd;
    snprintf(W.name, sizeof W.name, "%s", name && *name ? name : "host");
    W.t0 = mono_s();
    if (!W.reset) {
        const char *e = getenv(RESTART_ENV);
        W.reset = e && *e == '1' ? "restart" : "start";
        unsetenv(RESTART_ENV); /* not passed on to what this process runs */
    }
    W.have_st = false;
    rdof_lh_init(&W.lh, W.lh_e, RDOF_LH_DEFAULT);
    pthread_mutex_lock(&W.log_mu);
    if (!W.log.buf)
        rdof_bring_init(&W.log, W.log_buf, sizeof W.log_buf);
    pthread_mutex_unlock(&W.log_mu);
    W.running = true;
    if (pthread_create(&W.thr, NULL, server, NULL)) {
        W.running = false;
        close(fd);
        close(W.wake[0]);
        close(W.wake[1]);
        errno = EAGAIN;
        return -1;
    }
    return ntohs(a.sin_port);
}

void rdof_hostweb_stop(void)
{
    if (!W.running)
        return;
    W.running = false;
    (void)!write(W.wake[1], "x", 1);
    pthread_join(W.thr, NULL);
    close(W.lfd);
    close(W.wake[0]);
    close(W.wake[1]);
}

bool rdof_hostweb_running(void)
{
    return W.running;
}
