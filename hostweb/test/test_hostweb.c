// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The host web server: start on a free port with fake state and fetch the page, the status, last heard and the
 * log over a socket; the refused paths; the settings (a file: read, saved whole, the restart asked for; read only);
 * publishing from a thread while requests run. With --serve [PORT] [FILE] it stays up with moving fake state, for a
 * browser, its settings in FILE (editable; a save logs the restart it would do) or read only without one. */
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rdof/hostweb.h"

static int fails, checks;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

/* One request; the whole answer (headers and body) in out. */
static size_t http(int port, const char *req, char *out, size_t cap)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) {
        close(fd);
        return 0;
    }
    (void)!write(fd, req, strlen(req));
    size_t n = 0;
    ssize_t r;
    while (n < cap - 1 && (r = read(fd, out + n, cap - 1 - n)) > 0)
        n += (size_t)r;
    out[n] = 0;
    close(fd);
    return n;
}

static void fake(rdof_hostweb_status_t *s, int t)
{
    memset(s, 0, sizeof *s);
    s->role = RDOF_HOSTWEB_REPEATER;
    snprintf(s->name, sizeof s->name, "bench");
    snprintf(s->radio, sizeof s->radio, "SoapySDR PlutoSDR");
    snprintf(s->modem, sizeof s->modem, "SDR modem");
    snprintf(s->state, sizeof s->state, "idle");
    s->ready = true;
    s->rx_hz = 438000000;
    s->tx_hz = 438500000;
    s->cc = 1;
    s->cpu_load = 0.25;
    s->bursts_heard = (uint64_t)t * 3;
    s->slot[1].active = (t / 4) & 1;
    s->slot[1].src = 9800001;
    s->slot[1].dst = 9;
    s->slot[1].group = true;
    s->slot[1].has_ber = s->slot[1].has_rssi = true;
    s->slot[1].ber = 1.5f;
    s->slot[1].rssi = -71;
    s->parrot_on = true;
    snprintf(s->parrot_state, sizeof s->parrot_state, "%s", (t / 4) & 1 ? "recording" : "idle");
    s->parrot_slot = 2;
    s->parrot_replayed = (uint32_t)(t / 8);
    s->nextra = 1;
    snprintf(s->extra[0].name, sizeof s->extra[0].name, "late_chunks");
    s->extra[0].value = 2;
}

/* Two SDRs and a split's radio, as a tool with several devices would publish them */
static void fake_devices(rdof_hostweb_status_t *s, int t)
{
    s->nsdr = 2;
    for (int i = 0; i < 2; i++) {
        rdof_hostweb_sdr_t *d = &s->sdrs[i];
        snprintf(d->name, sizeof d->name, i ? "wide" : "pluto");
        snprintf(d->device, sizeof d->device, i ? "SoapySDR rtlsdr" : "libiio AD9361 local:");
        d->open = d->receiving = true;
        d->rate = i ? 2400000 : 768000;
        d->center_hz = i ? 438525000 : 438100000;
        d->gain_db = i ? NAN : 40;
        d->agc = i;
        d->samples_per_s = d->rate - 100;
        d->overflows = (uint64_t)i * 3;
        d->load = 0.42;
        d->nstage = 1;
        snprintf(d->stage[0].name, sizeof d->stage[0].name, "receiver");
        d->stage[0].share = 0.3f;
        d->tx = !i;
        d->tx_hz = 438500000, d->tx_lo_hz = 438250000;
        d->tx_on = (t / 4) & 1;
        d->hblink_peers = i ? 2 : 0, d->hblink_up = 1;
        d->nchan = 1;
        snprintf(d->chan[0].id, sizeof d->chan[0].id, i ? "M1" : "bench");
        snprintf(d->chan[0].name, sizeof d->chan[0].name, i ? "re\"ceive" : "receive");
        d->chan[0].freq = i ? 438425000 : 438000000, d->chan[0].in = true, d->chan[0].bursts = 12, d->chan[0].rssi = -80;
        d->chan[0].afc_hz = -12.34f, d->chan[0].idle_pct = 40.4f;
    }
    s->nmodem = 1;
    rdof_hostweb_modem_t *m = &s->modems[0];
    snprintf(m->name, sizeof m->name, "TX half");
    snprintf(m->kind, sizeof m->kind, "RadioFoundry radio");
    snprintf(m->port, sizeof m->port, "/dev/ttyACM0");
    snprintf(m->version, sizeof m->version, "RadioFoundry 1.2");
    m->open = m->ready = true;
    m->nextra = 1;
    snprintf(m->extra[0].name, sizeof m->extra[0].name, "misses");
    m->extra[0].value = 3;
}

/* The settings: a file, checked the way a tool would (here: a JSON object), saved whole */
static char cfg_path[512];
static int restarts;

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *t = calloc(1, 65536);
    if (t)
        (void)!fread(t, 1, 65535, f);
    fclose(f);
    return t;
}
static char *cfg_get(void *ctx) { return read_all(cfg_path); }
static bool cfg_save(void *ctx, const char *text, size_t len, char *err, size_t cap)
{
    if (len < 2 || text[0] != '{' || text[len - 1] != '}') {
        snprintf(err, cap, "not a JSON object");
        return false;
    }
    return rdof_hostweb_save_file(cfg_path, text, len, err, cap);
}
static size_t cfg_resolved(void *ctx, char *out, size_t cap)
{
    char *t = read_all(cfg_path);
    const int n = snprintf(out, cap, "resolved from %s: %s\n", cfg_path, t ? t : "(none)");
    free(t);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}
static void cfg_restart(void *ctx)
{
    restarts++;
    rdof_hostweb_logf("restart %d asked for (a tool would stop and rdof_hostweb_reexec now)", restarts);
}
static void settings(bool writable)
{
    rdof_hostweb_settings_t h = {0};
    h.writable = writable;
    snprintf(h.source, sizeof h.source, "%s %s", writable ? "file" : "command line flags, as", cfg_path);
    h.get = cfg_get;
    h.save = cfg_save;
    h.resolved = cfg_resolved;
    h.restart = cfg_restart;
    rdof_hostweb_settings(&h);
}

/* a POST with a body */
static size_t post(int port, const char *path, const char *body, char *out, size_t cap)
{
    char req[1024];
    snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s", path, strlen(body), body);
    return http(port, req, out, cap);
}

static void *spam(void *arg)
{
    rdof_hostweb_status_t s;
    for (int i = 0; i < 20000; i++) {
        fake(&s, i);
        rdof_hostweb_status(&s);
        rdof_hostweb_logf("parrot: line %d", i);
    }
    return arg;
}

int main(int argc, char **argv)
{
    static char b[200000];
    if (argc > 1 && !strcmp(argv[1], "--serve")) {
        if (argc > 3) {
            snprintf(cfg_path, sizeof cfg_path, "%s", argv[3]);
            settings(true);
        }
        int port = rdof_hostweb_start(NULL, argc > 2 ? atoi(argv[2]) : 0, "demo");
        if (port < 0) {
            perror("start");
            return 1;
        }
        printf("http://127.0.0.1:%d/\n", port);
        fflush(stdout);
        for (int t = 0;; t++) {
            rdof_hostweb_status_t s;
            fake(&s, t);
            fake_devices(&s, t);
            s.spectrum = true;
            rdof_hostweb_status(&s);
            for (int r = 0; r < 5; r++) { /* a noise floor and a tone that walks across the span */
                uint8_t row[RDOF_HOSTWEB_SPECTRUM_BINS];
                for (int i = 0; i < RDOF_HOSTWEB_SPECTRUM_BINS; i++)
                    row[i] = (uint8_t)(200 + rand() % 10);
                row[(t * 5 + r) % RDOF_HOSTWEB_SPECTRUM_BINS] = 80;
                rdof_hostweb_spectrum_row(row, RDOF_HOSTWEB_SPECTRUM_BINS, 438.1e6, 768000, 200);
            }
            if (t % 8 == 0)
                rdof_hostweb_logf("parrot: sending back slot 2 call %d", t / 8);
            sleep(1);
        }
    }

    CHECK(rdof_hostweb_start("999.1.1.1", 0, "x") < 0); /* a bad address */
    rdof_hostweb_logf("before start");
    int port = rdof_hostweb_start(NULL, 0, "test box");
    CHECK(port > 0);
    CHECK(rdof_hostweb_start(NULL, 0, "x") < 0); /* already running */

    /* before any status: the board without a radio */
    CHECK(http(port, "GET /api/status HTTP/1.1\r\n\r\n", b, sizeof b) > 0);
    CHECK(strstr(b, "200 OK") && strstr(b, "\"name\":\"test box\"") && !strstr(b, "\"roamer\""));
    CHECK(strstr(b, "\"rss\":") && !strstr(b, "\"rss\":0,") && strstr(b, "\"mem_total\":") && strstr(b, "\"mem_avail\":"));
    CHECK(strstr(b, "\"rss_peak\":") && !strstr(b, "heap_free") && !strstr(b, "\"crash\""));
    CHECK(strstr(b, "\"clock\":\"os\"") && strstr(b, "\"clock_os\":{\"source\":") && strstr(b, "\"known\":"));
    CHECK(strstr(b, "\"caps\":{\"ota\":false,\"restart\":false,\"wifi\":false,\"tasks\":false,\"coredump\":false,"
                    "\"board_mem\":false,\"sys_mem\":true,\"netclock\":false,\"os_clock\":true,\"links\":false,"
                    "\"settings\":false,\"settings_edit\":false,\"spectrum\":false}}"));
    CHECK(!strstr(b, "settings_source") && strstr(b, "\"reset\":\"start\""));

    CHECK(http(port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", b, sizeof b) > 40000);
    CHECK(strstr(b, "200 OK") && strstr(b, "text/html") && strstr(b, "/api/status") && strstr(b, "RadioFoundry"));
    CHECK(strstr(b, ".nocap{display:none") && strstr(b, "function applyCaps") && strstr(b, "data-cap=\"ota restart\""));
    CHECK(http(port, "GET /netclock.js HTTP/1.1\r\n\r\n", b, sizeof b) > 1000 && strstr(b, "text/javascript"));
    CHECK(http(port, "GET /wifilist.js HTTP/1.1\r\n\r\n", b, sizeof b) > 1000);

    rdof_hostweb_status_t s;
    fake(&s, 5);
    rdof_hostweb_status(&s);
    http(port, "GET /api/status HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "\"mode\":\"repeater\"") && strstr(b, "\"frequency\":438500000") && strstr(b, "\"colorCode\":1"));
    CHECK(strstr(b, "\"busy2\":true") && strstr(b, "\"src\":9800001") && strstr(b, "\"rssi\":-71"));
    CHECK(strstr(b, "\"parrot\":{\"state\":\"recording\"") && strstr(b, "\"late_chunks\":2") && strstr(b, "PlutoSDR"));
    s.error[0] = 'x';
    rdof_hostweb_status(&s);
    http(port, "GET /api/status HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "\"radio_ready\":false") && strstr(b, "\"radio_error\":\"x\""));
    CHECK(!strstr(b, "\"channels\""));
    /* the monitored channels: a peer's state only when it has one; an id can't break the JSON */
    s.nchan = 2;
    snprintf(s.chan[0].id, sizeof s.chan[0].id, "SDR01");
    s.chan[0].freq = 438075000, s.chan[0].bursts = 12, s.chan[0].rssi = -80, s.chan[0].peer = true;
    snprintf(s.chan[1].id, sizeof s.chan[1].id, "a\"b");
    s.chan[1].freq = 438150000;
    rdof_hostweb_status(&s);
    http(port, "GET /api/status HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "\"channels\":[{\"id\":\"SDR01\",\"freq\":438075000,\"bursts\":12,\"rssi\":-80,\"hblink\":false},"
                    "{\"id\":\"ab\",\"freq\":438150000,\"bursts\":0,\"rssi\":0}]"));

    rdof_hostweb_logf("parrot: sending back slot %d", 2);
    http(port, "GET /api/log HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "parrot: sending back slot 2\n") && strstr(b, "before start\n"));

    http(port, "GET /api/lastheard HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "\"calls\":[]"));
    rdof_tx_call_info_t c;
    memset(&c, 0, sizeof c);
    c.src = 9800001;
    c.dst = 9;
    c.group = true;
    c.slot = 2;
    c.cc = 1;
    c.frames = -1;
    rdof_hostweb_net_call("repeater", "bench", "start", &c, 0);
    http(port, "GET /api/lastheard?since=0 HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "\"dir\":\"net\"") && strstr(b, "\"src\":9800001") && strstr(b, "\"active\":true"));
    rdof_hostweb_net_call("repeater", "bench", "end", &c, 1.5);
    http(port, "GET /api/lastheard HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "\"active\":false"));

    /* what the host can't do: a clear JSON error, for every method */
    const char *no[] = {"GET /api/config HTTP/1.1\r\n\r\n", "GET /api/clock HTTP/1.1\r\n\r\n",
                        "GET /api/tasks HTTP/1.1\r\n\r\n",
                        "POST /api/restart HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
                        "POST /api/ota HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc", "DELETE /api/coredump HTTP/1.1\r\n\r\n"};
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) {
        http(port, no[i], b, sizeof b);
        CHECK(strstr(b, "501 Not Implemented") && strstr(b, "{\"error\":\"not on this platform\""));
    }
    http(port, "GET /nope HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "404"));
    http(port, "PUT / HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "405"));
    http(port, "garbage\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "400"));

    /* the settings: a file, editable */
    snprintf(cfg_path, sizeof cfg_path, "%s/rdof_hostweb_test_%ld.json", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
             (long)getpid());
    FILE *f = fopen(cfg_path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("{\"name\": \"one\"}", f);
        fclose(f);
    }
    settings(true);
    http(port, "GET /api/status HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "\"restart\":true") && strstr(b, "\"settings\":true,\"settings_edit\":true,"));
    CHECK(strstr(b, "\"settings_source\":\"file ") && strstr(b, "\"settings_ok\":true"));
    http(port, "GET /api/config HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "200 OK") && strstr(b, "\r\n\r\n{\"name\": \"one\"}") && strstr(b, "no-store"));
    http(port, "GET /api/config?download=1 HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "Content-Disposition: attachment; filename=\"") && strstr(b, ".json\""));
    http(port, "GET /api/resolved HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "200 OK") && strstr(b, "text/plain") && strstr(b, "resolved from ") && strstr(b, "\"one\""));
    /* refused: the file and the program as they were */
    post(port, "/api/config", "[1, 2]", b, sizeof b);
    CHECK(strstr(b, "400") && strstr(b, "not a JSON object") && restarts == 0);
    http(port, "POST /api/config HTTP/1.1\r\nContent-Length: 0\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "400") && restarts == 0);
    char *t = read_all(cfg_path);
    CHECK(t && !strcmp(t, "{\"name\": \"one\"}"));
    free(t);
    /* saved whole, then the restart */
    post(port, "/api/config", "{\"name\": \"two\", \"cc\": 1}", b, sizeof b);
    CHECK(strstr(b, "200 OK") && strstr(b, "saved; restarting") && restarts == 1);
    t = read_all(cfg_path);
    CHECK(t && !strcmp(t, "{\"name\": \"two\", \"cc\": 1}"));
    free(t);
    http(port, "GET /api/config HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "{\"name\": \"two\", \"cc\": 1}"));
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", cfg_path, (long)getpid());
    struct stat st;
    CHECK(stat(tmp, &st) != 0); /* no temporary file left */
    post(port, "/api/restart", "", b, sizeof b);
    CHECK(strstr(b, "200 OK") && strstr(b, "restarting") && restarts == 2);
    http(port, "GET /api/log HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "restart 2 asked for"));
    /* read only: shown, never written */
    settings(false);
    http(port, "GET /api/status HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "\"settings\":true,\"settings_edit\":false,") && strstr(b, "\"settings_source\":\"command line"));
    http(port, "GET /api/config HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "200 OK") && strstr(b, "\"two\""));
    post(port, "/api/config", "{\"name\": \"three\"}", b, sizeof b);
    CHECK(strstr(b, "403") && strstr(b, "read only") && restarts == 2);
    t = read_all(cfg_path);
    CHECK(t && strstr(t, "\"two\""));
    free(t);
    /* a file that can't be written: an error, nothing left behind */
    char err[200] = "";
    CHECK(!rdof_hostweb_save_file("/nonexistent-dir/x.json", "{}", 2, err, sizeof err) && err[0]);
    unlink(cfg_path);

    /* the spectrum: wanted once the page asks; rows newer than since, the channels in the span */
    CHECK(!rdof_hostweb_spectrum_wanted());
    fake(&s, 0);
    s.spectrum = true;
    s.nchan = 1;
    snprintf(s.chan[0].id, sizeof s.chan[0].id, "M\"1");
    s.chan[0].freq = 438200000;
    fake_devices(&s, 0);
    rdof_hostweb_status(&s);
    http(port, "GET /api/status HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "\"spectrum\":true}"));
    CHECK(strstr(b, "\"sdrs\":[{\"name\":\"pluto\",\"started\":true,\"frontend\":\"host\",\"rtl_open\":true,"
                    "\"rtl\":\"libiio AD9361 local:\",\"receiving\":true,\"sample_rate\":768000,\"center_hz\":438100000,"
                    "\"gain_db\":40.0,\"agc\":false,\"samples_per_s\":767900,"));
    CHECK(strstr(b, "\"cpu_load\":0.42,\"load_of\":\"the receive loop\",\"cpu_stages\":{\"receiver\":0.3},"
                    "\"tx_hz\":438500000,\"tx_lo_hz\":438250000,"));
    CHECK(strstr(b, "\"channels\":[{\"id\":\"bench\",\"name\":\"receive\",\"freq\":438000000,\"in\":true,\"bursts\":12,"
                    "\"rssi\":-80,\"afc\":-12.3,\"idle\":40}]},{\"name\":\"wide\""));
    CHECK(strstr(b, "\"agc\":true,") && strstr(b, "\"hblink_peers\":2,\"hblink_up\":1,"));
    CHECK(strstr(b, "{\"id\":\"M1\",\"name\":\"receive\",")); /* the quote taken out */
    CHECK(strstr(b, "\"modems\":[{\"name\":\"TX half\",\"kind\":\"RadioFoundry radio\",\"role\":\"\",\"port\":\"/dev/ttyACM0\","
                    "\"open\":true,\"ready\":true,\"version\":\"RadioFoundry 1.2\",\"state\":\"\",\"misses\":3}]"));
    CHECK(strstr(b, "\"cpu_process\":"));
#if defined(__APPLE__) || defined(__linux__)
    CHECK(strstr(b, "\"cpu\":[0") || strstr(b, "\"cpu\":[1"));
#endif
    http(port, "GET /api/spectrum HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "200 OK") && strstr(b, "\"seq\":0,") && strstr(b, "\"rows\":[]") && rdof_hostweb_spectrum_wanted());
    uint8_t row[RDOF_HOSTWEB_SPECTRUM_BINS];
    for (int i = 0; i < 3; i++) {
        memset(row, 200 - i, sizeof row);
        rdof_hostweb_spectrum_row(row, RDOF_HOSTWEB_SPECTRUM_BINS, 438.1e6, 768000, 200);
    }
    http(port, "GET /api/spectrum?since=0 HTTP/1.1\r\n\r\n", b, sizeof b);
    CHECK(strstr(b, "{\"seq\":3,\"bins\":512,\"row_ms\":200,\"rate\":768000,\"center\":438100000,\"rows\":[\"yMjI"));
    CHECK(strstr(b, "\"channels\":[{\"id\":\"bench\",\"freq\":438000000,\"in\":true},"
                    "{\"id\":\"TX\",\"freq\":438500000,\"in\":false},{\"id\":\"M_1\",\"freq\":438200000,\"in\":true}]}"));
    http(port, "GET /api/spectrum?since=2 HTTP/1.1\r\n\r\n", b, sizeof b);
    const char *rows = strstr(b, "\"rows\":[\"xsbG"); /* the third row (198) alone: 684 characters of base64 */
    CHECK(rows && strlen(rows) > 9 + 684 && !strncmp(rows + 9 + 684, "\"],", 3));
    http(port, "GET /api/spectrum?since=99 HTTP/1.1\r\n\r\n", b, sizeof b); /* ahead: the tool restarted */
    CHECK(strstr(b, "\"seq\":3,") && strstr(b, "\"rows\":[\"yMjI"));

    /* publishing from a thread while requests run */
    pthread_t th;
    pthread_create(&th, NULL, spam, NULL);
    for (int i = 0; i < 50; i++) {
        CHECK(http(port, "GET /api/status HTTP/1.1\r\n\r\n", b, sizeof b) > 0);
        CHECK(http(port, "GET /api/log HTTP/1.1\r\n\r\n", b, sizeof b) > 0);
    }
    pthread_join(th, NULL);

    rdof_hostweb_stop();
    CHECK(!rdof_hostweb_running());
    CHECK(http(port, "GET / HTTP/1.1\r\n\r\n", b, sizeof b) == 0);
    rdof_hostweb_status(&s); /* no-ops now */
    rdof_hostweb_logf("x");
    printf("hostweb: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
