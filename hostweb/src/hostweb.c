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
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
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

static struct {
    atomic_bool running;
    int lfd, wake[2];
    pthread_t thr;
    char name[32];
    time_t t0;
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
    /* last heard */
    pthread_mutex_t lh_mu;
    rdof_lh_t lh;
    rdof_lh_entry_t lh_e[RDOF_LH_DEFAULT];
} W = {.st_mu = PTHREAD_MUTEX_INITIALIZER, .log_mu = PTHREAD_MUTEX_INITIALIZER, .lh_mu = PTHREAD_MUTEX_INITIALIZER,
       .set_mu = PTHREAD_MUTEX_INITIALIZER};

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
    b->version = "host";
    b->idf = NULL;
    b->uptime_s = (long long)(time(NULL) - W.t0);
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

/* The kernel's NTP state, read only (modes 0: nothing is set or adjusted). */
static bool host_clock(void *ctx, rdof_webapi_clock_t *c)
{
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
static void host_status(void *ctx, rdof_json_t *j, rdof_webapi_at_t at)
{
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
    rdof_json_bool(j, "mqtt", s.mqtt);
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
static void platform(rdof_webapi_platform_t *p, rdof_hostweb_settings_t *set)
{
    pthread_mutex_lock(&W.set_mu);
    const bool have = W.have_set;
    if (have)
        *set = W.set;
    pthread_mutex_unlock(&W.set_mu);
    memset(p, 0, sizeof *p);
    p->caps = RDOF_WEBAPI_CAP_SYS_MEM | RDOF_WEBAPI_CAP_OS_CLOCK;
    p->board = host_board;
    p->memory = host_memory;
    p->clock = host_clock;
    p->status_json = host_status;
    p->status_cap = 16384;
    p->log_cap = LOG_BYTES + 1;
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
    W.t0 = time(NULL);
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
