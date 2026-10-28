// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The settings, version 3 (rdof/site.h) and ID sets (rdof/idset.h): every fixture in fixtures/config/v3 is taken or
 * refused as its name says; the spec's four sites resolve to the paths and policies it describes; each good one
 * runs as version 2 settings the broker and the role parsers take. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/broker.h"
#include "rdof/config.h"
#include "rdof/idset.h"
#include "rdof/sdr/config.h"
#include "rdof/setup.h"
#include "rdof/site.h"

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

static rdof_idset_t set_of(const char *items)
{
    rdof_idset_t s;
    char err[96];
    rdof_idset_clear(&s);
    if (!rdof_idset_add(&s, items, err, sizeof err))
        printf("(%s)\n", err);
    rdof_idset_finish(&s);
    return s;
}

static void test_idset(void)
{
    char t[128], err[96];
    rdof_idset_t s = set_of("*,!9,!4000-4099");
    CHECK(rdof_idset_has(&s, 1) && rdof_idset_has(&s, 3100) && !rdof_idset_has(&s, 9), "star less 9");
    CHECK(!rdof_idset_has(&s, 4000) && !rdof_idset_has(&s, 4099) && rdof_idset_has(&s, 4100), "less a range");
    CHECK(!rdof_idset_listed(&s, 3100), "* isn't listed");
    rdof_idset_format(&s, t, sizeof t);
    CHECK(!strcmp(t, "*,!9,!4000-4099"), "%s", t);

    s = set_of("!350");
    CHECK(s.star && rdof_idset_has(&s, 349) && !rdof_idset_has(&s, 350), "only ! starts from *");

    s = set_of("202, 300-399,!350");
    CHECK(rdof_idset_has(&s, 202) && rdof_idset_listed(&s, 202) && rdof_idset_has(&s, 399), "plain");
    CHECK(!rdof_idset_has(&s, 350) && !rdof_idset_has(&s, 201), "plain less");
    rdof_idset_format(&s, t, sizeof t);
    CHECK(!strcmp(t, "202,300-349,351-399"), "%s", t);
    rdof_idrange_t r[4];
    CHECK(rdof_idset_ranges(&s, r, 4) == 3 && r[1].lo == 300 && r[1].hi == 349 && r[2].lo == 351, "ranges");

    s = set_of("100-199,150-250,251");
    rdof_idset_format(&s, t, sizeof t);
    CHECK(!strcmp(t, "100-251"), "merged: %s", t);

    s = set_of("*,3100");
    CHECK(rdof_idset_listed(&s, 3100) && !rdof_idset_listed(&s, 3101) && rdof_idset_has(&s, 3101), "* and listed");
    rdof_idset_format(&s, t, sizeof t);
    CHECK(!strcmp(t, "*,3100"), "%s", t);

    s = set_of("9990");
    CHECK(rdof_idset_single(&s) == 9990, "single");
    s = set_of("9990-9991");
    CHECK(rdof_idset_single(&s) == 0, "not single");
    rdof_idset_clear(&s);
    CHECK(rdof_idset_empty(&s) && !rdof_idset_has(&s, 1), "empty");

    static const char *const bad[] = {"0", "16777216", "5-3", "!*", "x", "1-", "-1", ",", NULL};
    for (int k = 0; bad[k]; k++) {
        rdof_idset_clear(&s);
        CHECK(!rdof_idset_add(&s, bad[k], err, sizeof err), "refused: %s", bad[k]);
    }
}

static rdof_site_t S;

static const rdof_path_t *path(const char *name)
{
    int k = rdof_site_path(&S, name);
    return k >= 0 ? &S.path[k] : NULL;
}

static bool load(const char *dir, const char *file)
{
    char p[512], err[200];
    size_t n;
    snprintf(p, sizeof p, "%s/v3/%s", dir, file);
    char *t = slurp(p, &n);
    bool ok = t && rdof_site_parse(t, n, &S, err, sizeof err);
    CHECK(ok, "%s: %s", file, t ? err : "can't read");
    free(t);
    return ok;
}

/* The spec's section 10 sites. */
static void test_scenarios(const char *dir)
{
    char t[128];
    const rdof_path_t *p;
    if (load(dir, "good-commercial.json")) {
        CHECK(S.npath == 16 && S.napp == 2 && S.nnet == 1, "16 paths");
        p = path("438.0875");
        CHECK(p && rdof_idset_single(&p->pol.tg) == 202 && p->pol.dir == RDOF_FLOW_BOTH, "TG 202 both ways");
        CHECK(p && rdof_idset_has(&p->pol.pc, 1234567) && p->pol.apps == 3 && p->pol.net == 0, "pc, apps, net");
        CHECK(p && p->slot == 0 && p->tx_slot == 1 && p->tx_freq >= 0 && p->peer_id == 9800100, "simplex path");
        CHECK(p && p->pol.src[RDOF_PK_TG] == RDOF_SRC_OWN && p->pol.src[RDOF_PK_PC] == RDOF_SRC_TABLE, "sources");
        CHECK(S.ndev == 1 && !strcmp(S.dev[0].name, "sdr0") && S.dev[0].can_tx && S.dev[0].transmits == 0xffff,
              "the SDR transmits on all 16");
        CHECK(rdof_site_freq(&S, "438.4875") == 14 && rdof_site_freq(&S, "438.48750") == 14, "by frequency");
    }
    if (load(dir, "good-two-repeaters.json")) {
        CHECK(S.npath == 4, "two repeaters, two slots each: %d", S.npath);
        p = path("R1/1");
        CHECK(p && p->slot == 1 && !rdof_idset_has(&p->pol.tg, 9) && rdof_idset_has(&p->pol.tg, 3100), "open");
        CHECK(p && !rdof_idset_has(&p->pol.tg, 4050) && p->pol.apps == 0, "open: deny list, no apps");
        CHECK(p && p->pol.src[RDOF_PK_TG] == 0 && p->pol.src[RDOF_PK_APPS] == 0, "from policy open");
        p = path("R2/2");
        CHECK(p && rdof_idset_has(&p->pol.tg, 3120) && !rdof_idset_has(&p->pol.tg, 3121), "closed");
        CHECK(p && p->pol.apps == (1u << 2 | 1u << 3) && rdof_idset_has(&p->pol.pc, 9800999), "closed: apps, pc");
        CHECK(S.dev[0].hears == RDOF_HEARS_IDLE && !S.dev[0].can_tx && S.dev[1].hears == RDOF_HEARS_NONE,
              "split halves");
        CHECK(rdof_site_path_rx(&S, 438750000, 2) == rdof_site_path(&S, "R1/2"), "heard on R1's input, slot 2");
    }
    if (load(dir, "good-sdr-repeater-forwarders.json")) {
        CHECK(S.npath == 6, "a repeater's two and four forwarders: %d", S.npath);
        p = path("fwd C");
        CHECK(p && p->tx_freq < 0 && p->pol.dir == RDOF_FLOW_RF_NET && p->pol.net == 1 && p->pol.tg.star,
              "receive only, to mqtt, every TG");
        p = path("R1/2");
        CHECK(p && rdof_idset_has(&p->pol.ptt, 3250) && p->pol.ptt_hold_s == 600 && p->pol.net == 0, "ptt range");
        CHECK(S.dev[0].transmits == 1u << 1, "the SDR transmits only on rpt out: %llx",
              (unsigned long long)S.dev[0].transmits);
        CHECK(S.net[1].calls && S.net[1].report, "mqtt carries calls");
    }
    if (load(dir, "good-monitor.json")) {
        CHECK(S.npath == 8, "8 paths");
        p = path("yard 2");
        CHECK(p && S.freq[p->freq].cc == -1 && p->tx_freq < 0 && p->pol.dir == RDOF_FLOW_RF_NET, "any cc, rx only");
        CHECK(p && p->pol.tg.star && p->pol.src[RDOF_PK_TG] == RDOF_SRC_DEFAULTS, "every TG, from defaults");
        CHECK(!S.dev[0].can_tx, "an RTL-SDR doesn't transmit");
    }
    if (load(dir, "good-dcdm-per-path.json")) {
        p = path("dc/2");
        CHECK(p && p->slot == 2 && p->tx_slot == 2 && rdof_idset_empty(&p->pol.pc), "dcdm slot 2, no pc");
        CHECK(path("dc/1") && path("dc/1")->peer_id == 9800501 && p && p->peer_id == 9800501, "per_path: %u %u",
              path("dc/1") ? path("dc/1")->peer_id : 0, p ? p->peer_id : 0);
        p = path("mon/2");
        CHECK(p && p->peer_id == 9800599 && rdof_idset_single(&p->pol.tg) == 3120, "peer_ids, rx-only slots");
        CHECK(S.dev[1].transmits == 1, "pinned");
    }
    /* the resolved view */
    if (load(dir, "good-two-repeaters.json")) {
        static char out[16384];
        size_t n = rdof_site_resolved(&S, out, sizeof out);
        CHECK(n > 100 && n < sizeof out - 1, "resolved: %zu", n);
        CHECK(strstr(out, "\"name\":\"R1/1\"") && strstr(out, "\"tg\":\"*,!9,!4000-4099\""), "%.300s", out);
        CHECK(strstr(out, "\"tg\":\"3100,3120,3177\",\"path\":\"R1/2\",\"how\":\"static\""), "reverse view");
        CHECK(strstr(out, "\"tg\":\"policy open\""), "sources");
        snprintf(t, sizeof t, "%zu", n);
    }
}

/* Every fixture: good- taken, bad- refused; a good one runs as version 2. */
static void test_fixtures(const char *dir)
{
    char p[512], err[200];
    snprintf(p, sizeof p, "%s/v3", dir);
    DIR *d = opendir(p);
    CHECK(d, "%s", p);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) {
        if (!strstr(e->d_name, ".json"))
            continue;
        snprintf(p, sizeof p, "%s/v3/%s", dir, e->d_name);
        size_t len;
        char *t = slurp(p, &len);
        bool good = !strncmp(e->d_name, "good-", 5);
        bool ok = rdof_site_parse(t, len, &S, err, sizeof err);
        CHECK(ok == good, "%s: %s", e->d_name, ok ? "taken" : err);
        if (!good)
            printf("  %s: %s\n", e->d_name, err);
        CHECK(rdof_config_version(t, len) == 3, "%s: version 3", e->d_name);
        if (good && ok) {
            size_t vl;
            char *v2 = rdof_site_lower(t, len, &vl, err, sizeof err);
            CHECK(v2, "%s: lower: %s", e->d_name, err);
            static rdof_setup_t s;
            rdof_platform_t any = rdof_platform_host;
            any.uart_ports = true;
            bool sok = v2 && rdof_setup_parse(t, len, &s, err, sizeof err);
            CHECK(sok, "%s: as version 2: %s\n%s", e->d_name, err, v2 ? v2 : "");
            if (sok)
                CHECK(rdof_broker_check(&s, &any, err, sizeof err), "%s: broker: %s", e->d_name, err);
            static rdof_roles_t roles;
            static rdof_config_t cfg;
            if (sok && rdof_box_roles(t, len, &roles, err, sizeof err)) {
                for (int k = 0; k < roles.nradios; k++)
                    CHECK(rdof_config_parse_radio(t, len, roles.radios[k], &cfg, err, sizeof err),
                          "%s: radio %s: %s", e->d_name, roles.radios[k], err);
                static rdof_sdr_config_t sc;
                if (roles.has_sdr)
                    CHECK(rdof_sdr_config_parse(t, len, &sc, err, sizeof err), "%s: sdr: %s", e->d_name, err);
            }
            free(v2);
        }
        free(t);
        n++;
    }
    if (d)
        closedir(d);
    CHECK(n >= 10, "fixtures: %d", n);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "fixtures/config";
    test_idset();
    test_scenarios(dir);
    test_fixtures(dir);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
