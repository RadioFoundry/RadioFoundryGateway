// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The settings, version 2 (rdof/setup.h, rdof/broker.h): every version 1 fixture upgraded runs the same roles with
 * the same settings; every version 2 fixture is taken or refused as its name says (good-, bad-; bad-semantic- is
 * refused by the C only, the schema can't see it); the broker's claims, auto picks and releases. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/benchcfg.h"
#include "rdof/broker.h"
#include "rdof/config.h"
#include "rdof/sdr/config.h"
#include "rdof/setup.h"

static int fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = malloc((size_t)n + 1);
    *len = fread(t, 1, (size_t)n, f);
    t[*len] = 0;
    fclose(f);
    return t;
}

/* Everything one settings document runs, described: roles, each radio's settings, the SDR's, the bench's. */
static void describe_all(const char *t, size_t n, char *out, size_t cap)
{
    static rdof_roles_t roles;
    static rdof_config_t cfg;
    static rdof_sdr_config_t sc;
    char err[200];
    size_t o = 0;
#define PUT(...) o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__)
    if (!rdof_box_roles(t, n, &roles, err, sizeof err)) {
        PUT("roles: %s", err);
        return;
    }
    PUT("box %s hub %d radios %d sdr %d:%s\n", roles.box, roles.usb_hub, roles.nradios, roles.has_sdr, roles.sdr);
    for (int k = 0; k < roles.nradios; k++) {
        PUT("radio %s: ", roles.radios[k]);
        if (rdof_config_parse_radio(t, n, roles.radios[k], &cfg, err, sizeof err)) {
            o += rdof_config_describe(&cfg, out + o, cap - o);
            rdof_cfg_port_t p[2];
            int np = rdof_config_ports(&cfg, p);
            for (int i = 0; i < np; i++)
                PUT(" port %s %d/%d/%d %ld %s %s", p[i].modem_port, p[i].uart_num, p[i].uart_tx, p[i].uart_rx,
                    p[i].uart_speed, p[i].usb_serial, p[i].usb_port);
            PUT(" sdr %d %.1f split %d,%d,%d,%d,%.1f\n", cfg.sdr_duplex, cfg.tx_power_dbm, cfg.split_power_cap,
                cfg.split_prebuffer, cfg.split_lease_s, cfg.split_cooldown_s, cfg.split_max_key_s);
        } else {
            PUT("%s\n", err);
        }
    }
    if (roles.has_sdr) {
        if (rdof_sdr_config_parse(t, n, &sc, err, sizeof err))
            o += rdof_sdr_config_describe(&sc, out + o, cap - o);
        else
            PUT("sdr: %s\n", err);
    }
    rdof_bench_radio_t br[RDOF_BENCH_MAX_RADIOS];
    int nb = rdof_bench_radios_parse(t, n, br, RDOF_BENCH_MAX_RADIOS, err, sizeof err);
    PUT("bench %d", nb);
    for (int k = 0; k < nb; k++)
        PUT(" %s/%s/%d/%d/%d/%d/%ld/%04x:%04x", br[k].name, br[k].proto, br[k].port, br[k].uart, br[k].tx, br[k].rx,
            br[k].baud, br[k].vid, br[k].pid);
    rdof_benchtx_policy_t pol;
    if (rdof_bench_tx_parse(t, n, &pol, err, sizeof err) == 0) {
        PUT(" tx %d %.1f", pol.nfreqs, pol.max_key_s);
        for (int k = 0; k < pol.nfreqs; k++)
            PUT(" %u", (unsigned)pol.freqs_hz[k]);
    }
#undef PUT
}

static void v1_fixtures(const char *dir)
{
    DIR *d = opendir(dir);
    CHECK(d, "%s", dir);
    struct dirent *e;
    int n = 0;
    static char a[16384], b[16384];
    while (d && (e = readdir(d))) {
        if (!strstr(e->d_name, ".json"))
            continue;
        char path[512], err[200];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t len, ulen;
        char *t = slurp(path, &len);
        CHECK(t && rdof_config_version(t, len) == 1, "%s: version 1", e->d_name);
        char *up = rdof_config_upgrade(t, len, &ulen, err, sizeof err);
        CHECK(up, "%s: upgrade: %s", e->d_name, err);
        if (up) {
            static rdof_setup_t s;
            CHECK(rdof_config_version(up, ulen) == 2, "%s: upgraded is version 2", e->d_name);
            CHECK(rdof_setup_parse(up, ulen, &s, err, sizeof err), "%s: upgraded: %s\n%s", e->d_name, err, up);
            describe_all(t, len, a, sizeof a);
            describe_all(up, ulen, b, sizeof b);
            CHECK(!strcmp(a, b), "%s: runs the same\nv1: %s\nv2: %s\nupgraded: %s", e->d_name, a, b, up);
            /* and again: version 2 comes back as it is */
            size_t l2;
            char *again = rdof_config_upgrade(up, ulen, &l2, err, sizeof err);
            CHECK(again && l2 == ulen && !memcmp(again, up, ulen), "%s: upgrade twice", e->d_name);
            free(again);
        }
        free(up);
        free(t);
        n++;
    }
    if (d)
        closedir(d);
    CHECK(n >= 7, "%d version 1 fixtures", n);
}

static void v2_fixtures(const char *dir)
{
    DIR *d = opendir(dir);
    CHECK(d, "%s", dir);
    struct dirent *e;
    int n = 0;
    static char out[16384];
    while (d && (e = readdir(d))) {
        if (!strstr(e->d_name, ".json"))
            continue;
        char path[512], err[200] = "";
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t len;
        char *t = slurp(path, &len);
        static rdof_setup_t s;
        rdof_platform_t any = rdof_platform_host; /* a host with UARTs too: the fixtures use both */
        any.uart_ports = true;
        bool ok = rdof_setup_parse(t, len, &s, err, sizeof err) && rdof_broker_check(&s, &any, err, sizeof err);
        /* each running role's settings through its view */
        for (int k = 0; ok && k < s.nrole; k++) {
            if (!s.role[k].run)
                continue;
            if (s.role[k].type != RDOF_ROLE_MONITOR) {
                static rdof_config_t cfg;
                ok = rdof_config_parse_radio(t, len, s.role[k].name, &cfg, err, sizeof err);
            }
            for (int i = 0; ok && i < 2; i++) {
                int dev = i ? s.role[k].tx : s.role[k].rx;
                static rdof_sdr_config_t sc;
                if (dev >= 0 && s.dev[dev].kind == RDOF_DEV_SDR)
                    ok = rdof_sdr_config_parse_device(t, len, s.dev[dev].name, &sc, err, sizeof err);
            }
        }
        bool good = !strncmp(e->d_name, "good-", 5);
        CHECK(ok == good, "%s: %s (%s)", e->d_name, ok ? "taken" : "refused", err);
        if (!good && getenv("RDOF_SHOW_ERRORS"))
            printf("%s: %s\n", e->d_name, err);
        if (good) {
            size_t vl;
            char *v = rdof_config_view(t, len, NULL, &vl, err, sizeof err);
            CHECK(v, "%s: view: %s", e->d_name, err);
            if (v && getenv("RDOF_SHOW_VIEWS"))
                printf("%s:\n%s\n", e->d_name, v);
            free(v);
            (void)out;
        }
        free(t);
        n++;
    }
    if (d)
        closedir(d);
    CHECK(n >= 10, "%d version 2 fixtures", n);
}

static void broker(void)
{
    const char *t = "{\"version\": 2, \"cc\": 1, \"devices\": {"
                    "\"p\": {\"kind\": \"sdr\", \"driver\": \"iio\", \"rx\": {\"rate\": 1000000, \"center_hz\": 438.525},"
                    "        \"tx\": {}},"
                    "\"rtl\": {\"kind\": \"sdr\"},"
                    "\"rf1\": {\"kind\": \"radio\", \"port\": {\"path\": \"/dev/x\"}},"
                    "\"hs\": {\"kind\": \"modem\", \"port\": {\"path\": \"/dev/y\"}}},"
                    "\"roles\": {\"M\": {\"type\": \"monitor\", \"rx\": {\"device\": \"p\"}, \"channels\": [{\"ch\": 1}]}},"
                    "\"channels\": {\"1\": 438.4875},"
                    "\"bench\": {\"devices\": [\"rf1\"]}}";
    static rdof_setup_t s;
    static rdof_broker_t b;
    char err[200];
    CHECK(rdof_setup_parse(t, strlen(t), &s, err, sizeof err), "%s", err);
    rdof_broker_init(&b, &s, &rdof_platform_host);
    CHECK(rdof_broker_claim_role(&b, 0, err, sizeof err), "%s", err);
    int P = rdof_setup_device(&s, "p"), R = rdof_setup_device(&s, "rtl"), RF = rdof_setup_device(&s, "rf1");
    /* RX on the monitor's channel: shared, on p */
    rdof_claim_t q = {.dir = RDOF_DIR_RX, .device = -1, .freq = 438487500};
    CHECK(rdof_broker_claim(&b, &q, RDOF_OWNER_BENCH(1), err, sizeof err) == P, "rx shared");
    /* RX inside p's window */
    q.freq = 438725000;
    CHECK(rdof_broker_claim(&b, &q, RDOF_OWNER_BENCH(1), err, sizeof err) == P, "rx in the window");
    /* RX outside it: the idle RTL-SDR */
    q.freq = 438450000;
    CHECK(rdof_broker_claim(&b, &q, RDOF_OWNER_BENCH(1), err, sizeof err) == R, "rx on the idle SDR");
    /* TX: p's carrier first, then the bench radio, then busy */
    rdof_claim_t tx = {.dir = RDOF_DIR_TX, .device = -1, .freq = 438487500};
    CHECK(rdof_broker_claim(&b, &tx, RDOF_OWNER_BENCH(1), err, sizeof err) == P, "tx on the SDR's carrier");
    CHECK(rdof_broker_claim(&b, &tx, RDOF_OWNER_BENCH(2), err, sizeof err) == RF, "tx on the bench radio");
    CHECK(rdof_broker_claim(&b, &tx, RDOF_OWNER_BENCH(3), err, sizeof err) == -1, "tx busy");
    /* by name: the modem isn't the bench's */
    tx.device = rdof_setup_device(&s, "hs");
    CHECK(rdof_broker_claim(&b, &tx, RDOF_OWNER_BENCH(3), err, sizeof err) == -2, "not the bench's");
    rdof_broker_release(&b, RDOF_OWNER_BENCH(1));
    tx.device = -1;
    CHECK(rdof_broker_claim(&b, &tx, RDOF_OWNER_BENCH(3), err, sizeof err) == P, "released carrier");
    /* the board: one SDR, no path ports */
    const char *two = "{\"version\": 2, \"channels\": {\"1\": 438.4875, \"2\": 438.5}, \"cc\": 1, \"devices\": {"
                      "\"a\": {\"kind\": \"sdr\"}, \"b\": {\"kind\": \"sdr\"}},"
                      "\"roles\": {\"A\": {\"type\": \"monitor\", \"rx\": {\"device\": \"a\"}, \"channels\": [1]},"
                      "\"B\": {\"type\": \"monitor\", \"rx\": {\"device\": \"b\"}, \"channels\": [2]}}}";
    CHECK(rdof_setup_parse(two, strlen(two), &s, err, sizeof err), "%s", err);
    CHECK(rdof_broker_check(&s, &rdof_platform_host, err, sizeof err), "%s", err);
    CHECK(!rdof_broker_check(&s, &rdof_platform_board, err, sizeof err) && strstr(err, "runs 1"), "%s", err);
    rdof_roles_t roles;
    CHECK(!rdof_box_roles(two, strlen(two), &roles, err, sizeof err) && strstr(err, "one SDR"), "%s", err);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "fixtures/config";
    char p[512];
    snprintf(p, sizeof p, "%s/v1", dir);
    v1_fixtures(p);
    snprintf(p, sizeof p, "%s/v2", dir);
    v2_fixtures(p);
    broker();
    printf("setup: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
